#!/usr/bin/env python3
"""tray 单测: 菜单结构与图标生成(不起真实托盘, 无头可跑)。

覆盖:
- build_menu: 9 项(4 只读状态 + 分隔 + 2 动作 + 分隔 + 退出),
  只读行 enabled=False, 动作项回调保留
- build_icon_image: 64x64 RGBA 图
- update_menu: 重建后回调不丢(create_tray 注入)

运行: companion/.venv/bin/python companion/tests/test_tray.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tray import build_icon_image, build_menu, create_tray, update_menu  # noqa: E402

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")


def test_icon_image():
    img = build_icon_image()
    check("图标 64x64 RGBA", (img.size, img.mode), ((64, 64), "RGBA"))


def test_menu_structure():
    calls = []

    def on_settings():
        calls.append("settings")

    def on_diagnostics():
        calls.append("diagnostics")

    def on_quit():
        calls.append("quit")

    m = build_menu({"connected": True, "device": "AA:BB", "listening": True},
                   on_settings, on_diagnostics, on_quit)
    items = list(m.items)
    check("菜单 9 项", len(items), 9)
    check("状态行只读", items[0].enabled, False)
    check("连接状态文案", "已连接" in items[1].text, True)
    check("设备行文案", "AA:BB" in items[2].text, True)
    check("聆听状态文案", "聆听中" in items[3].text, True)
    check("动作行可点", items[5].enabled, True)
    # pystray 对回调做 functools 包装, 身份不同; 验证调用是否透传
    items[5]._action()
    items[6]._action()
    items[8]._action()
    check("Settings 回调透传", calls, ["settings", "diagnostics", "quit"])


def test_update_menu_keeps_callbacks():
    calls = []

    def on_settings():
        calls.append("settings")

    def on_diagnostics():
        calls.append("diagnostics")

    def on_quit():
        calls.append("quit")

    icon = create_tray({"connected": True, "device": "", "listening": False},
                       on_settings, on_diagnostics, on_quit)
    update_menu(icon, {"connected": False, "device": "", "listening": False})
    items = list(icon.menu.items)
    check("重建后 9 项", len(items), 9)
    check("重建后状态更新", "未连接" in items[1].text, True)
    items[5]._action()
    check("重建后回调不丢", calls, ["settings"])


def main():
    test_icon_image()
    test_menu_structure()
    test_update_menu_keeps_callbacks()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for x in FAILURES:
            print("  -", x)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()
