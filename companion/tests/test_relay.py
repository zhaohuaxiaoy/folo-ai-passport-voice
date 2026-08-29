#!/usr/bin/env python3
"""relay 单测: 分片重组(纯函数) + FakeTransport 断言 扫描→连接→订阅→帧喂
ASR→注入序列; voice 会话掉帧统计; 审批流(approval_request 写入 + agent.action);
转写下行(final:false 预览 / final:true 定稿 / 超长切分 / 写失败隔离)。

运行: companion/.venv/bin/python companion/tests/test_relay.py  (无需 pytest)
"""
import asyncio
import json
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from relay import (  # noqa: E402
    AUDIO_FRAME_BYTES, AUDIO_Q_MAX, EVENT_Q_MAX,
    CTRL_UUID, EVENT_UUID, AUDIO_UUID,
    TRANSCRIPT_TEXT_MAX, Relay, RelayError,
    reassemble_audio, reassemble_event, split_transcript,
    reassemble_adpcm, split_merged_notify, _focus_delay,
    INJECT_FOCUS_DELAY_DEFAULT, INJECT_FOCUS_DELAY_MAX,
    ADPCM_CHUNK_HDR, ADPCM_CHUNK_LAST,
)
from asr_client import StreamingASR, RESULTS_Q_MAX  # noqa: E402
from adpcm import (  # noqa: E402
    ADPCM_BLOCK_BYTES, ADPCM_BLOCK_SAMPLES, AdpcmState,
    decode_block, encode_block,
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


async def wait_session_done(relay, what="会话收尾完成"):
    """收尾后台化后:统计占位立即出现,done=True 才是收尾完成。"""
    await wait_until(lambda: relay.session_stats and relay.session_stats[-1]["done"],
                     what=what)


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

    async def write_gatt_char(self, uuid, data, response=False):
        # 与 BleakTransport 同契约:下行一律无响应写(免 ATT 确认 RTT)。
        # response 记录进事件元组(第 4 元素),测试断言锁住该契约。
        self.events.append(("write", uuid, bytes(data), response))

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


class FailingSubscribeTransport(FakeTransport):
    """start_notify 抛异常(模拟订阅失败): connect 已成功, 传输层必须清理。"""

    async def start_notify(self, uuid, handler):
        self.events.append(("subscribe", uuid))
        raise OSError("模拟订阅失败")


class FailingWriteTransport(FakeTransport):
    """CTRL 写总失败(模拟未连接/写失败): 下行不得 crash, 记日志继续。"""

    async def write_gatt_char(self, uuid, data, response=False):
        self.events.append(("write", uuid, bytes(data), response))
        raise OSError("模拟 CTRL 写失败")


def agent_status_writes(events):
    """提取 CTRL 下行里的 agent.status 行(JSON 对象列表)。"""
    return [json.loads(e[2]) for e in events
            if e[0] == "write" and e[1] == CTRL_UUID
            and json.loads(e[2]).get("type") == "agent.status"]


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


class FakeKeyAction:
    """记录 DOWN 键动作(enter/clear), 不触碰真实注入。"""

    def __init__(self):
        self.calls = []

    def __call__(self, action):
        self.calls.append(action)


class GatedASR(FakeASR):
    """connect 由门控制(测慢握手:ASR 连接未就绪不阻塞事件消费)。"""

    def __init__(self, script):
        super().__init__(script)
        self.gate = asyncio.Event()

    async def connect(self):
        await self.gate.wait()
        self.connected = True


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

    # 整帧直通(USB 通道): bytes 整帧零复制, frames[0] 即 chunk 本体
    chunk = b"\xee" * AUDIO_FRAME_BYTES
    buf, frames = reassemble([chunk])
    check("AUDIO 整帧直通", (len(frames), len(buf)), (1, 0))
    check("AUDIO 直通零复制(内存标识)", frames[0] is chunk, True)

    # 非 bytes(bytearray)整帧: 不走直通, 走原路径返回切片副本(类型契约保持:
    # 从 bytearray 累积缓冲切片 → bytearray, 与既有实现逐字节一致)
    ba = bytearray(b"\xdd" * AUDIO_FRAME_BYTES)
    buf, frames = reassemble([ba])
    check("AUDIO bytearray 仍出 1 帧", len(frames), 1)
    check("AUDIO bytearray 副本非本体", frames[0] is not ba, True)
    check("AUDIO bytearray 帧类型 bytearray(原路径)", type(frames[0]) is bytearray, True)

    # buf 有残留时整帧 chunk 不得直通(残留必须并入)
    buf, frames = reassemble([b"\xcd" * 100, b"\xee" * AUDIO_FRAME_BYTES])
    check("AUDIO 残留+整帧走重组", (len(frames), len(buf)), (1, 100))


def test_reassembly_event():
    line = b'{"event":"voice.start"}'
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


def _make_samples(seed):
    """语音状 16kHz 样本(与 test_adpcm.py 同式): 生成可解码的真实 block。"""
    out = []
    for i in range(ADPCM_BLOCK_SAMPLES):
        t = (i + seed * 997) / 16000.0
        v = (math.sin(2.0 * math.pi * 220.0 * t)
             + 0.3 * math.sin(2.0 * math.pi * 440.0 * t))
        out.append(int(v * 9000.0))
    return out


def _chunk(seq, idx, last, data):
    """构造带帧头的 notify 载荷(与固件 ble_audio.c 帧头格式一致)。"""
    b1 = idx | (ADPCM_CHUNK_LAST if last else 0)
    return bytes([seq, b1]) + data


def _adpcm_state():
    return {"seq": None, "buf": bytearray(), "last_idx": -1,
            "last_seq": None, "miss": 0, "unit": None}


def _frags(seq, blk, body):
    """按固件规则把一个 804B 块切成带帧头的分片(非末片长度恒为 body+2)。"""
    parts = [blk[i:i + body] for i in range(0, len(blk), body)]
    return [_chunk(seq, i, i == len(parts) - 1, p) for i, p in enumerate(parts)]


def test_reassembly_adpcm_merged():
    """CoreBluetooth 合并 notification 的拆分(C-1):任何合并方式的输出必须与
    逐片投递逐字节相同。合并只是整片拼接,所以判据就是"与不合并等价"。"""
    def feed(chunks, st=None):
        st = st or _adpcm_state()
        frames = []
        for c in chunks:
            st, got = reassemble_adpcm(st, c)
            frames += got
        return st, frames

    body = 251                      # MTU 256: body = 256-3-2, 满片 253B
    blk0 = encode_block(AdpcmState(), _make_samples(0))
    blk1 = encode_block(AdpcmState(), _make_samples(1))
    f0 = _frags(10, blk0, body)     # 4 片: 253,253,253,53
    f1 = _frags(11, blk1, body)
    check("合并测试前提: 4 片且非末片 253B",
          (len(f0), len(f0[0]), len(f0[-1])), (4, 253, 53))
    _, want = feed(f0 + f1)
    check("参考(逐片): 2 帧", len(want), 2)

    # 1) 两个满片合并成一次回调
    _, got = feed([f0[0] + f0[1], f0[2], f0[3]] + f1, _learned(body))
    check("合并两满片 == 逐片", got, want)

    # 2) 满片 + 末片合并(块尾被并进来)
    _, got = feed([f0[0], f0[1], f0[2] + f0[3]] + f1)
    check("合并满片+末片 == 逐片", got, want)

    # 3) 末片 + 下一块满片合并(跨块边界,长度在前的是短片 —— 按 unit 从左
    #    硬拆会错位,必须靠 LAST 位与"块还缺多少"定界)
    _, got = feed(f0[:3] + [f0[3] + f1[0]] + f1[1:])
    check("合并末片+下块满片 == 逐片", got, want)

    # 4) 一次回调裹住整块 + 下块两片
    _, got = feed([b"".join(f0) + f1[0] + f1[1]] + f1[2:], _learned(body))
    check("合并整块+跨块两片 == 逐片", got, want)

    # 5) 三满片合并
    _, got = feed([f0[0] + f0[1] + f0[2], f0[3]] + f1, _learned(body))
    check("合并三满片 == 逐片", got, want)

    # 6) unit 学习:第一个非末片载荷就是合并的(残留边角)—— 该块解错但不
    #    崩、不失步,后续干净满片把 unit 纠正回来,下一块正常
    st, got = feed([f0[0] + f0[1]] + f0[2:] + f1)
    check("首包即合并: unit 被后续满片纠正", st["unit"], 253)
    check("首包即合并: 下一块仍解出正确帧", got[-1:], want[-1:])

    # 7) 丢片 + 合并:账面"还缺多少"算大,只能靠"切口后必须是下一块片头"定界。
    #    有缺口的块按既有语义丢弃(计 miss),关键是下一块必须完好解出 —— 不拆
    #    的话它的片头落在数据中间,整个会话从此错位。
    lossy = [f0[0], f0[1], f0[3] + f1[0]] + f1[1:]
    check("丢片+合并的切口按片头校正",
          [len(x) for x in split_merged_notify(_learned(body), lossy[2])], [53, 253])
    st, got = feed(lossy, _learned(body))
    check("丢片块计 miss 并丢弃", (st["miss"], len(got)), (1, 1))
    check("丢片后下一块仍逐字节正确", got, want[-1:])

    # 8) 纯函数边界:碎片(<2B)原样返回,不足帧头的尾巴单独成片
    st = _learned(body)
    check("碎片原样返回", split_merged_notify(st, b"\x01"), [b"\x01"])
    pieces = split_merged_notify(st, f0[0] + b"\x07")
    check("尾部碎片单独成片", (len(pieces), pieces[-1]), (2, b"\x07"))


def _learned(body):
    """已学到 unit 的重组状态(真实会话第一片非末片即学到)。"""
    st = _adpcm_state()
    st["unit"] = body + ADPCM_CHUNK_HDR
    return st


def test_reassembly_adpcm():
    """BLE ADPCM 块重组(帧头感知): 单片直通 / 多片重组 / 丢片 / 重复片 /
    迟到片 / 新 seq 部分终结补零 / 碎片 / 失步清空。输出帧必须与直接
    decode_block 逐样本一致(重组路径解码契约)。"""
    def feed(chunks):
        st = {"seq": None, "buf": bytearray(), "last_idx": -1,
              "last_seq": None, "miss": 0}
        all_frames = []
        for c in chunks:
            st, frames = reassemble_adpcm(st, c)
            all_frames += frames
        return st, all_frames

    blk0 = encode_block(AdpcmState(), _make_samples(0))
    want0 = decode_block(blk0)   # 直接解码参考(1600 样本)

    # 1) 单片整块(MTU 大): [seq][0|LAST] + 804B → 1 帧 3200B
    st, frames = feed([_chunk(1, 0, True, blk0)])
    check("ADPCM 单片整块→1 帧", len(frames), 1)
    check("ADPCM 输出帧长 3200", len(frames[0]), 3200)
    check("ADPCM 输出与直接解码逐样本一致",
          list(struct.unpack("<1600h", frames[0])), want0)
    check("ADPCM 完块后缓冲清空", (st["buf"], st["seq"]), (bytearray(), None))
    check("ADPCM 完块无 miss", st["miss"], 0)

    # 2) 多片重组(MTU 185: body=180,804 = 4×180 + 84 → 5 片,末片 LAST)
    body = 180
    parts = [blk0[i:i + body] for i in range(0, len(blk0), body)]
    chunks = [_chunk(2, i, i == len(parts) - 1, p)
              for i, p in enumerate(parts)]
    st, frames = feed(chunks)
    check("ADPCM 多片重组→1 帧", len(frames), 1)
    check("ADPCM 多片输出与直接解码一致",
          list(struct.unpack("<1600h", frames[0])), want0)

    # 3) 丢片(片 1 缺失): is_last 终结但 len<804 → miss++ 无输出
    chunks = [_chunk(3, 0, False, parts[0]),
              _chunk(3, 2, False, parts[2]),
              _chunk(3, 4, True, parts[4])]
    st, frames = feed(chunks)
    check("ADPCM 丢片中片→无输出", frames, [])
    check("ADPCM 丢片 miss=1", st["miss"], 1)

    # 4) 重复片去重(idx 重复): 不破坏重组,块正常解出
    chunks = [_chunk(4, 0, False, parts[0]),
              _chunk(4, 0, False, parts[0]),   # 重复
              _chunk(4, 1, False, parts[1]),
              _chunk(4, 2, False, parts[2]),
              _chunk(4, 3, False, parts[3]),
              _chunk(4, 4, True, parts[4])]
    st, frames = feed(chunks)
    check("ADPCM 重复片去重→1 帧", len(frames), 1)
    check("ADPCM 去重后输出一致",
          list(struct.unpack("<1600h", frames[0])), want0)

    # 5) 迟到片: 已终结块的片再来(seq == last_seq)→ 丢弃, 状态不受扰
    chunks = [_chunk(5, 0, True, blk0),           # 块 5 完成
              _chunk(5, 0, True, parts[0]),       # 迟到片
              _chunk(6, 0, False, parts[0]),      # 块 6 开始(跨 seq)
              _chunk(6, 1, False, parts[1]),
              _chunk(6, 2, False, parts[2]),
              _chunk(6, 3, False, parts[3]),
              _chunk(6, 4, True, parts[4])]
    st, frames = feed(chunks)
    check("ADPCM 迟到片丢弃+块6正常", len(frames), 2)
    check("ADPCM 迟到片不增 miss", st["miss"], 0)

    # 6) 新 seq 部分终结: 块 7 缺 is_last 就被块 8 顶替 → 补零解码帧 + miss
    chunks = [_chunk(7, 0, False, parts[0]),
              _chunk(7, 1, False, parts[1]),
              _chunk(8, 0, True, blk0)]           # 新 seq: 块 7 部分终结
    st, frames = feed(chunks)
    check("ADPCM 部分终结→补零帧+块8帧", len(frames), 2)
    check("ADPCM 部分终结 miss=1", st["miss"], 1)
    # 补零帧 = 已收 360B 数据(4B 头 + 356B → 713 样本)解码 + 零 nibble 尾。
    # IMA code=0 解码为 +step/8 逼近、index 每步 -1 → step 衰减到 7 后
    # diffq=0, predictor 冻结在恒值(不是 0)——补零语义是"保节拍稳定输出",
    # 不产生发散/噪声。
    pad0 = list(struct.unpack("<1600h", frames[0]))
    check("ADPCM 补零帧长 3200", len(frames[0]), 3200)
    check("ADPCM 补零帧前缀=已收数据解码", pad0[:713], want0[:713])
    check("ADPCM 补零帧零尾冻结(不发散)",
          len(set(pad0[-100:])) == 1, True)
    check("ADPCM 块8输出一致",
          list(struct.unpack("<1600h", frames[1])), want0)

    # 7) 碎片: 帧头不足 2B / is_last 但 payload < 4B → miss++ 无输出
    st, frames = feed([b"\x00", b"", _chunk(9, 0, True, b"\x01\x02")])
    check("ADPCM 帧头碎片/短块→无输出", frames, [])
    check("ADPCM 碎片 miss=3", st["miss"], 3)

    # 8) 失步: 同 seq 累计 > 804(不可能的正常块)→ miss++ 且全清重对齐
    big = b"\x55" * 201
    chunks = [_chunk(10, i, i == 4, big) for i in range(5)]   # 1005 > 804
    st, frames = feed(chunks)
    check("ADPCM 超长块失步→无输出", frames, [])
    check("ADPCM 失步 miss=1", st["miss"], 1)
    check("ADPCM 失步后状态清空", (st["seq"], st["last_seq"]), (None, None))
    st, frames = feed([_chunk(11, 0, True, blk0)])   # 重新对齐: 正常
    check("ADPCM 失步后新块正常", len(frames), 1)
    check("ADPCM 失步恢复输出一致",
          list(struct.unpack("<1600h", frames[0])), want0)


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
    # 订阅后可能有校时下行(write)在尾部:核心序列只比前 4 项,
    # 校时下行内容由 test_time_sync_downlink 单独断言
    check("relay 操作序列", ops[:4],
          ["scan", "connect", "subscribe", "subscribe"])
    check("订阅顺序 EVENT→AUDIO", [e[1] for e in t.events[2:4]],
          [EVENT_UUID, AUDIO_UUID])

    # voice.start + 音频分片(MTU247: 13×244+28) + 第二帧整帧 + voice.end + status
    t.notify_event(b'{"event":"voice.start"}\n')
    # 屏障:等待 voice.start 处理完(会话已建),否则音频帧可能被 drain 丢弃
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    for _ in range(13):
        t.notify_audio(bytes(244))
    t.notify_audio(bytes(28))
    t.notify_audio(bytes(3200))
    # 中间结果: 仅下行设备屏幕预览, 不注入输入框(用户确认: 输入框只落定稿)
    await wait_until(lambda: len(transcript_writes(t.events)) >= 1,
                     what="中间结果下行预览")

    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计(占位)")
    await wait_until(lambda: len(injector.calls) >= 1, what="定稿注入")
    await wait_session_done(relay)   # 收尾后台化:final_text/rx 需等收尾完成

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


async def test_voice_flow_adpcm():
    """BLE 压缩路径全流程: voice.start 带 audio=ima_adpcm,块带 2B 帧头按
    MTU 185 语义分片(180B body,5 片/块)→ 重组解码 → 3200B PCM 帧上送
    ASR;统计按 100ms 帧口径;注入恰一次。"""
    t = FakeTransport()
    injector = FakeInjector()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory(
                      [("你好世界", True)], holder),
                  inject_fn=injector, timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: t.handlers, what="订阅")

    # 6 块(100ms 节拍),每块 5 片: 4×180B + 84B(末片 LAST)
    blks = [encode_block(AdpcmState(), _make_samples(b)) for b in range(6)]
    t.notify_event(b'{"event":"voice.start","audio":"ima_adpcm"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    body = 180
    for b, blk in enumerate(blks):
        parts = [blk[i:i + body] for i in range(0, len(blk), body)]
        for i, p in enumerate(parts):
            t.notify_audio(_chunk(b, i, i == len(parts) - 1, p))
            # 30 片 > AUDIO_Q_MAX(20): 同步连塞会丢帧——每片让出事件
            # 循环, 给 _drain_audio 消费机会(真实 BLE notify 有间隔)
            await asyncio.sleep(0)
    await wait_until(lambda: len(holder["asr"].sent_frames) >= 6,
                     what="6 帧全部上送")

    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计(占位)")
    await wait_until(lambda: len(injector.calls) >= 1, what="定稿注入")
    await wait_session_done(relay)

    asr = holder["asr"]
    check("ASR 收到 6 个 PCM 帧", len(asr.sent_frames), 6)
    check("帧长全 3200B", all(len(f) == 3200 for f in asr.sent_frames), True)
    for b, (blk, fr) in enumerate(zip(blks, asr.sent_frames)):
        want = decode_block(blk)     # 直接解码参考
        check(f"帧{b}重组输出一致",
              list(struct.unpack("<1600h", fr)), want)
    check("ASR 已结束流", asr.ended, True)
    check("注入只落定稿", injector.calls, ["你好世界"])

    stats = relay.session_stats[0]
    check("统计 rx_frames=6(100ms 口径)", stats["rx_frames"], 6)
    check("统计 final_text", stats["final_text"], "你好世界")

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_adpcm_state_resets_between_sessions():
    """同一连接上连续两次 ima_adpcm 会话:前一会话的残留半块不能污染后一会话。

    seq 是每会话从 0 起的 mod 256 块序号。上一会话末尾若留下未终结的半块
    (末片丢了/设备排空前断流),新会话第 0 块的 seq 会撞上残留的 last_seq,
    被当成"已终结块的迟到片"整块丢弃 —— 表现为新会话开头几个 100ms 凭空
    消失。只按 audio_format 变化复位挡不住这种情况(两次会话格式相同)。
    """
    t = FakeTransport()
    injector = FakeInjector()
    asrs = []

    def factory():
        a = FakeASR([("第二次会话", True)])
        asrs.append(a)
        return a

    relay = Relay(t, asr_factory=factory, inject_fn=injector,
                  timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: t.handlers, what="订阅")

    body = 180
    blks = [encode_block(AdpcmState(), _make_samples(b)) for b in range(3)]

    def send_block(seq, blk, drop_last=False):
        parts = [blk[i:i + body] for i in range(0, len(blk), body)]
        if drop_last:
            parts = parts[:-1]                 # 末片丢失 → 半块留在状态里
        for i, p in enumerate(parts):
            t.notify_audio(_chunk(seq, i, i == len(parts) - 1 and not drop_last, p))

    # 会话一: 第 0 块完整 + 第 1 块缺末片(残留)
    t.notify_event(b'{"event":"voice.start","audio":"ima_adpcm"}\n')
    await wait_until(lambda: relay._session is not None, what="会话一 start")
    send_block(0, blks[0])
    await wait_until(lambda: len(asrs[0].sent_frames) >= 1, what="会话一 1 帧")
    send_block(1, blks[1], drop_last=True)
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_session_done(relay)

    # 会话二: seq 从 0 重新开始 —— 与残留的 seq/last_seq 撞值
    t.notify_event(b'{"event":"voice.start","audio":"ima_adpcm"}\n')
    await wait_until(lambda: len(asrs) >= 2 and relay._session is not None,
                     what="会话二 start")
    send_block(0, blks[0])
    send_block(1, blks[1])
    await wait_until(lambda: len(asrs[1].sent_frames) >= 2, what="会话二 2 帧",
                     timeout=2.0)

    check("会话二收到 2 帧", len(asrs[1].sent_frames), 2)
    for i in (0, 1):
        check(f"会话二帧{i}逐字节正确",
              list(struct.unpack("<1600h", asrs[1].sent_frames[i])),
              decode_block(blks[i]))

    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 2, what="会话二统计")
    await wait_session_done(relay)
    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_unknown_format_fallback():
    """未知音频格式(如 ulaw)按 pcm 兜底: 裸 3200B 块直通 ASR。"""
    t = FakeTransport()
    injector = FakeInjector()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory(
                      [("未知格式兜底", True)], holder),
                  inject_fn=injector, timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: t.handlers, what="订阅")

    t.notify_event(b'{"event":"voice.start","audio":"ulaw"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    frame = b"\x55" * AUDIO_FRAME_BYTES
    t.notify_audio(frame)

    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计(占位)")
    await wait_until(lambda: len(injector.calls) >= 1, what="定稿注入")
    await wait_session_done(relay)

    asr = holder["asr"]
    check("未知格式按 pcm 直通", asr.sent_frames, [bytes(frame)])

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


# ---- 校时下行(连接建立后立即;每小时周期由 TIME_SYNC_INTERVAL_S 控制)----

def _time_set_writes(events):
    return [json.loads(e[2]) for e in events
            if e[0] == "write" and e[1] == CTRL_UUID
            and json.loads(e[2]).get("type") == "time.set"]


async def test_time_sync_downlink_ble():
    """BLE: 连接后立即下行 time.set(CTRL), epoch 为 UTC 整数秒;
    断开后 run 收束, 校时任务一并取消(无泄漏)。"""
    t = FakeTransport()
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: _time_set_writes(t.events), what="首次校时下行")
    w = _time_set_writes(t.events)[0]
    check("校时下行 type", w["type"], "time.set")
    check("校时 epoch 是 int", isinstance(w["epoch"], int), True)
    check("校时 epoch 近当前 UTC",
          abs(w["epoch"] - int(time.time())) < 5, True)

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")
    check("校时任务已收束", relay._time_task is None, True)


async def test_time_sync_downlink_usb():
    """USB: 校时走 SYS 命令面 `time set <epoch>`(console 命令表),
    且不产生 CTRL 协议行。"""
    t = FakeTransport()
    syscmds = []

    async def send_syscmd(line):
        syscmds.append(line)

    t.send_syscmd = send_syscmd   # 伪装 SerialTransport → USB 通道分支
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5, do_approval=False,
                  stdin_input=lambda prompt="": iter(()).__next__())  # stdin 立即 EOF
    task = asyncio.create_task(relay.run(device_addr="FAKE:USB"))
    await wait_until(lambda: len(syscmds) >= 1, what="USB 首次校时")
    line = syscmds[0]
    check("USB 校时命令前缀", line.startswith("time set "), True)
    epoch = int(line.split()[-1])
    check("USB 校时 epoch 近当前 UTC", abs(epoch - int(time.time())) < 5, True)
    check("USB 不产生 CTRL 下行", _time_set_writes(t.events), [])

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")
    check("校时任务已收束", relay._time_task is None, True)


# ---- DOWN 键动作上行(enter/clear → 注入)----

async def test_key_action_downlink():
    """key.action 上行 → 注入后端按动作调用; 未知动作忽略不注入。"""
    t = FakeTransport()
    keyer = FakeKeyAction()
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), key_action_fn=keyer,
                  timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"key.action","action":"enter"}\n')
    await wait_until(lambda: keyer.calls == ["enter"], what="回车注入")
    t.notify_event(b'{"event":"key.action","action":"clear"}\n')
    await wait_until(lambda: keyer.calls == ["enter", "clear"], what="清空注入")
    t.notify_event(b'{"event":"key.action","action":"bogus"}\n')
    await asyncio.sleep(0.05)   # 未知动作路径: 无注入发生
    check("未知动作不注入", keyer.calls, ["enter", "clear"])

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_on_phase_callback():
    """on_phase 回调: 连接序列 scanning→connecting→connected, 断连触发
    disconnected; 不传 on_phase 时无行为变化(既有测试已隐式覆盖)。"""
    t = FakeTransport()
    phases = []
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5,
                  on_phase=phases.append)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")
    check("on_phase 连接序列", phases, ["scanning", "connecting", "connected"])

    t.disconnect_cb()
    await wait_until(task.done, what="断连退出")
    check("on_phase 断连触发", "disconnected" in phases, True)



async def test_on_candidate_callback():
    """on_candidate 回调: 挂接后收全 partial+final, 零 transcript 下行
    (设备预览由悬浮窗取代), 注入仍只落定稿一次。"""
    t = FakeTransport()
    injector = FakeInjector()
    holder = {}
    cands = []
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory(
                      [("中间", False), ("你好", False), ("你好世界", True)],
                      holder),
                  inject_fn=injector, timeout=5,
                  on_candidate=lambda text, is_final:
                      cands.append((text, is_final)))
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_audio(bytes(3200))
    await wait_until(lambda: len(cands) >= 1, what="首个候选到达")
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(injector.calls) >= 1, what="定稿注入")
    await wait_session_done(relay)

    check("回调收全 partial+final", cands,
          [("中间", False), ("你好", False), ("你好世界", True)])
    check("挂接后零设备预览下行", transcript_writes(t.events), [])
    check("注入仍只落定稿一次", injector.calls, ["你好世界"])
    check("统计 final_text", relay.session_stats[0]["final_text"], "你好世界")

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_demo_approval_send_error_swallowed():
    """_demo_approval 发送失败(RelayError 等): 不外冒, 槽位清理
    (create_task 无人 await, 冒出会变 "Task exception was never retrieved")。"""
    t = FakeTransport()
    relay = Relay(t, asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5)

    async def boom(_):
        raise RelayError("CTRL 载荷超限")

    relay._send_ctrl = boom
    await relay._demo_approval()
    check("发送异常不外冒", relay._approval_waiter is None, True)
    check("approval 任务槽清理", relay._approval_task is None, True)


def test_paste_mac_dry_run_modes():
    """Mac paste_text dry_run: unicode/clipboard 路径打印与 mode 校验。"""
    import io
    from contextlib import redirect_stdout
    from inject import paste_text

    buf = io.StringIO()
    with redirect_stdout(buf):
        paste_text("你好", dry_run=True, mode="unicode")
    out = buf.getvalue()
    check("unicode dry_run 打印", "CGEvent" in out and "pbcopy" not in out, True)

    buf = io.StringIO()
    with redirect_stdout(buf):
        paste_text("你好", dry_run=True, mode="clipboard")
    out = buf.getvalue()
    check("clipboard dry_run 打印", "pbcopy" in out and "Cmd+V" in out, True)

    try:
        paste_text("hi", mode="bogus")
        check("未知 mode 拒绝", False, True)
    except Exception as e:
        check("未知 mode 拒绝", "auto|unicode|clipboard" in str(e), True)


def test_default_inject_fn_mode():
    """config inject_mode 透传: 非法值拒绝, auto 为缺省。"""
    from relay import _default_inject_fn
    try:
        _default_inject_fn({"inject_mode": "bogus"})
        check("inject_mode 非法拒绝", False, True)
    except RelayError as e:
        check("inject_mode 非法拒绝", "inject_mode" in str(e), True)
    fn = _default_inject_fn({})
    check("inject_mode 缺省 auto 可构造", callable(fn), True)


def test_key_action_mac_dry_run():
    """Mac key_action dry_run: enter/clear 的 osascript 命令序列。"""
    import io
    from contextlib import redirect_stdout
    from inject import key_action

    buf = io.StringIO()
    with redirect_stdout(buf):
        key_action("enter", dry_run=True)
    out = buf.getvalue()
    check("Mac enter 打印", "keystroke return" in out, True)

    buf = io.StringIO()
    with redirect_stdout(buf):
        key_action("clear", dry_run=True)
    out = buf.getvalue()
    # clear 按前台 app 分分支(终端 → Ctrl+E+Ctrl+U; 非终端 → Cmd+A+Delete),
    # dry-run 只打印当前分支 —— 环境相关, 两分支任一即通过
    terminal = 'keystroke "u" using control down' in out
    select_all = 'keystroke "a" using command down' in out
    check("Mac clear 打印(终端分支或全选)", terminal or select_all, True)
    check("Mac clear 打印删除", "key code 51" in out or terminal, True)

    try:
        key_action("bogus")
        check("未知动作拒绝", False, True)
    except Exception as e:
        check("未知动作拒绝", "enter|clear" in str(e), True)


async def test_approval_flow():
    t = FakeTransport()
    holder = {}
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([("转写文本", True)], holder),
                  inject_fn=FakeInjector(), timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')

    # 会话结束后自动发 approval_request(CTRL);转写下行也是 CTRL 写,按
    # type 精确等待,不依赖写序(转写/审批两任务并发,谁先落盘不确定)
    await wait_until(lambda: any(
        e[0] == "write" and json.loads(e[2]).get("type") == "agent.approval_request"
        for e in t.events), what="approval_request 写入")
    writes = [e for e in t.events if e[0] == "write"]
    req = next(json.loads(w[2]) for w in writes
               if json.loads(w[2]).get("type") == "agent.approval_request")
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


async def test_ctrl_write_no_response():
    """S2: 全部下行 CTRL 写必须是无响应写(response=False),免等 ATT 确认 RTT。"""
    t = FakeTransport()
    relay = Relay(t, asr_factory=make_fake_asr_factory([("hi", True)], {}),
                  inject_fn=FakeInjector(), timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_session_done(relay)

    ctrl_writes = [e for e in t.events if e[0] == "write" and e[1] == CTRL_UUID]
    check("有 CTRL 写", len(ctrl_writes) > 0, True)
    bad = [e for e in ctrl_writes if e[3] is not False]
    check("CTRL 写全部 response=False", bad, [])

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


class FailingConnectASR(FakeASR):
    """connect() 直接抛错:模拟 ASR 鉴权/网络失败(会话拿不到任何结果)。"""

    async def connect(self):
        raise OSError("模拟 ASR 连接失败")


class HangingConnectASR(FakeASR):
    """connect() 永久悬挂:模拟网络黑洞(审查 P1-4: 无超时曾卡死音频排空)。"""

    async def connect(self):
        await asyncio.Event().wait()


async def test_disconnect_cleanup():
    """M1: 会话进行中断连(BLE 断开)→ ASR 会话与任务全部收束,不泄漏。"""
    t = FakeTransport()
    holder = {}
    relay = Relay(t, asr_factory=make_fake_asr_factory([("部分转写", False)], holder),
                  inject_fn=FakeInjector(), timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    await wait_until(lambda: holder["asr"].connected, what="ASR 已连接")
    session = relay._session

    t.disconnect_cb()                  # BLE 断开
    await wait_until(task.done, what="断开退出")
    check("asr 已关闭", holder["asr"].closed, True)
    check("run_task 已收束", session._run_task is None or session._run_task.done(), True)
    check("results_task 已收束", session._results_task is None
          or session._results_task.done(), True)
    check("会话已清空", relay._session, None)


async def test_disconnect_during_final():
    """M1: end 收尾(等最终结果)中断连 → abort 放行,统计占位补全。"""
    t = FakeTransport()
    holder = {}
    relay = Relay(t, asr_factory=make_fake_asr_factory([("hi", False)], holder),
                  inject_fn=FakeInjector(), timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    # end 后台收尾:脚本无 final → 挂起等最终结果;统计占位已入列(done=False)
    await wait_until(lambda: relay.session_stats
                     and not relay.session_stats[-1]["done"], what="end 收尾挂起")

    t.disconnect_cb()                  # 断开 → abort 放行并发 end()
    await wait_until(task.done, what="断开退出")
    check("end 收束后占位补全", relay.session_stats[-1]["done"], True)
    check("asr 已关闭", holder["asr"].closed, True)


async def test_disconnect_overlapping_closing():
    """P1: 重叠收尾(重复 voice.start + voice.end)断连 → 收尾槽内全部会话
    abort,无 ASR 连接泄漏(单槽 _closing 只覆盖最新会话,旧会话泄漏)。
    探针已复现修复前 asr.closed=False。"""
    t = FakeTransport()
    holder = {}
    asrs = []
    def factory():
        a = FakeASR([])
        asrs.append(a)
        return a
    relay = Relay(t, asr_factory=factory, inject_fn=FakeInjector(), timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    # s1 进行中; 重复 voice.start → s1 进收尾槽后台收尾, s2 成为当前会话
    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: len(relay._closing) == 1, what="s1 进收尾槽")

    # s2 voice.end → s2 也进收尾槽(槽内: s1 + s2 两个后台收尾会话)
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay._closing) == 2, what="s1+s2 均在收尾槽")

    t.disconnect_cb()                  # 断开 → abort 槽内全部
    await wait_until(task.done, what="断开退出")
    check("槽内两会话 ASR 全部关闭(无泄漏)",
          len(asrs) == 2 and all(a.closed for a in asrs), True)
    check("收尾槽已清空", relay._closing, [])


async def test_subscribe_failure_cleanup():
    """审查 P1: connect 成功后订阅失败 → 传输层显式断开(不泄漏)。

    统一清理 finally 不覆盖连接/订阅阶段;修复前 BLE client/串口读线程/
    WS server 保持打开(端口占用、线程存活)。"""
    t = FailingSubscribeTransport()
    relay = Relay(t, asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5)
    try:
        await relay.run()
        check("订阅失败应抛 RelayError", False, True)
    except RelayError as e:
        check("订阅失败转 RelayError", "订阅失败" in str(e), True)
    check("传输层已断开", ("disconnect",) in t.events, True)


async def test_connect_timeout():
    """M1: ASR 连接悬挂 → 5s(测试注入 0.2s)超时走错误路径,不卡死排空。"""
    t = FakeTransport()
    holder = {}

    def factory():
        holder["asr"] = HangingConnectASR([])
        return holder["asr"]

    relay = Relay(t, asr_factory=factory, inject_fn=FakeInjector(),
                  timeout=5, connect_timeout_s=0.2)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))        # feed 挂起等 _connected
    await asyncio.sleep(0.6)           # 0.2s 超时生效
    check("连接超时错误已记录", relay._session._conn_error is not None, True)

    t.disconnect_cb()
    await wait_until(task.done, what="超时后断开仍干净退出")
    check("超时后 asr 已关闭", holder["asr"].closed, True)


def test_focus_delay_cfg():
    """inject_focus_delay(Windows 注入前等待秒数)读配置要能兜住手工编辑的
    脏值 —— 否则异常在"用户说完话要注入"的那一刻才炸,且看不出与配置有关。"""
    d = INJECT_FOCUS_DELAY_DEFAULT
    check("缺省(键不存在)", _focus_delay({}), d)
    check("正常值原样", _focus_delay({"inject_focus_delay": 0.5}), 0.5)
    check("字符串数字可用", _focus_delay({"inject_focus_delay": "1.5"}), 1.5)
    check("0 = 不等(单测/无人值守)", _focus_delay({"inject_focus_delay": 0}), 0.0)
    check("负数夹到 0", _focus_delay({"inject_focus_delay": -3}), 0.0)
    check("手滑多打一位夹到上限",
          _focus_delay({"inject_focus_delay": 200}), INJECT_FOCUS_DELAY_MAX)
    check("非数字退缺省", _focus_delay({"inject_focus_delay": "soon"}), d)
    check("null 退缺省", _focus_delay({"inject_focus_delay": None}), d)
    check("NaN 退缺省", _focus_delay({"inject_focus_delay": float("nan")}), d)


async def test_agent_done_always_sent_once():
    """agent.status done 是设备退出 TRANSCRIBING 的唯一信号:成功、ASR 连接
    失败、无最终结果超时三条路径都必须下行,且每会话恰一次(不重复)。

    修复前只有定稿路径发 done —— 失败/超时会话让设备停在 TRANSCRIBING 直到
    它自己的 STT 超时,期间还接不了新会话。
    """
    async def run_session(asr, timeout=5, feed_frame=True):
        t = FakeTransport()
        relay = Relay(t, asr_factory=lambda: asr, inject_fn=FakeInjector(),
                      timeout=timeout, do_approval=False, connect_timeout_s=0.2)
        task = asyncio.create_task(relay.run())
        await wait_until(lambda: t.handlers, what="订阅")
        t.notify_event(b'{"event":"voice.start"}\n')
        await wait_until(lambda: relay._session is not None, what="voice.start")
        if feed_frame:
            t.notify_audio(bytes(AUDIO_FRAME_BYTES))
        t.notify_event(b'{"event":"voice.end"}\n')
        await wait_session_done(relay)
        t.disconnect_cb()
        await wait_until(task.done, what="断开退出")
        return agent_status_writes(t.events)

    # 成功路径: 定稿发一次, _close_up 不重复发
    dones = await run_session(FakeASR([("你好", True)]))
    check("成功会话 done 恰一次",
          [d["state"] for d in dones], ["done"])

    # ASR 连接失败: 一个结果都没有, done 仍要发
    dones = await run_session(FailingConnectASR([]), feed_frame=False)
    check("连接失败也发 done", [d["state"] for d in dones], ["done"])

    # 无最终结果超时(results 只给中间结果后挂起)
    dones = await run_session(FakeASR([("中间结果", False)]), timeout=0.2)
    check("超时收尾也发 done", [d["state"] for d in dones], ["done"])


async def test_results_queue_bounded():
    """M2: ASR 结果队列有界——满时丢中间结果(计数),定稿保底不丢。"""
    s = StreamingASR.__new__(StreamingASR)   # 绕过 __init__(不读真实配置)
    s._results_q = asyncio.Queue(maxsize=4)
    s._dropped_results = 0
    for i in range(10):                      # 灌 10 条中间结果(容量 4)
        await s._put_result(("result", f"partial{i}", False))
    check("队列有界(≤4)", s._results_q.qsize(), 4)
    check("丢弃计数(6)", s._dropped_results, 6)
    # 定稿:await 入队保底(消费者放行),不丢
    async def drain_one():
        await s._results_q.get()
    t = asyncio.create_task(drain_one())
    await s._put_result(("result", "final", True))
    await t
    check("定稿不丢", s._results_q.qsize(), 4)
    check("定稿未计入丢弃", s._dropped_results, 6)


async def test_disconnect_queue_full():
    """M2: 队列满时断连写 sentinel 不抛 QueueFull,退出流程可靠。"""
    t = FakeTransport()
    relay = Relay(t, asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5)
    # 音频/事件队列都已满(模拟下行最慢的积压态)
    for _ in range(AUDIO_Q_MAX):
        relay._audio_q.put_nowait(("audio", b"x" * 3200))
    for _ in range(EVENT_Q_MAX):
        relay._event_q.put_nowait(("event", b'{"event":"x"}\n'))
    relay._stop.set()
    # P2-1: 断连收束经 call_soon_threadsafe 回事件循环,测试须注入 loop
    relay._loop = asyncio.get_running_loop()
    relay._on_disconnected()                 # 满队列写 stop sentinel:不得抛
    await asyncio.sleep(0)                   # 让收束协程(调度回调)执行完毕
    check("音频队列未越界", relay._audio_q.qsize(), AUDIO_Q_MAX)
    check("事件队列未越界", relay._event_q.qsize(), EVENT_Q_MAX)


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

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计(占位)")
    await wait_session_done(relay)
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

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计(占位)")
    await wait_session_done(relay)

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

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="会话统计(占位)")
    await wait_session_done(relay)

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
    cands = []
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([("中间", False)], holder),
                  inject_fn=FakeInjector(), timeout=0.2,
                  on_candidate=lambda text, is_final:
                      cands.append((text, is_final)))
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1,
                     what="超时收尾占位", timeout=3)
    check("超时后会话仍统计", len(relay.session_stats), 1)
    await wait_session_done(relay)   # 0.2s 超时后收尾完成
    check("超时 final_text 为空", relay.session_stats[0]["final_text"], "")
    check("超时 ASR 已关闭", holder["asr"].closed, True)
    check("超时收口悬浮窗(空文本 final)", cands[-1], ("", True))

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_slow_connect_does_not_block_events():
    """ASR 握手挂起(数百 ms)时 voice.end/status 照常处理,不压事件队列。

    回归第 5 轮卡顿修复:connect 曾同步 await 在事件消费路径上,握手期间
    事件队列冻结。现在连接后台化,feed 挂起等就绪(帧不丢只延迟)。
    """
    t = FakeTransport()
    asr = GatedASR([("结果", True)])
    relay = Relay(t, asr_factory=lambda: asr, inject_fn=FakeInjector(),
                  timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="收尾占位")
    check("收尾在等连接(done=False)", relay.session_stats[-1]["done"], False)

    # connect 未放行:status 帧照常即时挂载(不被握手阻塞)
    t.notify_event(b'{"event":"status","drop":5}\n')
    await wait_until(lambda: relay.session_stats[-1]["device_drop"] == 5,
                     what="握手未完成时 status 仍即时挂载")
    check("慢连接不阻塞 status 挂载", relay.session_stats[-1]["device_drop"], 5)

    asr.gate.set()   # 放行连接 → 会话继续,收尾完成
    await wait_session_done(relay)
    check("连接放行后会话有结果", relay.session_stats[-1]["final_text"], "结果")
    check("帧未丢(挂起等待后仍喂入)", len(asr.sent_frames), 1)

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_slow_final_does_not_block_status():
    """voice.end 后等最终结果(最长 timeout)期间,status 对账帧照常即时处理。

    回归第 5 轮卡顿修复:收尾曾同步 await 在事件消费路径上,60s 等最终
    结果会把 status 帧压住(与 _demo_approval 后台化是同一问题的残留)。
    """
    t = FakeTransport()
    holder = {}
    relay = Relay(t, asr_factory=make_fake_asr_factory([("中间", False)], holder),
                  inject_fn=FakeInjector(), timeout=30, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_until(lambda: len(relay.session_stats) >= 1, what="收尾占位")
    check("收尾进行中(done=False)", relay.session_stats[-1]["done"], False)

    t.notify_event(b'{"event":"status","drop":7}\n')
    await wait_until(lambda: relay.session_stats[-1]["device_drop"] == 7,
                     what="等最终结果期间 status 即时处理")
    check("慢 final 不阻塞 status", relay.session_stats[-1]["device_drop"], 7)
    check("收尾仍未完成(done=False)", relay.session_stats[-1]["done"], False)

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_duplicate_voice_start_no_cross_talk():
    """重复 voice.start:旧会话后台收尾,新会话独立,统计/注入不串台。"""
    t = FakeTransport()
    injector = FakeInjector()
    asrs = [FakeASR([("旧段", True)]), FakeASR([("第二段", True)])]

    def factory():
        return asrs.pop(0)

    relay = Relay(t, asr_factory=factory, inject_fn=injector,
                  timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"voice.start"}\n')
    await wait_until(lambda: relay._session is not None, what="voice.start 已处理")
    t.notify_audio(bytes(3200))
    # 重复 start(异常/快速连续会话):旧会话收尾不阻塞事件消费
    t.notify_event(b'{"event":"voice.start"}\n')
    # 屏障:等旧会话收尾占位入列(仍存在旧 _session,不能用 _session is None)
    await wait_until(lambda: len(relay.session_stats) >= 1, what="旧会话已收尾占位")
    t.notify_audio(bytes(3200))
    t.notify_event(b'{"event":"voice.end"}\n')
    await wait_session_done(relay)
    await wait_until(lambda: len(relay.session_stats) >= 2 and relay.session_stats[-1]["done"],
                     what="两会话收尾完成")
    check("两会话独立统计", [s["done"] for s in relay.session_stats],
          [True, True])
    check("两会话各自注入", sorted(injector.calls), ["旧段", "第二段"])
    check("两会话各有最终文本", [s["final_text"] for s in relay.session_stats],
          ["旧段", "第二段"])

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


# ---- GUI 命令桥 run_syscmd(诊断页用)----

async def test_run_syscmd():
    """USB 通道(transport 带 send_syscmd): run_syscmd 往返设备响应。

    在 relay.run() 运行期间并发调用(模拟 GUI 用 run_coroutine_threadsafe
    从 tk 线程发起), 验证与主循环共存安全。
    """
    t = FakeTransport()
    sent = []

    async def send_syscmd(line):
        sent.append(line)
        return f"resp:{line}"

    t.send_syscmd = send_syscmd   # 伪装 SerialTransport
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5, do_approval=False,
                  stdin_input=lambda prompt="": iter(()).__next__())  # stdin 立即 EOF
    task = asyncio.create_task(relay.run(device_addr="FAKE:USB"))
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    # relay 运行中并发调用(诊断页路径: 不做 run_coroutine_threadsafe,
    # 直接并发 await 等价)
    check("run_syscmd 往返", await relay.run_syscmd("st"), "resp:st")
    # USB 通道首次校时也会经 send_syscmd(time set), 故只断言 st 在列
    check("run_syscmd 命令透传", "st" in sent, True)
    check("run_syscmd 不产生 CTRL 下行",
          [e for e in t.events if e[0] == "write_gatt_char"], [])

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


async def test_run_syscmd_unsupported():
    """BLE 通道(无 send_syscmd): run_syscmd 明确报错。"""
    t = FakeTransport()           # 无 send_syscmd → _syscmd = None
    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), timeout=5)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    try:
        await relay.run_syscmd("st")
        check("无命令面通道明确报错", False, True)
    except RelayError as e:
        check("无命令面通道明确报错", "仅 USB 通道" in str(e), True)

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


# ---- 主入口 ----

async def main():
    print("== 分片重组 ==")
    test_reassembly_audio()
    test_reassembly_event()
    test_reassembly_adpcm()
    test_reassembly_adpcm_merged()
    test_focus_delay_cfg()
    print("== 转写切分(纯函数) ==")
    test_split_transcript()
    print("== relay 流程 ==")
    await test_voice_flow()
    await test_voice_flow_adpcm()
    await test_adpcm_state_resets_between_sessions()
    await test_unknown_format_fallback()
    await test_time_sync_downlink_ble()
    await test_time_sync_downlink_usb()
    await test_key_action_downlink()
    await test_on_phase_callback()
    await test_on_candidate_callback()
    await test_demo_approval_send_error_swallowed()
    test_paste_mac_dry_run_modes()
    test_default_inject_fn_mode()
    test_key_action_mac_dry_run()
    await test_run_syscmd()
    await test_run_syscmd_unsupported()
    await test_approval_flow()
    await test_ctrl_write_no_response()
    await test_disconnect_cleanup()
    await test_disconnect_during_final()
    await test_disconnect_overlapping_closing()
    await test_subscribe_failure_cleanup()
    await test_connect_timeout()
    await test_agent_done_always_sent_once()
    await test_results_queue_bounded()
    await test_disconnect_queue_full()
    await test_no_approval_and_no_inject()
    await test_transcript_downlink_long()
    await test_downlink_write_failure()
    await test_final_timeout()
    await test_slow_connect_does_not_block_events()
    await test_slow_final_does_not_block_status()
    await test_duplicate_voice_start_no_cross_talk()
    await test_no_device()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    asyncio.run(main())


async def test_key_action_slow_inject_does_not_freeze_drain():
    """审查 P1-3: 慢 key 注入(阻塞 ~1.5s)不得冻结事件循环 —— 期间音频帧
    照常处理(_on_audio_frame 计数), 注入完成后 drain 继续。"""
    t = FakeTransport()
    injected = []
    fired = {"n": 0}

    def slow_key_action(action):
        time.sleep(1.5)          # 模拟 Windows SendInput 逐键序列化(~2s/键)
        injected.append(action)

    relay = Relay(t,
                  asr_factory=make_fake_asr_factory([], {}),
                  inject_fn=FakeInjector(), key_action_fn=slow_key_action,
                  timeout=5, do_approval=False)
    task = asyncio.create_task(relay.run())
    await wait_until(lambda: len(t.events) >= 4, what="订阅完成")

    t.notify_event(b'{"event":"key.action","action":"enter"}\n')
    await asyncio.sleep(0.3)     # 注入进行中(线程池里睡 1.5s)

    async def on_audio(frame):
        fired["n"] += 1
    relay._on_audio_frame = on_audio
    t.notify_event(b'\x00' * 3200)   # 音频帧: 若事件循环被注入冻结则收不到
    await asyncio.sleep(0.3)
    check("注入期间音频帧未被冻结", fired["n"], 1)

    await wait_until(lambda: injected == ["enter"], what="注入完成")
    check("慢注入最终完成", injected, ["enter"])

    t.disconnect_cb()
    await wait_until(task.done, what="断开退出")


def test_type_unicode_auth_gate():
    """审查 P1-5: 辅助功能未授权时 CGEvent 路径抛 InjectError(不再静默
    假成功); 授权后正常执行。"""
    import sys as _sys
    import types as _types
    import inject

    fake_quartz = _types.ModuleType("Quartz")
    fake_quartz.CGEventCreateKeyboardEvent = lambda *a: None
    fake_quartz.CGEventKeyboardSetUnicodeString = lambda *a: None
    fake_quartz.CGEventPost = lambda *a: None
    fake_quartz.kCGHIDEventTap = 0
    fake_as = _types.ModuleType("ApplicationServices")
    fake_as.AXIsProcessTrusted = lambda: False

    old_quartz = _sys.modules.get("Quartz")
    old_as = _sys.modules.get("ApplicationServices")
    _sys.modules["Quartz"] = fake_quartz
    _sys.modules["ApplicationServices"] = fake_as
    try:
        try:
            inject._type_unicode("hi")
            check("未授权应抛 InjectError", False, True)
        except inject.InjectError as e:
            check("未授权抛 InjectError", "辅助功能未授权" in str(e), True)
        fake_as.AXIsProcessTrusted = lambda: True
        inject._type_unicode("hi")       # 已授权: 不抛
        check("已授权正常注入", True, True)
    finally:
        if old_quartz is None:
            _sys.modules.pop("Quartz", None)
        else:
            _sys.modules["Quartz"] = old_quartz
        if old_as is None:
            _sys.modules.pop("ApplicationServices", None)
        else:
            _sys.modules["ApplicationServices"] = old_as


def test_paste_auto_fallback_on_unauthorized():
    """审查 P1-5: auto 模式未授权 → unicode 抛 InjectError → 回退剪贴板
    (pbcopy 被调), 不再静默假成功。"""
    import sys as _sys
    import types as _types
    import inject

    fake_quartz = _types.ModuleType("Quartz")
    fake_quartz.CGEventCreateKeyboardEvent = lambda *a: None
    fake_quartz.CGEventKeyboardSetUnicodeString = lambda *a: None
    fake_quartz.CGEventPost = lambda *a: None
    fake_quartz.kCGHIDEventTap = 0
    fake_as = _types.ModuleType("ApplicationServices")
    fake_as.AXIsProcessTrusted = lambda: False
    _sys.modules["Quartz"] = fake_quartz
    _sys.modules["ApplicationServices"] = fake_as

    calls = []

    class R:
        returncode = 0
        stdout = b""
        stderr = b""

    def fake_run(cmds, **kw):
        calls.append(cmds[0])
        return R()

    orig_run = inject.subprocess.run
    inject.subprocess.run = fake_run
    try:
        inject.paste_text("你好")        # auto: unicode 抛 → 剪贴板回退
        check("auto 回退剪贴板(pbcopy 被调)", calls and calls[0] == "pbcopy",
              True)
    finally:
        inject.subprocess.run = orig_run
        _sys.modules.pop("Quartz", None)
        _sys.modules.pop("ApplicationServices", None)
