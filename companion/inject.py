#!/usr/bin/env python3
"""macOS 输入注入: 优先 unicode 键盘事件, 剪贴板+粘贴兜底。

两条注入路径(见 paste_text 的 mode 参数):
  - unicode: CGEvent 逐字符键盘事件(Quartz, 需要 pyobjc-framework-Quartz),
    完全不碰系统剪贴板 → 剪贴板历史管理工具(Paste/Maccy 等)零污染;
  - clipboard: pbcopy 写入剪贴板 + osascript 模拟 Cmd+V 粘贴, 注入后
    恢复旧剪贴板内容(仅文本; 剪贴板历史工具仍会留下记录)。

权限: osascript(System Events)与 CGEvent 注入都需要「辅助功能」授权
(系统设置 → 隐私与安全性 → 辅助功能)。未授权时 osascript 报错
(常见 -1743 / -25211), 本程序抛出带指引的明确异常。

用法:
  python3 companion/inject.py "要注入的文本"
  python3 companion/inject.py "文本" --dry-run   # 只打印将执行的命令
  python3 companion/inject.py "文本" --mode clipboard   # 强制剪贴板路径
"""
import argparse
import shutil
import subprocess
import sys
import time

GUIDE = ("请在 系统设置 → 隐私与安全性 → 辅助功能 中授权本程序(你的终端应用), "
         "然后重新运行。")


class InjectError(Exception):
    """注入失败(非 macOS / 缺系统工具 / 剪贴板失败 / 辅助功能未授权)。"""


def _type_unicode(text):
    """CGEvent 逐字符键盘事件注入(不碰剪贴板, 中文/emoji 无忧)。

    应用视角等同输入法上屏; 终端(Terminal.app/iTerm2)正常接受, 部分
    xterm.js 类终端(VS Code 集成终端)可能不消费 —— 那正是 mode="auto"
    回退剪贴板的场景。依赖 pyobjc-framework-Quartz, 未安装抛 InjectError。
    """
    try:
        from Quartz import (CGEventCreateKeyboardEvent,
                            CGEventKeyboardSetUnicodeString,
                            CGEventPost, kCGHIDEventTap)
    except ImportError as e:
        raise InjectError(
            f"缺少 pyobjc-framework-Quartz: pip install pyobjc-framework-Quartz "
            f"({e})") from e
    # 授权检查(审查 P1-5): CGEventPost 在无「辅助功能」授权时静默成功
    # (事件根本不送达), 用户会看到"已注入"但目标输入框毫无反应。
    # AXIsProcessTrusted 显式判定, False 抛带指引的 InjectError →
    # auto 模式回退剪贴板路径(有明确报错, 不再静默假成功)。
    try:
        from ApplicationServices import AXIsProcessTrusted
    except ImportError:
        pass   # 框架缺失(fre_state 懒加载场景): 保持旧行为
    else:
        if not AXIsProcessTrusted():
            raise InjectError("辅助功能未授权, CGEvent 注入被系统拦截。" + GUIDE)
    for ch in text:
        # 非 BMP 字符(emoji)占 2 个 UTF-16 单元, stringLength 按单元计
        units = len(ch.encode("utf-16-le")) // 2
        down = CGEventCreateKeyboardEvent(None, 0, True)
        CGEventKeyboardSetUnicodeString(down, units, ch)
        CGEventPost(kCGHIDEventTap, down)
        CGEventPost(kCGHIDEventTap, CGEventCreateKeyboardEvent(None, 0, False))


def _clipboard_snapshot():
    """读取当前剪贴板 → (has_text, text)。非文本(图片/文件)返回 (False, None)。"""
    r = subprocess.run(["osascript", "-e", "clipboard info"],
                       capture_output=True, text=True)
    if "text" not in (r.stdout or ""):
        return False, None
    p = subprocess.run(["pbpaste"], capture_output=True)
    return True, p.stdout.decode("utf-8", "replace")


def paste_text(text, dry_run=False, mode="auto"):
    """把 text 注入当前聚焦输入框。

    mode:
      "unicode"    CGEvent 逐字符键盘事件, 不碰剪贴板;
      "clipboard"  pbcopy + Cmd+V 粘贴, 注入后恢复旧剪贴板文本;
      "auto"(缺省) 优先 unicode, 基础设施不可用(缺 Quartz)时回退剪贴板。
    注意: 剪贴板路径即使恢复了旧内容, 剪贴板历史管理工具(Paste/Maccy
    等)仍会留下记录 —— 在意历史污染请用 unicode 模式。
    dry_run=True 只打印将执行的命令(单测与无权限验证用), 不实际执行。
    失败抛 InjectError。
    """
    if not isinstance(text, str):
        raise InjectError("text 必须是 str")
    if mode not in ("auto", "unicode", "clipboard"):
        raise InjectError(f"未知注入模式 {mode!r} (auto|unicode|clipboard)")
    if sys.platform != "darwin":
        raise InjectError(
            "仅支持 macOS: 注入依赖 CGEvent 与 osascript(System Events), "
            f"当前平台 {sys.platform} 不支持")
    for tool in ("pbcopy", "osascript"):
        if shutil.which(tool) is None:
            raise InjectError(f"缺少系统工具 {tool}(仅 macOS 自带)")

    if mode in ("auto", "unicode"):
        try:
            if dry_run:
                print(f"# 注入文本 {len(text)} 字符: {text!r}")
                print("# [CGEvent] 逐字符键盘事件注入(不碰剪贴板)")
                if mode == "auto":
                    print("# (auto: 基础设施不可用时回退剪贴板路径, 见下)")
            else:
                _type_unicode(text)
            return
        except InjectError as e:
            if mode == "unicode":
                raise
            print(f"[inject] unicode 注入不可用, 回退剪贴板: {e}",
                  file=sys.stderr)

    # clipboard: 备份 → 写入 → Cmd+V → 恢复旧文本(非文本内容无法恢复)
    old_ok, old = (False, None)
    if not dry_run:
        old_ok, old = _clipboard_snapshot()

    cmds = [
        ["pbcopy"],
        ["osascript", "-e",
         'tell application "System Events" to keystroke "v" using command down'],
    ]
    if dry_run:
        print(f"# 注入文本 {len(text)} 字符: {text!r}")
        print("# [clipboard] pbcopy 写入 + Cmd+V 粘贴, 注入后恢复旧剪贴板文本")
        for c in cmds:
            print("$", " ".join(c))
        return

    p = subprocess.run(cmds[0], input=text.encode("utf-8"),
                       capture_output=True)
    if p.returncode != 0:
        raise InjectError("pbcopy 写入剪贴板失败: "
                          + p.stderr.decode("utf-8", "replace").strip())

    p = subprocess.run(cmds[1], capture_output=True, text=True)
    if p.returncode != 0:
        err = p.stderr.strip()
        raise InjectError("粘贴失败(osascript 被拒)。" + GUIDE
                          + "\n原始错误: " + err)

    if old_ok:
        time.sleep(0.3)   # 给目标应用消费粘贴内容的时间, 再恢复旧剪贴板
        subprocess.run(["pbcopy"], input=old.encode("utf-8"),
                       capture_output=True)
    else:
        print("[inject] 剪贴板原本非文本/为空, 旧内容无法恢复"
              "(当前剪贴板已被替换)", file=sys.stderr)


# 终端模拟器 / 带内置终端的编辑器(前台 app 是它们时, clear 走 Ctrl+U)。
# 原因: Home+Shift+End 的 Shift+End 会被终端模拟器拦截为"终端文本选择",
# TUI(Claude Code/CodeX 等)收不到全选, Delete 只删 1 个字符; 而 Ctrl+U
# 是控制字符, 终端不拦截、原样转发(readline/Ink 系标准"删到行首")。
# bundle id 优先, 进程名兜底(_frontmost_is_terminal 两轮查询)。
_TERMINAL_APPS = {
    "com.apple.Terminal": "Terminal",
    "com.googlecode.iterm2": "iTerm2",
    "dev.warp.Warp-Stable": "Warp",
    "com.mitchellh.ghostty": "Ghostty",
    "net.kovidgoyal.kitty": "kitty",
    "org.alacritty": "Alacritty",
    "com.wezterm.wezterm": "WezTerm",
    "com.microsoft.VSCode": "Code",         # 内置终端跑 Claude Code/CodeX
    "com.todesktop.230313mzl4w4p92": "Cursor",
    "dev.zed.Zed": "Zed",
    "com.hyper": "Hyper",
}


def _frontmost_is_terminal():
    """前台 app 是否为终端类。osascript 失败/超时按"非终端"降级(不改变
    注入路径, 不恶化现状)。"""
    queries = [
        ('tell application "System Events" to get bundle identifier of '
         'first application process whose frontmost is true', "id"),
        ('tell application "System Events" to get name of '
         'first application process whose frontmost is true', "name"),
    ]
    for query, _attr in queries:
        try:
            p = subprocess.run(["osascript", "-e", query],
                               capture_output=True, text=True, timeout=3)
        except Exception:
            return False
        if p.returncode == 0 and p.stdout.strip():
            val = p.stdout.strip()
            return val in _TERMINAL_APPS or val in _TERMINAL_APPS.values()
    return False


def key_action(action, dry_run=False):
    """把按键动作注入当前聚焦输入框(不依赖剪贴板)。

    action: "enter"(回车提交)或 "clear"(清空输入框)。
    clear 按前台 app 分路径:
      - 终端类(Terminal/iTerm2/Warp/…): Ctrl+U 删到行首 —— TUI 里
        Shift+End 全选会被终端拦截, 只用 Ctrl+U 才能整行清空;
      - 非终端: Home+Shift+End 全选 + 删除(Chromium 里 Ctrl+U=查看源码, 避开)。
    与 paste_text 同一 osascript/System Events 通道, 同一授权要求。
    失败抛 InjectError。
    """
    if action not in ("enter", "clear"):
        raise InjectError(f"未知按键动作 {action!r} (enter|clear)")
    if sys.platform != "darwin":
        raise InjectError(
            "仅支持 macOS: 注入依赖 osascript(System Events), "
            f"当前平台 {sys.platform} 不支持")
    if shutil.which("osascript") is None:
        raise InjectError("缺少系统工具 osascript(仅 macOS 自带)")

    if action == "enter":
        cmds = [
            ["osascript", "-e",
             'tell application "System Events" to keystroke return'],
        ]
    else:   # clear: 终端走 Ctrl+U(删到行首); 非终端 Home+Shift+End 全选 + 删除
        if _frontmost_is_terminal():
            cmds = [
                ["osascript", "-e",
                 'tell application "System Events" to keystroke "u" using control down'],
            ]
        else:
            cmds = [
                ["osascript", "-e",
                 'tell application "System Events" to key code 115'],       # 115 = Home
                ["osascript", "-e",
                 'tell application "System Events" to key code 119 using {shift down}'],  # Shift+End
                ["osascript", "-e",
                 'tell application "System Events" to key code 51'],        # 51 = delete
            ]
    if dry_run:
        print(f"# 按键动作 {action!r}:")
        for c in cmds:
            print("$", " ".join(c))
        return

    for c in cmds:
        p = subprocess.run(c, capture_output=True, text=True)
        if p.returncode != 0:
            err = p.stderr.strip()
            raise InjectError(f"按键注入失败(osascript 被拒)。{GUIDE}"
                              + "\n原始错误: " + err)


def main():
    ap = argparse.ArgumentParser(
        description="把文本注入到当前聚焦的输入框(unicode 优先, 剪贴板兜底)")
    ap.add_argument("text", help="要注入的文本(中文/英文均可)")
    ap.add_argument("--mode", default="auto", choices=("auto", "unicode", "clipboard"),
                    help="注入路径: auto(默认, unicode 优先) / unicode / clipboard")
    ap.add_argument("--dry-run", action="store_true",
                    help="只打印将执行的命令, 不实际执行")
    args = ap.parse_args()
    try:
        paste_text(args.text, dry_run=args.dry_run, mode=args.mode)
    except InjectError as e:
        print(f"[inject] 错误: {e}", file=sys.stderr)
        sys.exit(1)
    if not args.dry_run:
        print(f"[inject] 已注入 {len(args.text)} 字符")


if __name__ == "__main__":
    main()
