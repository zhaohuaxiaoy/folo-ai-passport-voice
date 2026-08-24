#!/usr/bin/env python3
"""ble_prov 单测: FakeTransport 断言 扫描→连接→写→(pair→重写)→订阅→结果。

运行: python3 companion/tests/test_ble_prov.py  (无需 pytest)
"""
import asyncio
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ble_prov import BleakProvisioner, ProvisionError  # noqa: E402


class FakeTransport:
    """模拟 BLE 链路, 记录操作序列, 可配置未配对/结果/超时。"""

    def __init__(self):
        self.events = []
        self.fail_first_write = False      # 模拟未配对(ATT 0x0F)
        self.result_payload = {            # 设备主动通知的结果
            "cmd": "wifi_set", "status": "ok", "ip": "192.168.1.5"}
        self.notify_immediately = True
        self.devices = ["AA:BB:CC:DD:EE:FF"]
        self.pair_calls = 0

    async def scan_for_device(self, name, timeout):
        self.events.append(("scan", name))
        return self.devices[0] if self.devices else None

    async def connect(self, address):
        self.events.append(("connect", address))

    async def pair(self):
        self.pair_calls += 1
        self.events.append(("pair",))

    async def write_gatt_char(self, uuid, data):
        self.events.append(("write", json.loads(data)))
        if self.fail_first_write and self.pair_calls == 0:
            return 0x0F
        return 0

    async def start_notify(self, uuid, handler):
        self.events.append(("subscribe", uuid))
        if self.notify_immediately and self.result_payload is not None:
            handler(bytearray(json.dumps(self.result_payload).encode()))

    async def disconnect(self):
        self.events.append(("disconnect",))


def run(coro):
    return asyncio.run(coro)


FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


# ---- 基本流程: 写一次成功(已配对/开放网络) ----

async def test_ok_flow():
    t = FakeTransport()
    result = await BleakProvisioner(t).provision(
        "MyHomeNet", "placeholder-pass", timeout=5)
    seq = [e for e, *_ in t.events]
    check("ok 流程操作序列",
          seq, ["scan", "connect", "write", "subscribe", "disconnect"])
    check("ok 载荷 SSID/密码", t.events[2][1],
          {"cmd": "wifi_set", "ssid": "MyHomeNet", "pass": "placeholder-pass"})
    check("ok 结果含 IP", result.get("ip"), "192.168.1.5")
    check("未配对次数", t.pair_calls, 0)


# ---- 未配对: 写失败 → pair → 重写成功 ----

async def test_pair_retry_flow():
    t = FakeTransport()
    t.fail_first_write = True
    result = await BleakProvisioner(t).provision(
        "MyHomeNet", "placeholder-pass", timeout=5)
    seq = [e for e, *_ in t.events]
    check("未配对重试序列",
          seq, ["scan", "connect", "write", "pair", "write", "subscribe", "disconnect"])
    check("重试载荷一致", t.events[2][1], t.events[4][1])
    check("pair 只调一次", t.pair_calls, 1)
    check("重试后结果正常", result.get("status"), "ok")


# ---- 中文 SSID + 空密码(开放网络) ----

async def test_utf8_and_open_network():
    t = FakeTransport()
    await BleakProvisioner(t).provision("小区WiFi", "", timeout=5)
    payload = t.events[2][1]
    check("中文 SSID 原样写入", payload.get("ssid"), "小区WiFi")
    check("空密码=开放网络", payload.get("pass"), "")


# ---- 错误结果: status=error 返回 dict 而非抛异常 ----

async def test_error_result():
    t = FakeTransport()
    t.result_payload = {"cmd": "wifi_set", "status": "error",
                        "code": "auth_fail"}
    result = await BleakProvisioner(t).provision("MyHomeNet", "wrong", timeout=5)
    check("错误结果 code", result.get("code"), "auth_fail")
    check("错误结果 status", result.get("status"), "error")


# ---- 超时 ----

async def test_timeout():
    t = FakeTransport()
    t.result_payload = None   # 设备不回
    try:
        await BleakProvisioner(t).provision("MyHomeNet", "x", timeout=0.2)
        check("超时应抛 ProvisionError", "no-raise", "raised")
    except ProvisionError as e:
        check("超时抛 ProvisionError", "超时" in str(e), True)


# ---- 参数校验 ----

async def test_validation():
    t = FakeTransport()
    try:
        await BleakProvisioner(t).provision("", "x", timeout=1)
        check("空 SSID 应拒绝", "no-raise", "raised")
    except ProvisionError:
        check("空 SSID 应拒绝", "raised", "raised")
    try:
        await BleakProvisioner(t).provision("s" * 40, "x", timeout=1)
        check("超长 SSID 应拒绝", "no-raise", "raised")
    except ProvisionError:
        check("超长 SSID 应拒绝", "raised", "raised")
    try:
        await BleakProvisioner(t).provision("ok", "p" * 70, timeout=1)
        check("超长密码应拒绝", "no-raise", "raised")
    except ProvisionError:
        check("超长密码应拒绝", "raised", "raised")


# ---- 扫描不到设备 ----

async def test_no_device():
    t = FakeTransport()
    t.devices = []
    try:
        await BleakProvisioner(t).provision("MyHomeNet", "x", timeout=1)
        check("扫描不到应抛异常", "no-raise", "raised")
    except ProvisionError:
        check("扫描不到应抛异常", "raised", "raised")


async def main():
    await test_ok_flow()
    await test_pair_retry_flow()
    await test_utf8_and_open_network()
    await test_error_result()
    await test_timeout()
    await test_validation()
    await test_no_device()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    asyncio.run(main())
