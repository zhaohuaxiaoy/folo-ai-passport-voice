#!/usr/bin/env python3
"""系统托盘(pystray): macOS Menu Bar / Windows Tray。

菜单: 只读状态行(连接/设备/聆听) + 设置(Settings) + 诊断(Diagnostics)
+ 退出(Quit)。回调运行在 pystray 线程, 只允许 queue.put 回 tk 主线程,
不得直接碰 tk 控件。

用法:
  tray.create_tray(state, on_settings, on_diagnostics, on_quit) -> Icon
  tray.update_menu(icon, state)     # 状态变化后重建菜单(tk 线程调用)
  icon.run_detached() / icon.stop()
"""
import pystray
from PIL import Image, ImageDraw

MENU_SEPARATOR = pystray.Menu.SEPARATOR


def build_icon_image():
    """运行时绘制托盘图标(蓝底圆角徽章 + 白对勾), 不打包素材文件。"""
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([4, 4, 60, 60], radius=14, fill=(37, 99, 235, 255))
    d.line([(17, 33), (27, 44), (47, 21)], fill="white", width=6,
           joint="curve")
    return img


def build_menu(state, on_settings, on_diagnostics, on_quit):
    """按状态重建菜单。state: {"connected": bool, "device": str,
    "listening": bool}; 只读行 enabled=False 不可点击。"""
    conn = "已连接" if state.get("connected") else "未连接"
    device = state.get("device") or "—"
    listening = "聆听中" if state.get("listening") else "待命"
    return pystray.Menu(
        pystray.MenuItem("AI Passport", None, enabled=False),
        pystray.MenuItem(f"连接: {conn}", None, enabled=False),
        pystray.MenuItem(f"设备: {device}", None, enabled=False),
        pystray.MenuItem(f"语音: {listening}", None, enabled=False),
        MENU_SEPARATOR,
        pystray.MenuItem("设置 (Settings)", on_settings),
        pystray.MenuItem("诊断 (Diagnostics)", on_diagnostics),
        MENU_SEPARATOR,
        pystray.MenuItem("退出 (Quit)", on_quit),
    )


def update_menu(icon, state):
    """重建菜单(状态变化后调用; pystray 允许线程外更新, 统一从 tk 线程
    调用避免竞态)。回调保存在 create_tray 时注入的 _ai_cbs 上。"""
    icon.menu = build_menu(state, *icon._ai_cbs)


def create_tray(state, on_settings, on_diagnostics, on_quit):
    """创建托盘 Icon(尚未启动; 调用方 run_detached())。"""
    icon = pystray.Icon("AI Passport", build_icon_image(), "AI Passport")
    icon._ai_cbs = (on_settings, on_diagnostics, on_quit)
    icon.menu = build_menu(state, on_settings, on_diagnostics, on_quit)
    return icon
