#!/usr/bin/env python3
"""Windows 输入注入: 剪贴板(CF_UNICODETEXT) + SendInput Ctrl+V 粘贴。

与 macOS 的 companion/inject.py 同思路: 文本先写入系统剪贴板
(win32clipboard, CF_UNICODETEXT = UTF-16, 中文/emoji 无忧), 再 SendInput
模拟 Ctrl+V 粘贴到当前聚焦窗口。不逐字键入 —— 避免 CJK 输入法兼容问题,
粘贴语义与 macOS 一致(插入而非叠加)。Win10 与 Win11 共用同一套 Win32 API。

焦点护栏(Win10/11 前台差异统一处理): 注入前 GetForegroundWindow +
GetWindowText, 前台窗口标题含 python/cmd/PowerShell/conhost(即 relay
自己的控制台) → 抛 InjectError 并附 GUIDE, 不注入。不自动置顶
(Windows 前台锁定使 SetForegroundWindow 不可靠)。注入前先检查一次
(快速失败), 再等 focus_delay 秒给用户点击目标窗口的时间, 之后复查一次
(用户没切窗口则中止)。

契约与 inject.py 一致: paste_text(text, dry_run=False) / InjectError / GUIDE。
pywin32 懒加载(dry_run 与单测不触碰 win32, Mac 可跑)。

用法:
  python3 companion/inject_win.py "要注入的文本"
  python3 companion/inject_win.py "文本" --dry-run     # 只打印将执行的步骤
"""
import argparse
import re
import sys
import time

# 前台护栏: 命中这些即视为 relay 自己的控制台窗口(不注入)。
# Windows 终端标题常见形态: "python", "cmd.exe - ...", "PowerShell",
# "C:\Windows\System32\conhost.exe", "py", "pythonw"; Win10 经典控制台
# 标题固定为 "Command Prompt"(本地化为 "命令提示符")。
CONSOLE_TITLE_RE = re.compile(
    r"(python|cmd|powershell|pwsh|conhost|py\.exe|command prompt)",
    re.IGNORECASE)

GUIDE = ("请先点击目标输入窗口(relay 会等 inject_focus_delay 秒后再注入), "
         "不要停留在 relay 的控制台窗口上。")


class InjectError(Exception):
    """注入失败(非 Windows / 缺 pywin32 / 前台是控制台 / 剪贴板或按键失败)。"""


def _is_console_title(title):
    """前台护栏判定: 窗口标题是否为 relay 自己的控制台(纯函数, 可单测)。

    Win10/11 终端标题差异(如标题是否带路径/进程名大小写)由正则覆盖;
    relay 控制台一定含 python/cmd/powershell/conhost 之一。
    """
    return bool(CONSOLE_TITLE_RE.search(title or ""))


def _send_keys(win32api, win32con, combos):
    """按组合序列模拟按键: combos 为 (按下时按住的修饰键, 主键) 列表。

    每个组合: 修饰键(如 VK_CONTROL)下 → 主键下/上 → 修饰键上。
    keybd_event 是 SendInput 的 pywin32 简化封装, Win10/11 均有效。
    """
    for mod, key in combos:
        if mod:
            win32api.keybd_event(mod, 0, 0, 0)
        win32api.keybd_event(key, 0, 0, 0)
        win32api.keybd_event(key, 0, win32con.KEYEVENTF_KEYUP, 0)
        if mod:
            win32api.keybd_event(mod, 0, win32con.KEYEVENTF_KEYUP, 0)


def key_action(action, dry_run=False, focus_delay=2.0):
    """把按键动作注入当前聚焦窗口(不依赖剪贴板)。

    action: "enter"(回车提交)或 "clear"(Ctrl+A 全选 + Delete 清空)。
    focus_delay 语义与 paste_text 相同(等用户切到目标窗口)。
    失败抛 InjectError。
    """
    if action not in ("enter", "clear"):
        raise InjectError(f"未知按键动作 {action!r} (enter|clear)")
    if dry_run:
        print(f"# 按键动作 {action!r}:")
        if action == "enter":
            print("# [SendInput] VK_RETURN")
        else:
            print("# [SendInput] Ctrl+A 全选 + VK_DELETE 清空")
        return
    if sys.platform != "win32":
        raise InjectError(
            "仅支持 Windows: 注入依赖 pywin32 SendInput, "
            f"当前平台 {sys.platform} 不支持")
    try:
        import win32api
        import win32con
    except ImportError as e:
        raise InjectError(f"缺少 pywin32: pip install pywin32 ({e})") from e

    def _win32_err(e):
        return InjectError(f"Windows 注入失败: {e}")

    if focus_delay > 0:
        print(f"[inject] {focus_delay:.1f}s 后注入(请切换到目标输入窗口)...")
        time.sleep(focus_delay)

    try:
        if action == "enter":
            _send_keys(win32api, win32con,
                       [(None, win32con.VK_RETURN)])
        else:
            _send_keys(win32api, win32con,
                       [(win32con.VK_CONTROL, ord("A")),
                        (None, win32con.VK_DELETE)])
    except Exception as e:
        raise _win32_err(f"按键注入失败: {e}") from e


def _send_ctrl_v(win32api, win32con):
    """keybd_event 模拟 Ctrl+V(SendInput 的 pywin32 简化封装, Win10/11 均有效;
    按下→释放顺序: Ctrl 下, V 下, V 上, Ctrl 上)。"""
    win32api.keybd_event(win32con.VK_CONTROL, 0, 0, 0)
    win32api.keybd_event(ord("V"), 0, 0, 0)
    win32api.keybd_event(ord("V"), 0, win32con.KEYEVENTF_KEYUP, 0)
    win32api.keybd_event(win32con.VK_CONTROL, 0, win32con.KEYEVENTF_KEYUP, 0)


def paste_text(text, dry_run=False, focus_delay=2.0):
    """把 text 写入剪贴板并模拟 Ctrl+V 粘贴到当前聚焦窗口。

    dry_run=True 只打印将执行的步骤(单测与无权限验证用), 不实际执行。
    focus_delay: 注入前等待用户切换到目标窗口的秒数(config.local.json
    的 inject_focus_delay; 护栏先快查一次, 等待后再复查)。
    失败抛 InjectError。
    """
    if not isinstance(text, str):
        raise InjectError("text 必须是 str")
    if dry_run:
        print(f"# 注入文本 {len(text)} 字符: {text!r}")
        print(f"# 等 {focus_delay}s 供用户切换到目标窗口")
        print("# [win32clipboard] 写入剪贴板(CF_UNICODETEXT)")
        print("# [SendInput] Ctrl+V 粘贴到前台窗口")
        return
    if sys.platform != "win32":
        raise InjectError(
            "仅支持 Windows: 注入依赖 win32clipboard 与 SendInput, "
            f"当前平台 {sys.platform} 不支持")
    try:
        import win32api
        import win32clipboard
        import win32con
        import win32gui
    except ImportError as e:
        raise InjectError(f"缺少 pywin32: pip install pywin32 ({e})") from e

    # win32 调用失败统一转 InjectError(带错误文本, 不裸 pywintypes traceback)
    def _win32_err(e):
        return InjectError(f"Windows 注入失败: {e}")

    # 1. 焦点护栏(先查): 前台是 relay 控制台 → 快速失败, 不等待
    try:
        fg = win32gui.GetForegroundWindow()
        title = win32gui.GetWindowText(fg)
    except Exception as e:
        raise _win32_err(f"读取前台窗口失败: {e}") from e
    if _is_console_title(title):
        raise InjectError(f"前台是 relay 控制台窗口({title!r}), 拒绝注入。{GUIDE}")

    # 2. 等用户切换到目标窗口(focus_delay 秒); 期间用户点击目标窗口
    if focus_delay > 0:
        print(f"[inject] 当前前台: {title!r}; {focus_delay:.1f}s 后注入"
              "(请切换到目标输入窗口)...")
        time.sleep(focus_delay)

    # 3. 护栏复查: 用户仍停在控制台 → 中止
    try:
        fg = win32gui.GetForegroundWindow()
        title = win32gui.GetWindowText(fg)
    except Exception as e:
        raise _win32_err(f"读取前台窗口失败: {e}") from e
    if _is_console_title(title):
        raise InjectError(f"前台仍是 relay 控制台窗口({title!r}), 拒绝注入。{GUIDE}")

    # 4. 剪贴板写入(CF_UNICODETEXT: UTF-16, 中文无忧; OpenClipboard 可能被
    #    其他进程持有而失败 —— 明确报错而非裸 traceback)
    try:
        win32clipboard.OpenClipboard()
        try:
            win32clipboard.EmptyClipboard()
            win32clipboard.SetClipboardText(text, win32con.CF_UNICODETEXT)
        finally:
            win32clipboard.CloseClipboard()
    except Exception as e:
        raise _win32_err(f"剪贴板写入失败: {e}") from e

    # 5. Ctrl+V 粘贴(不抢焦点: 只向当前前台窗口发键; keybd_event 是
    #    SendInput 的 pywin32 简化封装, Win10/11 均有效)
    try:
        _send_ctrl_v(win32api, win32con)
    except Exception as e:
        raise _win32_err(f"按键注入失败: {e}") from e


def _main():
    ap = argparse.ArgumentParser(description="Windows 剪贴板粘贴注入")
    ap.add_argument("text", help="要注入的文本")
    ap.add_argument("--dry-run", action="store_true",
                    help="只打印将执行的步骤")
    ap.add_argument("--focus-delay", type=float, default=2.0,
                    help="注入前等待用户切换到目标窗口的秒数")
    args = ap.parse_args()
    try:
        paste_text(args.text, dry_run=args.dry_run,
                   focus_delay=args.focus_delay)
    except InjectError as e:
        print(f"[inject] 错误: {e}", file=sys.stderr)
        print(f"[inject] 指引: {GUIDE}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    _main()
