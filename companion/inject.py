#!/usr/bin/env python3
"""macOS 输入注入: 优先 unicode 键盘事件, 剪贴板+粘贴兜底。

两条注入路径(见 paste_text 的 mode 参数):
  - unicode: CGEvent 逐字符键盘事件(Quartz, 需要 pyobjc-framework-Quartz),
    完全不碰系统剪贴板 → 剪贴板历史管理工具(Paste/Maccy 等)零污染;
  - clipboard: pbcopy 写入剪贴板 + osascript 模拟 Cmd+V 粘贴, 注入后
    恢复旧剪贴板内容(仅文本; 剪贴板历史工具仍会留下记录)。

落地校验(2026-08-28 "有时候明明有焦点却没输入上去"): unicode 路径注入后读一次
聚焦元素的字数(原生 AX, 见 _ax_probe), 字数没动就慢速重打一次, 仍不动才判定失败
并回退剪贴板 —— 在此之前"已粘贴 N 字符"只代表"调用没抛异常", 与真落地无关, 失败
在日志里完全看不见。字数拿不到的元素(WPS 文档区等)算"无法校验", 保持放行。

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


# 逐字符事件的间隔。0 间隔时 26 个汉字 52 个事件能在 1~2ms 内全部 post 出去,
# 忙于渲染的目标应用会整批丢掉(2026-08-28 "有时候明明有焦点却没输入上去"的头号
# 嫌疑)。3ms/字对 40 字的转写只多 120ms, 但把事件摊到应用能消费的节奏上。
_TYPE_GAP_S = 0.003
_TYPE_GAP_SLOW_S = 0.015   # 确认一个字都没落地时的重打节奏


def _type_unicode(text, gap=_TYPE_GAP_S):
    """CGEvent 逐字符键盘事件注入(不碰剪贴板, 中文/emoji 无忧)。

    应用视角等同输入法上屏; 终端(Terminal.app/iTerm2)正常接受, 部分
    xterm.js 类终端(VS Code 集成终端)可能不消费 —— 那正是 mode="auto"
    回退剪贴板的场景。依赖 pyobjc-framework-Quartz, 未安装抛 InjectError。
    """
    try:
        from Quartz import (CGEventCreateKeyboardEvent,
                            CGEventKeyboardSetUnicodeString,
                            CGEventPost, CGEventSetFlags, kCGHIDEventTap)
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
        # 抬键也挂同一串 unicode: 只给按下事件挂字符时, 抬键就是一个"裸的
        # keycode 0"(= 'a' 键松开), Qt/Chromium 系应用(WPS/Electron)看抬键
        # 决定是否上屏, 裸抬键会让它把整个字符丢掉。
        for is_down in (True, False):
            ev = CGEventCreateKeyboardEvent(None, 0, is_down)
            CGEventKeyboardSetUnicodeString(ev, units, ch)
            # 必须显式清零修饰位!! 新建事件的 flags 默认继承"系统当前修饰键状态",
            # 而我们自己刚发过的 Cmd+A / Ctrl+U 会把那个状态**粘住**: 实测
            # Cmd+A 之后新建事件的 flags = 0x20100000(Command 位仍在), 于是接
            # 下来每个字符都变成 Cmd+字符被应用当快捷键吃掉 —— TextEdit 实测
            # 文档一个字都没进。这就是"双击清空之后那一句永远打不进去"的真因。
            CGEventSetFlags(ev, 0)
            CGEventPost(kCGHIDEventTap, ev)
        if gap:
            time.sleep(gap)


# clear/enter 的按键序列用"机制中立"的形式描述: (CGEvent keycode, 修饰位,
# 等价 osascript 片段)。两条通道同一份定义, 不会各写一遍写歪。
_CG_CMD = 1 << 20    # kCGEventFlagMaskCommand
_CG_CTRL = 1 << 18   # kCGEventFlagMaskControl

_K_RETURN = (36, 0, "keystroke return")
_K_DELETE = (51, 0, "key code 51")
_K_CMD_A = (0, _CG_CMD, 'keystroke "a" using command down')
_K_CTRL_E = (14, _CG_CTRL, 'keystroke "e" using control down')
_K_CTRL_U = (32, _CG_CTRL, 'keystroke "u" using control down')
_K_CMD_V = (9, _CG_CMD, 'keystroke "v" using command down')


def _cg_keys(keys):
    """CGEvent 发一串按键(keycode + 修饰位), 失败抛 InjectError 让调用方兜底。

    为什么 clear 也要走这条通道(2026-08-28 取证): 转写注入走的是 CGEvent
    (_type_unicode)并且实测有效 —— 日志里成排的"[inject] 已粘贴 N 字符"就是
    它;而 clear 原来只走 osascript(System Events keystroke), 真机日志显示
    osascript **exit 0 但输入框毫无变化**, 失败在日志上完全看不出来。同一台机
    器上一条通道有效、另一条静默无效, 就没有理由让 clear 继续用那条弱的。
    osascript 保留为兜底(缺 Quartz 时仍可用)。
    """
    try:
        from Quartz import (CGEventCreateKeyboardEvent, CGEventPost,
                            CGEventSetFlags, kCGHIDEventTap)
    except ImportError as e:
        raise InjectError(f"缺少 pyobjc-framework-Quartz ({e})") from e
    try:
        from ApplicationServices import AXIsProcessTrusted
    except ImportError:
        pass
    else:
        if not AXIsProcessTrusted():
            # CGEventPost 无授权时静默丢事件(同 _type_unicode 的坑)
            raise InjectError("辅助功能未授权, CGEvent 按键被系统拦截。" + GUIDE)
    for code, flags in keys:
        for is_down in (True, False):
            ev = CGEventCreateKeyboardEvent(None, code, is_down)
            # 无条件设(flags=0 也要设): 原来"flags 为 0 就不设"等于沿用系统
            # 当前修饰键状态, 于是 Cmd+A 之后那个 Delete 实际发出去的是
            # Cmd+Delete, 后续 _type_unicode 的每个字符也都带着 Cmd。
            CGEventSetFlags(ev, flags)
            CGEventPost(kCGHIDEventTap, ev)
        time.sleep(0.02)   # 给目标应用处理"全选"再收"删除"的间隙


# 只有这些 AX role 的"字数"可信, 才拿它当注入落地的判据。其余(WPS 文档区是
# AXSplitGroup、Electron 里常见 AXGroup/AXUnknown)一律算"无法校验" —— 校验不了
# 就保持旧行为放行, 绝不能把校验手段的缺失当成注入失败去重打(会打两遍)。
_TEXTISH_AX_ROLES = ("AXTextField", "AXTextArea", "AXComboBox", "AXSearchField")


def _ax_probe():
    """(前台 app 标识, 聚焦元素 AX role, 聚焦文本字数); 取不到的项为 None。

    走原生 AXUIElement(NSWorkspace 取前台 pid → AXUIElementCreateApplication):
    实测 0~40ms, 而同样三件事用 osascript 要三条脚本、每条 ~190ms。字数
    (AXNumberOfCharacters, 退到 len(AXValue))是注入第一次有了可校验的成功判据
    —— 在此之前"已粘贴 N 字符"只表示"没抛异常", 和真落地无关。
    探测永不抛异常: 失败返回 None, 由调用方按"不知道"处理。
    """
    try:
        from AppKit import NSWorkspace
        from ApplicationServices import (AXUIElementCopyAttributeValue,
                                         AXUIElementCreateApplication,
                                         AXUIElementSetMessagingTimeout)
    except ImportError:
        return None, None, None
    try:
        app = NSWorkspace.sharedWorkspace().frontmostApplication()
        if app is None:
            return None, None, None
        ident = app.bundleIdentifier() or app.localizedName()
        ident = str(ident) if ident else None
        ael = AXUIElementCreateApplication(app.processIdentifier())
        AXUIElementSetMessagingTimeout(ael, 1.0)   # 目标应用卡住时别拖住注入
        err, el = AXUIElementCopyAttributeValue(ael, "AXFocusedUIElement", None)
        if err != 0 or el is None:
            return ident, None, None
        err, role = AXUIElementCopyAttributeValue(el, "AXRole", None)
        role = str(role) if (err == 0 and role) else None
        err, n = AXUIElementCopyAttributeValue(el, "AXNumberOfCharacters", None)
        if err != 0 or not isinstance(n, int):
            err, val = AXUIElementCopyAttributeValue(el, "AXValue", None)
            n = len(val) if (err == 0 and isinstance(val, str)) else None
        return ident, role, n
    except Exception:   # noqa: BLE001 探测失败等价于"不知道", 不能阻断注入
        return None, None, None


def _settled_len(before, tries=3, delay=0.12):
    """等聚焦元素字数变化(应用更新 AX 有延迟), 最多 tries×delay。

    返回最后读到的字数; 读不到返回 None(= 无法校验, 不等于 0)。
    """
    last = before
    for _ in range(tries):
        time.sleep(delay)
        _, _, n = _ax_probe()
        if n is None:
            return None
        last = n
        if n != before:
            return n
    return last


def _verifiable(ident, role, n):
    """字数这个判据在当前聚焦元素上可信吗?

    终端类应用必须排除: Terminal.app 插入一行后 AXNumberOfCharacters /
    len(AXValue) 实测仍是 99 不动, Warp 更是恒定报 0 —— 字数不动在这里不等于
    "没落地"。若拿它当判据, 会把成功的注入判成失败, 于是重打一次再粘贴一次,
    结果是终端里出现两三份重复文本, 比原来的"静默失败"更糟。
    """
    if n is None or role not in _TEXTISH_AX_ROLES:
        return False
    if ident and (ident in _TERMINAL_APPS
                  or ident in _TERMINAL_APPS.values()):
        return False
    return True


def _type_and_verify(text):
    """逐字符注入并尽量校验落地。

    返回 True = 已确认落地 / 无法校验(保持旧行为放行);
    返回 False = 确认一个字都没进去(调用方决定回退剪贴板还是报错)。
    只在"字数一点没动"时重打一次 —— 部分落地绝不重打, 否则会叠字。
    """
    ident, role, before = _ax_probe()
    where = f"目标={ident!r} role={role!r}"
    _type_unicode(text)
    if not _verifiable(ident, role, before):
        print(f"[inject] {where} 无法校验落地(该元素不报可信字数)")
        return True
    want = len(text.encode("utf-16-le")) // 2
    after = _settled_len(before)
    if after is None or after != before:
        print(f"[inject] {where} 字数 {before}→{after}(期望 +{want})")
        return True
    print(f"[inject] {where} 字数没变({before}), 慢速重打一次", file=sys.stderr)
    _type_unicode(text, gap=_TYPE_GAP_SLOW_S)
    after = _settled_len(before)
    if after is None or after != before:
        print(f"[inject] {where} 重打后字数 {before}→{after}(期望 +{want})")
        return True
    return False


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
        if dry_run:
            print(f"# 注入文本 {len(text)} 字符: {text!r}")
            print("# [CGEvent] 逐字符键盘事件注入(不碰剪贴板)")
            print("# [校验] 注入后读聚焦元素字数, 一个字没进就重打, 仍不进则回退")
            if mode == "auto":
                print("# (auto: 基础设施不可用/两次都没落地时回退剪贴板, 见下)")
            return
        try:
            landed = _type_and_verify(text)
        except InjectError as e:
            if mode == "unicode":
                raise
            print(f"[inject] unicode 注入不可用, 回退剪贴板: {e}",
                  file=sys.stderr)
        else:
            if landed:
                return
            # 校验说得很确定: 字数一点没动, 两次都没动。这正是原来只会打出
            # "已粘贴 N 字符"、用户却看着输入框空着的那种失败。
            if mode == "unicode":
                raise InjectError(
                    "unicode 注入两次都没落到输入框(聚焦元素字数无变化)。"
                    "目标应用可能不吃合成键盘事件, 试 --mode clipboard。")
            print("[inject] unicode 两次都没落地, 回退剪贴板粘贴", file=sys.stderr)

    # clipboard: 备份 → 写入 → Cmd+V → 恢复旧文本(非文本内容无法恢复)
    old_ok, old = (False, None)
    verify_before = None
    if not dry_run:
        old_ok, old = _clipboard_snapshot()
        v_ident, v_role, n = _ax_probe()
        if _verifiable(v_ident, v_role, n):
            verify_before = n

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

    # Cmd+V 也优先 CGEvent: osascript(System Events)在打包 App 里实测会
    # exit 0 却毫无效果(见 _cg_keys 的取证注释), 那条通道只能当兜底。
    try:
        _cg_keys([(_K_CMD_V[0], _K_CMD_V[1])])
    except InjectError as e:
        print(f"[inject] CGEvent 粘贴不可用, 回退 osascript: {e}", file=sys.stderr)
        p = subprocess.run(cmds[1], capture_output=True, text=True)
        if p.returncode != 0:
            err = p.stderr.strip()
            raise InjectError("粘贴失败(osascript 被拒)。" + GUIDE
                              + "\n原始错误: " + err)

    # 粘贴同样要校验: 走到这条路多半是 unicode 已经确认没落地, 若粘贴也没落地,
    # 那就必须报错 —— 再打印一句"已粘贴 N 字符"就是第二次骗人。最常见的成因是
    # 焦点其实不在输入框上(前台窗口是对的, 但聚焦元素是标题/消息之类的静态文本)。
    # _settled_len 本身要等 ~0.4s, 顺带充当"给目标应用消费粘贴内容"的间隔。
    if verify_before is not None and _settled_len(verify_before) == verify_before:
        # 故意不恢复旧剪贴板: 这一句话是用户刚说的、丢了就没了, 留在剪贴板里
        # 他能自己 Cmd+V 救回来; 旧剪贴板内容相比之下可再生。
        raise InjectError(
            "注入没落到输入框(逐字符与剪贴板粘贴都试过, 聚焦元素字数没变)。"
            "多半是焦点不在输入框上 —— 点一下要输入的输入框, 文本已留在剪贴板, "
            "按 Cmd+V 即可贴入。")

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


def _frontmost_ident():
    """前台 app 的 bundle id(取不到则退到进程名); 都取不到返回 None。"""
    queries = [
        'tell application "System Events" to get bundle identifier of '
        'first application process whose frontmost is true',
        'tell application "System Events" to get name of '
        'first application process whose frontmost is true',
    ]
    for query in queries:
        try:
            p = subprocess.run(["osascript", "-e", query],
                               capture_output=True, text=True, timeout=3)
        except Exception:
            return None
        if p.returncode == 0 and p.stdout.strip():
            return p.stdout.strip()
    return None


def _frontmost_is_terminal():
    """前台 app 是否为终端类。探测失败按"非终端"降级(不改变注入路径)。"""
    val = _frontmost_ident()
    if val is None:
        return False
    return val in _TERMINAL_APPS or val in _TERMINAL_APPS.values()


# Cmd+A + Delete 是"清空输入框"在 macOS 的唯一正解, 但它在**非输入框**里可能是
# 破坏性的(Mail 里 Cmd+A + Delete = 删掉当前邮箱视图里的全部邮件)。所以清空前
# 先问一句"现在聚焦的是什么元素", 只挡列表类元素; AX 查询失败(权限/超时/
# 应用不实现)则放行——宁可少一道保险, 也不能让功能整体失灵。
# 2026-08-28 改为黑名单: 原来是白名单(只允许 AXTextField/AXTextArea/…),
# 但真机上 role 千奇百怪(Electron/网页里常见 AXGroup/AXUnknown, 查询失败还会是
# None), 白名单会把"其实是输入框"的场合整体挡死, 而这种挡死在日志里只留一行异常,
# 用户看到的就是"功能又不好用了"。真正需要拦的只有列表类元素 —— Cmd+A + Delete
# 在邮件列表/文件列表里删的是数据。其余一律放行。
_DANGEROUS_AX_ROLES = ("AXTable", "AXOutline", "AXList", "AXRow", "AXCell",
                       "AXBrowser", "AXCollection")


def _focused_ax_role():
    """当前聚焦元素的 AX role; 拿不到返回 None(表示"不知道", 不阻断)。"""
    script = ('tell application "System Events" to get role of '
              '(value of attribute "AXFocusedUIElement" of '
              '(first application process whose frontmost is true))')
    try:
        p = subprocess.run(["osascript", "-e", script],
                           capture_output=True, text=True, timeout=3)
    except Exception:
        return None
    if p.returncode != 0:
        return None
    return p.stdout.strip() or None


def key_action(action, dry_run=False):
    """把按键动作注入当前聚焦输入框(不依赖剪贴板)。

    action: "enter"(回车提交)或 "clear"(清空输入框)。
    clear 按前台 app 分路径:
      - 终端类(Terminal/iTerm2/Warp/…): Ctrl+E 到行尾 + Ctrl+U 删到行首
        (TUI 里 Cmd+A 是"选中全部输出", 不是选中输入行);
      - 非终端: Cmd+A 全选 + Delete(Chromium 里 Ctrl+U=查看源码, 避开;
        Home/Shift+End 在 macOS 只滚动不移光标, 见下方实测注释)。
        下手前先看聚焦元素的 AX role, 非文本元素直接报错不动手。
    通道: 优先 CGEvent(与 paste_text 的 unicode 路径同一条, 实测有效),
    缺 Quartz 时回退 osascript/System Events。两条都需要「辅助功能」授权。
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

    need_text_focus = False   # clear 的非终端分支才需要"聚焦在输入框"这道保险
    ident = None
    ax_role = None
    branch = "enter回车"
    if action == "enter":
        keys = [_K_RETURN]
    else:   # clear
        # 一次原生 AX 查询同时拿到前台 app 与聚焦元素 role, 顶替原来三条
        # osascript(~190ms/条)。缺 pyobjc 时逐项回退到 osascript 探测。
        ident, ax_role, _ = _ax_probe()
        if ident is None:
            ident = _frontmost_ident()
        is_term = ident is not None and (ident in _TERMINAL_APPS
                                         or ident in _TERMINAL_APPS.values())
        if is_term:
            # 终端: Ctrl+E 到行尾再 Ctrl+U 删到行首 —— readline/zle 里 Ctrl+U 只
            # 删光标之前, 光标停在中间时尾巴会留下; 先到行尾才是"整行清空"。
            keys = [_K_CTRL_E, _K_CTRL_U]
            branch = "终端Ctrl+E/U"
        else:
            # 2026-08-28 实测修正: 原来用 Home + Shift+End + Delete(Windows 习惯),
            # 在 macOS 上是错的 —— key code 115(Home)映射到 scrollToBeginningOf-
            # Document:, 只滚动视图, **不移动插入点**。刚粘完文本时光标本来就在末尾,
            # Shift+End 于是什么都没选中, Delete 退化成退格删一个字符。TextEdit 实测:
            # "AAAABBBBCCCC" → "AAAABBBBCCC"。Cmd+A(全选聚焦字段)+ Delete 才是
            # macOS 的正解, 同一实测下清空干净。
            need_text_focus = True   # 真正下手前先验聚焦元素(见 _DANGEROUS_AX_ROLES)
            keys = [_K_CMD_A, _K_DELETE]
            branch = "Cmd+A/Delete"
    cmds = [["osascript", "-e", "tell application \"System Events\" to " + osa]
            for _code, _flags, osa in keys]
    if dry_run:
        print(f"# 按键动作 {action!r}:")
        for c in cmds:
            print("$", " ".join(c))
        return

    # 避险闸门只在真正注入时生效(dry-run 只是打印, 没有破坏性)
    role = None
    if need_text_focus:
        role = ax_role if ax_role is not None else _focused_ax_role()
        if role in _DANGEROUS_AX_ROLES:
            raise InjectError(
                f"当前聚焦的是列表类元素(AX role={role}), 跳过清空 —— "
                "Cmd+A + Delete 在邮件/文件列表里会删数据。"
                "请先点进要清空的输入框再长按音量-。")

    # 诊断(2026-08-28 "还是不管用"):设备侧证明手势判对了、relay 侧证明事件到了、
    # osascript 还 exit 0 —— 唯一没有记录的就是"这一下打给了谁"。前台 app / 分支 /
    # 聚焦元素三样进日志, 下一次复现不必再猜。
    print(f"[key] {action}: 前台={ident!r} 分支={branch} 焦点role={role!r}")

    try:
        _cg_keys([(code, flags) for code, flags, _osa in keys])
        return
    except InjectError as e:
        print(f"[key] CGEvent 通道不可用, 回退 osascript: {e}", file=sys.stderr)

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
