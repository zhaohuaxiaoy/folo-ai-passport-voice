#!/usr/bin/env python3
"""Windows 输入注入: 优先 unicode 键盘事件, 剪贴板+粘贴兜底。

两条注入路径(见 paste_text 的 mode 参数):
  - unicode: SendInput KEYEVENTF_UNICODE 逐字符键盘事件, 完全不碰系统
    剪贴板 → 剪贴板历史管理工具零污染; 中文/emoji 经 UTF-16 码点直达;
  - clipboard: win32clipboard 写入(CF_UNICODETEXT = UTF-16) + SendInput
    模拟 Ctrl+V 粘贴, 注入后恢复旧剪贴板内容(仅文本; 历史工具仍留记录)。

焦点护栏(Win10/11 前台差异统一处理): 注入前 GetForegroundWindow +
GetWindowText, 前台窗口标题含 python/cmd/PowerShell/conhost(即 relay
自己的控制台) → 抛 InjectError 并附 GUIDE, 不注入。不自动置顶
(Windows 前台锁定使 SetForegroundWindow 不可靠)。注入前先检查一次
(快速失败), 再等 focus_delay 秒给用户点击目标窗口的时间, 之后复查一次
(用户没切窗口则中止)。护栏对所有注入路径生效(unicode 同样防注入控制台)。

契约与 inject.py 一致: paste_text(text, dry_run=False, mode="auto") /
InjectError / GUIDE。pywin32 懒加载(dry_run 与单测不触碰 win32, Mac 可跑)。

用法:
  python3 companion/inject_win.py "要注入的文本"
  python3 companion/inject_win.py "文本" --dry-run     # 只打印将执行的步骤
  python3 companion/inject_win.py "文本" --mode clipboard   # 强制剪贴板路径
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


def _type_unicode(win32api, win32con, text):
    """SendInput KEYEVENTF_UNICODE 逐字符注入(不碰剪贴板)。

    每个字符: DOWN(KEYEVENTF_UNICODE) + UP(KEYEVENTF_UNICODE|KEYEVENTF_KEYUP);
    非 BMP 字符(emoji)按 UTF-16 代理对发送。应用视角等同键盘输入,
    终端(Windows Terminal/ConHost)正常接受; xterm.js 类终端(如 VS Code
    集成终端)可能不消费 —— 那正是 mode="auto" 回退剪贴板的场景。
    """
    for ch in text:
        code = ord(ch)
        if code > 0xFFFF:
            code -= 0x10000
            pair = (0xD800 + (code >> 10), 0xDC00 + (code & 0x3FF))
        else:
            pair = (code,)
        for c in pair:
            try:
                win32api.SendInput(
                    (0, 0, c, win32con.KEYEVENTF_UNICODE),
                    (0, 0, c, win32con.KEYEVENTF_UNICODE | win32con.KEYEVENTF_KEYUP))
            except Exception as e:
                raise InjectError(f"SendInput unicode 注入失败: {e}") from e


def key_action(action, dry_run=False, focus_delay=2.0):
    """把按键动作注入当前聚焦窗口(不依赖剪贴板)。

    action: "enter"(回车提交)或 "clear"(Home+Shift+End 全选 + Delete 清空)。
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
            print("# [SendInput] VK_HOME + Shift+VK_END 全选 + VK_DELETE 清空")
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
                       [(None, win32con.VK_HOME),
                        (win32con.VK_SHIFT, win32con.VK_END),
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


def _clipboard_snapshot(win32clipboard, win32con):
    """读取当前剪贴板 → (has_text, text)。非文本(图片/文件)返回 (False, None)。"""
    win32clipboard.OpenClipboard()
    try:
        if win32clipboard.IsClipboardFormatAvailable(win32con.CF_UNICODETEXT):
            return True, win32clipboard.GetClipboardData(win32con.CF_UNICODETEXT)
    finally:
        win32clipboard.CloseClipboard()
    return False, None


def paste_text(text, dry_run=False, focus_delay=2.0, mode="auto"):
    """把 text 注入当前聚焦窗口。

    mode:
      "unicode"    SendInput KEYEVENTF_UNICODE 逐字符注入, 不碰剪贴板;
      "clipboard"  剪贴板 + Ctrl+V 粘贴, 注入后恢复旧剪贴板文本;
      "auto"(缺省) 优先 unicode, 基础设施不可用时回退剪贴板。
    注意: 剪贴板路径即使恢复了旧内容, 剪贴板历史管理工具仍会留下记录。
    dry_run=True 只打印将执行的步骤(单测与无权限验证用), 不实际执行。
    focus_delay: 注入前等待用户切换到目标窗口的秒数(config.local.json
    的 inject_focus_delay; 护栏先快查一次, 等待后再复查)。
    失败抛 InjectError。
    """
    if not isinstance(text, str):
        raise InjectError("text 必须是 str")
    if mode not in ("auto", "unicode", "clipboard"):
        raise InjectError(f"未知注入模式 {mode!r} (auto|unicode|clipboard)")
    if dry_run:
        print(f"# 注入文本 {len(text)} 字符: {text!r}")
        print(f"# 等 {focus_delay}s 供用户切换到目标窗口")
        if mode in ("auto", "unicode"):
            print("# [SendInput] KEYEVENTF_UNICODE 逐字符注入(不碰剪贴板)")
            if mode == "auto":
                print("# (auto: 基础设施不可用时回退剪贴板路径, 见下)")
        if mode in ("auto", "clipboard"):
            print("# [win32clipboard] 写入剪贴板(CF_UNICODETEXT) + Ctrl+V 粘贴, "
                  "注入后恢复旧剪贴板文本")
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

    # 4. 注入(unicode 优先; 护栏对两条路径同样生效)
    if mode in ("auto", "unicode"):
        try:
            _type_unicode(win32api, win32con, text)
            return
        except InjectError as e:
            if mode == "unicode":
                raise
            print(f"[inject] unicode 注入不可用, 回退剪贴板: {e}",
                  file=sys.stderr)

    # 5. clipboard: 快照 → 写入 → Ctrl+V → 恢复旧文本(非文本内容无法恢复)
    old_ok, old = _clipboard_snapshot(win32clipboard, win32con)
    try:
        win32clipboard.OpenClipboard()
        try:
            win32clipboard.EmptyClipboard()
            win32clipboard.SetClipboardText(text, win32con.CF_UNICODETEXT)
        finally:
            win32clipboard.CloseClipboard()
    except Exception as e:
        raise _win32_err(f"剪贴板写入失败: {e}") from e

    try:
        _send_ctrl_v(win32api, win32con)
    except Exception as e:
        raise _win32_err(f"按键注入失败: {e}") from e

    if old_ok:
        time.sleep(0.3)   # 给目标应用消费粘贴内容的时间, 再恢复旧剪贴板
        try:
            win32clipboard.OpenClipboard()
            try:
                win32clipboard.EmptyClipboard()
                win32clipboard.SetClipboardText(old, win32con.CF_UNICODETEXT)
            finally:
                win32clipboard.CloseClipboard()
        except Exception as e:
            raise _win32_err(f"剪贴板恢复失败: {e}") from e
    else:
        print("[inject] 剪贴板原本非文本/为空, 旧内容无法恢复"
              "(当前剪贴板已被替换)", file=sys.stderr)


def _main():
    ap = argparse.ArgumentParser(description="Windows 文本注入(unicode 优先, 剪贴板兜底)")
    ap.add_argument("text", help="要注入的文本")
    ap.add_argument("--dry-run", action="store_true",
                    help="只打印将执行的步骤")
    ap.add_argument("--focus-delay", type=float, default=2.0,
                    help="注入前等待用户切换到目标窗口的秒数")
    ap.add_argument("--mode", default="auto", choices=("auto", "unicode", "clipboard"),
                    help="注入路径: auto(默认, unicode 优先) / unicode / clipboard")
    args = ap.parse_args()
    try:
        paste_text(args.text, dry_run=args.dry_run,
                   focus_delay=args.focus_delay, mode=args.mode)
    except InjectError as e:
        print(f"[inject] 错误: {e}", file=sys.stderr)
        print(f"[inject] 指引: {GUIDE}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    _main()
