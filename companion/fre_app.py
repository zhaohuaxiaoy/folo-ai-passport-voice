#!/usr/bin/env python3
"""AI Passport 首次运行向导(tkinter 桌面应用)。

普通用户首次使用零命令行: 启动 → 自动发现设备 → 点击连接 → 授权引导
(Mac) → 状态页显示运行就绪。config.local.json 自动生成(白名单字段),
密钥等已有字段保留。

架构: tkinter 主线程 + 后台 asyncio 线程(跑 relay 全流程), UI 更新经
thread-safe queue + root.after 轮询(不跨线程碰控件)。

用法:
  python3 companion/fre_app.py            # 正常启动
  python3 companion/fre_app.py --dry-run  # 不真实连接, 用内置 fake 链路走通流程
"""
import argparse
import asyncio
import os
import queue
import sys
import threading
import tkinter as tk
from tkinter import font as tkfont

import fre_state


class _FakeTransport:
    """dry-run 专用: 与 relay 的 transport 同接口, 扫描固定地址, 连接即成功。"""

    def __init__(self):
        self._handlers = {}

    async def scan_for_device(self, name, timeout):
        return "AA:BB:CC:DD:EE:FF"

    async def connect(self, address, on_disconnect=None):
        pass

    async def start_notify(self, uuid, handler):
        self._handlers[uuid] = handler

    async def write_gatt_char(self, uuid, data, response=False):
        pass

    async def disconnect(self):
        pass


class _FakeInjector:
    """dry-run 专用: 注入后端空实现(不碰剪贴板/键盘)。"""

    def __call__(self, text):
        print(f"[fre:dry-run] 注入 {len(text)} 字符")


class FREApp:
    """tkinter 向导窗口。"""

    PAGES = ("found", "connecting", "permission", "status")

    def __init__(self, root, dry_run=False):
        self.root = root
        self.dry_run = dry_run
        self.phase_q = queue.Queue()     # 后台线程 → UI 的 phase 事件
        self.relay_task = None           # 后台 relay task
        self._loop = None                # 后台 asyncio 事件循环
        self._thread = None              # 后台 asyncio 线程
        self.device_addr = None
        self.last_error = ""

        self.cfg = fre_state.load_or_default_cfg()
        self.channel = self.cfg.get("channel", "ble")

        root.title("AI Passport")
        root.geometry("420x560")
        root.minsize(380, 480)
        self._build_ui()
        self.show_page("found")
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        # 自动开始扫描
        self.start_scan()

    # ---------------- UI 构建 ----------------

    def _build_ui(self):
        self._font_title = tkfont.Font(size=18, weight="bold")
        self._font_body = tkfont.Font(size=13)
        self._font_hint = tkfont.Font(size=11)

        self._pages = {}
        for name in self.PAGES:
            self._pages[name] = tk.Frame(self.root, padx=24, pady=24)
        self._build_found_page()
        self._build_connecting_page()
        self._build_permission_page()
        self._build_status_page()

        self._status_var = tk.StringVar(value="")
        tk.Label(self.root, textvariable=self._status_var,
                 font=self._font_hint, fg="#666",
                 anchor="w", padx=16).pack(side="bottom", fill="x")

    def _build_found_page(self):
        p = self._pages["found"]
        tk.Label(p, text="发现 AI Passport", font=self._font_title).pack(pady=(0, 8))
        tk.Label(p, text="确保设备已开机并处于广播状态", font=self._font_hint,
                 fg="#666").pack()
        self._device_var = tk.StringVar(value="正在扫描…")
        tk.Label(p, textvariable=self._device_var, font=self._font_body,
                 pady=12).pack()
        self._connect_btn = tk.Button(
            p, text="连接", font=self._font_body, width=16, height=2,
            command=self.connect, state="disabled")
        self._connect_btn.pack(pady=8)
        tk.Button(p, text="重新扫描", font=self._font_hint, command=self.start_scan
                  ).pack()
        self._channel_var = tk.StringVar(value=self.channel)
        tk.Frame(p).pack(pady=8)
        tk.Label(p, text="连接通道", font=self._font_hint, fg="#666").pack()
        for ch, label in (("ble", "蓝牙 BLE"), ("wifi", "WiFi"), ("usb", "USB 有线")):
            tk.Radiobutton(p, text=label, value=ch, variable=self._channel_var,
                           font=self._font_hint, command=self.on_channel_change
                           ).pack(anchor="center")

    def _build_connecting_page(self):
        p = self._pages["connecting"]
        tk.Label(p, text="正在连接…", font=self._font_title).pack(pady=(0, 12))
        self._connecting_var = tk.StringVar(value="")
        tk.Label(p, textvariable=self._connecting_var,
                 font=self._font_body).pack()

    def _build_permission_page(self):
        p = self._pages["permission"]
        tk.Label(p, text="需要授权", font=self._font_title).pack(pady=(0, 8))
        tk.Label(p, text="以下权限未开启, 语音与文字注入将无法工作:",
                 font=self._font_body, justify="left").pack(anchor="w")
        self._perm_var = tk.StringVar(value="")
        tk.Label(p, textvariable=self._perm_var, font=self._font_body,
                 fg="#b00", pady=8, justify="left").pack(anchor="w")
        tk.Button(p, text="打开系统设置", font=self._font_body,
                  command=lambda: self.open_permission()).pack(pady=6)
        tk.Button(p, text="我已授权, 继续", font=self._font_body,
                  command=self.continue_after_permission).pack(pady=6)

    def _build_status_page(self):
        p = self._pages["status"]
        tk.Label(p, text="✅ AI Passport 已连接", font=self._font_title
                 ).pack(pady=(0, 16))
        rows = (
            ("输入方式", "按住 ● 讲话 · 松开发送\n双击 OK 清空 · DOWN 回车"),
            ("连接", None),
            ("ASR", "Volcano"),
            ("Injection", None),
        )
        self._row_vars = {}
        for label, static in rows:
            row = tk.Frame(p)
            row.pack(fill="x", pady=4)
            tk.Label(row, text=label, font=self._font_body, width=10,
                     anchor="w").pack(side="left")
            v = tk.StringVar(value=static or "")
            self._row_vars[label] = v
            tk.Label(row, textvariable=v, font=self._font_body,
                     fg="#0a0").pack(side="left")
        tk.Button(p, text="重新设置", font=self._font_hint,
                  command=self.on_reset).pack(pady=16)

    # ---------------- 页面切换 ----------------

    def show_page(self, name):
        self.current_page = name          # UI 逻辑可测性(冒烟/断言用)
        for n, frame in self._pages.items():
            frame.pack_forget()
        self._pages[name].pack(fill="both", expand=True)

    def set_status(self, text):
        self._status_var.set(text)

    # ---------------- 扫描 ----------------

    def start_scan(self):
        self.show_page("found")
        self._device_var.set("正在扫描…")
        self._connect_btn.config(state="disabled")
        threading.Thread(target=self._scan_worker, daemon=True).start()

    def _scan_worker(self):
        """后台线程执行扫描(bleak 阻塞式), 结果经队列回 UI。"""
        if self.dry_run:
            result = ("AA:BB:CC:DD:EE:FF", "")
        else:
            try:
                from relay import BleakTransport
                addr = BleakTransport().scan_for_device("AI Passport", 5.0)
                result = (addr,)
            except Exception as e:
                result = (None, f"扫描失败: {e}")
        self.phase_q.put(("scan_result", result))

    def _on_scan_result(self, result):
        addr, err = result
        if addr:
            self.device_addr = addr
            self._device_var.set(f"AI Passport\n{addr}")
            self._connect_btn.config(state="normal")
            self.set_status("发现设备, 点击连接")
        else:
            self.device_addr = None
            self._device_var.set("未发现设备")
            self._connect_btn.config(state="disabled")
            self.set_status(err or "确认设备已开机并处于广播状态")

    # ---------------- 连接 ----------------

    def connect(self):
        self.last_error = ""
        # Mac 授权检查: 有缺失项 → 先进授权页
        missing = [name for name, st in fre_state.mac_permission_status()
                   if st == "missing"]
        if missing and not self.dry_run:
            self._perm_var.set("\n".join(
                "• 辅助功能" if m == "accessibility" else "• 蓝牙"
                for m in missing))
            self.show_page("permission")
            return
        self.show_page("connecting")
        self._connecting_var.set(f"连接通道: {self.channel}")

        self._thread = threading.Thread(target=self._relay_worker, daemon=True)
        self._thread.start()
        self.root.after(100, self.poll)

    def open_permission(self):
        for name, st in fre_state.mac_permission_status():
            if st == "missing":
                fre_state.open_system_settings(name)

    def continue_after_permission(self):
        self.connect()

    def _relay_worker(self):
        """后台 asyncio 线程: 跑 relay 全流程, phase 经队列回 UI。"""
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self._loop = loop
        try:
            from relay import Relay
            from relay import _build_transport
            from relay import _default_inject_fn, _default_key_action_fn

            if self.dry_run:
                transport = _FakeTransport()
                inject_fn = _FakeInjector()
                key_action_fn = lambda action: None  # noqa: E731
            else:
                # BLE 用 Relay 默认 transport; WiFi/USB 按 config 选传输层
                transport = (None if self.channel == "ble"
                             else _build_transport(self.cfg))
                inject_fn = _default_inject_fn(self.cfg)
                key_action_fn = _default_key_action_fn(self.cfg)

            relay = Relay(transport,
                          inject_fn=inject_fn, key_action_fn=key_action_fn,
                          on_phase=lambda ph: self.phase_q.put(("phase", ph)))
            # BLE 直接连已发现地址; WiFi/USB 由 relay 内部扫描/等待
            self.relay_task = loop.create_task(
                relay.run(self.device_addr if self.channel == "ble" else None))
            loop.run_until_complete(self.relay_task)
        except asyncio.CancelledError:
            pass   # 向导关闭/重新设置时的正常收束
        except Exception as e:
            self.phase_q.put(("error", str(e)))
            self.phase_q.put(("phase", "failed"))
        finally:
            self.phase_q.put(("relay_done", None))

    def poll(self):
        """UI 线程轮询后台事件(每 100ms)。"""
        try:
            while True:
                kind, payload = self.phase_q.get_nowait()
                if kind == "scan_result":
                    self._on_scan_result(payload)
                elif kind == "phase":
                    self._on_phase(payload)
                elif kind == "error":
                    self.last_error = payload
                elif kind == "relay_done":
                    self.set_status(self.last_error or "连接已断开")
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def _on_phase(self, phase):
        if phase == "connected":
            self.channel = self.cfg.get("channel", self.channel)
            self._row_vars["连接"].set(self.channel.upper())
            mode = self.cfg.get("inject_mode", "auto")
            self._row_vars["Injection"].set(
                {"unicode": "Unicode 键盘注入", "clipboard": "剪贴板",
                 "auto": "Auto (unicode 优先)"}.get(mode, mode))
            key = self.cfg.get("volcano_api_key", "")
            if key:
                self._row_vars["ASR"].set("Volcano")
            else:
                self._row_vars["ASR"].set("未配置 (设置 config)")
            self.show_page("status")
            self.set_status(f"已连接 {self.channel.upper()}, 按住 ● 讲话")
        elif phase == "disconnected":
            self.show_page("found")
            self.set_status("连接已断开")
        elif phase == "failed":
            self.show_page("found")
            self.set_status(self.last_error or "连接失败")

    def on_channel_change(self):
        self.channel = self._channel_var.get()

    def on_reset(self):
        if self._loop is not None and self.relay_task is not None:
            self._loop.call_soon_threadsafe(self.relay_task.cancel)
        self.show_page("found")
        self.set_status("")
        self.start_scan()

    def on_close(self):
        """收束后台 relay(取消 task + 等线程退出), 无残留。"""
        if self._loop is not None and self.relay_task is not None:
            self._loop.call_soon_threadsafe(self.relay_task.cancel)
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=5)
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description="AI Passport 首次运行向导")
    ap.add_argument("--dry-run", action="store_true",
                    help="不真实连接设备, 用内置 fake 链路走通流程")
    args = ap.parse_args()
    root = tk.Tk()
    FREApp(root, dry_run=args.dry_run)
    root.mainloop()


if __name__ == "__main__":
    main()
