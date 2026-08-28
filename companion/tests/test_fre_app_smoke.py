#!/usr/bin/env python3
"""fre_app 向导 dry-run 冒烟: 隐藏窗口走完 5 步全流程。

welcome → discover(自动探测命中) → asr_config(输入 Key) → 连接 → status。
断言: 最终停在 status 页且状态栏含"已连接"。无显示环境自动 SKIP。

运行: companion/.venv/bin/python companion/tests/test_fre_app_smoke.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ttkbootstrap as ttk  # noqa: E402

from fre_app import FREApp, THEME  # noqa: E402


def run_smoke():
    try:
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过冒烟")
        return 0
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)

    def step(n):
        try:
            if n == 0:
                app.start_discover()           # welcome → discover + 探测线程
            elif n == 1:
                assert app.probe_result is not None, "探测未完成"
                app._channel_var.set("usb")    # 手动指定 USB(dry-run 假命令面)
                app.on_discover_next()         # → asr_config
            elif n == 2:
                app._asr_key_var.set("smoke-key")
                app.on_asr_next()              # → connecting → relay 线程 → status
            elif n == 3:
                assert app.current_page == "status", f"页: {app.current_page}"
                assert "已连接" in app._status_var.get(), app._status_var.get()
                print("[PASS] 冒烟: 5 步走通, status 页 + 已连接")
                app.show_diagnostics()         # → 诊断页(USB 假命令面)
            elif n == 4:
                assert app.current_page == "diagnostics", app.current_page
                assert app.syscmd_available, "dry-run 假命令面应可用"
                app.diag_cmd("st")             # 命令桥往返
            elif n == 5:
                out = app._diag_out.get("1.0", "end")
                assert "[fake] st" in out, f"命令响应缺失: {out!r}"
                print("[PASS] 冒烟: 诊断页命令往返")
                root.quit()
                return
            root.after(600, lambda: step(n + 1))
        except Exception as e:  # noqa: BLE001
            print(f"[FAIL] 冒烟 step{n}: {e}")
            root.quit()

    root.after(200, lambda: step(0))
    root.after(20000, root.quit)   # 兜底(relay 正常 <3s)
    root.mainloop()
    root.destroy()
    ok = app.current_page in ("status", "diagnostics")
    print("[PASS] 冒烟: 全流程结束" if ok else
          f"[FAIL] 冒烟: 未到 status/diagnostics(停于 {app.current_page})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(run_smoke())


# ---- 单元: 通道切换 cfg 同步(审查 P1-1) ----
# _relay_worker 的 _build_transport 读 self.cfg: 只写盘不同步内存 cfg,
# 切换不生效。断言 on_channel_change / on_discover_next 后 cfg 同步。
def test_channel_cfg_sync():
    try:
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import FREApp, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    assert app.cfg.get("channel", "ble") == app.channel
    app._channel_var.set("usb")
    app.on_channel_change()
    assert app.cfg["channel"] == "usb", "on_channel_change 未同步内存 cfg"
    app._channel_var.set("usb")
    app.on_discover_next()
    assert app.cfg["channel"] == "usb", "on_discover_next 未同步内存 cfg"
    root.destroy()
    print("[PASS] 通道切换 cfg 同步")


# ---- 单元: 重复 connect 拒绝(审查 P2-2) ----
# 前一 relay 线程存活时再点连接: 不得叠线程, 记 last_error 并留在原页。
def test_connect_duplicate_rejected():
    try:
        import threading
        import time  # noqa: F401
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import FREApp, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    gate = threading.Event()

    def fake_worker():
        gate.wait(30)   # 模拟仍在跑的 relay 线程

    t = threading.Thread(target=fake_worker, daemon=True)
    t.start()
    app._thread = t
    app.connect()
    assert app.last_error, "重复 connect 应记录错误"
    assert app.current_page == "welcome", \
        f"重复 connect 不应离开当前页(实际 {app.current_page})"
    gate.set()
    t.join(timeout=2)
    root.destroy()
    print("[PASS] 重复 connect 被拒绝")


# ---- 单元: 候选字悬浮窗接线(审查: poll 驱动 candidate 事件) ----
# phase_q 灌 candidate → app.poll() → 悬浮窗显示候选字; final → 隐藏。
def test_candidate_poll_wiring():
    try:
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import FREApp, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    from floating import _TkFloat
    app._floating = _TkFloat(root)   # 接线测试锁定 Tk 实现(门面在 macOS 走 NSPanel)
    app.phase_q.put(("candidate", ("你好", False)))
    app.poll()
    root.update()
    assert app._floating is not None and app._floating is not False, \
        "partial 候选应创建悬浮窗"
    assert app._floating._label.cget("text") == "你好", \
        f"悬浮窗应显示候选字(实际 {app._floating._label.cget('text')!r})"
    app.phase_q.put(("candidate", ("你好世界", True)))
    app.poll()
    root.update()
    assert not app._floating._win.winfo_ismapped(), "final 后悬浮窗应隐藏"
    root.destroy()
    print("[PASS] candidate 事件驱动悬浮窗显隐")


# ---- 单元: session_end 不抢关悬浮窗(REVIEW P2-A) ----
# voice.end(session_end) 几乎总早于 ASR final: 抢关会窗闪没再闪回。
# 窗口只能由 final / disconnected / relay_done / error / failed 收口。
def test_session_end_keeps_floating():
    try:
        import time  # noqa: F401
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import FREApp, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    from floating import _TkFloat
    app._floating = _TkFloat(root)   # 接线测试锁定 Tk 实现(门面在 macOS 走 NSPanel)
    app.phase_q.put(("candidate", ("你好", False)))
    app.poll()
    root.update()
    assert app._floating is not None and app._floating is not False
    win = app._floating._win
    # session_end(voice.end)不得关闭窗口
    app.phase_q.put(("phase", "session_end"))
    app.poll()
    root.update()
    assert win.winfo_ismapped(), "session_end 后悬浮窗应保持可见"
    # 后续 partial 正常更新(等过 120ms 帧合并窗口, 模拟真实帧间隔)
    time.sleep(0.15)
    app.phase_q.put(("candidate", ("你好世界", False)))
    app.poll()
    root.update()
    assert app._floating._label.cget("text") == "你好世界", \
        f"session_end 后 partial 应继续更新(实际 {app._floating._label.cget('text')!r})"
    # final 才收口
    app.phase_q.put(("candidate", ("你好世界", True)))
    app.poll()
    root.update()
    assert not win.winfo_ismapped(), "final 后悬浮窗应隐藏"
    # failed 也收口
    app.phase_q.put(("candidate", ("再来", False)))
    app.poll()
    root.update()
    app.phase_q.put(("phase", "failed"))
    app.poll()
    root.update()
    assert not win.winfo_ismapped(), "failed 后悬浮窗应隐藏"
    root.destroy()
    print("[PASS] session_end 不抢关悬浮窗, final/failed 收口")


# ---- 单元: 向导 API Key 同步内存 cfg(REVIEW P2-B) ----
# on_asr_next 只写盘不同步 self.cfg → 状态页 ASR 行误显示「未配置」。
def test_asr_key_syncs_cfg():
    try:
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import FREApp, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    # 不依赖初始为空(本机 config.local.json 可能已配真实 key, 亦不回显它)
    app._asr_key_var.set("review-key")
    app.on_asr_next()
    assert app.cfg.get("volcano_api_key") == "review-key", \
        "on_asr_next 应同步内存 cfg(覆盖为新值)"
    root.destroy()
    print("[PASS] 向导 API Key 同步内存 cfg")


def test_qput_bounds():
    """phase_q 满: candidate 丢自己, 重要事件挤掉最老(PERF P2-1)。"""
    try:
        import queue as queue_mod
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import FREApp, PHASE_Q_MAX, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    for i in range(PHASE_Q_MAX):
        app._qput(("candidate", (str(i), False)))
    assert app.phase_q.qsize() == PHASE_Q_MAX
    app._qput(("candidate", ("overflow", False)))
    assert app.phase_q.qsize() == PHASE_Q_MAX, "满时 candidate 应丢自己"
    app._qput(("phase", "disconnected"))
    assert app.phase_q.qsize() == PHASE_Q_MAX
    kinds = []
    while True:
        try:
            kinds.append(app.phase_q.get_nowait()[0])
        except queue_mod.Empty:
            break
    assert "phase" in kinds, "重要事件应挤进队列"
    root.destroy()
    print("[PASS] phase_q 有界投递")


def test_probe_duplicate_skipped():
    """探测线程存活时 start_discover 不得叠第二个(PERF P2-2)。"""
    try:
        import threading
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import FREApp, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    gate = threading.Event()

    def fake():
        gate.wait(30)

    t = threading.Thread(target=fake, daemon=True)
    t.start()
    app._probe_thread = t
    app.start_discover()
    assert app._probe_thread is t, "不得替换仍在跑的探测线程"
    gate.set()
    t.join(timeout=2)
    root.destroy()
    print("[PASS] 探测互斥")


def test_diag_append_truncates():
    """诊断 Text 超 DIAG_MAX_LINES 截断头部(PERF P2-6)。"""
    try:
        import ttkbootstrap as ttk  # noqa: F811
        from fre_app import DIAG_MAX_LINES, FREApp, THEME  # noqa: F811
        root = ttk.Window(themename=THEME)
    except Exception as e:  # noqa: BLE001
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    app = FREApp(root, dry_run=True, no_tray=True)
    for i in range(DIAG_MAX_LINES + 30):
        app._diag_append(f"L{i}")
    last = int(float(app._diag_out.index("end-1c")))
    assert last <= DIAG_MAX_LINES + 1, f"诊断行数未截断: {last}"
    root.destroy()
    print("[PASS] 诊断页截断")
