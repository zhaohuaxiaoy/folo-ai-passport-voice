#!/usr/bin/env python3
"""USB 帧协议编解码测试(与固件 tests/test_usb_link.c 同构,共享测试意图)。

覆盖:组帧↔解析回环(各类型/各长度)、组帧拒绝(超上限)、垃圾前缀、半帧
跨 feed、0xA5 0xA5 0x5A 假锚点、超长 len、非法 type、坏 checksum、连续
多帧、日志噪声混流恢复。两侧状态机必须逐字节一致。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from serial_frame import (FRAME_AUDIO, FRAME_CTRL, FRAME_DONE, FRAME_ERR_BAD,
                          FRAME_ERR_OVERSIZE, FRAME_ERR_SUM, FRAME_EVENT,
                          FRAME_NONE, FRAME_SYS, FRAME_SYS_RESP, HEADER,
                          PAYLOAD_MAX, FrameDecoder, FrameError, encode_frame)

TYPES = (FRAME_EVENT, FRAME_AUDIO, FRAME_CTRL, FRAME_SYS, FRAME_SYS_RESP)


def assert_roundtrip(ftype, payload):
    frame = encode_frame(ftype, payload)
    assert len(frame) == HEADER + len(payload)

    dec = FrameDecoder()
    rc = FRAME_NONE
    for i, b in enumerate(frame):
        rc = dec.feed_byte(b)
        if i < len(frame) - 1:
            assert rc == FRAME_NONE, f"type={ftype} len={len(payload)} i={i} rc={rc}"
        else:
            assert rc == FRAME_DONE
    assert dec.type == ftype
    assert bytes(dec.payload) == bytes(payload)


def test_build_roundtrip():
    data = bytes((i * 7 + 1) & 0xFF for i in range(PAYLOAD_MAX))
    for t in TYPES:
        assert_roundtrip(t, b"")          # 空载荷
        assert_roundtrip(t, data[:1])
        assert_roundtrip(t, data[:128])   # SYS 上限附近
        assert_roundtrip(t, data[:2048])  # CTRL 上限附近
        assert_roundtrip(t, data[:3200])  # 音频帧
        assert_roundtrip(t, data)         # 载荷上限


def test_build_rejects():
    try:
        encode_frame(FRAME_EVENT, bytes(PAYLOAD_MAX + 1))
        assert False, "len 超上限应抛 FrameError"
    except FrameError:
        pass


def test_garbage_prefix():
    # 日志/启动噪声在前:重扫吞掉,帧仍恢复(与 C 端同字节噪声)
    noise = (b"I (1234) main: AI Passport " + "固件启动".encode("utf-8")
             + b"\r\n\x00\x01\xff")
    payload = b"\x01\x02\x03"
    frame = encode_frame(FRAME_EVENT, payload)

    dec = FrameDecoder()
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

    dec = FrameDecoder()
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

    dec = FrameDecoder()
    rc = FRAME_NONE
    for b in stream:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_EVENT
    assert bytes(dec.payload) == payload


def test_oversize_len():
    # payload_len 超上限:立即 ERR_OVERSIZE 重扫,且不缓冲巨型长度
    head = bytes((0xA5, 0x5A, FRAME_EVENT, 0x00, 0x11))  # len = 0x1100 > 3200
    dec = FrameDecoder()
    rc = FRAME_NONE
    for b in head:
        rc = dec.feed_byte(b)
    assert rc == FRAME_ERR_OVERSIZE

    # 边界(阈值 4096→3200 与固件契约一致):3201 → OVERSIZE;3200 → DONE
    head1 = bytes((0xA5, 0x5A, FRAME_AUDIO, 0x81, 0x0C))  # len = 3201
    dec = FrameDecoder()
    rc = FRAME_NONE
    for b in head1:
        rc = dec.feed_byte(b)
    assert rc == FRAME_ERR_OVERSIZE
    frame = encode_frame(FRAME_AUDIO, bytes(PAYLOAD_MAX))  # 3200 边界合法
    dec = FrameDecoder()
    for b in frame:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_AUDIO
    assert len(dec.payload) == PAYLOAD_MAX

    # 重扫后正常帧仍可解析
    frame = encode_frame(FRAME_SYS, b"\x01")
    for b in frame:
        rc = dec.feed_byte(b)
    assert rc == FRAME_DONE
    assert dec.type == FRAME_SYS


def test_bad_type():
    bads = (bytes((0xA5, 0x5A, 0x00, 0x01, 0x00, 0x00, 0x00)),   # type 0
            bytes((0xA5, 0x5A, 0x06, 0x01, 0x00, 0x00, 0x00)),   # type 6
            bytes((0xA5, 0x5A, 0xFF, 0x01, 0x00, 0x00, 0x00)))   # type 0xFF
    for bad in bads:
        dec = FrameDecoder()
        # 错误发生在第 3 字节,后续字节喂入会落回 NONE —— 用捕获标志断言
        saw = False
        for b in bad:
            if dec.feed_byte(b) == FRAME_ERR_BAD:
                saw = True
        assert saw


def test_bad_checksum():
    payload = b"\x10\x20\x30"
    frame = bytearray(encode_frame(FRAME_AUDIO, payload))
    frame[6] ^= 0xFF                      # 篡改一个载荷字节 → checksum 不符

    dec = FrameDecoder()
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

    dec = FrameDecoder()
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

    dec = FrameDecoder()
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
             test_bad_type, test_bad_checksum, test_concat_frames,
             test_log_noise_between_bytes]
    for t in tests:
        t()
        print(f"  {t.__name__} ok")
    print("test_serial_frame: 全部通过")


if __name__ == "__main__":
    main()
