#!/usr/bin/env python3
"""mDNS 发布: 把本机 Companion 服务广播为 _ai-passport._tcp(设备自动发现)。

设备只做解析器; 发布方是 Mac。端口取 config companion_port(默认 8765)。
上下文管理器用法(注册/注销须在事件循环外):
    with CompanionService(port=8765):
        asyncio.run(...)   # 此期间设备可发现本服务

注意: zeroconf 同步 API 不能在 asyncio 事件循环线程内调用(EventLoopBlocked),
所以本模块是同步 contextmanager, 不要放进协程里用。
"""
import argparse
import socket
import subprocess
import time
from contextlib import contextmanager

SERVICE_TYPE = "_ai-passport._tcp.local."
SERVICE_NAME = "AI Passport Companion._ai-passport._tcp.local."


def local_ipv4() -> str:
    """取本机局域网 IPv4: 优先 Wi-Fi 接口(en0/en1), 回退 UDP 默认路由探测。"""
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
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


@contextmanager
def CompanionService(port: int = 8765):
    """注册 _ai-passport._tcp(端口 port, TXT proto=1), 退出时注销。"""
    from zeroconf import ServiceInfo, Zeroconf
    ip = local_ipv4()
    info = ServiceInfo(
        SERVICE_TYPE,
        SERVICE_NAME,
        addresses=[socket.inet_aton(ip)],
        port=port,
        properties={"proto": "1"},
        server="ai-passport-companion.local.",
    )
    zc = Zeroconf()
    zc.register_service(info)
    print(f"mDNS 已发布 _ai-passport._tcp -> ws://{ip}:{port}")
    try:
        yield info
    finally:
        zc.unregister_service(info)
        zc.close()
        print("mDNS 已注销")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="mDNS 发布 _ai-passport._tcp, 供设备自动发现(验证用)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--duration", type=float, default=10.0,
                    help="注册持续秒数(期间可另开终端跑 dns-sd -B _ai-passport._tcp 验证)")
    args = ap.parse_args()
    with CompanionService(port=args.port):
        print(f"注册 {args.duration}s ... (验证: dns-sd -B _ai-passport._tcp)")
        try:
            time.sleep(args.duration)
        except KeyboardInterrupt:
            pass
