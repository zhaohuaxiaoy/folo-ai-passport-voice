#!/usr/bin/env python3
"""BLE 配网 CLI: 把 Mac 当前 Wi-Fi 密码经 BLE 推到 AI Passport 设备。

用法:
  python3 provision.py                          # 自动识别 SSID, getpass 输密码
  python3 provision.py --ssid MyNet             # 手动指定 SSID
  python3 provision.py --ssid MyNet --pass wpa2 # 密码走命令行(会进 shell 历史, 不推荐)
  python3 provision.py --device AA:BB:CC:DD:EE:FF  # 指定设备 BLE 地址(跳过扫描)

安全: 密码只经 getpass 输入, 只存在于进程内存与 BLE 加密链路;
绝不写盘、绝不打日志。config.local.json 只读(容忍缺失)。
"""
import argparse
import asyncio
import getpass
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ble_prov import BleakProvisioner, ProvisionError  # noqa: E402
from mac_ssid import get_ssid  # noqa: E402

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "config.local.json")


def load_config():
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def main():
    ap = argparse.ArgumentParser(
        description="BLE 配网: 把 Mac 当前 Wi-Fi 凭据推到 AI Passport 设备")
    ap.add_argument("--ssid", default=None,
                    help="目标 Wi-Fi SSID(默认自动识别当前网络)")
    ap.add_argument("--pass", dest="password", default=None,
                    help="Wi-Fi 密码(不提供则交互输入; 建议 getpass 而非命令行)")
    ap.add_argument("--device", default=None,
                    help="设备 BLE 地址(跳过扫描)")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="等待配网结果秒数(默认 60)")
    args = ap.parse_args()

    config = load_config()

    ssid = args.ssid or get_ssid()
    if not ssid:
        print("错误: 无法自动识别当前 Wi-Fi SSID, 请用 --ssid 指定", file=sys.stderr)
        sys.exit(1)

    password = args.password
    if password is None:
        password = getpass.getpass(f"Wi-Fi 密码({ssid}): ")

    print(f"配网: {ssid} -> 设备(BLE)...")
    try:
        result = asyncio.run(BleakProvisioner().provision(
            ssid, password, timeout=args.timeout, device_addr=args.device))
    except ProvisionError as e:
        print(f"配网失败: {e}", file=sys.stderr)
        sys.exit(1)

    if result.get("status") == "ok":
        ip = result.get("ip", "?")
        port = config.get("companion_port", 8765)
        print(f"配网成功! 设备已连上 Wi-Fi, IP = {ip}")
        print(f"设备将通过 mDNS 自动发现 Companion(端口 {port}):")
        print("  在 Mac 运行 `python3 tools/ws_test_server.py --mdns` 即可免配连接")
    else:
        code = result.get("code", "?")
        print(f"配网失败: {result.get('detail') or result.get('status')} "
              f"(code={code})")
        print("排查: 密码是否正确? 路由器是否开 2.4GHz? 可重跑 provision.py")
        sys.exit(1)


if __name__ == "__main__":
    main()
