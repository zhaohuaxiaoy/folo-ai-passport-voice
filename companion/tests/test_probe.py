#!/usr/bin/env python3
"""probe 单测: BLE→WiFi→USB 优先级探测, 注入 fake 传输层工厂。

覆盖:
- BLE 命中 → 直接返回, 不碰 WiFi/USB
- BLE 空 + WiFi 命中 → 返回 wifi(探测后断开, 不留连接)
- BLE/WiFi 空 + USB 命中 → 返回 usb
- 全空 → (None, None)
- 单通道异常 → 降级下一通道
- on_status 进度回调收到阶段文案

运行: companion/.venv/bin/python companion/tests/test_probe.py
"""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from probe import probe_channels  # noqa: E402

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")


class FakeBle:
    def __init__(self, addr):
        self._addr = addr
        self.scanned = False

    async def scan_for_device(self, name, timeout):
        self.scanned = True
        return self._addr

    async def connect(self, address, on_disconnect=None):
        pass

    async def disconnect(self):
        pass


class FakeWifi:
    def __init__(self, port, timeout):
        self.port = port
        self.timeout = timeout
        self.connected = False
        self.disconnected = False

    async def connect(self, address, on_disconnect=None):
        if self._accept:
            self.connected = True
        else:
            raise TimeoutError("no device")

    async def disconnect(self):
        self.disconnected = True


class FakeUsb:
    def __init__(self, port):
        self._port = port
        self.scanned = False

    async def scan_for_device(self, name, timeout):
        self.scanned = True
        return self._port

    async def connect(self, address, on_disconnect=None):
        pass

    async def disconnect(self):
        pass


def factories(ble_addr=None, wifi_accept=False, usb_port=None):
    objs = {}

    def ble():
        objs["ble"] = FakeBle(ble_addr)
        return objs["ble"]

    def wifi(port, timeout):
        objs["wifi"] = FakeWifi(port, timeout)
        objs["wifi"]._accept = wifi_accept
        return objs["wifi"]

    def usb():
        objs["usb"] = FakeUsb(usb_port)
        return objs["usb"]

    return {"ble": ble, "wifi": wifi, "usb": usb}, objs


def test_ble_hit_first():
    f, objs = factories(ble_addr="AA:BB")
    got = asyncio.run(probe_channels({}, factories=f))
    check("BLE 命中返回 ble", got, ("ble", "AA:BB"))
    check("BLE 命中不碰 WiFi/USB",
          hasattr(objs.get("wifi", None), "connected"), False)


def test_wifi_hit_after_ble_miss():
    f, objs = factories(wifi_accept=True)
    got = asyncio.run(probe_channels({"ws_port": 9000}, factories=f))
    check("WiFi 命中返回 wifi", got, ("wifi", "ws://127.0.0.1:9000"))
    check("WiFi 探测后断开", objs["wifi"].disconnected, True)
    check("WiFi 端口透传", objs["wifi"].port, 9000)


def test_usb_hit_last():
    f, objs = factories(usb_port="/dev/cu.usbmodem1")
    got = asyncio.run(probe_channels({}, factories=f))
    check("USB 命中返回 usb", got, ("usb", "/dev/cu.usbmodem1"))
    check("WiFi 未尝试连接", objs["wifi"].connected, False)


def test_all_miss():
    f, objs = factories()
    got = asyncio.run(probe_channels({}, factories=f))
    check("全空返回 (None, None)", got, (None, None))


def test_ble_error_falls_through():
    f, objs = factories()

    def ble_boom():
        class Boom:
            async def scan_for_device(self, name, timeout):
                raise OSError("adapter down")
        return Boom()
    f["ble"] = ble_boom
    got = asyncio.run(probe_channels({}, factories=f))
    check("BLE 异常降级 WiFi", got, (None, None))


def test_status_callback():
    f, objs = factories()
    msgs = []
    asyncio.run(probe_channels({}, on_status=msgs.append, factories=f))
    check("进度回调收到阶段文案", len(msgs) >= 2 and "蓝牙" in msgs[0], True)


def main():
    test_ble_hit_first()
    test_wifi_hit_after_ble_miss()
    test_usb_hit_last()
    test_all_miss()
    test_ble_error_falls_through()
    test_status_callback()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for x in FAILURES:
            print("  -", x)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()
