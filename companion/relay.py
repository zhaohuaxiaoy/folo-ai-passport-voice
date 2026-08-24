#!/usr/bin/env python3
"""BLE 直连中转程序: 收设备音频 → 火山 ASR 流式转写 → 注入当前输入框。

设备(ESP32-C3, 纯 BLE 外设, 广播名 "AI Passport"):
  Service 0xA2B0 (0000A2B0-0000-1000-8000-00805F9B34FB)
    0xA2B1 CTRL   WRITE|WRITE_ENC   Mac→设备: JSON 行 ≤2048B
    0xA2B2 EVENT  NOTIFY            设备→Mac: JSON 行(device.hello /
                                    voice.start / voice.end / workflow.switch /
                                    agent.action / status)
    0xA2B3 AUDIO  NOTIFY            设备→Mac: 3200B 裸 PCM 帧(100ms @16k/16bit)
ATT 分片: 任何超过 MTU-3 的载荷按 MTU-3 chunk 逐片 notify(ATT 载荷上限 =
MTU-3), EVENT 行以 '\\n' 结尾, AUDIO 帧正好 3200B。本程序负责重组
(reassemble_audio / reassemble_event 纯函数, 独立可测)。

流程: 扫描 "AI Passport" → 连接 → 订阅 EVENT+AUDIO → voice.start 开 ASR 流
→ 帧喂火山(中间结果实时下行设备屏幕预览 + 注入) → voice.end 收最终结果
(下行定稿帧 + 注入) → 审批演示(发 approval_request → 设备按键 → 收
agent.action)。voice 窗口掉帧统计(理论帧数 vs 实收帧数, 与设备 status 帧
drop 双端对账)。转写下行走 CTRL 特征, 与注入解耦(--no-inject 仍下行)。

传输层可注入(FakeTransport)以便无硬件单测。

用法:
  companion/.venv/bin/python companion/relay.py                       # 扫描+全流程
  companion/.venv/bin/python companion/relay.py --device AA:BB:CC:DD:EE:FF
  companion/.venv/bin/python companion/relay.py --no-inject           # 只转写
  companion/.venv/bin/python companion/relay.py --no-approval         # 关审批演示
  companion/.venv/bin/python companion/relay.py --dry-run             # 只转写+打印注入
"""
import argparse
import asyncio
import json
import sys
import time

SERVICE_UUID = "0000A2B0-0000-1000-8000-00805F9B34FB"
CTRL_UUID = "0000A2B1-0000-1000-8000-00805F9B34FB"
EVENT_UUID = "0000A2B2-0000-1000-8000-00805F9B34FB"
AUDIO_UUID = "0000A2B3-0000-1000-8000-00805F9B34FB"
DEVICE_NAME = "AI Passport"
AUDIO_FRAME_BYTES = 3200    # 100ms @16kHz/16bit/mono
AUDIO_FRAME_SEC = 0.1
CTRL_LINE_MAX = 2048        # 对齐固件 APP_PROTO_RX_CAP
SCAN_TIMEOUT = 15
# 下行 transcript 单条文本上限 = APP_TRANSCRIPT_MAX(128) - 1(固件 str_take 的
# cap-1 NUL 保险):超长由 split_transcript 切分,逐条下行,设备逐条覆盖显示
TRANSCRIPT_TEXT_MAX = 127


class RelayError(Exception):
    """中转失败(扫描不到/连接断开/超时/载荷超限)。"""


# ---- 分片重组(纯函数, 独立可测) ----

def reassemble_audio(buf, chunk):
    """AUDIO 分片重组: (累积缓冲, 一段 ATT 载荷) -> (新缓冲, 整帧列表)。

    设备按 MTU-3 chunk 逐片 notify, 一帧 3200B 可能跨多个 chunk, 一个
    chunk 也可能横跨两帧边界; 尾部不足一帧的字节留在缓冲等下一 chunk。
    """
    buf = buf + bytes(chunk)
    frames = []
    while len(buf) >= AUDIO_FRAME_BYTES:
        frames.append(buf[:AUDIO_FRAME_BYTES])
        buf = buf[AUDIO_FRAME_BYTES:]
    return buf, frames


def reassemble_event(buf, chunk):
    """EVENT 分片重组: (累积缓冲, 一段 ATT 载荷) -> (新缓冲, 整行列表)。

    事件行以 '\\n' 结尾, 可能跨 chunk; 未带结尾的尾部留在缓冲。
    """
    buf = buf + bytes(chunk)
    lines = []
    while b"\n" in buf:
        line, _, rest = buf.partition(b"\n")
        lines.append(line)
        buf = rest
    return buf, lines


def split_transcript(text, max_bytes=TRANSCRIPT_TEXT_MAX):
    """转写文本按字节上限切分(UTF-8 码点安全, 不切断多字节字符)。

    固件 transcript.text 是字节缓冲, 中文 3B/字; 段长按字节计, 逐段下行。
    单段超限 → 拆两条; 恰在上限内 → 原样单条。
    """
    if not text or len(text.encode("utf-8")) <= max_bytes:
        return [text] if text else []
    segs, cur, cur_bytes = [], [], 0
    for ch in text:
        b = len(ch.encode("utf-8"))
        if cur and cur_bytes + b > max_bytes:
            segs.append("".join(cur))
            cur, cur_bytes = [], 0
        cur.append(ch)
        cur_bytes += b
    if cur:
        segs.append("".join(cur))
    return segs


# ---- 传输层 ----

class BleakTransport:
    """默认传输层(bleak)。与 FakeTransport 同接口, 便于单测注入。"""

    def __init__(self):
        self._client = None

    async def scan_for_device(self, name, timeout):
        from bleak import BleakScanner
        dev = await BleakScanner.find_device_by_name(name, timeout=timeout)
        return dev.address if dev else None

    async def connect(self, address, on_disconnect=None):
        from bleak import BleakClient
        self._client = BleakClient(address, timeout=30,
                                   disconnected_callback=on_disconnect)
        await self._client.connect()
        # macOS: MTU 由 CoreBluetooth 与设备协商(设备 ATT_PREFERRED_MTU=517),
        # 无需也不支持中央侧指定; 长载荷 write_gatt_char 自动按 MTU 分包。

    async def write_gatt_char(self, uuid, data):
        await self._client.write_gatt_char(uuid, data, response=True)

    async def start_notify(self, uuid, handler):
        # bleak 3.x 回调签名 (characteristic, data)
        await self._client.start_notify(uuid, lambda _c, d: handler(d))

    async def disconnect(self):
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:
                pass


class Relay:
    """BLE 中转主逻辑。transport / asr_factory / inject_fn 均可注入(单测)。"""

    def __init__(self, transport=None, *, asr_factory=None, inject_fn=None,
                 timeout=60.0, do_inject=True, do_approval=True,
                 dry_run=False):
        from asr_client import StreamingASR
        from inject import paste_text
        self._transport = transport or BleakTransport()
        self._asr_factory = asr_factory or (lambda: StreamingASR())
        self._inject_fn = inject_fn or paste_text
        self.timeout = timeout
        self.do_inject = do_inject
        self.do_approval = do_approval
        self.dry_run = dry_run
        self._queue = asyncio.Queue()
        self._stop = asyncio.Event()
        self._session = None        # 当前 voice 会话(None=空闲)
        self._approval_waiter = None
        self._approval_task = None  # 审批演示后台任务
        self._device_drop = None    # 最近 status 帧的设备掉帧数
        self.session_stats = []     # 每个 voice 会话的掉帧统计(AC3 对账)
        self.decisions = []         # 收到的 agent.action 列表

    # -- 主流程 --

    async def run(self, device_addr=None):
        t = self._transport
        if not device_addr:
            addr = await t.scan_for_device(DEVICE_NAME, SCAN_TIMEOUT)
            if not addr:
                raise RelayError(
                    f"未发现设备 {DEVICE_NAME}(设备开机并处于 BLE 广播状态?)")
            device_addr = addr
        print(f"[relay] 连接 {device_addr} ...")
        try:
            await t.connect(device_addr, on_disconnect=self._on_disconnected)
            print("[relay] 已连接, 订阅 EVENT/AUDIO")
            await t.start_notify(EVENT_UUID, self._cb("event"))
            await t.start_notify(AUDIO_UUID, self._cb("audio"))
        except RelayError:
            raise
        except Exception as e:
            raise RelayError(f"连接/订阅失败: {e}") from e
        print("[relay] 等待语音(PTT 按住说话; Ctrl+C 退出)")
        try:
            await self._drain()
        finally:
            if self._approval_task is not None:
                self._approval_task.cancel()
                try:
                    await self._approval_task
                except (asyncio.CancelledError, Exception):
                    pass
                self._approval_task = None
            await t.disconnect()

    def _on_disconnected(self, *_):
        print("[relay] BLE 连接已断开", file=sys.stderr)
        self._stop.set()
        self._queue.put_nowait(("stop", b""))

    def _cb(self, kind):
        def handler(data):
            # bleak 回调线程安全: 投递到事件循环队列, 由 drain 协程处理
            self._queue.put_nowait((kind, bytes(data)))
        return handler

    async def _drain(self):
        audio_buf = bytearray()
        event_buf = bytearray()
        while not self._stop.is_set():
            kind, chunk = await self._queue.get()
            if kind == "stop":
                break
            if kind == "audio":
                audio_buf, frames = reassemble_audio(audio_buf, chunk)
                for fr in frames:
                    await self._on_audio_frame(fr)
            else:
                event_buf, lines = reassemble_event(event_buf, chunk)
                for ln in lines:
                    await self._on_event_line(ln)

    # -- 事件处理 --

    async def _on_event_line(self, line):
        try:
            ev = json.loads(line)
        except (ValueError, UnicodeDecodeError):
            print(f"[event] 畸形 JSON 行, 丢弃: {line[:80]!r}", file=sys.stderr)
            return
        etype = ev.get("event")
        if etype == "device.hello":
            print(f"[event] device.hello proto={ev.get('proto')}")
        elif etype == "workflow.switch":
            print(f"[event] workflow.switch current={ev.get('current')}")
        elif etype == "voice.start":
            await self._on_voice_start(ev)
        elif etype == "voice.end":
            await self._on_voice_end()
        elif etype == "status":
            # voice.end 后设备补发的会话对账帧: 挂到上一个完成的会话
            self._device_drop = ev.get("drop")
            if self.session_stats:
                self.session_stats[-1]["device_drop"] = self._device_drop
            print(f"[event] status drop={self._device_drop}")
        elif etype == "agent.action":
            self.decisions.append(ev)
            print(f"[event] agent.action action={ev.get('action')} "
                  f"taskId={ev.get('taskId')}")
            if self._approval_waiter is not None:
                self._approval_waiter.set()
        else:
            print(f"[event] 未知事件类型: {etype!r}")

    # -- voice 会话状态机 --

    async def _on_voice_start(self, ev):
        if self._session is not None:
            print("[voice] 收到重复 voice.start, 先结束上一个会话",
                  file=sys.stderr)
            await self._end_session()
        print(f"[voice] start workflow={ev.get('workflow')}")
        self._session = _VoiceSession(self, self._asr_factory())
        await self._session.begin()

    async def _on_audio_frame(self, frame):
        if self._session is not None:
            await self._session.feed(frame)
        # 空闲期音频帧直接丢弃(不计入统计)

    async def _on_voice_end(self):
        if self._session is None:
            print("[voice] 收到 voice.end 但无进行中会话, 忽略",
                  file=sys.stderr)
            return
        await self._end_session()
        # 审批演示放后台任务: 不阻塞 drain, status/agent.action 等后续事件
        # 照常处理(否则 status 对账帧会被压在队列里等按键)
        if self.do_approval and self._approval_task is None:
            self._approval_task = asyncio.create_task(self._demo_approval())

    async def _end_session(self):
        s = self._session
        self._session = None
        await s.end()
        print("[relay] 等待下一次语音(Ctrl+C 退出)")

    # -- 注入 --

    def _inject(self, text):
        if not text:
            return
        if self.dry_run:
            print(f"[inject:dry-run] 将粘贴 {len(text)} 字符: {text[:60]!r}")
        elif self.do_inject:
            try:
                self._inject_fn(text)
                print(f"[inject] 已粘贴 {len(text)} 字符")
            except Exception as e:
                print(f"[inject] 失败: {e}", file=sys.stderr)

    # -- 转写下行(设备屏幕预览/定稿;与注入解耦:--no-inject 下照发) --

    async def _downlink_transcript(self, text, is_final):
        """把转写文本下行到设备屏幕: partial → final:false(预览态),
        定稿 → final:true。超长按 UTF-8 码点边界分多条。失败只记日志, 不中断。
        """
        if not text:
            return
        for seg in split_transcript(text):
            line = {"type": "transcript", "text": seg, "final": is_final}
            try:
                await self._send_ctrl(line)
            except Exception as e:
                print(f"[tx] transcript 下行失败(未连接/写失败): {e}",
                      file=sys.stderr)

    # -- 审批演示 --

    async def _demo_approval(self):
        """转写注入后模拟 agent 工作流: 发审批请求, 等设备按键决策。"""
        try:
            waiter = asyncio.Event()
            self._approval_waiter = waiter
            req = {
                "type": "agent.approval_request",
                "taskId": "task-001",
                "title": "Deploy to production",
                "target": "api.example.com",
                "diffSummary": "+12 -3 in deploy.sh",
                "riskLevel": "high",
            }
            await self._send_ctrl(req)
            print("[approval] 已发审批请求, 等设备按键决策(●/▲/▼)...")
            try:
                await asyncio.wait_for(waiter.wait(), timeout=self.timeout)
            except asyncio.TimeoutError:
                print(f"[approval] {self.timeout:.0f}s 未收到 agent.action",
                      file=sys.stderr)
        finally:
            self._approval_waiter = None
            self._approval_task = None

    async def _send_ctrl(self, obj):
        payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        if len(payload) > CTRL_LINE_MAX:
            raise RelayError(f"CTRL 载荷超 {CTRL_LINE_MAX}B, 未发送")
        try:
            await self._transport.write_gatt_char(CTRL_UUID, payload)
        except Exception as e:
            raise RelayError(f"CTRL 写入失败: {e}") from e

    @staticmethod
    def print_stats(s):
        """打印会话掉帧统计(AC3 对账)。"""
        dev = (f"  设备掉帧 {s['device_drop']}"
               if s.get("device_drop") is not None else "")
        print(f"[voice] 会话 {s['duration']:.2f}s: 理论帧 {s['theory_frames']} "
              f"实收 {s['rx_frames']} 差 {s['missed']} "
              f"({s['drop_pct']:.1f}%){dev}")
        if s["final_text"]:
            print(f"[voice] 最终转写: {s['final_text'][:120]}")


class _VoiceSession:
    """一次 voice.start..voice.end 会话: ASR 流 + 实时注入 + 掉帧统计。"""

    def __init__(self, relay, asr):
        self.relay = relay
        self.asr = asr
        self.start_mono = time.monotonic()
        self.end_mono = None
        self.rx_frames = 0
        self.final_text = ""
        self._final_received = asyncio.Event()
        self._results_task = None

    async def begin(self):
        await self.asr.connect()
        self._results_task = asyncio.create_task(self._results_loop())

    async def _results_loop(self):
        """消费 ASR 结果: 中间结果实时下行预览 + 注入, 最终结果收尾并置完成事件。

        每包 result.text 是全量累计文本: 下行让设备屏幕实时预览(partial →
        final:false 预览态), 定稿(voice.end 最终文本) → final:true 落定。
        下行与注入解耦: 任何下行失败只记日志, 不影响注入与收尾。
        """
        try:
            async for text, is_final in self.asr.results():
                if text:
                    await self.relay._downlink_transcript(text, is_final)
                if is_final:
                    if text:
                        self.final_text = text
                        await asyncio.to_thread(self.relay._inject, text)
                    self._final_received.set()
                    return
                if text:
                    await asyncio.to_thread(self.relay._inject, text)
        except Exception as e:
            print(f"[asr] 结果流异常: {e}", file=sys.stderr)
            self._final_received.set()

    async def feed(self, frame):
        self.rx_frames += 1
        await self.asr.send_frame(frame)

    async def end(self):
        """voice.end: 结束 ASR 流, 等最终结果, 打印掉帧统计。"""
        try:
            await self.asr.send_end()
            try:
                await asyncio.wait_for(self._final_received.wait(),
                                       timeout=self.relay.timeout)
            except asyncio.TimeoutError:
                print(f"[voice] {self.relay.timeout:.0f}s 内未收到最终结果, "
                      "以最后中间结果收尾", file=sys.stderr)
        finally:
            await self.asr.close()
            if self._results_task is not None:
                self._results_task.cancel()
                try:
                    await self._results_task
                except (asyncio.CancelledError, Exception):
                    pass
            self.end_mono = time.monotonic()
        stats = self._stats()
        self.relay.session_stats.append(stats)
        self.relay.print_stats(stats)

    def _stats(self):
        """AC3 掉帧统计: 会话时长推理论帧数 vs 实收帧数。"""
        dur = self.end_mono - self.start_mono
        theory = max(0, round(dur / AUDIO_FRAME_SEC))
        missed = theory - self.rx_frames
        pct = (missed / theory * 100.0) if theory else 0.0
        return {"duration": dur, "theory_frames": theory,
                "rx_frames": self.rx_frames, "missed": missed,
                "drop_pct": pct, "device_drop": None,
                "final_text": self.final_text}


# ---- CLI ----

def main():
    ap = argparse.ArgumentParser(
        description="AI Passport BLE 中转: 设备音频 → 火山 ASR → 注入输入框",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--device", default=None,
                    help="BLE 地址(默认扫描 'AI Passport')")
    ap.add_argument("--no-inject", action="store_true",
                    help="只转写不注入(调试)")
    ap.add_argument("--no-approval", action="store_true",
                    help="关闭审批演示(不发 approval_request)")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="等待 ASR 最终结果/审批决策的超时(秒)")
    ap.add_argument("--dry-run", action="store_true",
                    help="等价 --no-inject, 且注入动作只打印将执行的命令")
    args = ap.parse_args()

    relay = Relay(
        transport=BleakTransport(),
        timeout=args.timeout,
        do_inject=not (args.no_inject or args.dry_run),
        do_approval=not args.no_approval,
        dry_run=args.dry_run,
    )
    try:
        asyncio.run(relay.run(args.device))
    except KeyboardInterrupt:
        print("\n[relay] Ctrl+C, 已退出")
    except RelayError as e:
        print(f"[relay] 错误: {e}", file=sys.stderr)
        print("[relay] 请确认设备开机且在 BLE 广播范围, 然后重新运行本程序",
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
