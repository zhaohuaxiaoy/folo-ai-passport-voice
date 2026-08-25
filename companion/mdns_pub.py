#!/usr/bin/env python3
"""mDNS 发布 _ai-passport._tcp: 设备(STA)自动发现本机 WS 服务。

由 companion/relay.py 的 wifi 通道使用(mdns 发布 = 设备侧的"广播地址"。
BLE 通道设备靠广播名扫描; WiFi 通道设备靠 mDNS 查询找到 PC 的 WS server)。

跨平台: local_ipv4() 在 macOS 优先 Wi-Fi 接口(ipconfig getifaddr),
Windows/Linux 回退 UDP 默认路由探测(connect 不发包, 仅触发路由选择,
返回"设备 STA 能连到的地址")。
"""
import socket
import subprocess
import sys


def local_ipv4() -> str:
    """取本机局域网 IPv4(设备 STA 需能连到的地址; 127.* 视为不可用)。

    macOS: 优先 Wi-Fi 接口(en0/en1); 其他平台: UDP 默认路由探测。
    均失败抛 RuntimeError(附排查指引)。
    """
    if sys.platform == "darwin":
        for iface in ("en0", "en1"):
            try:
                out = subprocess.run(["ipconfig", "getifaddr", iface],
                                     capture_output=True, text=True, timeout=3)
                ip = out.stdout.strip()
                if ip and not ip.startswith("127."):
                    return ip
            except (OSError, subprocess.SubprocessError):
                continue
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))     # 不发包: 仅让内核选默认路由接口
        ip = s.getsockname()[0]
        if ip and not ip.startswith("127."):
            return ip
    except OSError:
        pass
    finally:
        s.close()
    raise RuntimeError("无法确定本机局域网 IPv4 地址(请检查网络连接)")


def publish_mdns(port: int):
    """发布 _ai-passport._tcp, 返回 (Zeroconf, ServiceInfo) 供退出时 close。

    必须在事件循环外调用: zeroconf 同步 API 内部提交任务到自己的 loop 并等待,
    若在 running loop 线程内调用会自阻塞(EventLoopBlocked, 见
    tools/ws_test_server.py 注释)。
    """
    try:
        from zeroconf import ServiceInfo, Zeroconf
    except ImportError:
        sys.exit("缺少 zeroconf 库: pip install zeroconf")
    ip = local_ipv4()
    info = ServiceInfo(
        "_ai-passport._tcp.local.",
        "AI Passport Companion._ai-passport._tcp.local.",
        addresses=[socket.inet_aton(ip)],
        port=port,
        properties={"proto": "1"},
        server="ai-passport-companion.local.",
    )
    zc = Zeroconf()
    zc.register_service(info)
    print(f"mDNS 已发布 _ai-passport._tcp -> ws://{ip}:{port}")
    return zc, info
