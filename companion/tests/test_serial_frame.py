#!/usr/bin/env python3
"""USB 帧协议编解码测试(与固件 tests/test_usb_link.c 同构,共享测试意图)。

覆盖:组帧↔解析回环(各类型/各长度)、组帧拒绝(超上限)、垃圾前缀、半帧
跨 feed、0xA5 0xA5 0x5A 假锚点、超长 len、非法 type、方向过滤(DOWN 只收
CTRL/SYS,UP 只收 EVENT/AUDIO/SYS_RESP)、坏 checksum、连续多帧、日志噪声
混流恢复。两侧状态机必须逐字节一致。

方向感知契约(审查 P1):解码器按本端接收方向过滤合法类型 + 类型上限校验,
均在写 payload 前拒绝。每帧类型固定方向:DEC_DIR 表给出 → 回环/恢复用例
必须用与类型匹配的方向构造解码器,否则合法帧会被 ERR_BAD 拒。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from serial_frame import (DIR_DOWN, DIR_UP, FRAME_AUDIO, FRAME_CTRL,
                          FRAME_DONE, FRAME_ERR_BAD, FRAME_ERR_OVERSIZE,
                          FRAME_ERR_SUM, FRAME_EVENT, FRAME_NONE, FRAME_SYS,
                          FRAME_SYS_RESP, HEADER, TYPE_MAX_PAYLOAD,
                          FrameDecoder, FrameError, encode_frame)

TYPES = (FRAME_EVENT, FRAME_AUDIO, FRAME_CTRL, FRAME_SYS, FRAME_SYS_RESP)

# 每帧类型的固有接收方向(下行 CTRL/SYS;上行 EVENT/AUDIO/SYS_RESP)
DEC_DIR = {FRAME_CTRL: DIR_DOWN, FRAME_SYS: DIR_DOWN,
           FRAME_EVENT: DIR_UP, FRAME_AUDIO: DIR_UP, FRAME_SYS_RESP: DIR_UP}


def assert_roundtrip(ftype, payload):
    frame = encode_frame(ftype, payload)
    assert len(frame) == HEADER + len(payload)

    dec = FrameDecoder(dir=DEC_DIR[ftype])
    rc = FRAME_NONE
    for i, b in enumerate(frame):
        rc = dec.feed_byte(b)
        if i < len(frame) - 1:
            assert rc == FRAME_NONE, f"type={ftype} len={len(payload)} i={i} rc={rc}"
        else:
            assert rc == FRAME_DONE
    assert dec.type == ftype
    assert bytes(dec.payload) == bytes(payload)


def feed_head(dec, head):
    """喂帧头(不期待 DONE),返回是否出现过 ERR_BAD(违约在第 3 字节即返回,
    后续字节把 rc 覆盖回 NONE,必须用捕获标志断言)。"""
    saw = False
    for b in head:
        if dec.feed_byte(b) == FRAME_ERR_BAD:
            saw = True
    return saw


def test_build_roundtrip():
    for t in TYPES:
        maxp = TYPE_MAX_PAYLOAD[t]
        data = bytes((i * 7 + 1) & 0xFF for i in range(maxp))
        assert_roundtrip(t, b"")                        # 空载荷
        assert_roundtrip(t, data[:1])
        assert_roundtrip(t, data[:min(maxp, 128)])      # 通用小载荷
        assert_roundtrip(t, data)                       # 类型上限边界


def test_build_rejects():
    # 组帧拒绝:每类型上限 +1 均抛 FrameError(方向无关,编码端约束)
    for t in TYPES:
        try:
            encode_frame(t, bytes(TYPE_MAX_PAYLOAD[t] + 1))
            assert False, f"type={t} len 超上限应抛 FrameError"
        except FrameError:
            pass


def test_garbage_prefix():
    # 日志/启动噪声在前:重扫吞掉,帧仍恢复(与 C 端同字节噪声)
    noise = (b"I (1234) main: AI Passport " + "固件启动".encode("utf-8")
             + b"\r\n\x00\x01\xff")
    payload = b"\x01\x02\x03"
    frame = encode_frame(FRAME_EVENT, payload)

    dec = FrameDecoder(dir=DIR_UP)
    rc = FRAME_NONE
    for b in noise:
        rc = dec.feed_byte(b)
    for b in frame:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_EVENT
    assert bytes(dec.payload) == payload


def test_half_frame_resume():
    # 半帧挂起(模拟串口分片到达),补喂后完成
    payload = bytes(range(100))
    frame = encode_frame(FRAME_CTRL, payload)

    dec = FrameDecoder(dir=DIR_DOWN)
    rc = FRAME_NONE
    for i, b in enumerate(frame[:10]):    # 只有帧头前 10 字节
        assert dec.feed_byte(b) == FRAME_NONE
    for i, b in enumerate(frame[10:]):
        rc = dec.feed_byte(b)
        if i < len(frame) - 10 - 1:
            assert rc == FRAME_NONE
        else:
            assert rc == FRAME_DONE
    assert dec.type == FRAME_CTRL
    assert bytes(dec.payload) == payload


def test_false_anchor():
    # 0xA5 0xA5 0x5A:第二、三字节构成合法帧头,不得丢帧
    payload = b"\xAA\xBB"
    frame = encode_frame(FRAME_EVENT, payload)
    stream = b"\xA5" + frame                 # 假 magic0 + 真帧紧随

    dec = FrameDecoder(dir=DIR_UP)
    rc = FRAME_NONE
    for b in stream:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_EVENT
    assert bytes(dec.payload) == payload


def test_oversize_len():
    # payload_len 超类型上限:立即 ERR_OVERSIZE 重扫,且不缓冲巨型长度
    # (0xA5, 0x5A, type, len_lo, len_hi):0x1100=4352 > 每类型上限(3200)
    for t in TYPES:
        dec = FrameDecoder(dir=DEC_DIR[t])
        rc = FRAME_NONE
        for b in (0xA5, 0x5A, t, 0x00, 0x11):
            rc = dec.feed_byte(b)
        assert rc == FRAME_ERR_OVERSIZE, \
            f"type={t} len=0x1100 应 OVERSIZE, rc={rc}"

    # 每类型上限 +1 → OVERSIZE(写 payload 前拒绝)
    cases = (
        (FRAME_EVENT, 0x01, 0x02, DIR_UP),     # 513 > 512
        (FRAME_AUDIO, 0x81, 0x0C, DIR_UP),     # 3201 > 3200
        (FRAME_CTRL, 0x01, 0x08, DIR_DOWN),    # 2049 > 2048
        (FRAME_SYS, 0x81, 0x00, DIR_DOWN),     # 129 > 128
        (FRAME_SYS_RESP, 0x01, 0x08, DIR_UP),  # 2049 > 2048
    )
    for t, lo, hi, d in cases:
        dec = FrameDecoder(dir=d)
        rc = dec.feed_byte(0xA5)
        rc = dec.feed_byte(0x5A)
        rc = dec.feed_byte(t)
        rc = dec.feed_byte(lo)
        rc = dec.feed_byte(hi)
        assert rc == FRAME_ERR_OVERSIZE, f"type={t} 超上限应 OVERSIZE, rc={rc}"

    # 边界:CTRL 2048 DOWN / AUDIO 3200 UP → DONE
    for t, d, n in ((FRAME_CTRL, DIR_DOWN, 2048), (FRAME_AUDIO, DIR_UP, 3200)):
        frame = encode_frame(t, bytes(n))
        dec = FrameDecoder(dir=d)
        for b in frame:
            rc = dec.feed_byte(b)
        assert rc == FRAME_DONE, f"type={t} len={n} 边界应 DONE"
        assert dec.type == t and len(dec.payload) == n

    # 重扫后正常帧仍可解析
    dec = FrameDecoder(dir=DIR_DOWN)
    frame = encode_frame(FRAME_SYS, b"\x01")
    for b in frame:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_SYS


def test_bad_type():
    # type 越界(0/6/0xFF):两方向都非法 → ERR_BAD(捕获标志断言)
    bads = (bytes((0xA5, 0x5A, 0x00, 0x01, 0x00, 0x00, 0x00)),   # type 0
            bytes((0xA5, 0x5A, 0x06, 0x01, 0x00, 0x00, 0x00)),   # type 6
            bytes((0xA5, 0x5A, 0xFF, 0x01, 0x00, 0x00, 0x00)))   # type 0xFF
    for bad in bads:
        for d in (DIR_DOWN, DIR_UP):
            dec = FrameDecoder(dir=d)
            assert feed_head(dec, bad), f"type=0x{bad[2]:02X} dir={d} 应 ERR_BAD"


def test_dir_filter():
    # 方向过滤(审查 P1):下行端拒上行类型帧、上行端拒下行类型帧,
    # 均在写 payload 前 ERR_BAD
    down = FrameDecoder(dir=DIR_DOWN)
    for head in ((0xA5, 0x5A, FRAME_AUDIO, 0x80, 0x0C),   # 3200B 音频帧下行
                 (0xA5, 0x5A, FRAME_EVENT, 0x01, 0x00),
                 (0xA5, 0x5A, FRAME_SYS_RESP, 0x01, 0x00)):
        assert feed_head(down, head), "下行端应拒上行类型"
    # 违约重扫后,合法下行帧恢复
    frame = encode_frame(FRAME_CTRL, b"\x01")
    rc = FRAME_NONE
    for b in frame:
        rc = down.feed_byte(b)
    assert rc == FRAME_DONE and down.type == FRAME_CTRL

    up = FrameDecoder(dir=DIR_UP)
    for head in ((0xA5, 0x5A, FRAME_CTRL, 0x01, 0x00),
                 (0xA5, 0x5A, FRAME_SYS, 0x01, 0x00)):
        assert feed_head(up, head), "上行端应拒下行类型"
    frame = encode_frame(FRAME_EVENT, b"\x02")
    rc = FRAME_NONE
    for b in frame:
        rc = up.feed_byte(b)
    assert rc == FRAME_DONE and up.type == FRAME_EVENT


def test_bad_checksum():
    payload = b"\x10\x20\x30"
    frame = bytearray(encode_frame(FRAME_AUDIO, payload))
    frame[6] ^= 0xFF                      # 篡改一个载荷字节 → checksum 不符

    dec = FrameDecoder(dir=DIR_UP)
    rc = FRAME_NONE
    for b in frame:
        rc = dec.feed_byte(b)
    assert rc == FRAME_ERR_SUM

    # 重扫后正常帧仍可解析
    frame2 = encode_frame(FRAME_EVENT, payload)
    for b in frame2:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_EVENT


def test_concat_frames():
    # 两帧紧连:两次 DONE,载荷互不串扰
    p1, p2 = b"\x01\x02\x03", b"\x04\x05\x06\x07\x08"
    stream = encode_frame(FRAME_EVENT, p1) + encode_frame(FRAME_AUDIO, p2)

    dec = FrameDecoder(dir=DIR_UP)
    frames = []
    rc = FRAME_NONE
    for b in stream:
        rc = dec.feed_byte(b)
        if rc == FRAME_DONE:
            frames.append((dec.type, bytes(dec.payload)))
    assert rc == FRAME_DONE
    assert frames == [(FRAME_EVENT, p1), (FRAME_AUDIO, p2)]


def test_log_noise_between_bytes():
    # 日志行从帧中间插入:payload 累积被噪声填满 → checksum 失败 → 立即
    # 重扫;噪声期间不得 DONE;噪声结束后后续完整帧恢复。
    # 帧头 10 字节后插入:剩余 payload = 64-10+1(checksum) = 55 字节。
    payload = bytes(range(64))
    frame = encode_frame(FRAME_AUDIO, payload)
    noise = ("E (500) mode: 模式切换失败\r\n"
             "E (501) mode: 重试(日志填满剩余载荷)\r\n").encode("utf-8")
    assert len(noise) >= 55

    dec = FrameDecoder(dir=DIR_UP)
    rc = FRAME_NONE
    for b in frame[:10]:
        rc = dec.feed_byte(b)
    for b in noise:
        rc = dec.feed_byte(b)
    assert rc != FRAME_DONE                    # 噪声期间:残帧 ERR 或落回 NONE

    # 后续完整帧恢复
    frame2 = encode_frame(FRAME_SYS_RESP, b"\x42")
    for b in frame2:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_SYS_RESP
    assert bytes(dec.payload) == b"\x42"


def main():
    tests = [test_build_roundtrip, test_build_rejects, test_garbage_prefix,
             test_half_frame_resume, test_false_anchor, test_oversize_len,
             test_bad_type, test_dir_filter, test_bad_checksum,
             test_concat_frames, test_log_noise_between_bytes]
    for t in tests:
        t()
        print(f"  {t.__name__} ok")
    print("test_serial_frame: 全部通过")


if __name__ == "__main__":
    main()
