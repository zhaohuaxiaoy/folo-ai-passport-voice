#!/usr/bin/env python3
"""macOS 输入注入: 剪贴板 + Cmd+V 粘贴到当前聚焦的输入框。

原理: 文本先写入系统剪贴板(pbcopy), 再用 osascript 模拟 Cmd+V 粘贴
(System Events)。中文/英文/标点统一走剪贴板粘贴, 避免逐字键盘事件对
CJK 输入法的兼容问题。

权限: System Events keystroke 需要「辅助功能」授权(系统设置 → 隐私与
安全性 → 辅助功能)。未授权时 osascript 报错(常见 -1743 / -25211),
本程序抛出带指引的明确异常。

用法:
  python3 companion/inject.py "要注入的文本"
  python3 companion/inject.py "文本" --dry-run   # 只打印将执行的命令
"""
import argparse
import shutil
import subprocess
import sys

GUIDE = ("请在 系统设置 → 隐私与安全性 → 辅助功能 中授权本程序(你的终端应用), "
         "然后重新运行。")


class InjectError(Exception):
    """注入失败(非 macOS / 缺系统工具 / 剪贴板失败 / 辅助功能未授权)。"""


def paste_text(text, dry_run=False):
    """把 text 写入剪贴板并模拟 Cmd+V 粘贴到当前聚焦输入框。

    dry_run=True 只打印将执行的命令(单测与无权限验证用), 不实际执行。
    失败抛 InjectError。
    """
    if not isinstance(text, str):
        raise InjectError("text 必须是 str")
    if sys.platform != "darwin":
        raise InjectError(
            "仅支持 macOS: 注入依赖 pbcopy 与 osascript(System Events), "
            f"当前平台 {sys.platform} 不支持")
    for tool in ("pbcopy", "osascript"):
        if shutil.which(tool) is None:
            raise InjectError(f"缺少系统工具 {tool}(仅 macOS 自带)")

    cmds = [
        ["pbcopy"],
        ["osascript", "-e",
         'tell application "System Events" to keystroke "v" using command down'],
    ]
    if dry_run:
        print(f"# 注入文本 {len(text)} 字符: {text!r}")
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


def key_action(action, dry_run=False):
    """把按键动作注入当前聚焦输入框(不依赖剪贴板)。

    action: "enter"(回车提交)或 "clear"(Cmd+A 全选 + 删除清空)。
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
    else:   # clear: Cmd+A 全选, 再删除(退格)
        cmds = [
            ["osascript", "-e",
             'tell application "System Events" to keystroke "a" using command down'],
            ["osascript", "-e",
             'tell application "System Events" to key code 51'],   # 51 = delete
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
        description="把文本注入到当前聚焦的输入框(剪贴板 + Cmd+V)")
    ap.add_argument("text", help="要注入的文本(中文/英文均可)")
    ap.add_argument("--dry-run", action="store_true",
                    help="只打印将执行的命令, 不实际执行")
    args = ap.parse_args()
    try:
        paste_text(args.text, dry_run=args.dry_run)
    except InjectError as e:
        print(f"[inject] 错误: {e}", file=sys.stderr)
        sys.exit(1)
    if not args.dry_run:
        print(f"[inject] 已粘贴 {len(args.text)} 字符")


if __name__ == "__main__":
    main()
