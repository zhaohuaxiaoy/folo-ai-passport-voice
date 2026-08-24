#!/usr/bin/env python3
"""relay 单测: 分片重组(纯函数) + FakeTransport 断言 扫描→连接→订阅→帧喂
ASR→注入序列; voice 会话掉帧统计; 审批流(approval_request 写入 + agent.action);
转写下行(final:false 预览 / final:true 定稿 / 超长切分 / 写失败隔离)。

运行: companion/.venv/bin/python companion/tests/test_relay.py  (无需 pytest)
"""
import asyncio
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from relay import (  # noqa: E402
    AUDIO_FRAME_BYTES, CTRL_UUID, EVENT_UUID, AUDIO_UUID,
    TRANSCRIPT_TEXT_MAX, Relay, RelayError,
    reassemble_audio, reassemble_event, split_transcript,
)

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


async def wait_until(cond, timeout=3.0, what="condition"):
    deadline = time.monotonic() + timeout
    while not cond():
        if time.monotonic() > deadline:
            raise TimeoutError(f"等待超时: {what}")
        await asyncio.sleep(0.01)


# ---- 模拟件 ----

class FakeTransport:
    """模拟 BLE 链路: 记录操作序列, 捕获通知回调供测试注入分片数据。"""

    def __init__(self, devices=("AA:BB:CC:DD:EE:FF",)):
        self.events = []        # (op, *args)
        self.devices = list(devices)
        self.handlers = {}      # uuid -> handler
        self.disconnect_cb = None

    async def scan_for_device(self, name, timeout):
        self.events.append(("scan", name))
        return self.devices[0] if self.devices else None

    async def connect(self, address, on_disconnect=None):
        self.events.append(("connect", address))
        self.disconnect_cb = on_disconnect

    async def write_gatt_char(self, uuid, data):
        self.events.append(("write", uuid, bytes(data)))

    async def start_notify(self, uuid, handler):
        self.events.append(("subscribe", uuid))
        self.handlers[uuid] = handler

    async def disconnect(self):
        self.events.append(("disconnect",))

    # -- 测试驱动: 注入分片数据 --
    def notify_event(self, chunk):
        self.handlers[EVENT_UUID](bytearray(chunk))

    def notify_audio(self, chunk):
        self.handlers[AUDIO_UUID](bytearray(chunk))


class FailingWriteTransport(FakeTransport):
    """CTRL 写总失败(模拟未连接/写失败): 下行不得 crash, 记日志继续。"""

    async def write_gatt_char(self, uuid, data):
        self.events.append(("write", uuid, bytes(data)))
        raise OSError("模拟 CTRL 写失败")


def transcript_writes(events):
    """提取 CTRL 下行里的 transcript 行(JSON 对象列表)。"""
    return [json.loads(e[2]) for e in events
            if e[0] == "write" and e[1] == CTRL_UUID
            and json.loads(e[2]).get("type") == "transcript"]


class FakeASR:
    """脚本化 ASR: 记录 send_frame/send_end, 按脚本回 (text, is_final)。"""

    def __init__(self, script):
        self.script = list(script)   # [(text, is_final), ...]
        self.sent_frames = []
        self.connected = False
        self.ended = False
        self.closed = False

    async def connect(self):
        self.connected = True

    async def send_frame(self, pcm):
        self.sent_frames.append(bytes(pcm))

    async def send_end(self):
        self.ended = True

    async def results(self):
        for text, is_final in self.script:
            yield text, is_final
            if is_final:
                return
        # 脚本没有最终帧: 挂起(触发超时收尾路径)
        await asyncio.Event().wait()

    async def close(self):
        self.closed = True


class FakeInjector:
    def __init__(self):
        self.calls = []

    def __call__(self, text):
        self.calls.append(text)


def make_fake_asr_factory(script, holder):
    def factory():
        holder["asr"] = FakeASR(script)
        return holder["asr"]
    return factory


# ---- 分片重组(纯函数) ----

def test_reassembly_audio():
    def reassemble(chunks):
        buf, all_frames = bytearray(), []
        for c in chunks:
            buf, frames = reassemble_audio(buf, c)
            all_frames += frames
        return buf, all_frames

    # MTU 247(244+28): 3200 = 13*244 + 28
    chunks = [bytes(244)] * 13 + [bytes(28)]
    buf, frames = reassemble(chunks)
    check("AUDIO MTU247 分片→1 整帧", len(frames), 1)
    check("AUDIO 帧长 3200", len(frames[0]), AUDIO_FRAME_BYTES)
    check("AUDIO 缓冲清空", len(buf), 0)

    # chunk 横跨帧边界: 2000B 段, 6400B → 2 帧
    chunks = [b"\xab" * 2000, b"\xab" * 2000, b"\xab" * 2000, b"\xab" * 400]
    buf, frames = reassemble(chunks)
    check("AUDIO 跨界→2 整帧", len(frames), 2)
    check("AUDIO 帧内容完整", frames[0] == b"\xab" * 3200
          and frames[1] == b"\xab" * 3200, True)
    check("AUDIO 跨界后缓冲清空", len(buf), 0)

    # 尾部不足一帧: 留在缓冲等下一 chunk
    buf, frames = reassemble([b"\xcd" * 3200 + b"\xcd" * 100])
    check("AUDIO 尾部长尾留缓冲", (len(frames), len(buf)), (1, 100))

    # 空 chunk 无副作用
    buf, frames = reassemble([bytearray(b"\x00" * 500), b""])
    check("AUDIO 空 chunk 无副作用", (len(frames), len(buf)), (0, 500))


def test_reassembly_event():
    line = b'{"event":"voice.start","workflow":"build"}'
    nl = line + b"\n"

    # 单行跨 chunk
    buf, lines = reassemble_event(bytearray(), nl[:10])
    check("EVENT 首段无整行", (len(lines), len(buf)), (0, 10))
    buf, lines = reassemble_event(buf, nl[10:])
    check("EVENT 行跨 chunk 重组", (len(lines), len(buf)),
          (1, 0))
    check("EVENT 行内容完整", lines[0], line)

    # 一个 chunk 多行 + 跨界尾部(首行完整 + 第二行前 7 字节)
    two = nl + nl
    buf, lines = reassemble_event(bytearray(), two[:len(nl) + 7])
    check("EVENT chunk 内多行(首行+半行)", (len(lines), len(buf)), (1, 7))
    buf, lines = reassemble_event(buf, two[len(nl) + 7:])
    check("EVENT 跨界第二行", (len(lines), len(buf)), (1, 0))

    # 行内嵌 \n 的 JSON(字符串值含换行)也应按行边界切分
    with_nl = b'{"event":"transcript","text":"a\\nb"}\n'
    buf, lines = reassemble_event(bytearray(), with_nl)
    check("EVENT JSON 内换行不误切", (len(lines), len(buf)), (1, 0))


def test_reassembly_overflow():
    """重组缓冲超限保护: 链路失步时清空缓冲, 不无限增长内存。"""
    # audio 天然有界(整帧提取后余数恒 <3200B); 显式小上限验证保护逻辑
    buf, frames = reassemble_audio(bytearray(), bytes(150), max_buf=100)
    check("AUDIO 超限清空", (len(frames), len(buf)), (0, 0))
    buf, frames = reassemble_audio(bytearray(), bytes(3200), max_buf=100)
    check("AUDIO 失步后重新对齐", (len(frames), len(buf)), (1, 0))

    # event 未成行尾部无界: 默认上限 4KB 触发清空
    buf, lines = reassemble_event(bytearray(), b"x" * (4 * 1024 + 10))
    check("EVENT 默认超限清空", (len(lines), len(buf)), (0, 0))
    buf, lines = reassemble_event(bytearray(), b"x" * 100 + b"\n",
                                  max_buf=100)
    check("EVENT 失步后重组", (len(lines), len(buf)), (1, 0))


# ---- relay 全流程 ----

async def test_voice_flow():
    t = FakeTransport()
    injector = FakeInjector()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory(
                      [("你好", False), ("你好世界", True)], holder),
                  inject_fn=injector, timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    ops = [e[0] for e in t.events]
    check("relay 操作序列", ops,
          ["scan", "connect", "subscribe", "subscribe"])
    check("订阅顺序 EVENT→AUDIO", [e[1] for e in t.events[2:4]],
          [EVENT_UUID, AUDIO_UUID])

    # voice.start + 音频分片(MTU247: 13×244+28) + 第二帧整帧 + voice.end + status
    t.notify_event(b'{"event":"voice.start","workflow":"build"}\n')
    for _ in range(13):
        t.notify_audio(bytes(244))
    t.notify_audio(bytes(28))
    t.notify_audio(bytes(3200))
    # 中间结果: 仅下行设备屏幕预览, 不注入输入框(用户确认: 输入框只落定稿)
    await wait_until(lambda: len(transcript_writes(t.events)) >= 1,
                     what="中间结果下行预览")

    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计")
    await wait_until(lambda: len(injector.calls) >= 1, what="定稿注入")

    asr = holder["asr"]
    check("ASR 收到 2 个整帧", len(asr.sent_frames), 2)
    check("ASR 帧长 3200", all(len(f) == AUDIO_FRAME_BYTES
                               for f in asr.sent_frames), True)
    check("ASR 已结束流", asr.ended, True)
    check("ASR 已关闭", asr.closed, True)
    check("注入只落定稿(无中间叠加)", injector.calls, ["你好世界"])

    tx = transcript_writes(t.events)
    check("下行条数", [(w["text"], w.get("final")) for w in tx],
          [("你好", False), ("你好世界", True)])
    check("partial → final:false", tx[0].get("final"), False)
    check("定稿 → final:true", tx[1].get("final"), True)

    stats = relay.session_stats[0]
    check("统计 rx_frames", stats["rx_frames"], 2)
    check("统计 final_text", stats["final_text"], "你好世界")

    # status 帧(voice.end 后补发)挂到会话对账
    t.notify_event(b'{"event":"status","drop":3}\n')
    await wait_until(lambda: relay.session_stats[0]["device_drop"] == 3,
                     what="status 对账")
    check("设备掉帧对账", relay.session_stats[0]["device_drop"], 3)

    # 断开 → drain 退出 → disconnect
    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")
    check("断开后 disconnect 调用",
          [e[0] for e in t.events].count("disconnect"), 1)


async def test_approval_flow():
    t = FakeTransport()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([("转写文本", True)], holder),
                  inject_fn=FakeInjector(), timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start","workflow":"build"}\n')
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')

    # 会话结束后自动发 approval_request(CTRL)
    await wait_until(lambda: any(e[0] == "write" and e[1] == CTRL_UUID
                                 for e in t.events), what="approval_request 写入")
    writes = [e for e in t.events if e[0] == "write"]
    req = json.loads(writes[-1][2])
    check("approval_request 载荷 type", req.get("type"), "agent.approval_request")
    check("approval_request 载荷 taskId", req.get("taskId"), "task-001")
    check("approval_request 载荷 riskLevel", req.get("riskLevel"), "high")

    # 设备按键 → agent.action 事件
    t.notify_event(b'{"event":"agent.action","taskId":"task-001",'
                   b'"action":"approve"}\n')
    await wait_until(lambda: len(relay.decisions) >= 1, what="agent.action")
    check("决策记录", relay.decisions[0].get("action"), "approve")
    check("决策 taskId", relay.decisions[0].get("taskId"), "task-001")

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_no_approval_and_no_inject():
    t = FakeTransport()
    injector = FakeInjector()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([("最终", True)], holder),
                  inject_fn=injector, timeout=5,
                  do_inject=False, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start","workflow":"build"}\n')
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计")
    await asyncio.sleep(0.05)   # 给审批窗口一点时间(应不发生)
    writes = [json.loads(e[2]) for e in t.events if e[0] == "write"]
    check("no-inject 不调用注入", injector.calls, [])
    check("no-approval 不写审批",
          [w for w in writes if w.get("type") == "agent.approval_request"], [])
    tx = [w for w in writes if w.get("type") == "transcript"]
    check("no-inject 仍下行(显示独立)", [(w["text"], w.get("final")) for w in tx],
          [("最终", True)])
    check("no-inject 仍喂 ASR", len(holder["asr"].sent_frames), 1)

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


def test_split_transcript():
    check("空文本空列表", split_transcript(""), [])
    check("短文本单条", split_transcript("你好"), ["你好"])
    check("ASCII 恰好上限单条",
          split_transcript("x" * TRANSCRIPT_TEXT_MAX),
          ["x" * TRANSCRIPT_TEXT_MAX])
    segs = split_transcript("x" * (TRANSCRIPT_TEXT_MAX + 1))
    check("ASCII 超限两条", [len(s) for s in segs], [TRANSCRIPT_TEXT_MAX, 1])
    check("ASCII 拼接复原", "".join(segs), "x" * (TRANSCRIPT_TEXT_MAX + 1))
    # UTF-8: 中文 3B/字, 不得切断码点; 150B → 42字(126B) + 8字(24B)
    segs = split_transcript("你" * 50)
    check("UTF-8 码点边界切分",
          [len(s.encode("utf-8")) for s in segs], [126, 24])
    check("UTF-8 拼接复原", "".join(segs), "你" * 50)
    # 中英混排: 每段本身都是合法 UTF-8(未被切断)
    mixed = "你a好" * 60   # (3+1+3)B × 60 = 420B
    segs = split_transcript(mixed)
    check("混排不切断码点", all(s.encode("utf-8").decode("utf-8") == s
                               for s in segs), True)
    check("混排拼接复原", "".join(segs), mixed)
    check("混排段长合规", all(len(s.encode("utf-8")) <= TRANSCRIPT_TEXT_MAX
                              for s in segs), True)


async def test_transcript_downlink_long():
    """超长文本 >127B 分多条下行(设备单条上限), 每段定稿标记一致。"""
    t = FakeTransport()
    injector = FakeInjector()
    holder = {}
    long_text = "x" * 200
    cjk_text = "你" * 50
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory(
                      [(long_text, False), (cjk_text, False), ("收尾", True)],
                      holder),
                  inject_fn=injector, timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start","workflow":"build"}\n')
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计")

    tx = transcript_writes(t.events)
    ascii_segs = [w["text"] for w in tx if w["text"].startswith("x")]
    check("长文本分段数", len(ascii_segs), 2)
    check("分段 ≤127B", all(len(s.encode("utf-8")) <= TRANSCRIPT_TEXT_MAX
                            for s in ascii_segs), True)
    check("长文本拼接复原", "".join(ascii_segs), long_text)
    check("非末段定稿标记", tx[0].get("final"), False)

    cjk_segs = [w["text"] for w in tx if w["text"].startswith("你")]
    check("UTF-8 分段数", len(cjk_segs), 2)
    check("UTF-8 段字节长", [len(s.encode("utf-8")) for s in cjk_segs],
          [126, 24])
    check("UTF-8 拼接复原", "".join(cjk_segs), cjk_text)
    check("末段定稿标记", tx[-1].get("final"), True)
    check("定稿段数", len([w for w in tx if w.get("final")]), 1)

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_downlink_write_failure():
    """CTRL 写失败不得 crash: 会话照常收尾、注入照常、统计照常。"""
    t = FailingWriteTransport()
    injector = FakeInjector()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory(
                      [("中间", False), ("最终", True)], holder),
                  inject_fn=injector, timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start","workflow":"build"}\n')
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计")

    tx = transcript_writes(t.events)
    check("写失败仍记录下行意图", [(w["text"], w.get("final")) for w in tx],
          [("中间", False), ("最终", True)])
    check("写失败不阻断注入(定稿一次)", injector.calls, ["最终"])
    check("写失败不阻断统计", relay.session_stats[0]["final_text"], "最终")

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_final_timeout():
    t = FakeTransport()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([("中间", False)], holder),
                  inject_fn=FakeInjector(), timeout=0.2)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start","workflow":"build"}\n')
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1,
                     what="超时收尾", timeout=3)
    check("超时后会话仍统计", len(relay.session_stats), 1)
    check("超时 final_text 为空", relay.session_stats[0]["final_text"], "")
    check("超时 ASR 已关闭", holder["asr"].closed, True)

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_no_device():
    t = FakeTransport(devices=[])
    relay = Relay(t, timeout=5)
    try:
        await relay.run()
        check("扫描不到应抛 RelayError", "no-raise", "raised")
    except RelayError as e:
        check("扫描不到应抛 RelayError", "AI Passport" in str(e), True)
    check("扫描失败不连接",
          [e[0] for e in t.events], ["scan"])


# ---- 主入口 ----

async def main():
    print("== 分片重组 ==")
    test_reassembly_audio()
    test_reassembly_event()
    print("== 转写切分(纯函数) ==")
    test_split_transcript()
    print("== relay 流程 ==")
    await test_voice_flow()
    await test_approval_flow()
    await test_no_approval_and_no_inject()
    await test_transcript_downlink_long()
    await test_downlink_write_failure()
    await test_final_timeout()
    await test_no_device()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    asyncio.run(main())
