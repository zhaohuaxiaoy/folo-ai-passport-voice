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
→ 帧喂火山(中间结果实时下行设备屏幕预览) → voice.end 收最终结果
(下行定稿帧 + 注入输入框一次) → 审批演示(发 approval_request → 设备按键
→ 收 agent.action)。用户确认的行为: 输入框只落定稿, 中间预览在设备屏幕。
voice 窗口掉帧统计(理论帧数 vs 实收帧数, 与设备 status 帧 drop 双端对账)。
转写下行走 CTRL 特征, 与注入解耦(--no-inject 仍下行)。

传输层可注入(FakeTransport)以便无硬件单测。

用法:
  companion/.venv/bin/python companion/relay.py                       # 扫描+全流程
  companion/.venv/bin/python companion/relay.py --device AA:BB:CC:DD:EE:FF
  companion/.venv/bin/python companion/relay.py --no-inject           # 只转写
  companion/.venv/bin/python companion/relay.py --no-approval         # 关审批演示
  companion/.venv/bin/python companion/relay.py --dry-run             # 只转写+打印注入

通道与注入后端由 companion/config.local.json 的 channel 决定:
  "ble"(缺省)  → BLE 直连(设备广播 "AI Passport");注入 = 平台默认后端
  "wifi"       → 本机起 WS server(端口 ws_port), 设备 STA 主动连
                  (无蓝牙 Windows 电脑);注入 = Windows 剪贴板后端
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
AUDIO_Q_MAX = 100   # 音频帧(3200B)上限 ≈10s 积压;ASR 停顿时丢帧保内存
EVENT_Q_MAX = 64    # 控制事件上限(正常秒级几条;兜底防失控)
CTRL_LINE_MAX = 2048        # 对齐固件 APP_PROTO_RX_CAP
SCAN_TIMEOUT = 15
# 下行 transcript 单条文本上限 = APP_TRANSCRIPT_MAX(128) - 1(固件 str_take 的
# cap-1 NUL 保险):超长由 split_transcript 切分,逐条下行,设备逐条覆盖显示
TRANSCRIPT_TEXT_MAX = 127
# 重组缓冲上限:超限 = 协议违约/链路失步(如 MTU 变化导致的分片流错位),
# 直接清空等下一 chunk 重新对齐, 防内存无限增长。
REASSEMBLE_AUDIO_MAX = 64 * 1024   # 64KB ≈ 20 帧未重组
REASSEMBLE_EVENT_MAX = 4 * 1024    # 4KB 未成行


class RelayError(Exception):
    """中转失败(扫描不到/连接断开/超时/载荷超限)。"""


# ---- 分片重组(纯函数, 独立可测) ----

def reassemble_audio(buf, chunk, max_buf=REASSEMBLE_AUDIO_MAX):
    """AUDIO 分片重组: (累积缓冲 bytearray, 一段 ATT 载荷) -> (新缓冲, 整帧列表)。

    设备按 MTU-3 chunk 逐片 notify, 一帧 3200B 可能跨多个 chunk, 一个
    chunk 也可能横跨两帧边界; 尾部不足一帧的字节留在缓冲等下一 chunk。
    未重组缓冲超过 max_buf(默认 64KB)视为链路失步: 清空缓冲等下一 chunk
    重新对齐, 防内存无限增长。原地 extend/del: 避免每 chunk 整缓冲拷贝
    (buf + bytes 是 O(n) 重分配)。
    """
    buf.extend(chunk)
    frames = []
    while len(buf) >= AUDIO_FRAME_BYTES:
        frames.append(buf[:AUDIO_FRAME_BYTES])
        del buf[:AUDIO_FRAME_BYTES]
    if len(buf) > max_buf:
        buf.clear()   # 失步: 丢弃, 下一 chunk 重新对齐
    return buf, frames


def reassemble_event(buf, chunk, max_buf=REASSEMBLE_EVENT_MAX):
    """EVENT 分片重组: (累积缓冲 bytearray, 一段 ATT 载荷) -> (新缓冲, 整行列表)。

    事件行以 '\\n' 结尾, 可能跨 chunk; 未带结尾的尾部留在缓冲。
    未成行缓冲超过 max_buf(默认 4KB, 远大于 EVENT_LINE_MAX 512)视为失步:
    清空缓冲, 防内存无限增长。原地 extend/partition, 无整缓冲重分配。
    """
    buf.extend(chunk)
    lines = []
    while b"\n" in buf:
        line, _, rest = buf.partition(b"\n")
        lines.append(line)
        buf = rest
    if len(buf) > max_buf:
        buf.clear()   # 失步: 丢弃
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
        # response=False: 无响应写,免等 ATT 确认 RTT(~5-20ms),转写预览/审批更快落屏。
        # 固件 CTRL 特征已加 WRITE_NO_RSP(回调零改动);下行失败本就不重试,语义不变。
        await self._client.write_gatt_char(uuid, data, response=False)

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
                 dry_run=False, connect_timeout_s=5.0):
        from asr_client import StreamingASR
        from inject import paste_text
        self._transport = transport or BleakTransport()
        self._asr_factory = asr_factory or (lambda: StreamingASR())
        self._inject_fn = inject_fn or paste_text
        self.timeout = timeout
        self.connect_timeout_s = connect_timeout_s
        self.do_inject = do_inject
        self.do_approval = do_approval
        self.dry_run = dry_run
        # 音频与控制事件分开消费(见 _drain_audio/_drain_events):
        # ASR 暂停时音频队列有界(丢帧而非无限增长),voice.end/status 等
        # 控制事件不被音频帧压队(独立队列,即时处理)。
        self._audio_q = asyncio.Queue(maxsize=AUDIO_Q_MAX)
        self._event_q = asyncio.Queue(maxsize=EVENT_Q_MAX)
        self._dropped_audio = 0     # 音频队列满丢弃计数(累计,进程内)
        self._stop = asyncio.Event()
        self._session = None        # 当前 voice 会话(None=空闲)
        self._closing = None        # 收尾中的会话(end 后台化后,断连时 abort 覆盖)
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
            # 断连/退出:收束会话——取消连接与结果任务、关 ASR WebSocket、
            # 补统计占位。覆盖"进行中"与"end 收尾中"两种(_closing)。
            # 否则 _run/_results 任务与 ASR 连接泄漏到进程退出
            # (审查 P1: 合成测试已复现断开后 asr.closed=False、结果任务存活)。
            if self._session is not None:
                await self._session.abort()
                self._session = None
            if self._closing is not None:
                await self._closing.abort()
                self._closing = None
            await t.disconnect()

    def _safe_put(self, q, item):
        """有界队列安全写: 满则不抛(断开流程不被 QueueFull 打断)。
        sentinel 丢失可接受(_stop.set() 已驱动 drain 退出);数据帧满 = 丢帧(计数)。"""
        try:
            q.put_nowait(item)
        except asyncio.QueueFull:
            if q is self._audio_q:
                self._dropped_audio += 1
            print("[relay] 队列满,条目丢弃", file=sys.stderr)

    def _on_disconnected(self, *_):
        print("[relay] BLE 连接已断开", file=sys.stderr)
        self._stop.set()
        self._safe_put(self._audio_q, ("stop", b""))
        self._safe_put(self._event_q, ("stop", b""))

    def _cb(self, kind):
        def handler(data):
            # bleak 回调线程安全: 投递到事件循环队列, 由 drain 协程处理
            payload = bytes(data)
            if kind == "audio":
                try:
                    self._audio_q.put_nowait((kind, payload))
                except asyncio.QueueFull:
                    # 有界上限:ASR 卡住时丢音频帧(可容忍,会话级掉帧统计兜底)
                    self._dropped_audio += 1
                    if self._dropped_audio % 100 == 1:
                        print(f"[relay] 音频队列满,丢弃 1 帧"
                              f"(累计 {self._dropped_audio})", file=sys.stderr)
            else:
                try:
                    self._event_q.put_nowait((kind, payload))
                except asyncio.QueueFull:
                    print("[relay] 事件队列满,丢弃控制帧(异常)",
                          file=sys.stderr)
        return handler

    async def _drain(self):
        # 音频(ASR 慢路径)与控制事件(快路径)各自独立消费:
        # voice.end/status 等不再排在音频帧后面。
        await asyncio.gather(self._drain_audio(), self._drain_events())

    async def _drain_audio(self):
        audio_buf = bytearray()
        while not self._stop.is_set():
            kind, chunk = await self._audio_q.get()
            if kind == "stop":
                break
            audio_buf, frames = reassemble_audio(audio_buf, chunk)
            for fr in frames:
                await self._on_audio_frame(fr)

    async def _drain_events(self):
        event_buf = bytearray()
        while not self._stop.is_set():
            kind, chunk = await self._event_q.get()
            if kind == "stop":
                break
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
            # 内存有界:与 session_stats 同款截断(每会话至多 1 条,100 条足够复盘)
            if len(self.decisions) > 100:
                self.decisions.pop(0)
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
            s = self._session
            self._session = None
            self._closing = s              # 收尾中的会话:断连时 abort 也要覆盖它
            asyncio.create_task(s.end())   # 旧会话后台收尾(不阻塞)
        print(f"[voice] start workflow={ev.get('workflow')}")
        self._session = _VoiceSession(self, self._asr_factory())
        await self._session.begin()        # 立即返回(ASR 连接后台化)

    async def _on_audio_frame(self, frame):
        s = self._session
        if s is None:
            return   # 空闲期音频帧直接丢弃(不计入统计)
        try:
            await s.feed(frame)
        except Exception as e:
            # 会话边界并发:voice.end 已关 ASR 流时在途帧投喂失败——丢弃即可
            print(f"[audio] 帧投喂失败(会话已结束?): {e}", file=sys.stderr)

    async def _on_voice_end(self):
        if self._session is None:
            print("[voice] 收到 voice.end 但无进行中会话, 忽略",
                  file=sys.stderr)
            return
        s = self._session
        self._session = None
        self._closing = s              # 收尾中的会话:断连时 abort 也要覆盖它
        # 会压住 status 对账帧与后续事件(与 _demo_approval 同源问题)。
        # 会话统计先占位(session_stats 立即出现),status 帧挂到它——
        # 收尾完成时只补 final_text 并打印(见 _VoiceSession.end)。
        asyncio.create_task(s.end())
        # 审批演示放后台任务: 不阻塞 drain, status/agent.action 等后续事件
        # 照常处理(否则 status 对账帧会被压在队列里等按键)
        if self.do_approval and self._approval_task is None:
            self._approval_task = asyncio.create_task(self._demo_approval())

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
    """一次 voice.start..voice.end 会话: ASR 流 + 下行预览 + 定稿注入 + 掉帧统计。

    生命周期全部后台化(Relay 事件路径零阻塞):
      begin() 只起连接协程(握手数百 ms 不压事件队列);
      feed() 在连接就绪前挂起(帧不丢,只延迟),连接失败抛错由调用方兜底;
      end() 由 Relay 以 create_task 调用,在后台做 send_end/等最终/close/统计,
      session_stats 在收尾开始时即占位(device_drop=None),status 帧可立即
      挂到它——收尾完成只补 final_text/打印。
    """

    def __init__(self, relay, asr):
        self.relay = relay
        self.asr = asr
        self.start_mono = time.monotonic()
        self.end_mono = None
        self.rx_frames = 0
        self.final_text = ""
        self._connected = asyncio.Event()   # ASR 连接就绪
        self._conn_error = None             # 连接失败异常(非 None 后 feed 抛错)
        self._final_received = asyncio.Event()
        self._results_task = None
        self._run_task = None
        self._ended = False                 # end() 幂等
        self._stats_ref = None              # 本会话的统计占位(收尾完成时补全)

    async def begin(self):
        # 连接与结果循环后台化:握手(网络)数百 ms 内事件队列照常消费
        self._run_task = asyncio.create_task(self._run())

    async def _run(self):
        try:
            # 连接加超时(审查 P1-4):_open_ws 悬挂时 wait_for 取消 connect,
            # 走 _conn_error 路径——feed() 的 _connected.wait() 随之返回并抛错,
            # 不再无限等待挂死音频排空。asr.close() 幂等,半途取消安全。
            await asyncio.wait_for(self.asr.connect(),
                                   timeout=self.relay.connect_timeout_s)
        except Exception as e:
            # 连接失败/超时:让 feed/end 看到错误(不悬挂,不崩溃 relay)
            self._conn_error = e
            self._connected.set()
            self._final_received.set()
            return
        self._connected.set()
        self._results_task = asyncio.create_task(self._results_loop())

    async def _results_loop(self):
        """消费 ASR 结果: 中间结果仅下行设备屏幕预览, 定稿收尾并注入一次。

        每包 result.text 是全量累计文本: partial → 下行 final:false 预览态
        (不注入——剪贴板粘贴是插入语义, 多次注入会叠加文本; 用户确认的
        行为是输入框只落定稿), 定稿(voice.end 最终文本) → final:true 落定
        + 注入输入框一次。
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
        except Exception as e:
            print(f"[asr] 结果流异常: {e}", file=sys.stderr)
            self._final_received.set()

    async def feed(self, frame):
        # 连接未就绪:挂起等待(帧不丢只延迟);连接失败:抛错由 _on_audio_frame 兜底
        if not self._connected.is_set():
            await self._connected.wait()
        if self._conn_error is not None:
            raise self._conn_error
        self.rx_frames += 1
        await self.asr.send_frame(frame)

    async def abort(self):
        """断连/退出快速收束: 取消连接与结果任务、关 ASR、补统计占位。
        与 end() 的区别: 不等最终结果(断开时没有结果可等)。幂等;
        与并发 end() 竞态时 end 的 await 会被 set 的事件立即放行。"""
        if self._run_task is not None:
            self._run_task.cancel()
            try:
                await self._run_task
            except (asyncio.CancelledError, Exception):
                pass
            self._run_task = None
        if self._results_task is not None:
            self._results_task.cancel()
            try:
                await self._results_task
            except (asyncio.CancelledError, Exception):
                pass
            self._results_task = None
        try:
            await self.asr.close()
        except Exception as e:
            print(f"[voice] ASR close 异常: {e}", file=sys.stderr)
        # 放行并发等待者: feed() 不再悬挂, 并发的 end() 立即收束(不挂到超时)
        self._final_received.set()
        self._connected.set()
        if self._stats_ref is not None and not self._stats_ref["done"]:
            self._stats_ref["rx_frames"] = self.rx_frames
            self._stats_ref["final_text"] = self.final_text or "(disconnected)"
            self._stats_ref["done"] = True
            self.relay.print_stats(self._stats_ref)

    async def end(self):
        """voice.end 收尾(后台调用,非阻塞 Relay): 结束 ASR 流, 等最终结果,
        关闭, 统计占位→补全。幂等: 重复 start/end 竞态下第二次调用直接返回。"""
        if self._ended:
            return
        self._ended = True
        self.end_mono = time.monotonic()   # voice.end 时刻(理论帧数基准)
        # 占位立即入列:status 帧挂到 session_stats[-1] 不依赖收尾完成。
        # 保存本会话的引用:收尾完成时经 _stats_ref 补全——并发收尾时
        # session_stats[-1] 可能是其他会话的占位(重复 start/后台化后常见)。
        stats = self._stats()
        stats["done"] = False
        self._stats_ref = stats
        self.relay.session_stats.append(stats)
        if len(self.relay.session_stats) > 100:
            self.relay.session_stats.pop(0)   # 有界:长时间运行不无限增长

        if self._conn_error is not None:
            print(f"[voice] ASR 连接失败, 会话无结果: {self._conn_error}",
                  file=sys.stderr)
            await self._close_up()
            return
        if not self._connected.is_set():
            await self._connected.wait()   # 等握手完成(本协程在后台,不阻塞 Relay)
        if self._conn_error is not None:
            print(f"[voice] ASR 连接失败, 会话无结果: {self._conn_error}",
                  file=sys.stderr)
            await self._close_up()
            return
        try:
            await self.asr.send_end()
            try:
                await asyncio.wait_for(self._final_received.wait(),
                                       timeout=self.relay.timeout)
            except asyncio.TimeoutError:
                print(f"[voice] {self.relay.timeout:.0f}s 内未收到最终结果, "
                      "结束会话(无定稿注入)", file=sys.stderr)
        except Exception as e:
            print(f"[voice] 收尾异常: {e}", file=sys.stderr)
        await self._close_up()

    async def _close_up(self):
        """收尾收束: 关 ASR、取消结果循环、补全统计占位、打印。"""
        try:
            await self.asr.close()
        except Exception as e:
            print(f"[voice] ASR close 异常: {e}", file=sys.stderr)
        if self._results_task is not None:
            self._results_task.cancel()
            try:
                await self._results_task
            except (asyncio.CancelledError, Exception):
                pass
        stats = self._stats_ref          # end() 预占位时保存的本会话引用
        stats["rx_frames"] = self.rx_frames
        stats["final_text"] = self.final_text
        stats["done"] = True
        self.relay.print_stats(stats)

    def _stats(self):
        """AC3 掉帧统计: 会话时长推理论帧数 vs 实收帧数。

        理论帧数按 floor 计(固件 100ms 定时发帧, 会话起止帧数为下取整
        语义), missed 保底 0(防止 round 高估产生负掉帧)。
        """
        dur = self.end_mono - self.start_mono
        theory = max(0, int(dur / AUDIO_FRAME_SEC))
        missed = max(0, theory - self.rx_frames)
        pct = (missed / theory * 100.0) if theory else 0.0
        return {"duration": dur, "theory_frames": theory,
                "rx_frames": self.rx_frames, "missed": missed,
                "drop_pct": pct, "device_drop": None,
                "final_text": self.final_text}


# ---- CLI ----

def _build_transport(cfg):
    """按 config.local.json 的 channel 选传输层(Windows 移植 P4+):

    - "ble"(缺省): bleak BLE 直连, macOS/Windows 蓝牙均可;
    - "wifi": 本机起 WS server, 设备 STA 主动连(无蓝牙 Windows 电脑);
      WsTransport 由 ws_transport.py 提供(P5 落地, 本阶段未实现)。
    """
    channel = cfg.get("channel", "ble")
    if channel == "ble":
        return BleakTransport()
    if channel == "wifi":
        from ws_transport import WsTransport   # 懒加载:模块缺失即报错(尚未实现)
        return WsTransport(port=int(cfg.get("ws_port", 8765)),
                           connect_timeout=float(cfg.get("ws_connect_timeout", 120)))
    raise RelayError(
        f"config.local.json 的 channel 值无效: {channel!r}(应为 \"ble\" 或 \"wifi\")")


def _default_inject_fn(cfg):
    """按平台选注入后端(契约一致: 单参 text):

    Windows → inject_win(剪贴板 CF_UNICODETEXT + SendInput Ctrl+V), 带入
    inject_focus_delay(注入前等用户切到目标窗口的秒数);
    其余(macOS) → inject(pbcopy + osascript Cmd+V)。
    """
    if sys.platform == "win32":
        from inject_win import paste_text
        delay = float(cfg.get("inject_focus_delay", 2.0))
        return lambda text: paste_text(text, focus_delay=delay)
    from inject import paste_text
    return paste_text


def main():
    ap = argparse.ArgumentParser(
        description="AI Passport 中转: 设备音频 → 火山 ASR → 注入输入框"
                    "(通道/注入后端由 config.local.json 决定)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--device", default=None,
                    help="BLE 地址(默认扫描 'AI Passport';wifi 通道忽略)")
    ap.add_argument("--no-inject", action="store_true",
                    help="只转写不注入(调试)")
    ap.add_argument("--no-approval", action="store_true",
                    help="关闭审批演示(不发 approval_request)")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="等待 ASR 最终结果/审批决策的超时(秒)")
    ap.add_argument("--dry-run", action="store_true",
                    help="等价 --no-inject, 且注入动作只打印将执行的命令")
    args = ap.parse_args()

    from asr_client import load_config
    cfg = load_config()
    relay = Relay(
        transport=_build_transport(cfg),
        inject_fn=_default_inject_fn(cfg),
        timeout=args.timeout,
        do_inject=not (args.no_inject or args.dry_run),
        do_approval=not args.no_approval,
        dry_run=args.dry_run,
    )
    # wifi 通道: mDNS 发布 _ai-passport._tcp(设备 STA 自动发现本 WS 服务)。
    # 必须在事件循环外调用(zeroconf 同步 API 自阻塞, 见 mdns_pub.py 注释)。
    zc = None
    if cfg.get("channel", "ble") == "wifi":
        from mdns_pub import publish_mdns
        zc, _ = publish_mdns(int(cfg.get("ws_port", 8765)))
    try:
        asyncio.run(relay.run(args.device))
    except KeyboardInterrupt:
        print("\n[relay] Ctrl+C, 已退出")
    except RelayError as e:
        print(f"[relay] 错误: {e}", file=sys.stderr)
        print("[relay] 请确认设备已开机并处于连接范围"
              "(ble: BLE 广播; wifi: 与电脑同一局域网), 然后重新运行本程序",
              file=sys.stderr)
        sys.exit(1)
    finally:
        if zc is not None:
            try:
                zc.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
