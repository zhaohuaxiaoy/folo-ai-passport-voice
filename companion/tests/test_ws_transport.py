#!/usr/bin/env python3
"""ws_transport 单测: 本机回环 WS(测试里用真实 websockets client 模拟设备侧)。

覆盖:
- 5 方法契约(WsTransport 与 BleakTransport/FakeTransport 对齐)
- 帧类型契约: 下行=文本帧 + 行尾 '\n'; 上行 EVENT=文本帧 / AUDIO=二进制帧
- 订阅前缓冲(设备 hello 在 start_notify 之前到达不丢)
- 断连回调; 连接超时; 单设备(第二台被拒)
- relay 集成: WsTransport + FakeASR 全流程
  (hello → voice.start → 音频帧 → voice.end → 定稿注入 + 下行定稿文本帧)

运行: companion/.venv/bin/python companion/tests/test_ws_transport.py
"""
import asyncio
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import websockets

from relay import (  # noqa: E402
    AUDIO_FRAME_BYTES, CTRL_UUID, EVENT_UUID, AUDIO_UUID,
    Relay, RelayError,
)
from ws_transport import WsTransport, WsError  # noqa: E402
from test_relay import (  # noqa: E402
    wait_until, wait_session_done,
    FakeASR, FakeInjector, make_fake_asr_factory,
)

FAILURES = []


def check(name, got, want):
    """本地 check: 失败进本模块 FAILURES(不走 test_relay 的全局,退出码真实)。"""
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


async def wait_ws_connected(t):
    """等服务器收到设备连接(connect 任务随后返回)。"""
    await wait_until(lambda: t._ws is not None, what="服务器收到设备连接")


async def open_device(t):
    """设备侧连入 WsTransport 的 server, 返回 websockets client。

    必须先等服务端完成 start_serving(端口才绑定、t.port 才更新为实际
    端口)——否则读到初始 0 会被 websockets 当缺省端口(80)去连, 报
    ConnectionRefusedError(真实炸过的 flaky 源, 不能靠 sleep 侥幸)。
    """
    await wait_until(lambda: t._server is not None, what="WS server 已监听")
    ws = await websockets.connect(f"ws://127.0.0.1:{t.port}", open_timeout=3)
    await wait_ws_connected(t)
    return ws


# ---- 5 方法契约 + 帧类型 ----

def test_frame_contracts():
    """下行文本帧 + '\n'; 上行 EVENT 文本 / AUDIO 二进制; 订阅前缓冲不丢。"""
    async def scenario():
        t = WsTransport(port=0)
        got = {"event": [], "audio": []}

        async def on_event(d):
            got["event"].append(d)

        async def on_audio(d):
            got["audio"].append(d)

        task = asyncio.create_task(t.connect("ws://x"))   # 内部等首台设备
        await asyncio.sleep(0.05)
        ws = await open_device(t)
        await task

        # 设备连上立即发 hello(文本帧)——relay 侧还没 start_notify
        await ws.send('{"event":"device.hello","proto":1}\n')
        await asyncio.sleep(0.05)
        check("订阅前消息进入缓冲", len(t._pending), 1)

        await t.start_notify(EVENT_UUID, on_event)
        await t.start_notify(AUDIO_UUID, on_audio)
        check("两次订阅后缓冲清空", len(t._pending), 0)
        # 回调是 async 的:_dispatch 经 create_task 投递,断言前须让出事件循环一轮
        await wait_until(lambda: len(got["event"]) == 1,
                         what="缓冲 hello 冲刷到 EVENT 回调")
        check("缓冲 hello 冲刷到 EVENT 回调",
              got["event"], [b'{"event":"device.hello","proto":1}\n'])

        # 下行契约: 文本帧 + '\n'(设备 feed_rx 按 \n 切行, 二进制帧被忽略)
        await t.write_gatt_char(CTRL_UUID, b'{"type":"transcript","text":"hi","final":false}')
        raw = await asyncio.wait_for(ws.recv(), timeout=2)
        check("下行是文本帧(str)", isinstance(raw, str), True)
        check("下行带行尾换行", raw.endswith("\n"), True)
        check("下行 JSON 可解析且文本正确", json.loads(raw)["text"], "hi")

        # 上行 AUDIO: 二进制帧 → AUDIO 回调
        await ws.send(bytes(AUDIO_FRAME_BYTES))
        await wait_until(lambda: len(got["audio"]) == 1, what="AUDIO 帧到达")
        check("AUDIO 二进制帧分发(3200B)", len(got["audio"][0]), AUDIO_FRAME_BYTES)
        check("EVENT 回调未收到音频帧", len(got["event"]), 1)

        await t.disconnect()
        await ws.close()
    asyncio.run(scenario())
    if FAILURES:
        sys.exit(1)


def test_subscribe_order_irrelevant():
    """start_notify 顺序无关: AUDIO 先注册, 缓冲的 EVENT 行仍在第二次注册后冲刷。"""
    async def scenario():
        t = WsTransport(port=0)
        got = []
        task = asyncio.create_task(t.connect("ws://x"))
        await asyncio.sleep(0.05)
        ws = await open_device(t)
        await task
        await ws.send('{"event":"voice.start","workflow":"build"}\n')
        await asyncio.sleep(0.05)
        await t.start_notify(AUDIO_UUID, lambda d: got.append(("a", d)))
        check("仅 AUDIO 注册,EVENT 缓冲未冲刷", len(t._pending), 1)
        await t.start_notify(EVENT_UUID, lambda d: got.append(("e", d)))
        check("EVENT 后注册,缓冲行冲刷", got, [("e", b'{"event":"voice.start","workflow":"build"}\n')])
        await t.disconnect()
        await ws.close()
    asyncio.run(scenario())
    if FAILURES:
        sys.exit(1)


def test_disconnect_callback():
    """设备断连 → on_disconnect 回调(与 BLE disconnected_callback 对齐)。"""
    async def scenario():
        t = WsTransport(port=0)
        dcb = {"called": False}
        task = asyncio.create_task(
            t.connect("ws://x", on_disconnect=lambda: dcb.__setitem__("called", True)))
        await asyncio.sleep(0.05)
        ws = await open_device(t)
        await task
        await ws.close()
        await wait_until(lambda: dcb["called"], what="断连回调")
        check("设备断连触发 on_disconnect", dcb["called"], True)
        check("断连后 _ws 已清空", t._ws, None)
        await t.disconnect()
    asyncio.run(scenario())
    if FAILURES:
        sys.exit(1)


def test_connect_timeout():
    """等待设备超时 → WsError(relay 会包成 RelayError 退出)。"""
    async def scenario():
        t = WsTransport(port=0, connect_timeout=0.2)
        try:
            await t.connect("ws://x")
            check("连接超时应抛 WsError", False, True)
        except WsError as e:
            check("连接超时抛 WsError", "超时" in str(e), True)
    asyncio.run(scenario())
    if FAILURES:
        sys.exit(1)


def test_single_device():
    """v1 单设备: 第二台连接被拒(busy), 不影响第一台。"""
    async def scenario():
        t = WsTransport(port=0)
        task = asyncio.create_task(t.connect("ws://x"))
        await asyncio.sleep(0.05)
        ws1 = await open_device(t)
        await task
        ws2 = await websockets.connect(f"ws://127.0.0.1:{t.port}", open_timeout=3)
        try:
            await asyncio.wait_for(ws2.recv(), timeout=2)
            # 被拒连接会收到 close: recv 抛 ConnectionClosed
        except websockets.exceptions.ConnectionClosed as e:
            check("第二台被拒(code=4000 busy)", e.code, 4000)
        # 第一台仍可用
        await t.write_gatt_char(CTRL_UUID, b'{"type":"transcript","text":"x","final":false}')
        raw = await asyncio.wait_for(ws1.recv(), timeout=2)
        check("第一台连接不受影响", json.loads(raw)["text"], "x")
        await t.disconnect()
        await ws1.close()
        await ws2.close()
    asyncio.run(scenario())
    if FAILURES:
        sys.exit(1)


# ---- relay 集成: 真实 WS 回环全流程 ----

def test_relay_over_ws():
    """relay + WsTransport: hello → voice.start → 音频帧 → voice.end →
    定稿注入 + 下行定稿文本帧; 设备断开 → relay 收束退出。"""
    async def scenario():
        t = WsTransport(port=0)
        injector = FakeInjector()
        holder = {}
        relay = Relay(t,
                      asr_factory=make_fake_asr_factory(
                          [("你好", False), ("你好世界", True)], holder),
                      inject_fn=injector, timeout=5, do_approval=False)
        task = asyncio.create_task(relay.run())   # scan → connect(等设备)
        await asyncio.sleep(0.05)
        ws = await open_device(t)

        # 设备侧上行: hello → voice.start → 音频帧 → voice.end
        await ws.send('{"event":"device.hello","proto":1}\n')
        await ws.send('{"event":"voice.start","workflow":"build"}\n')
        await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
        for _ in range(3):
            await ws.send(bytes(AUDIO_FRAME_BYTES))
            await asyncio.sleep(0.02)
        await ws.send('{"event":"voice.end"}\n')
        await wait_session_done(relay)

        check("定稿注入输入框一次", injector.calls, ["你好世界"])

        # 下行: relay 发过预览(final:false)与定稿(final:true)文本帧
        down = []
        while True:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=0.3)
            except asyncio.TimeoutError:
                break
            if isinstance(raw, str) and raw.strip():
                down.append(json.loads(raw))
        finals = [d for d in down if d.get("type") == "transcript" and d.get("final")]
        previews = [d for d in down if d.get("type") == "transcript" and not d.get("final")]
        check("下行预览(final:false)", previews and previews[0]["text"], "你好")
        check("下行定稿(final:true)", finals and finals[0]["text"], "你好世界")

        # 设备断开 → relay 收束退出(与 BLE 断连语义对齐)
        await ws.close()
        await wait_until(lambda: task.done(), what="relay 随断连收束退出")
        check("relay 正常收束(无异常)", task.exception() is None, True)

    asyncio.run(scenario())
    if FAILURES:
        sys.exit(1)


def main():
    test_frame_contracts()
    test_subscribe_order_irrelevant()
    test_disconnect_callback()
    test_connect_timeout()
    test_single_device()
    test_relay_over_ws()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()
