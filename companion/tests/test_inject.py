#!/usr/bin/env python3
"""inject.key_action 的 clear 分支单测(Mac 侧, 不触碰真实 osascript)。

覆盖:
- 前台为终端类(iTerm2)→ clear 走 Ctrl+E + Ctrl+U(整行清空, 不依赖光标位置)
- 前台非终端 → clear 走 Cmd+A + Delete(macOS 上 Home 只滚动不移光标, 见 inject.py)
- 非终端且聚焦元素不是文本类 → 直接拒绝, 一个键都不发(Cmd+A+Delete 在邮件列表里删数据)
- 前台探测失败(辅助功能未授权/脚本超时)→ 按非终端降级, 不改变注入路径
- 进程名兜底(bundle id 查不到时用进程名)
- enter 分支不受影响

真实按键注入(焦点/CGEvent/辅助功能授权)为 Mac 真机项, 见 NOT RUN 清单。
运行: companion/.venv/bin/python companion/tests/test_inject.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import inject  # noqa: E402

FAILURES = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{name}: got {got!r}, want {want!r}")
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")


def patch_subprocess(frontmost_bundle, frontmost_name, fail_probe=False,
                     ax_role="AXTextArea", fail_ax=False, cg="fail",
                     native=None):
    """上下文管理器: 替换 inject.subprocess.run, 退出自动还原 —— 防止
    全局替换污染后续测试(同进程共用 subprocess 模块)。

    cg 控制 CGEvent 通道(生产路径首选它, 见 inject._cg_keys):
      "fail"   模拟缺 Quartz → 回退 osascript, 断言仍看 state["sent"];
      "record" 通道可用 → 按键记进 state["cg"], osascript 一条都不该发。
    native 控制原生 AX 探测 _ax_probe 的返回值(生产路径首选它):
      None(缺省) 模拟缺 pyobjc → (None, None, None), 逐项回退 osascript 探测,
                 上面那批 osascript 断言仍然有效;
      (ident, role, nchars) 模拟原生探测可用 —— 此时不该再发探测用 osascript。
    """
    import contextlib
    import subprocess as _real

    @contextlib.contextmanager
    def _patch():
        state = {"sent": [], "cg": []}

        def cg_keys(keys):
            state["cg"].append(list(keys))
            if cg == "fail":
                raise inject.InjectError("fake: 缺 Quartz")

        def run(cmd, **kw):
            # AX 聚焦元素查询也含 "frontmost", 必须先分流(否则被当成前台探测)
            if cmd[0] == "osascript" and "AXFocusedUIElement" in cmd[-1]:
                if fail_ax:
                    return _real.CompletedProcess(cmd, 1, stderr="ax denied")
                return _real.CompletedProcess(cmd, 0, stdout=(ax_role or "") + "\n")
            if cmd[0] == "osascript" and "frontmost" in cmd[-1]:
                if fail_probe:
                    return _real.CompletedProcess(cmd, 1, stderr="probe denied")
                q = cmd[-1]
                val = frontmost_bundle if "bundle identifier" in q else frontmost_name
                return _real.CompletedProcess(cmd, 0, stdout=(val or "") + "\n")
            state["sent"].append(cmd)
            return _real.CompletedProcess(cmd, 0)

        orig = inject.subprocess.run
        orig_cg = inject._cg_keys
        orig_probe = inject._ax_probe
        inject.subprocess.run = run
        inject._cg_keys = cg_keys
        inject._ax_probe = lambda: (native if native is not None
                                    else (None, None, None))
        try:
            yield state
        finally:
            inject.subprocess.run = orig
            inject._cg_keys = orig_cg
            inject._ax_probe = orig_probe

    return _patch()


def test_clear_terminal_ctrl_u():
    """前台 iTerm2: clear 发 Ctrl+E + Ctrl+U, 不碰 Cmd+A。"""
    with patch_subprocess("com.googlecode.iterm2", "iTerm2") as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("终端 clear 发 2 条命令", len(sent), 2)
    check("终端 clear 先 Ctrl+E",
          "using control down" in sent[0][-1] and '"e"' in sent[0][-1], True)
    check("终端 clear 再 Ctrl+U",
          "using control down" in sent[1][-1] and '"u"' in sent[1][-1], True)
    check("终端 clear 不碰 Cmd+A",
          any("command down" in c[-1] for c in sent), False)


def test_clear_nonterminal_select_all():
    """前台普通 app(非终端): Cmd+A 全选 + Delete —— macOS 上唯一能清干净的组合。

    2026-08-28 TextEdit 实测: 旧的 Home+Shift+End+Delete 只删掉一个字符
    ("AAAABBBBCCCC" → "AAAABBBBCCC"), 因为 key code 115 只滚动视图不移光标。
    """
    with patch_subprocess("com.tencent.xinWeChat", "WeChat") as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("非终端 clear 发 2 条命令", len(sent), 2)
    check("非终端含 Cmd+A",
          '"a"' in sent[0][-1] and "command down" in sent[0][-1], True)
    check("非终端含 Delete(51)", "key code 51" in sent[1][-1], True)
    check("非终端不再用 Home(115)", any("key code 115" in c[-1] for c in sent), False)


def test_clear_nonterminal_refuses_non_text_focus():
    """聚焦元素不是文本类(如 Mail 的邮件列表 AXTable): 拒绝, 一个键都不发。"""
    with patch_subprocess("com.apple.mail", "Mail", ax_role="AXTable") as state:
        try:
            inject.key_action("clear")
            check("非文本聚焦应拒绝", False, True)
        except inject.InjectError as e:
            check("非文本聚焦应拒绝", "AX role=AXTable" in str(e), True)
    check("拒绝时不发任何按键", len(state["sent"]), 0)


def test_clear_ax_probe_failure_proceeds():
    """AX 查询失败(应用不实现/超时): 放行, 不让保险把功能整体卡死。"""
    with patch_subprocess("com.tencent.xinWeChat", "WeChat", fail_ax=True) as state:
        inject.key_action("clear")
    check("AX 查询失败仍注入 2 条", len(state["sent"]), 2)


def test_probe_failure_falls_back():
    """前台探测失败(辅助功能被拒): 按非终端降级, 注入仍执行。"""
    with patch_subprocess("", "", fail_probe=True) as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("探测失败降级为 2 条", len(sent), 2)
    check("降级仍含 Cmd+A", "command down" in sent[0][-1], True)


def test_probe_name_fallback():
    """bundle id 查不到(bundle identifier 查询返回空)→ 进程名兜底命中终端。"""
    with patch_subprocess("", "Ghostty") as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("进程名兜底走终端分支", len(sent), 2)
    check("进程名兜底为 Ctrl+E/U",
          all("using control down" in c[-1] for c in sent), True)


def test_clear_prefers_cgevent():
    """CGEvent 通道可用时 clear 走它, 一条 osascript 按键都不发。

    2026-08-28 取证: 转写注入(CGEvent)有效, 而 clear 的 osascript 通道
    exit 0 却毫无效果 —— 所以 clear 也改走 CGEvent, osascript 只兜底。
    """
    with patch_subprocess("com.tencent.xinWeChat", "WeChat", cg="record") as state:
        inject.key_action("clear")
    check("CGEvent 通道被调用一次", len(state["cg"]), 1)
    check("CGEvent 序列为 Cmd+A 再 Delete",
          state["cg"][0], [(0, inject._CG_CMD), (51, 0)])
    check("CGEvent 生效后不发 osascript 按键", len(state["sent"]), 0)


def test_clear_unknown_role_proceeds():
    """聚焦元素 role 陌生(Electron/网页常见 AXGroup): 放行, 不再被白名单挡死。"""
    with patch_subprocess("com.tencent.xinWeChat", "WeChat",
                          ax_role="AXGroup", cg="record") as state:
        inject.key_action("clear")
    check("陌生 role 仍注入", len(state["cg"]), 1)


def test_enter_unaffected():
    """enter 分支不探测前台, 仍只发 return。"""
    with patch_subprocess("com.googlecode.iterm2", "iTerm2") as state:
        inject.key_action("enter")
    check("enter 只发 1 条", len(state["sent"]), 1)
    check("enter 为 return", "keystroke return" in state["sent"][0][-1], True)


# --- 原生 AX 探测(生产首选路径) ---

def test_clear_native_probe_no_osascript():
    """原生探测可用时: clear 不再发任何探测用 osascript, 键序不变。"""
    with patch_subprocess("", "", cg="record",
                          native=("com.tencent.xinWeChat", "AXTextArea", 7)) as st:
        inject.key_action("clear")
    check("原生探测下不发 osascript", len(st["sent"]), 0)
    check("原生探测下键序仍 Cmd+A/Delete", st["cg"][0],
          [(0, inject._CG_CMD), (51, 0)])


def test_clear_native_dangerous_role_refuses():
    """原生探测拿到列表类 role: 同样拒绝下手(闸门不依赖探测通道)。"""
    with patch_subprocess("", "", cg="record",
                          native=("com.apple.mail", "AXTable", None)) as st:
        try:
            inject.key_action("clear")
            check("列表类 role 应拒绝", False, True)
        except inject.InjectError as e:
            check("列表类 role 拒绝", "列表类元素" in str(e), True)
    check("拒绝时一个键都不发", len(st["cg"]), 0)


# --- 落地校验(unicode 路径) ---

def patch_typing(lands, role="AXTextArea", ident="com.tencent.xinWeChat",
                 start=0, countable=True):
    """替换 _ax_probe / _type_unicode / _cg_keys / time.sleep / subprocess.run。

    lands: 每次注入(逐字符打字, 或剪贴板 Cmd+V)让聚焦元素字数增加多少 —— 依次
    取用, 用尽后重复最后一个。0 = 那次注入一个字都没落地。这样建模比"直接给
    _ax_probe 排一串返回值"贴近真实: 字数只会因为注入而变。
    countable=False 模拟不报字数的元素(WPS 文档区)。
    state["typed"] 记每次 _type_unicode 的 gap, state["sent"] 记外部命令。
    """
    import contextlib
    import subprocess as _real

    @contextlib.contextmanager
    def _patch():
        state = {"typed": [], "sent": [], "cg": [], "n": start}
        seq = list(lands)

        def land():
            state["n"] += seq.pop(0) if len(seq) > 1 else seq[0]

        def probe():
            return ident, role, (state["n"] if countable else None)

        def type_unicode(text, gap=inject._TYPE_GAP_S):
            state["typed"].append(gap)
            land()

        def cg_keys(keys):
            state["cg"].append(list(keys))
            if list(keys) == [(inject._K_CMD_V[0], inject._CG_CMD)]:
                land()   # Cmd+V 粘贴也算一次注入

        def run(cmd, **kw):
            state["sent"].append(cmd[0])
            # _clipboard_snapshot: 报"有文本"且旧内容为 OLD —— 这样"失败时不恢复
            # 旧剪贴板(把这句话留给用户 Cmd+V)"才是可断言的(pbcopy 次数)。
            if cmd[0] == "osascript":
                return _real.CompletedProcess(cmd, 0, stdout="text")
            if cmd[0] == "pbpaste":
                return _real.CompletedProcess(cmd, 0, stdout=b"OLD")
            return _real.CompletedProcess(cmd, 0, stdout=b"")

        orig = (inject._ax_probe, inject._type_unicode, inject.subprocess.run,
                inject.time.sleep, inject._cg_keys)
        inject._ax_probe = probe
        inject._type_unicode = type_unicode
        inject.subprocess.run = run
        inject.time.sleep = lambda _s: None
        inject._cg_keys = cg_keys
        try:
            yield state
        finally:
            (inject._ax_probe, inject._type_unicode, inject.subprocess.run,
             inject.time.sleep, inject._cg_keys) = orig

    return _patch()


def test_type_verify_landed_once():
    """字数按预期增长: 只打一遍, 不碰剪贴板。"""
    with patch_typing([4]) as st:
        inject.paste_text("你好世界")
    check("落地只打一遍", len(st["typed"]), 1)
    check("落地不碰剪贴板", st["sent"], [])


def test_type_verify_partial_no_retype():
    """只落地一部分(丢字): 也不重打 —— 重打会叠字, 宁可让用户看见少字。"""
    with patch_typing([2]) as st:
        inject.paste_text("你好世界")
    check("部分落地不重打", len(st["typed"]), 1)


def test_type_verify_retries_then_clipboard():
    """两遍打字都没落地 → 回退剪贴板粘贴, 粘贴落地则算成功。"""
    with patch_typing([0, 0, 4]) as st:
        inject.paste_text("你好世界")
    check("重打一次(共两遍)", len(st["typed"]), 2)
    check("第二遍用慢速节奏", st["typed"][1], inject._TYPE_GAP_SLOW_S)
    check("回退剪贴板(pbcopy 被调)", "pbcopy" in st["sent"], True)
    check("Cmd+V 走 CGEvent", st["cg"][-1],
          [(inject._K_CMD_V[0], inject._CG_CMD)])
    # 粘贴成功 → 写入 + 恢复旧剪贴板 = pbcopy 两次
    check("成功后恢复旧剪贴板", st["sent"].count("pbcopy"), 2)


def test_type_verify_all_channels_fail_raises():
    """打字两遍 + 粘贴都没落地: 必须报错, 不许再打印"已粘贴 N 字符"。"""
    with patch_typing([0]) as st:
        try:
            inject.paste_text("你好世界")
            check("三次都没落地应报错", False, True)
        except inject.InjectError as e:
            check("报错点明焦点问题", "焦点不在输入框" in str(e), True)
            check("报错指路 Cmd+V", "Cmd+V" in str(e), True)
    check("报错前试过剪贴板", "pbcopy" in st["sent"], True)
    # 失败时把这句话留在剪贴板(不恢复旧内容): pbcopy 只该被调一次
    check("失败不恢复旧剪贴板", st["sent"].count("pbcopy"), 1)


def test_type_verify_unicode_mode_raises():
    """unicode 模式不许碰剪贴板: 两次都没落地就报错(而不是假成功)。"""
    with patch_typing([0]) as st:
        try:
            inject.paste_text("你好世界", mode="unicode")
            check("unicode 模式应报错", False, True)
        except inject.InjectError as e:
            check("unicode 模式报错", "没落到输入框" in str(e), True)
    check("unicode 模式不碰剪贴板", "pbcopy" in st["sent"], False)


def test_type_verify_unverifiable_passes():
    """元素不报字数(WPS 文档区 AXSplitGroup): 算无法校验, 打一遍就放行。"""
    with patch_typing([0], role="AXSplitGroup", countable=False) as st:
        inject.paste_text("你好世界")
    check("无法校验只打一遍", len(st["typed"]), 1)
    check("无法校验不回退剪贴板", st["sent"], [])


def test_type_verify_terminal_never_retypes():
    """终端字数判据不可信(Terminal.app 插入后 nchars 实测不动): 一律不重打。

    否则"字数没变"会把成功的注入判成失败, 重打 + 粘贴 = 终端里三份重复文本。
    """
    with patch_typing([0], ident="com.apple.Terminal") as st:
        inject.paste_text("你好世界")
    check("终端只打一遍", len(st["typed"]), 1)
    check("终端不回退剪贴板", st["sent"], [])


def patch_quartz():
    """注入假 Quartz 模块, 记录每个 CGEvent 的 flags 是否被显式设置。

    flags 是这轮真机故障的根因所在: CGEventCreateKeyboardEvent 新建的事件
    默认继承"系统当前修饰键状态", 我们自己刚发过的 Cmd+A 会把 Command 位粘住,
    于是后面每个字符都变成 Cmd+字符被应用当快捷键吃掉(实测一个字都进不去)。
    所以"每个事件都必须显式设过 flags"是硬约束, flags=0 也要设。
    """
    import contextlib
    import types

    @contextlib.contextmanager
    def _patch():
        posted = []

        def create(src, code, is_down):
            # flags=None 代表"从没设过"→ 真机上等于沿用系统粘住的修饰键
            return {"code": code, "down": bool(is_down), "flags": None}

        fake = types.ModuleType("Quartz")
        fake.kCGHIDEventTap = 0
        fake.CGEventCreateKeyboardEvent = create
        fake.CGEventKeyboardSetUnicodeString = lambda ev, n, s: None
        fake.CGEventSetFlags = lambda ev, f: ev.__setitem__("flags", f)
        fake.CGEventPost = lambda tap, ev: posted.append(dict(ev))
        old_mod = sys.modules.get("Quartz")
        old_sleep = inject.time.sleep
        sys.modules["Quartz"] = fake
        inject.time.sleep = lambda _s: None
        try:
            yield posted
        finally:
            inject.time.sleep = old_sleep
            if old_mod is None:
                del sys.modules["Quartz"]
            else:
                sys.modules["Quartz"] = old_mod

    return _patch()


def test_unicode_events_always_set_flags():
    """逐字符注入的每个事件(按下+抬起)都显式清零 flags。"""
    with patch_quartz() as posted:
        inject._type_unicode("ab", gap=0)
    check("2 字 → 4 个事件", len(posted), 4)
    check("每个事件都清零 flags", [e["flags"] for e in posted], [0, 0, 0, 0])


def test_cg_keys_always_set_flags():
    """_cg_keys 的 flags=0 也要显式设 —— 原来"为 0 就不设"正是 bug 所在。"""
    with patch_quartz() as posted:
        inject._cg_keys([(51, 0)])          # 裸 Delete
    check("裸键显式设 flags=0", [e["flags"] for e in posted], [0, 0])
    with patch_quartz() as posted:
        inject._cg_keys([(0, inject._CG_CMD), (51, 0)])   # Cmd+A 然后裸 Delete
    check("带修饰键与裸键各自的 flags",
          [(e["code"], e["flags"]) for e in posted],
          [(0, inject._CG_CMD), (0, inject._CG_CMD), (51, 0), (51, 0)])


def main():
    test_clear_terminal_ctrl_u()
    test_clear_nonterminal_select_all()
    test_clear_nonterminal_refuses_non_text_focus()
    test_clear_ax_probe_failure_proceeds()
    test_probe_failure_falls_back()
    test_probe_name_fallback()
    test_clear_prefers_cgevent()
    test_clear_unknown_role_proceeds()
    test_enter_unaffected()
    test_clear_native_probe_no_osascript()
    test_clear_native_dangerous_role_refuses()
    test_type_verify_landed_once()
    test_type_verify_partial_no_retype()
    test_type_verify_retries_then_clipboard()
    test_type_verify_all_channels_fail_raises()
    test_type_verify_unicode_mode_raises()
    test_type_verify_unverifiable_passes()
    test_type_verify_terminal_never_retypes()
    test_unicode_events_always_set_flags()
    test_cg_keys_always_set_flags()
    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURES:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\nAll tests passed.")


if __name__ == "__main__":
    main()
