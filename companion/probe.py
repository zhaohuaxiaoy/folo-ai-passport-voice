#!/usr/bin/env python3
"""通道自动探测: BLE → USB 优先级(向导发现页用)。

传输层注入式(与 relay 相同的 5 方法契约), 测试可传 fake 工厂单测;
探测只验证"设备可达", 不启动完整 relay。
"""
import sys

DEVICE_NAME = "AI Passport"
BLE_SCAN_TIMEOUT = 5.0


def _default_factories():
    """真实传输层工厂(函数内懒加载, 模块顶层无平台依赖)。"""
    def ble():
        from relay import BleakTransport
        return BleakTransport()

    def usb():
        from serial_transport import SerialTransport
        return SerialTransport()

    return {"ble": ble, "usb": usb}


async def probe_channels(cfg=None, on_status=None, *, factories=None):
    """按 BLE → USB 顺序探测, 返回首个命中的 (channel, addr)。

    cfg: 可含 usb_port。
    on_status(str): 进度回调(UI 状态行, 可 None)。
    factories: 传输层工厂注入(测试): {"ble": f, "usb": f}。
    全部失败返回 (None, None); 各通道异常打印 stderr(不吞错, 进度可见)。
    """
    cfg = cfg or {}
    f = factories or _default_factories()
    if on_status:
        on_status("正在扫描蓝牙 (BLE)…")
    try:
        addr = await f["ble"]().scan_for_device(DEVICE_NAME, BLE_SCAN_TIMEOUT)
        if addr:
            return "ble", addr
    except Exception as e:  # noqa: BLE001 探测容错: 单通道失败降级下一通道
        print(f"[probe] BLE 探测异常: {e}", file=sys.stderr)

    if on_status:
        on_status("未发现 BLE 设备, 扫描 USB…")
    try:
        addr = await f["usb"]().scan_for_device(DEVICE_NAME, 1.0)
        if addr:
            return "usb", addr
    except Exception as e:  # noqa: BLE001
        print(f"[probe] USB 探测异常: {e}", file=sys.stderr)

    if on_status:
        on_status("未发现 AI Passport 设备, 可手动选择通道后重试")
    return None, None
