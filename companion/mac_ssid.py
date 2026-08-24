#!/usr/bin/env python3
"""获取 Mac 当前 Wi-Fi SSID(配网 CLI 用)。

降级链:
1. networksetup -getairportnetwork <iface>(macOS 11~14 系统自带, 最快)
2. system_profiler SPAirPortDataType(macOS 15.x 起 networksetup 仍可用,
   但保留此路作兼容; 新格式输出 Current Network Information: 下一行)
3. 手动输入(交互兜底, 不落盘)

解析函数独立可注入, 便于对真实/伪造输出做单测。
"""
import subprocess
import sys

DEFAULT_IFACE = "en0"


def parse_networksetup_output(text: str) -> str:
    """解析 `networksetup -getairportnetwork` 输出, 返回 SSID 或空串。

    macOS 11~14 格式:
      Current Wi-Fi Network: MySSID
    未关联:
      You are not associated with an AirPort network.
    """
    for line in text.splitlines():
        if "Current Wi-Fi Network:" in line:
            ssid = line.split(":", 1)[1].strip()
            if ssid and "(not associated)" not in ssid:
                return ssid
    return ""


def parse_system_profiler_output(text: str) -> str:
    """解析 `system_profiler SPAirPortDataType` 输出, 返回 SSID 或空串。

    格式:
      Current Network Information:
          MySSID: 6, -54dBm, channel 149, 5 GHz, 802.11ax
    或旧版单行:
      Other Local Wi-Fi Networks:
    """
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if "Current Network Information:" in line:
            # 下一行是 "    <SSID>: <channel>, <rssi>dBm, ..."
            for nxt in lines[i + 1:i + 3]:
                nxt = nxt.strip()
                if nxt and ":" in nxt and "dBm" in nxt:
                    return nxt.split(":", 1)[0].strip()
            return ""
    return ""


def _networksetup_ssid(iface: str) -> str:
    try:
        out = subprocess.run(["networksetup", "-getairportnetwork", iface],
                             capture_output=True, text=True, timeout=5)
        return parse_networksetup_output(out.stdout)
    except (OSError, subprocess.SubprocessError):
        return ""


def _system_profiler_ssid() -> str:
    try:
        out = subprocess.run(["system_profiler", "SPAirPortDataType"],
                             capture_output=True, text=True, timeout=30)
        return parse_system_profiler_output(out.stdout)
    except (OSError, subprocess.SubprocessError):
        return ""


def _manual_ssid() -> str:
    try:
        return input("无法自动识别 Wi-Fi SSID, 请手动输入: ").strip()
    except EOFError:
        return ""


def get_ssid(iface: str = DEFAULT_IFACE) -> str:
    """按降级链取当前 SSID; 全部失败则手动输入(允许在单测中跳过)。

    只返回 SSID 文本 —— 密码绝不经过本模块。
    """
    for fn in (_networksetup_ssid, _system_profiler_ssid, _manual_ssid):
        ssid = fn(iface) if fn is _networksetup_ssid else fn()
        if ssid:
            return ssid
    return ""


if __name__ == "__main__":
    ssid = get_ssid()
    if ssid:
        print(ssid)
    else:
        print("(未连接到 Wi-Fi)", file=sys.stderr)
        sys.exit(1)
