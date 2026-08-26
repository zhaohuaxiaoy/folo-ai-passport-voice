#!/usr/bin/env python3
"""AI Passport 安装/首次配置/设备诊断向导(tkinter + ttkbootstrap)。

5 步流程: 欢迎 → 发现设备(BLE→WiFi→USB 自动探测, 可手动) → ASR 配置
(火山 Key + 真实测试连接) → 输入注入权限(macOS) → 完成(状态页)。
进阶: 状态页 Advanced 进入诊断页(见 fre_app.py 诊断节)。

架构: tkinter 主线程 + 后台 asyncio 线程(跑 relay 全流程), UI 更新经
thread-safe queue + root.after 轮询(不跨线程碰控件)。GUI 只是壳,
核心业务在 relay / asr_client / probe 等模块, 全部可单测、CLI 可跑。

用法:
  python3 companion/fre_app.py            # 正常启动
  python3 companion/fre_app.py --dry-run  # 不真实连接, 内置 fake 链路走通流程
  python3 companion/fre_app.py --no-tray  # 连接成功后不启动系统托盘
"""
import argparse
import asyncio
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import font as tkfont
from tkinter import messagebox

import ttkbootstrap as ttk

import fre_state

THEME = "litera"

CHANNEL_LABELS = (("ble", "蓝牙 BLE"), ("wifi", "WiFi"), ("usb", "USB 有线"))
INJECT_LABELS = {"unicode": "Unicode 键盘注入", "clipboard": "剪贴板",
                 "auto": "Auto (unicode 优先)"}

# 诊断页快捷命令(设备 console 命令表, 与 main/console_cmds.c 对齐):
# (按钮名, 命令, 需确认?)  前两项需确认: 恢复出厂 / 重启
DIAG_QUICK = (
    ("Mode", "mode", False),
    ("WiFi", "wifi status", False),
    ("WebSocket", "ws status", False),
    ("mDNS", "mdns", False),
    ("Logs", "log", False),
    ("System", "st", False),
    ("Factory Reset", "factory", True),
    ("reboot", "reboot", True),
)


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

    async def send_syscmd(self, line):
        """伪装 SerialTransport: dry-run 下诊断命令有假响应。"""
        return f"[fake] {line}"


class _FakeInjector:
    """dry-run 专用: 注入后端空实现(不碰剪贴板/键盘)。"""

    def __call__(self, text):
        print(f"[fre:dry-run] 注入 {len(text)} 字符")


class FREApp:
    """tkinter 向导窗口。页面名与测试/冒烟断言耦合。"""

    PAGES = ("welcome", "discover", "asr_config", "permission", "connecting",
             "status", "diagnostics")

    def __init__(self, root, dry_run=False, no_tray=False):
        self.root = root
        self.dry_run = dry_run
        self.no_tray = no_tray
        self.phase_q = queue.Queue()     # 后台线程 → UI 的事件
        self.relay_task = None           # 后台 relay task
        self._loop = None                # 后台 asyncio 事件循环
        self._thread = None              # 后台 asyncio 线程
        self.device_addr = None
        self.probe_result = None         # ("ble"|"wifi"|"usb", addr)
        self.last_error = ""
        self._tray = None                # pystray Icon(连接成功后启动)
        self._tray_state = {"connected": False, "device": "", "listening": False}

        self.cfg = fre_state.load_or_default_cfg()
        self.channel = self.cfg.get("channel", "ble")

        root.title("AI Passport")
        root.geometry("440x600")
        root.minsize(400, 520)
        self._build_ui()
        self.show_page("welcome")
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(100, self.poll)

    # ---------------- UI 构建 ----------------

    def _build_ui(self):
        self._font_title = tkfont.Font(size=18, weight="bold")
        self._font_body = tkfont.Font(size=13)
        self._font_hint = tkfont.Font(size=11)

        self._pages = {}
        for name in self.PAGES:
            self._pages[name] = ttk.Frame(self.root, padding=24)
        self._build_welcome_page()
        self._build_discover_page()
        self._build_asr_config_page()
        self._build_permission_page()
        self._build_connecting_page()
        self._build_status_page()
        self._build_diagnostics_page()

        self._status_var = tk.StringVar(value="")
        ttk.Label(self.root, textvariable=self._status_var,
                  font=self._font_hint, bootstyle="secondary",
                  anchor="w", padding=12).pack(side="bottom", fill="x")

    def _page(self, name):
        return self._pages[name]

    def _build_welcome_page(self):
        p = self._page("welcome")
        ttk.Label(p, text="欢迎使用 AI Passport",
                  font=self._font_title).pack(pady=(16, 10))
        ttk.Label(p, text=(
            "一个会听话的可穿戴 AI 硬件。\n"
            "本向导将完成: 发现设备 → 语音识别配置 → 授权 → 连接。"),
            font=self._font_body, justify="center").pack(pady=(0, 12))
        ttk.Label(p, text=(
            "准备: 设备已开机;\n"
            "如需 USB 连接, 请先用数据线接上电脑。"),
            font=self._font_hint, bootstyle="secondary",
            justify="center").pack(pady=(0, 24))
        ttk.Button(p, text="开始", bootstyle="primary",
                   command=self.start_discover).pack(pady=8)

    def _build_discover_page(self):
        p = self._page("discover")
        ttk.Label(p, text="发现设备", font=self._font_title).pack(pady=(0, 8))
        self._probe_var = tk.StringVar(value="")
        ttk.Label(p, textvariable=self._probe_var, font=self._font_hint,
                  bootstyle="secondary", wraplength=360).pack()
        self._device_var = tk.StringVar(value="尚未探测")
        ttk.Label(p, textvariable=self._device_var, font=self._font_body,
                  padding=(0, 10)).pack()

        ttk.Label(p, text="连接通道", font=self._font_hint,
                  bootstyle="secondary", padding=(0, 8)).pack()
        self._channel_var = tk.StringVar(value=self.channel)
        for ch, label in CHANNEL_LABELS:
            ttk.Radiobutton(p, text=label, value=ch, variable=self._channel_var,
                            command=self.on_channel_change).pack(anchor="w")
        ttk.Button(p, text="重新探测", bootstyle="secondary",
                   command=self.start_discover).pack(pady=(14, 4))
        ttk.Button(p, text="下一步", bootstyle="primary",
                   command=self.on_discover_next).pack(pady=4)

    def _build_asr_config_page(self):
        p = self._page("asr_config")
        ttk.Label(p, text="语音识别配置", font=self._font_title).pack(pady=(0, 8))
        ttk.Label(p, text=(
            "输入火山引擎豆包语音识别的 API Key。\n"
            "Key 仅保存在本机配置文件(config.local.json), 不会上传或写入日志。"),
            font=self._font_hint, bootstyle="secondary",
            justify="left", wraplength=380).pack(anchor="w", pady=(0, 10))
        self._asr_key_var = tk.StringVar(
            value=self.cfg.get("volcano_api_key", ""))
        ttk.Entry(p, textvariable=self._asr_key_var, show="*",
                  font=self._font_body).pack(fill="x", pady=(0, 8))
        ttk.Button(p, text="测试连接", bootstyle="info",
                   command=self.test_asr_connection).pack(anchor="w")
        self._asr_test_var = tk.StringVar(value="")
        ttk.Label(p, textvariable=self._asr_test_var, font=self._font_hint,
                  bootstyle="secondary", wraplength=380, justify="left",
                  padding=(0, 8)).pack(anchor="w")
        ttk.Button(p, text="下一步", bootstyle="primary",
                   command=self.on_asr_next).pack(pady=(12, 4))

    def _build_connecting_page(self):
        p = self._page("connecting")
        ttk.Label(p, text="正在连接…", font=self._font_title).pack(pady=(0, 12))
        self._connecting_var = tk.StringVar(value="")
        ttk.Label(p, textvariable=self._connecting_var,
                  font=self._font_body).pack()

    def _build_permission_page(self):
        p = self._page("permission")
        ttk.Label(p, text="需要授权", font=self._font_title).pack(pady=(0, 8))
        ttk.Label(p, text="以下权限未开启, 语音与文字注入将无法工作:",
                  font=self._font_body, justify="left").pack(anchor="w")
        self._perm_var = tk.StringVar(value="")
        ttk.Label(p, textvariable=self._perm_var, font=self._font_body,
                  bootstyle="danger", padding=(0, 8),
                  justify="left").pack(anchor="w")
        ttk.Button(p, text="打开系统设置", bootstyle="secondary",
                   command=self.open_permission).pack(pady=6)
        ttk.Button(p, text="我已授权, 继续", bootstyle="primary",
                   command=self.continue_after_permission).pack(pady=6)

    def _build_status_page(self):
        p = self._page("status")
        ttk.Label(p, text="✅ AI Passport 已连接", font=self._font_title
                  ).pack(pady=(0, 16))
        rows = (
            ("输入方式", "按住 ● 讲话 · 松开发送\n双击 OK 清空 · DOWN 回车"),
            ("连接", None),
            ("ASR", "Volcano"),
            ("Injection", None),
        )
        self._row_vars = {}
        for label, static in rows:
            row = ttk.Frame(p)
            row.pack(fill="x", pady=4)
            ttk.Label(row, text=label, font=self._font_body, width=10,
                      anchor="w").pack(side="left")
            v = tk.StringVar(value=static or "")
            self._row_vars[label] = v
            ttk.Label(row, textvariable=v, font=self._font_body,
                      bootstyle="success").pack(side="left")
        self._diag_btn = ttk.Button(p, text="Advanced 诊断", bootstyle="info",
                                    command=self.show_diagnostics)
        self._diag_btn.pack(pady=(4, 4))
        ttk.Button(p, text="重新设置", bootstyle="secondary",
                   command=self.on_reset).pack(pady=4)

    def _build_diagnostics_page(self):
        p = self._page("diagnostics")
        ttk.Label(p, text="设备诊断 (Advanced)", font=self._font_title
                  ).pack(pady=(0, 8))
        self._diag_hint = tk.StringVar(value="")
        ttk.Label(p, textvariable=self._diag_hint, font=self._font_hint,
                  bootstyle="secondary", wraplength=380, justify="left",
                  padding=(0, 6)).pack(anchor="w")
        btns = ttk.Frame(p)
        btns.pack(fill="x", pady=6)
        for i, (label, cmd, needs_conf) in enumerate(DIAG_QUICK):
            ttk.Button(btns, text=label, bootstyle="info-outline",
                       command=lambda c=cmd, n=needs_conf:
                           self.diag_cmd(c, confirm=n)
                       ).grid(row=i // 4, column=i % 4, padx=2, pady=2,
                              sticky="ew")
        for col in range(4):
            btns.columnconfigure(col, weight=1)
        self._diag_entry_var = tk.StringVar()
        ttk.Entry(p, textvariable=self._diag_entry_var,
                  font=self._font_body).pack(fill="x", pady=(4, 2))
        ttk.Button(p, text="执行", bootstyle="primary",
                   command=self.diag_run_entry).pack(anchor="e")
        self._diag_out = tk.Text(p, height=10, font=tkfont.Font(
            family="Menlo", size=11), state="disabled")
        sb = ttk.Scrollbar(p, orient="vertical", command=self._diag_out.yview)
        self._diag_out.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self._diag_out.pack(fill="both", expand=True, pady=(6, 0))
        ttk.Button(p, text="返回", bootstyle="secondary",
                   command=lambda: self.show_page("status")).pack(pady=6)

    # ---------------- 页面切换 ----------------

    def show_page(self, name):
        self.current_page = name          # UI 逻辑可测性(冒烟/断言用)
        for n, frame in self._pages.items():
            frame.pack_forget()
        self._pages[name].pack(fill="both", expand=True)

    def set_status(self, text):
        self._status_var.set(text)

    # ---------------- 探测 ----------------

    def start_discover(self):
        self.show_page("discover")
        self._probe_var.set("正在探测…")
        self._device_var.set("尚未探测")
        self.probe_result = None
        threading.Thread(target=self._probe_worker, daemon=True).start()

    def _probe_worker(self):
        """后台线程执行通道探测(BLE→WiFi→USB), 结果经队列回 UI。"""
        if self.dry_run:
            time.sleep(0.3)   # 模拟探测耗时
            result = ("ble", "AA:BB:CC:DD:EE:FF")
        else:
            try:
                from probe import probe_channels
                result = asyncio.run(probe_channels(
                    self.cfg,
                    on_status=lambda m: self.phase_q.put(("probe_progress", m))))
            except Exception as e:  # noqa: BLE001
                result = (None, None)
                self.phase_q.put(("probe_progress", f"探测异常: {e}"))
        self.phase_q.put(("probe_result", result))

    def _on_probe_result(self, result):
        channel, addr = result
        self.probe_result = result
        if channel:
            # 自动探测命中 → 推荐该通道(用户仍可手动改)
            self._channel_var.set(channel)
            self.channel = channel
            name = dict(CHANNEL_LABELS).get(channel, channel)
            self._device_var.set(f"{name}\n{addr}")
            if channel == "ble":
                self.device_addr = addr
            self.set_status(f"发现设备 ({name}), 可下一步")
        else:
            self.device_addr = None
            self._device_var.set("未发现设备")
            self.set_status("未发现设备, 可手动选择通道后下一步")

    # ---------------- ASR 配置 ----------------

    def test_asr_connection(self):
        self._asr_test_var.set("正在测试…")
        threading.Thread(target=self._test_asr_worker, daemon=True).start()

    def _test_asr_worker(self):
        """后台线程: 零音频握手验证 Key(不阻塞 UI)。"""
        key = self._asr_key_var.get().strip()
        if self.dry_run:
            time.sleep(0.2)
            result = ("ok", "连接成功 (dry-run)")
        elif not key:
            result = ("fail", "请先输入 API Key")
        else:
            try:
                from asr_client import asr_test_connection
                cfg = {**self.cfg, "volcano_api_key": key}
                asyncio.run(asr_test_connection(cfg, timeout=8))
                result = ("ok", "连接成功, Key 有效")
            except Exception as e:  # noqa: BLE001
                result = ("fail", str(e))
        self.phase_q.put(("asr_test", result))

    def _on_asr_test(self, result):
        ok, msg = result
        self._asr_test_var.set(("✓ " if ok else "✗ ") + msg)

    # ---------------- 流程推进 ----------------

    def on_discover_next(self):
        self.channel = self._channel_var.get()
        # 同步内存 cfg(审查 P1-1): _relay_worker 的 _build_transport 读
        # self.cfg —— 只写盘不同步则 BLE→WiFi 切换仍按旧 channel 建传输层,
        # 向导里切换通道不生效(连接必然超时)。
        self.cfg = {**self.cfg, "channel": self.channel}
        if not self.dry_run:
            fre_state.write_cfg({"channel": self.channel})
        self.show_page("asr_config")

    def on_asr_next(self):
        key = self._asr_key_var.get().strip()
        if key and not self.dry_run:
            fre_state.write_asr_cfg(key)
        self.connect()

    def on_channel_change(self):
        self.channel = self._channel_var.get()
        self.cfg = {**self.cfg, "channel": self.channel}   # 见 on_discover_next 注释

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
            self._relay = relay                      # 诊断页命令桥用
            self.syscmd_available = getattr(relay, "_syscmd", None) is not None
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
                if kind == "probe_progress":
                    self._probe_var.set(payload)
                elif kind == "probe_result":
                    self._on_probe_result(payload)
                elif kind == "asr_test":
                    self._on_asr_test(payload)
                elif kind == "phase":
                    self._on_phase(payload)
                elif kind == "syscmd_resp":
                    self._on_syscmd_resp(payload)
                elif kind == "tray_action":
                    self._on_tray_action(payload)
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
            self._row_vars["Injection"].set(INJECT_LABELS.get(mode, mode))
            key = self.cfg.get("volcano_api_key", "")
            if key:
                self._row_vars["ASR"].set("Volcano")
            else:
                self._row_vars["ASR"].set("未配置")
            self.show_page("status")
            self.set_status(f"已连接 {self.channel.upper()}, 按住 ● 讲话")
            self._tray_phase("connected")
        elif phase == "session_start":
            self._tray_phase("listening")
        elif phase == "session_end":
            self._tray_phase("idle")
        elif phase == "disconnected":
            self._tray_phase("disconnected")
            self.show_page("discover")
            self.set_status("连接已断开")
        elif phase == "failed":
            self._tray_phase("disconnected")
            self.show_page("discover")
            self.set_status(self.last_error or "连接失败")

    # ---------------- 托盘 ----------------

    def _tray_phase(self, what):
        """按 relay phase 更新托盘状态(无托盘/未启动时无操作)。"""
        if self._tray is None:
            return
        st = self._tray_state
        if what == "connected":
            st.update(connected=True, listening=False,
                      device=self.device_addr or "")
            self._start_tray()
        elif what == "listening":
            st["listening"] = True
        elif what == "idle":
            st["listening"] = False
        elif what == "disconnected":
            st.update(connected=False, listening=False)
        try:
            from tray import update_menu
            update_menu(self._tray, st)
        except Exception as e:  # noqa: BLE001 托盘异常不阻塞主流程
            print(f"[fre] 托盘菜单更新失败: {e}", file=sys.stderr)

    def _start_tray(self):
        """连接成功后启动托盘(非 dry-run 且未禁用)。回调只 queue.put 回主线程。"""
        if self._tray is not None or self.no_tray or self.dry_run:
            return
        try:
            from tray import create_tray
        except Exception as e:  # noqa: BLE001
            print(f"[fre] 托盘不可用: {e}", file=sys.stderr)
            return
        try:
            self._tray = create_tray(
                self._tray_state,
                on_settings=lambda: self.phase_q.put(("tray_action",
                                                      "settings")),
                on_diagnostics=lambda: self.phase_q.put(("tray_action",
                                                         "diagnostics")),
                on_quit=lambda: self.phase_q.put(("tray_action", "quit")))
            self._tray.run_detached()
            self.set_status("已驻留托盘 (关闭窗口不退出, 托盘 Quit 退出)")
        except Exception as e:  # noqa: BLE001
            print(f"[fre] 托盘启动失败: {e}", file=sys.stderr)
            self._tray = None

    def _on_tray_action(self, action):
        if action == "settings":
            self.root.deiconify()
            self.root.lift()
        elif action == "diagnostics":
            self.root.deiconify()
            self.root.lift()
            self.show_diagnostics()
        elif action == "quit":
            self._shutdown()

    def on_reset(self):
        if self._loop is not None and self.relay_task is not None:
            self._loop.call_soon_threadsafe(self.relay_task.cancel)
        self.set_status("")
        self.start_discover()

    # ---------------- 诊断页 ----------------

    def show_diagnostics(self):
        syscmd_ok = getattr(self, "syscmd_available", False)
        if syscmd_ok:
            self._diag_hint.set(
                f"USB 命令面已连接(channel={self.channel}), 可直接执行设备"
                " console 命令。Factory Reset / reboot 需确认。")
        else:
            self._diag_hint.set(
                f"当前通道 {self.channel.upper()} 无 SYS 命令面 "
                "(仅 USB 通道支持完整诊断); 以下为只读运行状态。")
        self.show_page("diagnostics")

    def _diag_append(self, text):
        self._diag_out.configure(state="normal")
        self._diag_out.insert("end", text.rstrip("\n") + "\n")
        self._diag_out.see("end")
        self._diag_out.configure(state="disabled")

    def diag_run_entry(self):
        cmd = self._diag_entry_var.get().strip()
        if not cmd:
            return
        self.diag_cmd(cmd)

    def diag_cmd(self, cmd, confirm=False):
        if confirm:
            sure = messagebox.askyesno(
                "确认", f"命令 {cmd} 将作用于设备:\n"
                        f"{'恢复出厂(清空 NVS)并重启' if cmd == 'factory' else '重启设备'}。"
                        "确认执行?")
            if not sure:
                return
        self._diag_append(f"> {cmd}")
        if not getattr(self, "syscmd_available", False):
            self._diag_append("! 当前通道无 SYS 命令面(仅 USB 通道支持)")
            return
        relay = getattr(self, "_relay", None)
        if relay is None or self.relay_task is None or self.relay_task.done():
            self._diag_append("! relay 未运行")
            return
        fut = asyncio.run_coroutine_threadsafe(relay.run_syscmd(cmd),
                                                self._loop)

        def on_done(f):
            try:
                resp = f.result()
                self.phase_q.put(("syscmd_resp", resp))
            except Exception as e:  # noqa: BLE001
                self.phase_q.put(("syscmd_resp", f"! {e}"))

        fut.add_done_callback(on_done)

    def _on_syscmd_resp(self, resp):
        self._diag_append(str(resp))

    def on_close(self):
        """关闭窗口: 托盘驻留时隐藏到托盘, 否则完整退出。"""
        if self._tray is not None:
            self.root.withdraw()
            return
        self._shutdown()

    def _shutdown(self):
        """完整退出: 收束后台 relay(取消 task + 等线程退出) + 停托盘。"""
        if self._loop is not None and self.relay_task is not None:
            self._loop.call_soon_threadsafe(self.relay_task.cancel)
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=5)
        if self._tray is not None:
            try:
                self._tray.stop()
            except Exception:  # noqa: BLE001
                pass
            self._tray = None
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description="AI Passport 首次运行向导")
    ap.add_argument("--dry-run", action="store_true",
                    help="不真实连接设备, 用内置 fake 链路走通流程")
    ap.add_argument("--no-tray", action="store_true",
                    help="连接成功后不启动系统托盘")
    args = ap.parse_args()
    root = ttk.Window(themename=THEME)
    FREApp(root, dry_run=args.dry_run, no_tray=args.no_tray)
    root.mainloop()


if __name__ == "__main__":
    main()
