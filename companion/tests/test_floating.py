#!/usr/bin/env python3
"""floating.py 候选字悬浮窗测试。

纯函数(_pick_family/_anchor)无显示环境直接断言; 窗口用例按
test_fre_app_smoke.py 的 SKIP 模式(无显示环境打印 SKIP)。

运行: companion/.venv/bin/python -m pytest companion/tests/test_floating.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from floating import _pick_family, _anchor  # noqa: E402


# ---- 纯函数(无窗口) ----

def test_pick_family():
    assert _pick_family(["Arial", "Microsoft YaHei UI"],
                        ("Microsoft YaHei UI", "Microsoft YaHei", "SimHei")) \
        == "Microsoft YaHei UI"
    assert _pick_family(["Arial"], ("Microsoft YaHei UI", "SimHei")) is None
    assert _pick_family(["SimHei"], ("Microsoft YaHei UI", "SimHei")) == "SimHei"
    print("[PASS] _pick_family 平台字体候选链")


def test_anchor():
    # 底部居中: 窗底落在屏幕 80% 高度线
    assert _anchor(1920, 1080, 360, 120) == ((1920 - 360) // 2,
                                             int(1080 * 0.8) - 120)
    # 窗口过高: y 钳制到 0 不越界
    assert _anchor(800, 600, 400, 500) == (200, 0)
    # 窗口过宽: x 钳制到 0
    assert _anchor(300, 300, 400, 100) == (0, 140)
    print("[PASS] _anchor 底部居中定位")


# ---- 窗口用例(无显示环境 SKIP) ----

def test_floating_window_show_hide():
    try:
        import ttkbootstrap as ttk
        from floating import FloatingCandidate
        root = ttk.Window(themename="darkly")
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    f = FloatingCandidate(root)
    f.show("你好世界")
    root.update()
    assert f._win is not None and f._win.winfo_ismapped(), "show 后窗口应可见"
    assert f._label.cget("text") == "你好世界", "候选文本应更新"
    assert f._win.attributes("-topmost"), "窗口应置顶"
    assert f._win.overrideredirect(), "窗口应无边框"
    # show 幂等: 第二次 show 更新文本不重建窗口
    win = f._win
    f.show("第二段候选")
    root.update()
    assert f._win is win, "show 不应重建窗口"
    assert f._label.cget("text") == "第二段候选", "幂等更新文本"
    f.hide()
    root.update()
    root.update()   # macOS overrideredirect 窗口 withdraw 状态延迟一拍
    assert not f._win.winfo_ismapped(), "hide 后窗口应隐藏"
    f.show("再次显示")      # hide 后复用窗口
    root.update()
    assert f._win is win and f._win.winfo_ismapped(), "hide 后 show 复用窗口"
    f.destroy()
    root.destroy()
    print("[PASS] 悬浮窗 show/hide 生命周期")


if __name__ == "__main__":
    test_pick_family()
    test_anchor()
    test_floating_window_show_hide()
    print("全部通过")
