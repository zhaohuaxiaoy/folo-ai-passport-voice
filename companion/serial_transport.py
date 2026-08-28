#!/usr/bin/env python3
"""USB 有线通道传输层(SerialTransport): 与设备 USB-Serial-JTAG 串口直连。

与 relay.py 的 BleakTransport 同 5 方法契约(传输层注入式,
relay 核心零改动): scan_for_device / connect / write_gatt_char / start_notify /
disconnect,另加 send_syscmd(line) -> str 扩展(USB 模式无控制台,命令经
SYS 帧下行,响应 SYS_RESP 上行)。

帧协议契约(companion/serial_frame.py ↔ 固件 usb_link_framing.c 逐字节同构):
  [magic 0xA5 0x5A][type][len LE][payload][sum8];EVENT/AUDIO 设备→PC,
  CTRL/SYS PC→设备,SYS_RESP 设备→PC(请求-响应有序配对)。
设备侧在 USB 模式下独占串口(esp_log 已重定向 RAM 环),数据流无串扰。

与 BLE 通道的差异点:
  - connect(address): address = 串口路径(scan_for_device 返回或 config
    usb_port 直给);握手 = 发 SYS "ping" → 设备回 pong + device.hello;
  - 读线程:select(0.5s 超时)等可读 → read(4096) → FrameDecoder 逐帧;
    SYS_RESP → 握手 pong / send_syscmd future;EVENT/AUDIO → 订阅前缓冲
    (PENDING_MAX=64)或分发到 handler(经 call_soon_threadsafe 进事件循环);
  - 拔线:read 抛异常或 select 可读但读空(EOF)→ on_disconnect(对齐 BLE:
    收束退出,v1 不自动重连);
  - CTRL 无 MTU 限制:整帧一次写(4096 驱动 ring,3200B 音频帧不加分片)。
"""
import asyncio
import os
import select
import sys
import threading
import time

import serial
from serial.tools import list_ports

from serial_frame import (FRAME_AUDIO, FRAME_CTRL, FRAME_DONE, FRAME_EVENT,
                          FRAME_SYS, FRAME_SYS_RESP, DIR_UP, FrameDecoder,
                          encode_frame)

SERVICE_UUID = "0000A2B0-0000-1000-8000-00805F9B34FB"   # 契约占位(USB 无特征)
CTRL_UUID = "0000A2B1-0000-1000-8000-00805F9B34FB"
EVENT_UUID = "0000A2B2-0000-1000-8000-00805F9B34FB"
AUDIO_UUID = "0000A2B3-0000-1000-8000-00805F9B34FB"

PENDING_MAX = 64              # 订阅前缓冲上限(device.hello 等秒级几条)
BAUDRATE = 115200             # USB-Serial-JTAG 忽略波特率,取惯例值
HANDSHAKE_TIMEOUT = 3.0       # 握手 ping → pong 超时(plan 契约)
SYS_TIMEOUT = 5.0             # send_syscmd 响应超时

# ESP32-C3 USB-Serial-JTAG:VID 0x303A (Espressif), PID 0x1001 (USB-JTAG/CDC)
VID_ESPRESSIF = 0x303A
PID_USB_JTAG = 0x1001


class SerialError(Exception):
    """USB 通道错误(未找到设备/连接超时/未连接/发送失败)。"""


def _matches(port):
    """端口是否 ESP32-C3 USB-Serial-JTAG(VID/PID 精确;兜底描述/hwid 关键词)。"""
    if port.vid == VID_ESPRESSIF and port.pid == PID_USB_JTAG:
        return True
    hay = f"{port.description} {port.hwid}"
    return "JTAG" in hay or "303A" in hay


def _serial_key(port):
    """去重键:serial_number 优先,无则 hwid(同一设备 tty/cu 双项同键)。"""
    return port.serial_number or port.hwid or port.device


class SerialTransport:
    """USB 串口传输层(单设备;v1 不自动重连,对齐 BLE 语义)。

    port: 直连串口路径(relay 从 config usb_port 读入);None = 自动扫描。
    """

    def __init__(self, port=None, handshake_timeout=HANDSHAKE_TIMEOUT,
                 sys_timeout=SYS_TIMEOUT):
        self._fixed_port = port
        self._handshake_timeout = handshake_timeout
        self._sys_timeout = sys_timeout
        self._ser = None
        self._reader = None
        self._stop = False
        # 下行写串行锁(CTRL/SYS 并发 to_thread 防帧交错)。必须 asyncio.Lock:
        # threading.Lock 的同步 acquire 若遇锁被持有时会阻塞事件循环线程,
        # 使持锁方 to_thread 的完成回调无法调度 → 确定性死锁(审查 P1 回归)。
        self._write_lock = asyncio.Lock()
        self._loop = None
        self._on_disconnect = None
        self._evt_handler = None
        self._aud_handler = None
        self._pending = []          # (type, payload) 订阅前缓冲
        self._pong = None           # asyncio.Event: 握手 pong
        self._sys_resp_future = None
        # 帧解码状态机(跨 read 保留:帧可分片到达)。companion 永远收设备上行,
        # 方向 DIR_UP:只收 EVENT/AUDIO/SYS_RESP,下行类型帧(CTRL/SYS)被拒
        # (方向感知契约,审查 P1)。
        self._dec = FrameDecoder(dir=DIR_UP)

    # -- 5 方法契约(对齐 BleakTransport) --

    async def scan_for_device(self, name, timeout):
        """枚举 USB 串口匹配 ESP32-C3;返回串口路径(端口地址)或 None。

        name/timeout 忽略(USB 无广播发现,枚举即时完成;保留签名对齐契约)。
        """
        if self._fixed_port:
            return self._fixed_port
        cands = self._candidates()
        if not cands:
            print("[usb] 未找到 AI Passport 设备(VID 0x303A PID 0x1001);"
                  "请确认:设备 USB 线已连接(双通道常开,插线即用)", file=sys.stderr)
            return None
        if len(cands) > 1:
            print(f"[usb] 发现 {len(cands)} 个候选端口:", file=sys.stderr)
            for p in cands:
                print(f"  {p.device}  ({p.description})", file=sys.stderr)
            print("[usb] 取第一个;多设备可用 config usb_port 指定端口",
                  file=sys.stderr)
        return cands[0].device

    async def connect(self, address, on_disconnect=None):
        """打开串口 → 起读线程 → SYS ping/pong 握手;超时抛 SerialError。"""
        self._on_disconnect = on_disconnect
        self._loop = asyncio.get_running_loop()
        try:
            self._ser = serial.Serial(address, BAUDRATE, timeout=0)
        except serial.SerialException as e:
            raise SerialError(
                f"无法打开串口 {address}: {e}\n"
                "(确认 USB 已连接、设备处于 USB 模式、端口未被占用)") from e
        self._ser.reset_input_buffer()      # 清陈旧字节(启动残帧/换线残留)
        self._pong = asyncio.Event()
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop,
                                        name="usb_reader", daemon=True)
        self._reader.start()
        print(f"[usb] 已打开 {address},握手(ping→pong, "
              f"{self._handshake_timeout:.0f}s 超时)...")
        try:
            await self._write_frame(FRAME_SYS, b"ping")
            await asyncio.wait_for(self._pong.wait(),
                                   timeout=self._handshake_timeout)
        except asyncio.TimeoutError:
            await self.disconnect()
            raise SerialError(
                f"握手超时({self._handshake_timeout:.0f}s 无 pong):"
                "设备未处于 USB 模式或固件未运行 usb_link")
        print("[usb] 握手完成(设备在线)")

    async def write_gatt_char(self, uuid, data):
        """CTRL 整帧写(uuid 忽略;USB 无 MTU 分片,4096 ring 容整帧)。"""
        if self._ser is None or not self._ser.is_open:
            raise SerialError("设备未连接,下行丢弃")
        await self._write_frame(FRAME_CTRL, bytes(data))

    async def start_notify(self, uuid, handler):
        """注册 EVENT/AUDIO handler;两个都注册后冲刷订阅前缓冲。"""
        if uuid == EVENT_UUID:
            self._evt_handler = handler
        elif uuid == AUDIO_UUID:
            self._aud_handler = handler
        else:
            raise SerialError(f"未知特征: {uuid}(USB 通道仅 EVENT/AUDIO 两类)")
        if self._evt_handler is not None and self._aud_handler is not None:
            pending, self._pending = self._pending, []
            for ftype, payload in pending:
                self._dispatch_frame(ftype, payload)

    async def disconnect(self):
        """关闭串口并收束读线程(relay 收束退出时调用,不触发 on_disconnect)。

        等读线程退出不阻塞事件循环(复核 R2):同步 join 在事件循环线程会
        卡住最多 2s —— 改异步轮询(is_alive + 短 sleep,2s 截止)。读线程在
        _stop 置位 + close 后,select 抛异常或 0.5s 超时必然退出。
        """
        # 告别帧:设备端不靠 SOF 判主机关端口(线插着 SOF 一直有),不发 bye
        # 就得等 10 帧写失败才收束会话,期间 link_channel 粘在 USB,设备把
        # PTT 音频投进死通道并显示 "USB BUSY"。best-effort,失败不影响关闭。
        if self._ser is not None and self._ser.is_open:
            try:
                await asyncio.wait_for(self._write_frame(FRAME_SYS, b"bye"), 0.5)
            except Exception:
                pass
        self._stop = True
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None
        if self._reader is not None:
            deadline = time.monotonic() + 2.0
            while self._reader.is_alive() and time.monotonic() < deadline:
                await asyncio.sleep(0.02)
            self._reader = None

    # -- 扩展: SYS 命令面 --

    async def send_syscmd(self, line):
        """发 SYS 命令,等待设备 SYS_RESP 文本。

        设备端 console_cmds_run_line 同步执行,响应与请求有序配对(单
        future 串行即可;stdin 交互 v1 不发并发命令)。
        """
        if self._ser is None or not self._ser.is_open:
            raise SerialError("设备未连接,命令丢弃")
        fut = self._loop.create_future()
        self._sys_resp_future = fut
        try:
            await self._write_frame(FRAME_SYS, line.encode("utf-8"))
            text = await asyncio.wait_for(fut, timeout=self._sys_timeout)
        except asyncio.TimeoutError:
            self._sys_resp_future = None
            raise SerialError(f"SYS 命令超时({self._sys_timeout:.0f}s): {line}")
        return text

    # -- 内部 --

    def _candidates(self):
        """枚举 + 匹配 + 去重(macOS tty/cu 双项同 serial_number → 优先 cu.)。"""
        infos = [p for p in list_ports.comports() if _matches(p)]
        by_key = {}
        for p in infos:
            key = _serial_key(p)
            if key in by_key:
                # 同名双项:cu.*(可写)优先 tty.*(只读)
                if p.device.startswith("/dev/cu."):
                    by_key[key] = p
            else:
                by_key[key] = p
        return list(by_key.values())

    async def _write_frame(self, ftype, payload):
        frame = encode_frame(ftype, payload)
        # CTRL 与 SYS 两个并发任务可同时进 to_thread 写同一串口 → 帧字节交错。
        # async with 持锁:等锁协程挂起不阻塞事件循环,持锁方 await 恢复后释放,
        # 两个 to_thread 严格串行(审查 P1:threading.Lock 版本会确定性死锁)。
        async with self._write_lock:
            await asyncio.to_thread(self._ser.write, frame)

    def _read_loop(self):
        """读线程:select 等可读 → read → FrameDecoder → 投递到事件循环。

        退出路径(均触发 on_disconnect,对齐 BLE 断连回调):
          - read 抛异常(拔线/端口被占/驱动错误);
          - select 报可读但 read 返回空 = EOF(主机侧拔线/设备复位)。
        """
        try:
            fd = self._ser.fileno()
            while not self._stop:
                try:
                    r, _, _ = select.select([fd], [], [], 0.5)
                except (OSError, ValueError):
                    break                  # fd 已失效(close/拔线)
                if not r:
                    continue               # 超时无数据(循环顶部查 stop)
                try:
                    data = self._ser.read(4096)
                except (serial.SerialException, OSError) as e:
                    if not self._stop:   # disconnect 竞态(close 后 select 已返回)
                        print(f"[usb] 读取异常: {e}", file=sys.stderr)
                    break
                if self._stop:
                    break
                if not data:
                    # select 可读但读空 = EOF(pty 关闭 / 设备复位)
                    print("[usb] 串口读到 EOF(设备侧已关闭)", file=sys.stderr)
                    break
                self._feed(data)
        finally:
            if not self._stop and self._loop is not None:
                try:
                    self._loop.call_soon_threadsafe(self._notify_disconnect)
                except RuntimeError:
                    pass                   # 事件循环已关闭(程序收束中)

    def _feed(self, data):
        # 解码状态机跨 read 保留:一帧可被 USB 物理层分片,分多次 read 到达
        for b in data:
            if self._dec.feed_byte(b) == FRAME_DONE:
                self._on_frame(self._dec.type, bytes(self._dec.payload))

    def _on_frame(self, ftype, payload):
        """读线程上下文:按帧类型路由(全部 call_soon_threadsafe 进事件循环)。

        EVENT/AUDIO 也统一投递:路由动作(pending 判定/append)只在事件循环
        线程执行,与 start_notify 的交换清空同线程 —— 消除跨线程竞态
        (审查 P2-1:读线程在交换后 append,帧滞留永不冲刷)。"""
        if ftype == FRAME_SYS_RESP:
            self._loop.call_soon_threadsafe(self._on_sys_resp, payload)
            return
        self._loop.call_soon_threadsafe(self._route_frame, ftype, payload)

    def _route_frame(self, ftype, payload):
        """事件循环线程:handler 齐备 → 分发;未齐 → 订阅前缓冲(有界)。

        _pending 只在本线程访问(与 start_notify 同线程),无竞态。"""
        if self._evt_handler is not None and self._aud_handler is not None:
            self._dispatch_frame(ftype, payload)
        elif len(self._pending) < PENDING_MAX:
            self._pending.append((ftype, payload))
        else:
            print("[usb] 订阅前消息过多, 丢弃", file=sys.stderr)

    def _on_sys_resp(self, payload):
        """事件循环线程:SYS_RESP 路由(握手 pong / send_syscmd future)。"""
        if not self._pong.is_set() and payload == b"pong":
            self._pong.set()
            return
        if self._sys_resp_future is not None:
            fut, self._sys_resp_future = self._sys_resp_future, None
            if not fut.done():
                fut.set_result(payload.decode("utf-8", "replace"))

    def _dispatch_frame(self, ftype, payload):
        """事件循环线程:分发到 handler(兼容同步/异步回调,异常不扩散)。"""
        handler = (self._evt_handler if ftype == FRAME_EVENT
                   else self._aud_handler)
        if handler is None:
            return
        try:
            res = handler(payload)
            if asyncio.iscoroutine(res):
                asyncio.create_task(res).add_done_callback(
                    lambda t: (t.exception() is not None and
                               print(f"[usb] 回调协程异常: {t.exception()}",
                                     file=sys.stderr)))
        except Exception:
            pass    # 回调异常不扩散进读线程(与 bleak 回调语义一致)

    def _notify_disconnect(self):
        """事件循环线程:断连回调(对齐 BleakTransport 直调语义)。"""
        if self._on_disconnect is None:
            return
        try:
            res = self._on_disconnect()
            if asyncio.iscoroutine(res):
                asyncio.create_task(res).add_done_callback(
                    lambda t: (t.exception() is not None and
                               print(f"[usb] 断连回调协程异常: {t.exception()}",
                                     file=sys.stderr)))
        except Exception:
            pass
