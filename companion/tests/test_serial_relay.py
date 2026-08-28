#!/usr/bin/env python3
"""relay + SerialTransport 集成测试(os.openpty 假设备侧)。

覆盖:
- _build_transport "usb" 分支(SerialTransport + usb_port 直连 + 无效 channel)
- 全流程: 握手 ping/pong → hello → voice.start → 音频帧 → voice.end
  → 定稿注入 + 下行 transcript(final:false 预览 / final:true 定稿)
- SYS 命令往返(relay._handle_stdin_line("!mode usb") → 设备收 SYS 帧)
- 设备关闭(master) → relay 随断连收束退出

运行: companion/.venv/bin/python companion/tests/test_serial_relay.py
"""
import asyncio
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from relay import (  # noqa: E402
    AUDIO_FRAME_BYTES, CTRL_UUID, EVENT_UUID, AUDIO_UUID,
    Relay, RelayError, _build_transport,
)
from serial_frame import (  # noqa: E402
    FRAME_AUDIO, FRAME_CTRL, FRAME_EVENT, FRAME_SYS, FRAME_SYS_RESP,
    encode_frame,
)
from serial_transport import SerialTransport  # noqa: E402
from test_relay import (  # noqa: E402
    wait_until, wait_session_done, FakeInjector, make_fake_asr_factory,
)
from test_serial_transport import (  # noqa: E402
    close_pair, make_pair, read_all_frames, read_frame, write_all,
)

FAILURES = []


def check(name, got, want):
    """本地 check: 失败进本模块 FAILURES(不走 test_relay 的全局)。"""
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


def _eof_input(prompt=""):
    """注入 stdin: 立即 EOF(stdin 循环收束, 不阻塞测试)。"""
    raise EOFError


def device_script(master, results):
    """假设备侧(独立线程): 握手 → 事件流 → 收下行 → 收 SYS → 关闭。

    事件流在订阅完成前就发送: SerialTransport 的订阅前缓冲(pending)负责
    暂存, relay 的 hello/voice.start 不丢——这正是缓冲的设计意图。
    """
    results.setdefault("down", [])

    def expect(frame, what):
        f = read_frame(master, timeout=3.0)
        assert f == frame, f"设备侧等 {what}, 实际 {f}"

    # 1. 握手
    expect((FRAME_SYS, b"ping"), "握手 ping")
    write_all(master, encode_frame(FRAME_SYS_RESP, b"pong"))

    # 2. 事件流(全部在订阅前发出, 走 pending 缓冲)。
    #    下行窗口从 voice.start 起算: 预览(final:false)在 ASR 首结果到达
    #    时即下行, 早于 voice.end —— 窗口必须覆盖它, 否则丢在 pty 缓冲。
    write_all(master, encode_frame(FRAME_EVENT,
                                   b'{"event":"device.hello","proto":2}\n'))
    write_all(master, encode_frame(FRAME_EVENT,
                                   b'{"event":"voice.start"}\n'))
    sys_frame = None
    deadline = time.monotonic() + 2.5
    for _ in range(3):
        write_all(master, encode_frame(FRAME_AUDIO, bytes(AUDIO_FRAME_BYTES)))
        time.sleep(0.02)
    write_all(master, encode_frame(FRAME_EVENT, b'{"event":"voice.end"}\n'))

    # 3. 收 relay 下行(transcript 预览/定稿帧, 窗口内 SYS 命令可能到达
    #    ——不能吞进 down 列表, 记录后跳出(它就是第 4 步要等的)。
    #    read_all_frames: 一次 os.read 可能含多帧, 全部取出不丢帧。
    while time.monotonic() < deadline and sys_frame is None:
        for f in read_all_frames(master, timeout=0.2):
            if f[0] == FRAME_CTRL:
                results["down"].append(f[1])
            elif f[0] == FRAME_SYS:
                sys_frame = f[1]

    # 4. 等 SYS 命令(若未在窗口内收到)
    if sys_frame is None:
        f = read_frame(master, timeout=3.0)
        assert f is not None and f[0] == FRAME_SYS, f"SYS 命令未到, 实际 {f}"
        sys_frame = f[1]
    results["sys"] = sys_frame
    write_all(master, encode_frame(FRAME_SYS_RESP, b"switching to USB (reboot)"))

    # 5. 关闭 master → 读线程 EOF → on_disconnect → relay 收束
    time.sleep(0.2)
    os.close(master)


def test_build_transport_usb():
    t = _build_transport({"channel": "usb"})
    check("usb 分支返回 SerialTransport",
          isinstance(t, SerialTransport), True)
    t2 = _build_transport({"channel": "usb", "usb_port": "/dev/cu.usbmodem1"})
    check("usb_port 非空直连(不扫描)", t2._fixed_port, "/dev/cu.usbmodem1")
    try:
        _build_transport({"channel": "bogus"})
        check("无效 channel 抛 RelayError", False, True)
    except RelayError:
        check("无效 channel 抛 RelayError", True, True)


def test_relay_over_usb():
    """全流程 + !命令 syscmd 往返 + 断连收束。"""
    async def scenario():
        master, slave, port = make_pair()
        t = SerialTransport(handshake_timeout=1.0)
        injector = FakeInjector()
        relay = Relay(t,
                      asr_factory=make_fake_asr_factory(
                          [("你好", False), ("你好世界", True)], {}),
                      inject_fn=injector, timeout=5, do_approval=False,
                      stdin_input=_eof_input)
        results = {}
        dev = asyncio.create_task(asyncio.to_thread(device_script, master,
                                                    results))
        task = asyncio.create_task(relay.run(device_addr=port))  # 直连, 不扫描

        # 全流程: hello → voice.start → 音频帧 → voice.end → 定稿注入
        await wait_session_done(relay)
        check("定稿注入输入框一次", injector.calls, ["你好世界"])

        # !命令 SYS 往返(替代 USB 模式缺失的控制台)
        await relay._handle_stdin_line("!mode usb")
        await wait_until(lambda: "sys" in results, what="SYS 命令到达设备")
        check("SYS 帧内容(mode usb)", results["sys"], b"mode usb")

        # 设备关闭(master) → relay 随断连收束
        await asyncio.wait_for(task, 5.0)
        check("relay 正常收束(无异常)", task.exception() is None, True)
        await dev

        # 下行断言: 预览(final:false) + 定稿(final:true)
        # (json.dumps 输出带空格 `{"type": "transcript", ...}`, 按解析后字段过滤)
        parsed = []
        for p in results["down"]:
            try:
                parsed.append(json.loads(p))
            except ValueError:
                continue
        downs = [d for d in parsed if d.get("type") == "transcript"]
        previews = [d for d in downs if not d.get("final")]
        finals = [d for d in downs if d.get("final")]
        check("下行预览(final:false)", previews and previews[0]["text"], "你好")
        check("下行定稿(final:true)", finals and finals[0]["text"], "你好世界")

        await t.disconnect()
        os.close(slave)

    asyncio.run(scenario())
    if FAILURES:
        sys.exit(1)


def main():
    test_build_transport_usb()
    test_relay_over_usb()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()


async def test_stdin_loop_stop_while_blocked():
    """审查 P1-4: stop 置位时阻塞中的 stdin 读不得无限等待 —— _stdin_loop
    以 FIRST_COMPLETED 竞争立即收束(GUI 退出线程 join 不再挂死)。"""
    from threading import Event as TEvent
    gate = TEvent()

    def blocked_input(prompt):
        gate.wait(10)          # 模拟 stdin 阻塞读(永不自行返回)

    t = _build_transport({"channel": "usb"})
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5, do_approval=False,
                  stdin_input=blocked_input)
    loop_task = asyncio.create_task(relay._stdin_loop())
    await asyncio.sleep(0.3)             # input 线程已进入阻塞
    relay._stop.set()                    # 断开/退出置位
    await asyncio.wait_for(loop_task, 1.0)   # 1s 内必须收束(旧实现挂死)
    check("stop 竞争下 _stdin_loop 及时收束", loop_task.done(), True)
    gate.set()                           # 释放阻塞线程(测试进程退出不等 10s)
    await asyncio.sleep(0.05)
