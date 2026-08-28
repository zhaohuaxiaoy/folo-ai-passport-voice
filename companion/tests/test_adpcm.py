#!/usr/bin/env python3
"""adpcm 单测: 与固件 main/adpcm.c 的共享向量(位序/表/重建公式逐字节对齐) +
往返 SNR + 块独立可解 + 边界参数。

运行: companion/.venv/bin/python companion/tests/test_adpcm.py  (无需 pytest)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from adpcm import (  # noqa: E402
    ADPCM_BLOCK_BYTES, ADPCM_BLOCK_SAMPLES, AdpcmState,
    decode_block, decode_block_samples, encode_block,
)

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


# 语音状 16kHz 测试信号(与 C 端 tests/test_adpcm.c 的 make_signal 同式)
def make_signal(n, seed):
    out = []
    for i in range(n):
        t = (i + seed * n) / 16000.0
        env = 0.55 + 0.45 * math.sin(2.0 * math.pi * 3.0 * t)
        v = (0.62 * math.sin(2.0 * math.pi * 220.0 * t)
             + 0.28 * math.sin(2.0 * math.pi * 440.0 * t)
             + 0.10 * math.sin(2.0 * math.pi * 1750.0 * t))
        out.append(int(env * v * 11000.0))
    return out


def snr_db(ref, got):
    sig = sum(s * s for s in ref)
    err = sum((a - b) ** 2 for a, b in zip(ref, got))
    if err == 0.0:
        return 999.0
    return 10.0 * math.log10(sig / err)


# 1) 共享向量:编码字节必须与 C 端 tests/test_adpcm.c 固化值一致
def test_shared_vector_encode():
    samples = [0, 1000, 2000, 4000, 8000, 4000, 0, -4000,
               -8000, -4000, 0, 100, 50, 0, -50, -100]
    blk = encode_block(AdpcmState(), samples)
    check("编码共享向量(000000007777e7ff68080808)",
          blk.hex(), "000000007777e7ff68080808")


# 2) 共享向量:解码样本必须与 C 端打印值一致(跨语言对拍锁定)
def test_shared_vector_decode():
    samples = [0, 1000, 2000, 4000, 8000, 4000, 0, -4000,
               -8000, -4000, 0, 100, 50, 0, -50, -100]
    blk = encode_block(AdpcmState(), samples)
    got = decode_block_samples(blk, 16)
    want = [0, 11, 41, 104, 240, 533, -14, -1134,
            -3537, -3880, 180, -373, 130, -327, 88, -290]
    check("解码共享向量(C 端固化)", got, want)
    check("解码首样本精确", got[0], samples[0])


# 3) 往返 SNR:4:1 压缩率下的失真下限(门槛 18dB,与 C 端一致)
def test_roundtrip_snr():
    pcm = make_signal(ADPCM_BLOCK_SAMPLES, 0)
    blk = encode_block(AdpcmState(), pcm)
    check("编码长度 804", len(blk), ADPCM_BLOCK_BYTES)
    out = decode_block(blk)
    check("解码样本数", len(out), ADPCM_BLOCK_SAMPLES)
    check("首样本精确", out[0], pcm[0])
    snr = snr_db(pcm, out)
    print(f"  往返 SNR = {snr:.1f} dB (门槛 18)")
    check("SNR ≥ 18dB", snr >= 18.0, True)


# 4) 块独立:编码端 index 跨块延续(音质),但每块头部带快照 → 丢块不传染
def test_block_independence():
    pcm_blocks = [make_signal(ADPCM_BLOCK_SAMPLES, b) for b in range(5)]
    st = AdpcmState()
    enc = [encode_block(st, p) for p in pcm_blocks]
    ref = [decode_block(e) for e in enc]
    for drop in range(5):
        for b in range(5):
            if b == drop:
                continue
            check(f"丢块{drop}后块{b}逐字节一致", decode_block(enc[b]), ref[b])
    for b in range(5):
        snr = snr_db(pcm_blocks[b], ref[b])
        check(f"块{b} SNR ≥ 18dB", snr >= 18.0, True)


# 5) 边界:短数据返回空;奇数样本末字节高半字节留 0
def test_bounds():
    check("短数据拒绝", decode_block(b"\x00" * 3), [])
    odd_in = [i * 300 - 1200 for i in range(9)]
    blk = encode_block(AdpcmState(), odd_in, samples=9)
    check("奇数样本编码长度 8", len(blk), 8)
    out = decode_block_samples(blk, 9)
    check("奇数样本解码", out[0], odd_in[0])
    check("奇数样本解码长度", len(out), 9)


def main():
    test_shared_vector_encode()
    test_shared_vector_decode()
    test_roundtrip_snr()
    test_block_independence()
    test_bounds()
    if FAILURES:
        print("test_adpcm: FAILURES")
        for f in FAILURES:
            print(" ", f)
        sys.exit(1)
    print("test_adpcm: OK")


if __name__ == "__main__":
    main()
