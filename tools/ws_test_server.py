#!/usr/bin/env python3
"""AI Passport 开发用模拟 Companion。

监听 WebSocket,打印设备发来的 JSON 行与音频帧统计,并按模式回发
transcript / approval_request / agent.status / mac.metrics。

用法:
  python3 tools/ws_test_server.py                          # 只打印
  python3 tools/ws_test_server.py --transcript 'hello 123'
  python3 tools/ws_test_server.py --loop-transcript 'Hello from AI Passport 123'
  python3 tools/ws_test_server.py --approval               # 全链路:转写→审批请求
  python3 tools/ws_test_server.py --paste '中文内容'        # 注入方式=paste
  python3 tools/ws_test_server.py --mdns                   # 同时发布 _ai-passport._tcp(设备自动发现)

依赖: pip install websockets  (--mdns 需 pip install zeroconf)
"""
import argparse
import asyncio
import json
import socket
import subprocess
import sys

try:
    import websockets
except ImportError:
    sys.exit("缺少 websockets 库: pip install websockets")

# 设备 WS 目标默认 ws://192.168.1.100:8765 —— 用 `ws set <url>` 改到本机 IP。
HELLO_SEEN = {"n": 0}
AUDIO = {"bytes": 0, "frames": 0, "voice_open": False}


async def tx(ws, obj: dict):
    """下行 JSON 行。设备侧按 '\\n' 切行(ws_client.c feed_rx),缺换行会导致整行
    积在累积器里直到下一条 —— 早期 ws_test_server 漏了换行,是设备解析 bug 的源头。"""
    await ws.send(json.dumps(obj, ensure_ascii=False) + "\n")


def on_line(line: str, mode: dict):
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        print(f"[json] <== (畸形) {line[:100]}")
        return
    ev = msg.get("event") or msg.get("type")
    print(f"[{ev}] <== {line[:200]}")

    if ev == "device.hello":
        HELLO_SEEN["n"] += 1
    elif ev == "voice.start":
        AUDIO["voice_open"] = True
        AUDIO["bytes"] = 0
        AUDIO["frames"] = 0
    elif ev == "voice.end":
        AUDIO["voice_open"] = False
        dur_s = AUDIO["frames"] * 0.1
        print(f"[audio] 共 {AUDIO['frames']} 帧 / {AUDIO['bytes']} B "
              f"(约 {dur_s:.1f}s)")
        asyncio.create_task(reply_after_voice_end(mode))
    elif ev == "agent.action":
        print(f"[action] 用户决策: {msg.get('action')} (taskId={msg.get('taskId')})")


async def reply_after_voice_end(mode: dict):
    """voice.end 后的 Mac 端回包序列。"""
    ws = mode["ws"]
    mode["replies_done"] += 1

    # 1) 转写结果(注入方式可配)
    if mode["transcript"] or mode["paste"]:
        text = mode["transcript"] or mode["paste"]
        inject = "paste" if mode["paste"] else "type"
        await tx(ws, {"type": "transcript", "text": text, "inject_mode": inject})
        print(f"[transcript] ==> {inject}: {text}")

    # 2) 审批请求(--approval)
    if mode["approval"]:
        await asyncio.sleep(1.0)
        await tx(ws, {
            "type": "agent.approval_request",
            "taskId": "task-001",
            "title": "Deploy to production",
            "target": "api.example.com",
            "diffSummary": "+12 -3 in deploy.sh",
            "riskLevel": "high",
        })
        print("[approval] ==> 已发审批请求,等物理按键决策")

    # 3) agent.status 收尾:done(不带审批时)或 running(审批后由按键动作推进)
    await asyncio.sleep(1.5)
    state = "running" if mode["approval"] else "done"
    await tx(ws, {
        "type": "agent.status", "state": state,
        "message": "Agent finished the task" if state == "done"
                   else "Agent is working after approval",
    })
    print(f"[agent.status] ==> {state}")


async def metrics_loop(ws, period: float):
    """周期性状态栏数据:mac.metrics。"""
    i = 0
    while True:
        await tx(ws, {
            "type": "mac.metrics",
            "cpu": 20 + (i * 7) % 40,
            "ram": 55 + (i * 3) % 30,
            "battery": 80,
            "charging": False,
            "activeApp": "VS Code",
        })
        await asyncio.sleep(period)
        i += 1


async def handle(ws, mode: dict):
    mode["ws"] = ws
    metrics_task = asyncio.create_task(metrics_loop(ws, 2.0))
    print("== 设备已连接 ==")
    try:
        async for raw in ws:
            if isinstance(raw, str):
                on_line(raw, mode)
            else:
                AUDIO["bytes"] += len(raw)
                if AUDIO["voice_open"] and len(raw) == 3200:
                    AUDIO["frames"] += 1
                else:
                    print(f"[audio] <== 二进制 {len(raw)} B "
                          f"(voice_open={AUDIO['voice_open']})")
    finally:
        metrics_task.cancel()
        print("== 设备断开 ==")


def local_ipv4() -> str:
    """取本机局域网 IPv4:优先 Wi-Fi 接口(en0/en1),回退 UDP 默认路由探测。"""
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


def publish_mdns(port: int):
    """发布 _ai-passport._tcp,让设备 mDNS 自动发现本 WS 服务。

    必须在事件循环外调用:zeroconf 同步 API 内部提交任务到自己的 loop 并等待,
    若在 running loop 线程内调用会自阻塞(EventLoopBlocked)。
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


def main():
    ap = argparse.ArgumentParser(description="AI Passport 模拟 Companion")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--transcript", default="", help="voice.end 后回发转写文本(type)")
    ap.add_argument("--loop-transcript", default="",
                    help="每轮 voice.end 都回发该文本(打字演示)")
    ap.add_argument("--paste", default="", help="回发转写(inject_mode=paste)")
    ap.add_argument("--approval", action="store_true", help="转写后发审批请求")
    ap.add_argument("--mdns", action="store_true",
                    help="发布 _ai-passport._tcp(设备自动发现,需 zeroconf)")
    args = ap.parse_args()

    mode = {
        "transcript": args.loop_transcript or args.transcript,
        "paste": args.paste,
        "approval": args.approval,
        "replies_done": 0,
        "ws": None,
    }

    print(f"监听 ws://0.0.0.0:{args.port}  (Ctrl+C 退出)")
    if mode["transcript"]:
        print(f"voice.end 后回发 transcript(type): {mode['transcript']}")
    if mode["paste"]:
        print(f"voice.end 后回发 transcript(paste): {mode['paste']}")
    if mode["approval"]:
        print("voice.end 后还会发 agent.approval_request")
    print("设备侧配网后建议开 auto:`ws set auto`,即可被 mDNS 自动发现" if args.mdns
          else "设备侧先配 Wi-Fi:`wifi set <ssid> <pass>`,再 `ws set ws://<本机IP>:%d`" % args.port)

    async def serve(websocket, path=None):
        await handle(websocket, mode)

    # mDNS 注册放事件循环外(zeroconf 同步 API 不能在 running loop 线程内调用)
    zc = info = None
    if args.mdns:
        zc, info = publish_mdns(args.port)

    # websockets 17 的 serve() 是同步工厂,必须在事件循环内调用(否则 no running loop)
    async def run_server():
        try:
            async with websockets.serve(serve, "0.0.0.0", args.port):
                await asyncio.Future()   # 常驻,直到 Ctrl+C
        finally:
            pass   # 注销在循环外做(见下)

    try:
        asyncio.run(run_server())
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        if zc:   # 循环已关闭,可安全注销
            zc.unregister_service(info)
            zc.close()


if __name__ == "__main__":
    main()
