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
                     ax_role="AXTextArea", fail_ax=False, cg="fail"):
    """上下文管理器: 替换 inject.subprocess.run, 退出自动还原 —— 防止
    全局替换污染后续测试(同进程共用 subprocess 模块)。

    cg 控制 CGEvent 通道(生产路径首选它, 见 inject._cg_keys):
      "fail"   模拟缺 Quartz → 回退 osascript, 断言仍看 state["sent"];
      "record" 通道可用 → 按键记进 state["cg"], osascript 一条都不该发。
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
        inject.subprocess.run = run
        inject._cg_keys = cg_keys
        try:
            yield state
        finally:
            inject.subprocess.run = orig
            inject._cg_keys = orig_cg

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
    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURES:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\nAll tests passed.")


if __name__ == "__main__":
    main()
