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
