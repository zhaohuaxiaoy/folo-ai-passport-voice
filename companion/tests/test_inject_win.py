#!/usr/bin/env python3
"""inject_win 单测(Mac 可跑, 不触碰真实 win32 API)。

覆盖:
- dry_run 在任何平台可跑(不 import pywin32)
- 非 str 抛 InjectError
- 非 win32 平台真实注入 → InjectError(平台指引)
- 模拟 win32 平台但缺 pywin32 → InjectError(懒加载指引)
- 前台护栏纯函数 _is_console_title(Win10/11 终端标题形态)

真实粘贴(剪贴板/焦点/SendInput)为 Windows 真机项, 见 P7 NOT RUN 清单。
运行: companion/.venv/bin/python companion/tests/test_inject_win.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import inject_win  # noqa: E402

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")


def test_dry_run_any_platform():
    """dry_run 不触碰 win32: 任何平台可打印注入计划。"""
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    text = "你好 world"
    with redirect_stdout(buf):
        inject_win.paste_text(text, dry_run=True)
    out = buf.getvalue()
    check("dry_run 打印字符数", f"{len(text)} 字符" in out, True)
    check("dry_run 打印剪贴板步骤", "CF_UNICODETEXT" in out, True)
    check("dry_run 打印粘贴步骤", "Ctrl+V" in out, True)
    check("dry_run 打印等待提示", "切换" in out, True)


def test_key_action_dry_run():
    """key_action dry_run: enter/clear 打印对应按键序列; 未知动作拒绝。"""
    import io
    from contextlib import redirect_stdout

    buf = io.StringIO()
    with redirect_stdout(buf):
        inject_win.key_action("enter", dry_run=True)
    check("key enter 打印", "VK_RETURN" in buf.getvalue(), True)

    buf = io.StringIO()
    with redirect_stdout(buf):
        inject_win.key_action("clear", dry_run=True)
    out = buf.getvalue()
    check("key clear 打印全选", "VK_HOME" in out and "VK_END" in out, True)
    check("key clear 打印删除", "VK_DELETE" in out, True)

    try:
        inject_win.key_action("bogus")
        check("未知动作拒绝", False, True)
    except inject_win.InjectError as e:
        check("未知动作拒绝", "enter|clear" in str(e), True)


def test_non_str_rejected():
    try:
        inject_win.paste_text(12345)
        check("非 str 抛 InjectError", False, True)
    except inject_win.InjectError as e:
        check("非 str 抛 InjectError", "str" in str(e), True)


def test_non_win32_rejected():
    """Mac 上真实注入 → 平台指引错误(不执行任何 win32 调用)。"""
    try:
        inject_win.paste_text("你好")
        check("非 win32 抛 InjectError", False, True)
    except inject_win.InjectError as e:
        check("非 win32 抛 InjectError", "win32clipboard" in str(e), True)


def test_win32_without_pywin32():
    """模拟 win32 平台: pywin32 缺失 → 懒加载失败给安装指引(dry_run 不受影响)。"""
    saved = sys.platform
    sys.platform = "win32"
    try:
        try:
            inject_win.paste_text("你好")
            check("缺 pywin32 抛 InjectError", False, True)
        except inject_win.InjectError as e:
            check("缺 pywin32 抛 InjectError", "pywin32" in str(e), True)
        # dry_run 仍不 import win32: 模拟平台下也可跑
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            inject_win.paste_text("hi", dry_run=True)
        check("模拟 win32 下 dry_run 仍可跑", "Ctrl+V" in buf.getvalue(), True)
    finally:
        sys.platform = saved


def test_console_title_guard():
    """前台护栏判定(Win10/11 终端标题形态全覆盖)。"""
    cases = [
        ("python", True), ("python.exe", True),
        ("cmd.exe - C:\\work", True), ("Command Prompt", True),
        ("PowerShell", True), ("pwsh", True), ("Windows PowerShell", True),
        ("C:\\Windows\\System32\\conhost.exe", True),
        ("Visual Studio Code", False), ("微信", False),
        ("WeChat", False), ("", False),
    ]
    for title, want in cases:
        got = inject_win._is_console_title(title)
        if got != want:
            FAILURES.append(f"护栏判定 {title!r}: got {got}, want {want}")
    print(f"[{'PASS' if not any('护栏' in f for f in FAILURES) else 'FAIL'}] "
          f"护栏判定 {len(cases)} 例")


def test_win32_api_failure_wrapped():
    """win32 API 失败必须转 InjectError(不裸 pywintypes traceback)。

    用 fake win32 模块模拟 API 抛错: 任意环节失败 → InjectError 带环节说明。
    真实 win32 行为为 Windows 真机项(NOT RUN)。
    """
    saved = sys.platform
    saved_mods = {}
    try:
        sys.platform = "win32"
        # fake win32 模块: 普通属性访问即抛(模拟 API 调用失败);
        # import 系统的元属性(__spec__ 等)返回 None 让 import 正常走通
        class _Boom:
            def __getattr__(self, name):
                if name.startswith("__"):
                    return None
                raise RuntimeError(f"模拟 {name} 失败")
        import sys as _sys
        for m in ("win32api", "win32clipboard", "win32con", "win32gui"):
            saved_mods[m] = _sys.modules.get(m)
            _sys.modules[m] = _Boom()
        try:
            inject_win.paste_text("你好", focus_delay=0)
            check("win32 API 失败转 InjectError", False, True)
        except inject_win.InjectError as e:
            msg = str(e)
            check("win32 API 失败转 InjectError", "注入失败" in msg, True)
            check("错误带环节说明", "读取前台窗口失败" in msg, True)
        # 无 focus_delay 睡眠: focus_delay=0 跳过快查后直接复查, 仍在护栏处失败
    finally:
        sys.platform = saved
        import sys as _sys
        for m, mod in saved_mods.items():
            if mod is None:
                _sys.modules.pop(m, None)
            else:
                _sys.modules[m] = mod


def main():
    test_dry_run_any_platform()
    test_key_action_dry_run()
    test_non_str_rejected()
    test_non_win32_rejected()
    test_win32_without_pywin32()
    test_console_title_guard()
    test_win32_api_failure_wrapped()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()
