#!/usr/bin/env python3
"""mac_ssid 单测: 喂真实/伪造 networksetup / system_profiler 输出断言解析。

运行: python3 companion/tests/test_mac_ssid.py  (无需 pytest)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mac_ssid import parse_networksetup_output, parse_system_profiler_output

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


# ---- networksetup(macOS 11~14) ----

check("networksetup 正常输出",
      parse_networksetup_output("Current Wi-Fi Network: MyHomeNet\n"),
      "MyHomeNet")
check("networksetup 中文 SSID",
      parse_networksetup_output("Current Wi-Fi Network: 我家WiFi\n"),
      "我家WiFi")
check("networksetup 未关联(英文)",
      parse_networksetup_output(
          "You are not associated with an AirPort network.\n"),
      "")
check("networksetup 未关联(macOS 14+ 提示)",
      parse_networksetup_output(
          "You are not associated with an AirPort network.\n"
          "(If you wish to turn Wi-Fi on, run \"networksetup -setairportpower en0 on\".)\n"),
      "")
check("networksetup 空输出",
      parse_networksetup_output(""),
      "")
check("networksetup 不含关键字",
      parse_networksetup_output("Hardware Port: Wi-Fi, Device: en0\n"),
      "")

# ---- system_profiler(macOS 15.x 兼容路) ----

SP_REAL = """Software Versions:
    CoreWLAN: 17.0.0

    Current Network Information:
        FoloToy: 6, -54dBm, channel 149, 5 GHz, 802.11ax
"""
check("system_profiler 新版格式",
      parse_system_profiler_output(SP_REAL),
      "FoloToy")
check("system_profiler 中文 SSID",
      parse_system_profiler_output(
          "    Current Network Information:\n"
          "        小区WiFi: 44, -61dBm, channel 6\n"),
      "小区WiFi")
check("system_profiler 未连接(无 Current Network Information)",
      parse_system_profiler_output(
          "    802.11ac: 5GHz SSID List: None\n"
          "    Other Local Wi-Fi Networks:\n"
          "        Neighbor1: 6, -70dBm, channel 1\n"),
      "")
check("system_profiler 空输出",
      parse_system_profiler_output(""),
      "")


if __name__ == "__main__":
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")
