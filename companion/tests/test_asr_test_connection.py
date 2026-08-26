#!/usr/bin/env python3
"""asr_test_connection 单测: 本地 fake WS server 三场景验证零音频握手。

覆盖:
- ack(空 full server response)→ 成功, 返回 {}
- error 帧(type=0xF)→ RuntimeError, 消息含服务端 code/message
- 握手后服务端立即断开 → RuntimeError(连接异常)
- 服务端不回包 → RuntimeError(超时)

运行: companion/.venv/bin/python companion/tests/test_asr_test_connection.py
"""
import asyncio
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import websockets  # noqa: E402

from asr_client import asr_test_connection  # noqa: E402

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


def check_raises(name, fn, want_substr):
    """fn 必须抛 RuntimeError 且消息含 want_substr。"""
    try:
        fn()
    except RuntimeError as e:
        ok = want_substr in str(e)
        if not ok:
            FAILURES.append(f"{name}: RuntimeError 消息 {str(e)!r} 缺 {want_substr!r}")
        print(f"[{'PASS' if ok else 'FAIL'}] {name} ({str(e)[:60]}...)")
        return
    except Exception as e:  # noqa: BLE001
        FAILURES.append(f"{name}: 抛了 {type(e).__name__} 而非 RuntimeError: {e}")
        print(f"[FAIL] {name} (抛 {type(e).__name__})")
        return
    FAILURES.append(f"{name}: 未抛异常")
    print(f"[FAIL] {name} (未抛)")


def server_frame_ack():
    """空 full server response: Header + Sequence + Size(0), 无 payload。"""
    return b"\x11\x90\x01\x00" + struct.pack(">I", 0) + struct.pack(">I", 0)


def server_frame_error(code, message):
    """error 帧: Header(type=0xF) + code(4) + msize(4) + message。"""
    body = message.encode("utf-8")
    return (b"\x11\xf0\x01\x00" + struct.pack(">I", code) +
            struct.pack(">I", len(body)) + body)


async def serve(mode, path, recv=True):
    """fake ASR server。mode: ack / error / close / silent。"""

    async def handler(ws):
        if recv:
            await ws.recv()          # full client request
        if mode == "ack":
            await ws.send(server_frame_ack())
        elif mode == "error":
            await ws.send(server_frame_error(4010005, "invalid key"))
        elif mode == "close":
            await ws.close()
        elif mode == "silent":
            await asyncio.sleep(30)  # 不回包, 等客户端超时
        await ws.wait_closed()

    server = await websockets.serve(handler, "127.0.0.1", 0, max_size=None)
    port = server.sockets[0].getsockname()[1]
    cfg = {"volcano_api_key": "test-key",
           "volcano_ws_url": f"ws://127.0.0.1:{port}",
           "volcano_resource_id": "volc.bigasr.sauc.duration"}
    try:
        return await path(cfg)
    finally:
        server.close()
        await server.wait_closed()


def test_ack():
    async def path(cfg):
        data = await asr_test_connection(cfg)
        return data

    out = asyncio.run(serve("ack", path))
    check("ack 握手成功返回空 dict", out, {})


def test_error_frame():
    async def path(cfg):
        await asr_test_connection(cfg)
        return None

    check_raises("error 帧 → RuntimeError 含 code",
                 lambda: asyncio.run(serve("error", path)), "4010005")


def test_server_close():
    async def path(cfg):
        await asr_test_connection(cfg)
        return None

    check_raises("服务端立即断开 → RuntimeError 连接异常",
                 lambda: asyncio.run(serve("close", path)), "ASR 连接异常")


def test_timeout():
    async def path(cfg):
        await asr_test_connection(cfg, timeout=0.5)
        return None

    check_raises("服务端不回包 → RuntimeError 超时",
                 lambda: asyncio.run(serve("silent", path)), "超时")


def test_parse_server_gunzip_cap():
    """gzip 输出超 GZIP_MAX → unknown/too_large, 不解出巨型对象(PERF P2-5)。"""
    import gzip
    from asr_client import GZIP_MAX, parse_server
    blob = gzip.compress(b"a" * (GZIP_MAX + 1))
    raw = (b"\x11\x90\x11\x00" + struct.pack(">I", 0)
           + struct.pack(">I", len(blob)) + blob)
    kind, _flags, data = parse_server(raw)
    check("超限 gunzip 不返回巨型 result", kind, "unknown")
    check("超限标记 too_large", "too_large" in data, True)


def test_parse_server_size_cap():
    from asr_client import WS_MAX_SIZE, parse_server
    raw = (b"\x11\x90\x11\x00" + struct.pack(">I", 0)
           + struct.pack(">I", WS_MAX_SIZE + 1) + b"x")
    kind, _flags, data = parse_server(raw)
    check("超大 size 字段拒绝", kind, "unknown")
    check("超大 size 标记", data.get("too_large"), WS_MAX_SIZE + 1)


def main():
    test_ack()
    test_error_frame()
    test_server_close()
    test_timeout()
    test_parse_server_gunzip_cap()
    test_parse_server_size_cap()
    if FAILURES:
        print(f"\n{len(FAILURES)} 个失败:")
        for f in FAILURES:
            print(" -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()
