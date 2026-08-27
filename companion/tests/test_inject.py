#!/usr/bin/env python3
"""inject.key_action 的 clear 分支单测(Mac 侧, 不触碰真实 osascript)。

覆盖:
- 前台为终端类(iTerm2)→ clear 走 Ctrl+U(终端不拦截控制字符,TUI 整行清空)
- 前台非终端 → clear 走 Home+Shift+End+Delete(Chromium 里 Ctrl+U=查看源码,避开)
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


def patch_subprocess(frontmost_bundle, frontmost_name, fail_probe=False):
    """上下文管理器: 替换 inject.subprocess.run, 退出自动还原 —— 防止
    全局替换污染后续测试(mdns_pub/ws_transport 等共用 subprocess 模块)。"""
    import contextlib
    import subprocess as _real

    @contextlib.contextmanager
    def _patch():
        state = {"sent": []}

        def run(cmd, **kw):
            if cmd[0] == "osascript" and "frontmost" in cmd[-1]:
                if fail_probe:
                    return _real.CompletedProcess(cmd, 1, stderr="probe denied")
                q = cmd[-1]
                val = frontmost_bundle if "bundle identifier" in q else frontmost_name
                return _real.CompletedProcess(cmd, 0, stdout=(val or "") + "\n")
            state["sent"].append(cmd)
            return _real.CompletedProcess(cmd, 0)

        orig = inject.subprocess.run
        inject.subprocess.run = run
        try:
            yield state
        finally:
            inject.subprocess.run = orig

    return _patch()


def test_clear_terminal_ctrl_u():
    """前台 iTerm2: clear 只发 Ctrl+U 一条。"""
    with patch_subprocess("com.googlecode.iterm2", "iTerm2") as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("终端 clear 只发 1 条命令", len(sent), 1)
    check("终端 clear 为 Ctrl+U",
          "using control down" in sent[0][-1] and '"u"' in sent[0][-1], True)
    check("终端 clear 不碰 Shift+End", any("119" in c[-1] for c in sent), False)


def test_clear_nonterminal_home_shift_end():
    """前台普通 app(非终端): 保持 Home+Shift+End+Delete 序列。"""
    with patch_subprocess("com.tencent.xinWeChat", "WeChat") as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("非终端 clear 发 3 条命令", len(sent), 3)
    check("非终端含 Home(115)", "key code 115" in sent[0][-1], True)
    check("非终端含 Shift+End(119)", "key code 119" in sent[1][-1], True)
    check("非终端含 Delete(51)", "key code 51" in sent[2][-1], True)


def test_probe_failure_falls_back():
    """前台探测失败(辅助功能被拒): 按非终端降级, 注入仍执行原序列。"""
    with patch_subprocess("", "", fail_probe=True) as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("探测失败降级为 3 条原序列", len(sent), 3)
    check("降级仍含 Home", "key code 115" in sent[0][-1], True)


def test_probe_name_fallback():
    """bundle id 查不到(bundle identifier 查询返回空)→ 进程名兜底命中终端。"""
    with patch_subprocess("", "Ghostty") as state:
        inject.key_action("clear")
    sent = state["sent"]
    check("进程名兜底走 Ctrl+U", len(sent), 1)
    check("进程名兜底为 Ctrl+U", "using control down" in sent[0][-1], True)


def test_enter_unaffected():
    """enter 分支不探测前台, 仍只发 return。"""
    with patch_subprocess("com.googlecode.iterm2", "iTerm2") as state:
        inject.key_action("enter")
    check("enter 只发 1 条", len(state["sent"]), 1)
    check("enter 为 return", "keystroke return" in state["sent"][0][-1], True)


def main():
    test_clear_terminal_ctrl_u()
    test_clear_nonterminal_home_shift_end()
    test_probe_failure_falls_back()
    test_probe_name_fallback()
    test_enter_unaffected()
    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURES:")
        for f in FAILURES:
            print("  -", f)
        sys.exit(1)
    print("\nAll tests passed.")


if __name__ == "__main__":
    main()
