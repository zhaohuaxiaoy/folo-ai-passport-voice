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


def test_invalid_mode_rejected():
    """未知 mode 在平台检查之前拒绝(任何平台可测)。"""
    try:
        inject_win.paste_text("hi", mode="bogus")
        check("未知 mode 拒绝", False, True)
    except inject_win.InjectError as e:
        check("未知 mode 拒绝", "auto|unicode|clipboard" in str(e), True)


class _FakeWin32:
    """成功路径 fake win32: 记录调用, 护栏放行, SendInput 可按需抛错。"""
    def __init__(self, sendinput_boom=False):
        self.calls = []
        self.sendinput_boom = sendinput_boom

    def GetForegroundWindow(self):
        return 123

    def GetWindowText(self, _hwnd):
        return "Visual Studio Code"

    def SendInput(self, *inputs):
        if self.sendinput_boom:
            raise RuntimeError("模拟 SendInput 失败")
        self.calls.append(("SendInput", inputs))
        return len(inputs)

    def keybd_event(self, *args):
        self.calls.append(("keybd_event", args))


class _FakeClipboard:
    """记录调用; text=None 表示当前剪贴板非文本。"""
    def __init__(self, text=None):
        self.calls = []
        self.text = text

    def OpenClipboard(self):
        self.calls.append("Open")

    def CloseClipboard(self):
        self.calls.append("Close")

    def EmptyClipboard(self):
        self.calls.append("Empty")

    def SetClipboardText(self, t, _fmt):
        self.calls.append(("Set", t))
        self.text = t

    def GetClipboardData(self, _fmt):
        return self.text

    def IsClipboardFormatAvailable(self, _fmt):
        return self.text is not None


class _FakeCon:
    KEYEVENTF_UNICODE = 0x4
    KEYEVENTF_KEYUP = 0x2
    CF_UNICODETEXT = 13
    VK_RETURN = 0x0D
    VK_HOME = 0x24
    VK_END = 0x23
    VK_DELETE = 0x2E
    VK_SHIFT = 0x10
    VK_CONTROL = 0x11


def _install_fakes(api, clip, con):
    import sys as _sys
    saved = {}
    for m, mod in (("win32api", api), ("win32clipboard", clip),
                   ("win32con", con), ("win32gui", api)):
        saved[m] = _sys.modules.get(m)
        _sys.modules[m] = mod
    return saved


def _restore_fakes(saved):
    import sys as _sys
    for m, mod in saved.items():
        if mod is None:
            _sys.modules.pop(m, None)
        else:
            _sys.modules[m] = mod


def test_unicode_path_no_clipboard():
    """mode=unicode: SendInput KEYEVENTF_UNICODE 逐字符, 完全不碰剪贴板。"""
    saved_platform = sys.platform
    sys.platform = "win32"
    saved = None
    try:
        api, clip, con = _FakeWin32(), _FakeClipboard(text="旧内容"), _FakeCon()
        saved = _install_fakes(api, clip, con)
        inject_win.paste_text("你好", focus_delay=0, mode="unicode")
        sends = [c for c in api.calls if c[0] == "SendInput"]
        check("unicode 逐字符两次 SendInput", len(sends) == 2, True)
        check("每字符 DOWN+UP 成对",
              all(len(s[1]) == 2 for s in sends), True)
        check("不触碰剪贴板(含快照)", len(clip.calls) == 0, True)
    finally:
        sys.platform = saved_platform
        if saved:
            _restore_fakes(saved)


def test_unicode_surrogate_pair():
    """emoji(U+1F600) 拆 UTF-16 代理对 0xD83D/0xDE00 发送。"""
    saved_platform = sys.platform
    sys.platform = "win32"
    saved = None
    try:
        api, clip, con = _FakeWin32(), _FakeClipboard(), _FakeCon()
        saved = _install_fakes(api, clip, con)
        inject_win.paste_text("😀", focus_delay=0, mode="unicode")
        sends = [c for c in api.calls if c[0] == "SendInput"]
        codes = [s[1][0][2] for s in sends]     # 每个 SendInput 首个 input 的 wScan
        check("代理对 0xD83D/0xDE00", codes == [0xD83D, 0xDE00], True)
    finally:
        sys.platform = saved_platform
        if saved:
            _restore_fakes(saved)


def test_auto_fallback_to_clipboard():
    """auto: SendInput 失败 → 回退剪贴板(写入 + 粘贴 + 恢复旧文本)。"""
    saved_platform = sys.platform
    sys.platform = "win32"
    saved = None
    try:
        api = _FakeWin32(sendinput_boom=True)
        clip = _FakeClipboard(text="旧内容")
        con = _FakeCon()
        saved = _install_fakes(api, clip, con)
        inject_win.paste_text("你好", focus_delay=0)     # mode 默认 auto
        sets = [c for c in clip.calls if c[0] == "Set"]
        check("回退写入转写文本", len(sets) >= 1 and sets[0][1] == "你好", True)
        check("旧文本最后恢复", sets[-1][1] == "旧内容", True)
    finally:
        sys.platform = saved_platform
        if saved:
            _restore_fakes(saved)


def test_unicode_forced_raises_on_failure():
    """mode=unicode 强制: SendInput 失败直接抛 InjectError, 不回退。"""
    saved_platform = sys.platform
    sys.platform = "win32"
    saved = None
    try:
        api = _FakeWin32(sendinput_boom=True)
        clip = _FakeClipboard()
        con = _FakeCon()
        saved = _install_fakes(api, clip, con)
        try:
            inject_win.paste_text("你好", focus_delay=0, mode="unicode")
            check("unicode 强制失败抛 InjectError", False, True)
        except inject_win.InjectError as e:
            check("unicode 强制失败抛 InjectError", "SendInput" in str(e), True)
            check("强制模式不碰剪贴板", len(clip.calls) == 0, True)
    finally:
        sys.platform = saved_platform
        if saved:
            _restore_fakes(saved)


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
    test_invalid_mode_rejected()
    test_non_win32_rejected()
    test_win32_without_pywin32()
    test_console_title_guard()
    test_win32_api_failure_wrapped()
    test_unicode_path_no_clipboard()
    test_unicode_surrogate_pair()
    test_auto_fallback_to_clipboard()
    test_unicode_forced_raises_on_failure()
    if FAILURES:
        print(f"\n{len(FAILURES)} 项失败:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\n全部通过")


if __name__ == "__main__":
    main()
