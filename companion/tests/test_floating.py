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
        import time  # noqa: F401
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
    # show 幂等: 第二次 show 更新文本不重建窗口(等过合并窗口, 模拟真实帧间隔)
    win = f._win
    time.sleep(0.15)
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


def test_floating_show_same_text_noop():
    """重复 partial 文本 show() 应短路(零 configure/geometry); hide 重置后
    同文本不得被吞(新会话首帧)。"""
    try:
        import time  # noqa: F401
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
    time.sleep(0.15)            # 过合并窗口: 后续 show 均立即渲染

    configured, moved = [], []
    orig_cfg = f._label.configure
    def spy_cfg(**kw):
        configured.append(kw)
        return orig_cfg(**kw)
    f._label.configure = spy_cfg
    orig_geo = f._win.geometry
    def spy_geo(*a):
        moved.append(a)
        return orig_geo(*a)
    f._win.geometry = spy_geo

    f.show("你好世界")               # 同文本: 应短路
    root.update()
    assert configured == [], f"重复文本不应再 configure({configured})"
    assert moved == [], f"重复文本不应重定位({moved})"
    assert f._last_text == "你好世界"

    f.show("你好, 天气")             # 新文本: 正常刷新
    root.update()
    assert len(configured) == 1, "新文本应触发 configure"
    assert f._label.cget("text") == "你好, 天气"

    f.hide()                         # hide 重置幂等缓存
    root.update()
    root.update()   # macOS overrideredirect 窗口 withdraw 状态延迟一拍
    f.show("你好世界")               # 同文本但已 hide: 不得被短路吞
    root.update()
    assert len(configured) == 2, "hide 后同文本应正常显示"
    assert f._win.winfo_ismapped(), "hide 后 show 应可见"
    f.destroy()
    root.destroy()
    print("[PASS] show 同文本幂等短路 + hide 重置")


def test_floating_high_frequency_merge():
    """高频 partial 帧合并: 首帧立即渲染, 合并窗口内只刷最新帧。"""
    try:
        import time  # noqa: F401
        import ttkbootstrap as ttk
        from floating import FloatingCandidate
        root = ttk.Window(themename="darkly")
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    f = FloatingCandidate(root)
    f.show("第一帧")
    root.update()
    assert f._label.cget("text") == "第一帧", "首帧应立即渲染"
    f.show("第二帧")
    f.show("第三帧")
    root.update()
    assert f._label.cget("text") == "第一帧", "合并期内不应逐帧渲染"
    time.sleep(0.25)          # > _FLUSH_MS(120ms), 合并窗口已过
    root.update()
    assert f._label.cget("text") == "第三帧", "合并期后应刷新为最新帧"
    f.destroy()
    root.destroy()
    print("[PASS] 高频 partial 帧合并")


if __name__ == "__main__":
    test_pick_family()
    test_anchor()
    test_floating_window_show_hide()
    test_floating_show_same_text_noop()
    test_floating_high_frequency_merge()
    print("全部通过")
