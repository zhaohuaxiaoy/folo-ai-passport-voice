#!/usr/bin/env python3
"""WiFi 通道传输层(WsTransport): 本机起 WS server, 设备(STA)主动连。

与 relay.py 的 BleakTransport 同 5 方法契约(传输层注入式, relay 核心零改动):
  scan_for_device / connect / write_gatt_char / start_notify / disconnect

角色反转 vs BLE 直连:
  - 设备是 WS client, PC 是 WS server(端口 ws_port, 默认 8765);
  - 设备发现由 mDNS 异步完成(mdns_pub.py 发布 _ai-passport._tcp),
    scan_for_device 只返回日志用地址;
  - v1 语义与 BLE 对齐: 单设备; 设备断连 → on_disconnect() → relay 收束退出
    (不自动重接受)。

帧类型契约(设备侧 ws_client.c):
  - 下行(PC→设备): 必须是文本帧(op_code 0x1, 二进制帧被设备忽略), 且
    行尾带 '\\n'(设备 feed_rx 按 \\n 切行; 缺换行 = 行累积器永不完成,
    早期 ws_test_server 的 bug 源头)。BLE CTRL 写不带 '\\n' 直接整写解析,
    此处必须补 —— 协议层两通道的差异点。
  - 上行(设备→PC): EVENT 行 = 文本帧(设备序列化已带 '\\n');
    AUDIO 帧 = 二进制帧(3200B 裸 PCM @100ms/16k/16bit)。帧类型由
    websockets 消息类型天然区分(str → EVENT, bytes → AUDIO)。

订阅前缓冲: 设备连上即发 device.hello, 而 relay.run 在 connect 返回后才
start_notify —— 间隙消息缓冲, 两个处理器都注册后冲刷(不丢 hello/抢发
的 voice.start; 上限 64 条防失控)。
"""
import asyncio
import sys

import websockets

SERVICE_UUID = "0000A2B0-0000-1000-8000-00805F9B34FB"
CTRL_UUID = "0000A2B1-0000-1000-8000-00805F9B34FB"
EVENT_UUID = "0000A2B2-0000-1000-8000-00805F9B34FB"
AUDIO_UUID = "0000A2B3-0000-1000-8000-00805F9B34FB"

MAX_WS_MESSAGE = 64 * 1024    # 消息上限: 音频帧 3200B / 事件行 ≤512B, 64KB 兜底
PENDING_MAX = 64              # 订阅前缓冲上限(device.hello 等秒级几条)


class WsError(Exception):
    """WiFi 通道错误(连接超时/未连接/发送失败)。"""


class WsTransport:
    """本机 WS server 传输层(设备 STA 主动连; 单设备, v1 不自动重接受)。"""

    def __init__(self, port=8765, connect_timeout=120.0):
        self.port = port
        self.connect_timeout = connect_timeout
        self._ws = None                 # 当前设备连接(websockets server 侧句柄)
        self._server = None
        self._connected = asyncio.Event()
        self._on_disconnect = None
        self._evt_handler = None
        self._aud_handler = None
        self._pending = []              # (str=EVENT 行 | bytes=AUDIO 帧)

    # -- 5 方法契约(对齐 BleakTransport) --

    async def scan_for_device(self, name, timeout):
        """返回 ws://<本机IP>:<port>(仅日志; 设备发现由 mDNS 异步完成)。"""
        from mdns_pub import local_ipv4
        try:
            ip = local_ipv4()
        except RuntimeError as e:
            print(f"[ws] {e}", file=sys.stderr)
            return None
        return f"ws://{ip}:{self.port}"

    async def connect(self, address, on_disconnect=None):
        """起 WS server 等设备连入(connect_timeout 秒), 断连回调对齐 BLE。"""
        self._on_disconnect = on_disconnect
        # websockets 17: serve() 是 async 工厂, await 后完成绑定监听
        self._server = await websockets.serve(self._handle, "0.0.0.0",
                                              self.port,
                                              max_size=MAX_WS_MESSAGE)
        # 实际绑定端口(port=0 供测试取随机端口)
        self.port = self._server.server.sockets[0].getsockname()[1]
        print(f"[ws] WS server 监听 :{self.port}, 等待设备连接"
              f"(最多 {self.connect_timeout:.0f}s)...")
        try:
            await asyncio.wait_for(self._connected.wait(),
                                   timeout=self.connect_timeout)
        except asyncio.TimeoutError:
            await self.disconnect()
            raise WsError(
                f"等待设备连接超时({self.connect_timeout:.0f}s): 确认设备已"
                "配好 WiFi 且 ws 目标指向本机(设备控制台 ws set auto, "
                "或 ws set ws://<本机IP>:<端口>)")

    async def write_gatt_char(self, uuid, data):
        """下行文本帧 + 行尾 '\\n'(uuid 忽略; 设备仅解析文本帧, 契约见模块注释)。"""
        if self._ws is None:
            raise WsError("设备未连接, 下行丢弃")
        await self._ws.send((bytes(data) + b"\n").decode("utf-8"))

    async def start_notify(self, uuid, handler):
        if uuid == EVENT_UUID:
            self._evt_handler = handler
        elif uuid == AUDIO_UUID:
            self._aud_handler = handler
        else:
            raise WsError(f"未知特征: {uuid}(WiFi 通道仅 EVENT/AUDIO 两类)")
        if self._evt_handler is not None and self._aud_handler is not None:
            pending, self._pending = self._pending, []
            for raw in pending:
                self._dispatch(raw)

    async def disconnect(self):
        """关闭设备连接与服务器(relay 收束退出时调用)。"""
        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:
                pass
            self._ws = None
        if self._server is not None:
            self._server.close()
            try:
                await self._server.wait_closed()
            except Exception:
                pass
            self._server = None

    # -- 内部 --

    def _dispatch(self, raw):
        handler = self._evt_handler if isinstance(raw, str) else self._aud_handler
        if handler is None:
            return
        try:
            res = handler(raw.encode("utf-8") if isinstance(raw, str) else raw)
            if asyncio.iscoroutine(res):
                asyncio.create_task(res)   # 兼容 async 回调(relay 用同步回调)
        except Exception:
            pass    # 回调异常不扩散进连接处理(与 bleak 回调语义一致)

    async def _handle(self, ws):
        """设备连接处理器(v1 单设备: 已有连接时拒绝新连接)。"""
        if self._ws is not None:
            await ws.close(code=4000, reason="busy: device already connected")
            return
        self._ws = ws
        self._connected.set()
        print(f"[ws] 设备已连接: {ws.remote_address}")
        try:
            async for raw in ws:
                if self._evt_handler is not None and self._aud_handler is not None:
                    self._dispatch(raw)
                elif len(self._pending) < PENDING_MAX:
                    self._pending.append(raw)   # 订阅前缓冲(设备 hello 等)
                else:
                    print("[ws] 订阅前消息过多, 丢弃", file=sys.stderr)
        finally:
            self._ws = None
            print("[ws] 设备已断开", file=sys.stderr)
            if self._on_disconnect is not None:
                self._on_disconnect()
