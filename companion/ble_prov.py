#!/usr/bin/env python3
"""BLE 配网客户端: 扫描设备 → 写 Wi-Fi 凭据 → 等配网结果。

协议与固件 prov_protocol / ble_provisioning 对齐:
- 服务 0000A1B0-0000-1000-8000-00805F9B34FB
- 写   0000A1B1 WRITE|WRITE_ENC  载荷 {"cmd":"wifi_set","ssid":..,"pass":..} ≤512B
- 订阅 0000A1B2 NOTIFY           结果 {"cmd","status":"ok|error","code","ip","detail"}
- 未配对写 WRITE_ENC 特征 → 设备拒写(ATT 0x0F) → pair() 后重试一次

安全: 密码只存在于本进程内存与 BLE 加密链路, 绝不打印/落盘。
传输层可注入(FakeTransport)以便无硬件单测。
"""
import asyncio
import json

SERVICE_UUID = "0000A1B0-0000-1000-8000-00805F9B34FB"
CMD_UUID = "0000A1B1-0000-1000-8000-00805F9B34FB"
RESULT_UUID = "0000A1B2-0000-1000-8000-00805F9B34FB"
DEVICE_NAME = "AI Passport KB"
CMD_SIZE_MAX = 512


class ProvisionError(Exception):
    """配网失败(扫描不到/连接失败/写入失败/超时/结果畸形)。"""


class BleakTransport:
    """默认传输层(bleak)。与 FakeTransport 同接口, 便于单测注入。"""

    def __init__(self):
        self._client = None

    async def scan_for_device(self, name, timeout):
        from bleak import BleakScanner
        dev = await BleakScanner.find_device_by_name(name, timeout=timeout)
        return dev.address if dev else None

    async def connect(self, address):
        from bleak import BleakClient
        self._client = BleakClient(address)
        await self._client.connect()

    async def pair(self):
        await self._client.pair()

    async def write_gatt_char(self, uuid, data):
        """写特征。返回 0=成功; 非 0=失败(上层据此 pair+重试)。"""
        try:
            await self._client.write_gatt_char(uuid, data, response=True)
            return 0
        except Exception:
            # macOS CoreBluetooth 下未配对写 WRITE_ENC 特征报错无统一错误码,
            # 统一走 pair+重试路径。
            return 0x0F

    async def start_notify(self, uuid, handler):
        # bleak 3.x 回调签名 (characteristic, data)
        await self._client.start_notify(uuid, lambda _c, d: handler(d))

    async def disconnect(self):
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:
                pass


class BleakProvisioner:
    """配网流程: 扫描 → 连接 → 写(失败则 pair 重试) → 订阅 → 等结果。

    transport 可注入(FakeTransport)做无硬件单测。provision() 返回设备
    回的结果 dict({"status":"ok|error", ...}); 失败抛 ProvisionError。
    """

    def __init__(self, transport=None):
        self._transport = transport or BleakTransport()

    async def provision(self, ssid, password, timeout=60.0, device_addr=None):
        if not 1 <= len(ssid.encode("utf-8")) <= 32:
            raise ProvisionError("SSID 长度须在 1..32 字节(UTF-8)")
        if len(password.encode("utf-8")) > 63:
            raise ProvisionError("密码长度须 ≤63 字节(UTF-8)")

        t = self._transport
        if not device_addr:
            device_addr = await t.scan_for_device(DEVICE_NAME, timeout=15)
            if not device_addr:
                raise ProvisionError(
                    f"未发现设备 {DEVICE_NAME}(设备开机并处于 BLE 广播状态?)")

        await t.connect(device_addr)
        payload = json.dumps({"cmd": "wifi_set", "ssid": ssid, "pass": password},
                             ensure_ascii=False).encode("utf-8")
        if len(payload) > CMD_SIZE_MAX:
            raise ProvisionError("载荷超 512B(SSID+密码长度已校验, 不应发生)")

        err = await t.write_gatt_char(CMD_UUID, payload)
        if err:
            # 未配对 → 设备拒写 → 配对后重试一次(Just Works SC, 无需输 PIN)
            await t.pair()
            err = await t.write_gatt_char(CMD_UUID, payload)
            if err:
                raise ProvisionError(f"写入配网命令失败(ATT 0x{err:02X})")

        result = asyncio.get_running_loop().create_future()

        def on_result(raw):
            if not result.done():
                result.set_result(raw)

        await t.start_notify(RESULT_UUID, on_result)
        try:
            raw = await asyncio.wait_for(result, timeout)
        except asyncio.TimeoutError:
            raise ProvisionError(
                f"{int(timeout)}s 超时未收到配网结果(设备是否已连上路由器?)")
        finally:
            await t.disconnect()

        try:
            return json.loads(raw)
        except (TypeError, json.JSONDecodeError):
            raise ProvisionError("设备返回了无法解析的结果")
