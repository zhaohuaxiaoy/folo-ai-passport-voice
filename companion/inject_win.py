#!/usr/bin/env python3
"""Windows 输入注入(P6 完整实现, 当前为骨架): 剪贴板 + Ctrl+V 粘贴。

原理(与 macOS 的 companion/inject.py 同思路): 文本先写入系统剪贴板
(win32clipboard, CF_UNICODETEXT / UTF-16), 再 SendInput 模拟 Ctrl+V 粘贴
到当前聚焦窗口。中文/英文/标点统一走剪贴板粘贴, 避免逐字键盘事件对
CJK 输入法的兼容问题。Win10 与 Win11 共用同一套 Win32 API。

焦点护栏(P6): 注入前 GetForegroundWindow + GetWindowText 检查前台窗口
是否是 relay 自己的控制台窗口(标题含 python/cmd/PowerShell)→ 抛
InjectError 并附 GUIDE; 不自动置顶(Windows 前台锁定使
SetForegroundWindow 不可靠)。inject_focus_delay(config.local.json,
默认 2.0s)给用户点击目标窗口的时间。

用法:
  python3 companion/inject_win.py "要注入的文本"
  python3 companion/inject_win.py "文本" --dry-run   # 只打印将执行的步骤
"""
import argparse
import sys

GUIDE = ("请先点击目标输入窗口(relay 会等 inject_focus_delay 秒后再注入), "
         "不要停留在 relay 的控制台窗口上。")


class InjectError(Exception):
    """注入失败(非 Windows / 前台是控制台窗口 / 剪贴板或键盘事件失败)。"""


def paste_text(text, dry_run=False):
    """把 text 写入剪贴板并模拟 Ctrl+V 粘贴到当前聚焦窗口。

    dry_run=True 只打印将执行的步骤(单测与无权限验证用), 不实际执行。
    失败抛 InjectError。非 Windows 平台调用即抛(单测在 Mac 上仅走 dry_run)。
    """
    if not isinstance(text, str):
        raise InjectError("text 必须是 str")
    if dry_run:
        # dry_run 在任何平台可跑(单测/Mac 验证用): 不触碰 win32 API
        print(f"# 注入文本 {len(text)} 字符: {text!r}")
        print("# [win32clipboard] 写入剪贴板(CF_UNICODETEXT)")
        print("# [SendInput] Ctrl+V 粘贴到前台窗口")
        return
    if sys.platform != "win32":
        raise InjectError(
            "仅支持 Windows: 注入依赖 win32clipboard 与 SendInput, "
            f"当前平台 {sys.platform} 不支持")
    # P6 实现: win32clipboard.SetClipboardText + SendInput + 前台护栏
    raise InjectError("Windows 注入尚未实现(companion/inject_win.py P6 阶段), "
                      "请暂用 --no-inject 运行")


def _main():
    ap = argparse.ArgumentParser(description="Windows 剪贴板粘贴注入")
    ap.add_argument("text", help="要注入的文本")
    ap.add_argument("--dry-run", action="store_true",
                    help="只打印将执行的步骤")
    args = ap.parse_args()
    try:
        paste_text(args.text, dry_run=args.dry_run)
    except InjectError as e:
        print(f"[inject] 错误: {e}", file=sys.stderr)
        print(f"[inject] 指引: {GUIDE}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    _main()
