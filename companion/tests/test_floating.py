#!/usr/bin/env python3
"""floating.py 候选字悬浮窗测试。

纯函数(_pick_family/_anchor)无显示环境直接断言; 窗口用例按
test_fre_app_smoke.py 的 SKIP 模式(无显示环境打印 SKIP)。

运行: companion/.venv/bin/python -m pytest companion/tests/test_floating.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from floating import (  # noqa: E402
    HWND_TOPMOST, SWP_NOACTIVATE, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
    _anchor, _anchor_cocoa, _pick_family, _present_window,
    _win32_apply_noactivate,
    _win32_exstyle_noactivate,
)


# ---- 纯函数(无窗口) ----

def test_pick_family():
    assert _pick_family(["Arial", "Microsoft YaHei UI"],
                        ("Microsoft YaHei UI", "Microsoft YaHei", "SimHei")) \
        == "Microsoft YaHei UI"
    assert _pick_family(["Arial"], ("Microsoft YaHei UI", "SimHei")) is None
    assert _pick_family(["SimHei"], ("Microsoft YaHei UI", "SimHei")) == "SimHei"
    print("[PASS] _pick_family 平台字体候选链")


def test_anchor():
    # 底部居中: 窗底落在屏幕 80% 高度线(距屏幕底部向上 20%,微信语音输入式)
    assert _anchor(1920, 1080, 360, 120) == ((1920 - 360) // 2,
                                             int(1080 * 0.8) - 120)
    # 窗口过高: y 钳制到 0 不越界
    assert _anchor(800, 600, 400, 500) == (200, 0)
    # 窗口过宽: x 钳制到 0
    assert _anchor(300, 300, 400, 100) == (0, 140)
    print("[PASS] _anchor 底部居中定位")


def test_anchor_cocoa():
    """Cocoa 口径(原点左下)必须落在与 Tk 相同的位置: 下边缘距屏底 20%。"""
    for sw, sh, w, h in ((1920, 1080, 360, 120), (2560, 1440, 400, 88),
                         (1440, 900, 300, 60)):
        x_tk, y_tk = _anchor(sw, sh, w, h)
        x_co, y_co = _anchor_cocoa(sw, sh, w, h)
        assert x_co == x_tk, (x_co, x_tk)
        # 同一落点: Tk 的"上边距顶" + 高 + Cocoa 的"下边距底" = 屏高
        assert y_tk + h + y_co == sh, (y_tk, h, y_co, sh)
        # 下边缘就在 20% 线上(不是 80%: 修复前正是把它顶到了屏幕上部)
        assert y_co == sh - int(sh * 0.8), (y_co, sh)
    # 窗口比屏幕高: 两个口径都夹到 0(不给负原点)
    assert _anchor_cocoa(800, 600, 400, 700) == (200, 0)
    print("[PASS] _anchor_cocoa 与 Tk 同落点")


# ---- Windows 不激活置顶(REVIEW P2-D, 无显示 / 非 Windows 可跑) ----

def test_win32_exstyle_noactivate():
    assert _win32_exstyle_noactivate(0) == (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)
    assert _win32_exstyle_noactivate(0x8) == (
        0x8 | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)
    print("[PASS] _win32_exstyle_noactivate 位或")


class _FakeUser32:
    """注入给 _win32_apply_noactivate / _present_window, 不碰真实 user32。"""

    def __init__(self, styles=None, parent=0, boom=False):
        self.styles = {} if styles is None else dict(styles)
        self.parent = parent
        self.boom = boom
        self.set_pos = []

    def GetParent(self, hwnd):
        return self.parent

    def GetWindowLongPtrW(self, hwnd, _idx):
        if self.boom:
            raise OSError("fake")
        return self.styles.get(hwnd, 0)

    def SetWindowLongPtrW(self, hwnd, _idx, val):
        self.styles[hwnd] = val
        return 0

    def SetWindowPos(self, hwnd, after, _x, _y, _cx, _cy, flags):
        self.set_pos.append((hwnd, after, flags))
        return 1


class _FakeWin:
    def __init__(self, hwnd=42, mapped=False):
        self._hwnd = hwnd
        self.mapped = mapped
        self.deiconified = 0
        self.lifted = 0

    def winfo_id(self):
        return self._hwnd

    def winfo_ismapped(self):
        return self.mapped

    def deiconify(self):
        self.deiconified += 1
        self.mapped = True

    def lift(self):
        self.lifted += 1

    def attributes(self, *a):
        return None


def test_win32_apply_noactivate_sets_bits():
    u = _FakeUser32(styles={42: 0x8})
    assert _win32_apply_noactivate(42, user32=u) is True
    assert u.styles[42] & WS_EX_NOACTIVATE
    assert u.styles[42] & WS_EX_TOOLWINDOW
    assert u.styles[42] & 0x8, "不得清掉已有扩展样式"
    assert u.set_pos, "应 SetWindowPos 置顶"
    hwnd, after, flags = u.set_pos[0]
    assert hwnd == 42 and after == HWND_TOPMOST
    assert flags & SWP_NOACTIVATE
    print("[PASS] _win32_apply_noactivate 写 NOACTIVATE 并不激活置顶")


def test_win32_apply_noactivate_failure():
    assert _win32_apply_noactivate(0) is False
    u = _FakeUser32(boom=True)
    assert _win32_apply_noactivate(42, user32=u) is False
    print("[PASS] _win32_apply_noactivate 失败返回 False")


def test_present_window_win32_skips_lift():
    """Windows 映出窗口: deiconify 但不 lift(lift 会抢前台)。"""
    import floating
    orig_plat = floating.sys.platform
    orig_user32 = floating._win32_user32
    u = _FakeUser32()
    try:
        floating.sys.platform = "win32"
        floating._win32_user32 = lambda: u
        win = _FakeWin()
        _present_window(win)
        assert win.deiconified == 1, "应 deiconify"
        assert win.lifted == 0, "Windows 成功 NOACTIVATE 时不得 lift"
        assert u.styles.get(42, 0) & WS_EX_NOACTIVATE
    finally:
        floating.sys.platform = orig_plat
        floating._win32_user32 = orig_user32
    print("[PASS] _present_window Windows 不 lift")


def test_present_window_win32_lift_on_failure():
    """NOACTIVATE 失败时降级 lift, 窗仍能显示。"""
    import floating
    orig_plat = floating.sys.platform
    orig_user32 = floating._win32_user32
    u = _FakeUser32(boom=True)
    try:
        floating.sys.platform = "win32"
        floating._win32_user32 = lambda: u
        win = _FakeWin()
        _present_window(win)
        assert win.deiconified == 1
        assert win.lifted == 1, "失败应降级 lift"
    finally:
        floating.sys.platform = orig_plat
        floating._win32_user32 = orig_user32
    print("[PASS] _present_window Windows 失败降级 lift")


def test_present_window_darwin_no_lift():
    """macOS: 只 deiconify, 不 lift(lift 会让 WindowServer 转圈)。"""
    import floating
    orig_plat = floating.sys.platform
    try:
        floating.sys.platform = "darwin"
        win = _FakeWin()
        _present_window(win)
        assert win.deiconified == 1
        assert win.lifted == 0, "macOS 不得每帧 lift"
    finally:
        floating.sys.platform = orig_plat
    print("[PASS] _present_window macOS 不 lift")


def test_present_window_other_lifts():
    """非 macOS/Windows: deiconify + lift。"""
    import floating
    orig_plat = floating.sys.platform
    try:
        floating.sys.platform = "linux"
        win = _FakeWin()
        _present_window(win)
        assert win.deiconified == 1 and win.lifted == 1
    finally:
        floating.sys.platform = orig_plat
    print("[PASS] _present_window 其他平台仍 lift")


# ---- 窗口用例(无显示环境 SKIP) ----

def test_floating_window_show_hide():
    try:
        import time  # noqa: F401
        import ttkbootstrap as ttk
        from floating import _TkFloat  # 窗口用例锁定 Tk 实现(门面在 macOS 走 NSPanel)
        root = ttk.Window(themename="darkly")
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    f = _TkFloat(root)
    f.show("你好世界")
    root.update()
    assert f._win is not None and f._visible, "show 后窗口应可见"
    assert f._label.cget("text") == "你好世界", "候选文本应更新"
    assert f._win.attributes("-topmost"), "窗口应置顶"
    assert f._win.overrideredirect(), "窗口应无边框(无关闭/最小化/最大化)"
    # show 幂等: 第二次 show 更新文本不重建窗口(等过合并窗口, 模拟真实帧间隔)
    win = f._win
    time.sleep(0.15)
    f.show("第二段候选")
    root.update()
    assert f._win is win, "show 不应重建窗口"
    assert f._label.cget("text") == "第二段候选", "幂等更新文本"
    f.hide()
    root.update()
    root.update()   # macOS 窗口 withdraw 状态延迟一拍
    assert not f._visible, "hide 后窗口应隐藏"
    f.show("再次显示")      # hide 后复用窗口
    root.update()
    assert f._win is win and f._visible, "hide 后 show 复用窗口"
    f.destroy()
    root.destroy()
    print("[PASS] 悬浮窗 show/hide 生命周期")


def test_floating_show_same_text_noop():
    """重复 partial 文本 show() 应短路(零 configure/geometry); hide 重置后
    同文本不得被吞(新会话首帧)。"""
    try:
        import time  # noqa: F401
        import ttkbootstrap as ttk
        from floating import _TkFloat  # 窗口用例锁定 Tk 实现(门面在 macOS 走 NSPanel)
        root = ttk.Window(themename="darkly")
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    f = _TkFloat(root)
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
    assert f._visible, "hide 后 show 应可见"
    f.destroy()
    root.destroy()
    print("[PASS] show 同文本幂等短路 + hide 重置")


def test_floating_high_frequency_merge():
    """高频 partial 帧合并: 首帧立即渲染, 合并窗口内只刷最新帧。"""
    try:
        import time  # noqa: F401
        import ttkbootstrap as ttk
        from floating import _TkFloat  # 窗口用例锁定 Tk 实现(门面在 macOS 走 NSPanel)
        root = ttk.Window(themename="darkly")
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    f = _TkFloat(root)
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


def test_flush_does_not_reset_topmost_each_frame():
    """热路径不得每帧 attributes(-topmost): macOS 上会打满 WindowServer。"""
    try:
        import time  # noqa: F401
        import ttkbootstrap as ttk
        from floating import _TkFloat  # 窗口用例锁定 Tk 实现(门面在 macOS 走 NSPanel)
        root = ttk.Window(themename="darkly")
    except Exception as e:  # noqa: BLE001
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    f = _TkFloat(root)
    f.show("你好")
    root.update()
    time.sleep(0.15)
    calls = []
    orig = f._win.attributes
    def spy(*a, **k):
        calls.append(a)
        return orig(*a, **k)
    f._win.attributes = spy
    f.show("你好世界")
    root.update()
    topmost = [c for c in calls if c and c[0] == "-topmost"]
    assert topmost == [], f"每帧 -topmost 会导致转圈: {topmost}"
    f.destroy()
    root.destroy()
    print("[PASS] 热路径不每帧 -topmost")


def test_floating_position_tracks_text():
    """长文本换行后窗口锚点按新尺寸计算(热路径不 update_idletasks,
    win.reqw 滞后——必须用 label req 尺寸 + 面板内边距)。"""
    try:
        import time  # noqa: F401
        import ttkbootstrap as ttk
        from floating import _TkFloat, _PAD_X  # noqa: F401
        root = ttk.Window(themename="darkly")
    except Exception as e:  # noqa: BLE001 无显示环境(TclError)等
        print(f"SKIP: 无法创建窗口({e}); 跳过")
        return
    root.withdraw()
    f = _TkFloat(root)
    f.show("短")
    root.update()
    time.sleep(0.15)            # 过合并窗口
    f.show("这是一段非常长的中文候选文本用于验证换行后窗口尺寸是否"
           "及时更新, 足够长到换行显示成多行")
    # 不 update_idletasks: _flush 已按 label 同步 req 尺寸重设 geometry
    geo = f._win.geometry()     # "WxH+x+y" 或 "+x+y"
    x = int(geo.split("+")[1])
    w = f._label.winfo_reqwidth() + 2 * _PAD_X
    want_x = max(0, (f._sw - w) // 2)
    assert x == want_x, f"长文本锚点应随尺寸更新(实际 x={x}, want {want_x})"
    f.destroy()
    root.destroy()
    print("[PASS] 长文本位置随尺寸更新(热路径无 idle)")


if __name__ == "__main__":
    test_pick_family()
    test_anchor()
    test_anchor_cocoa()
    test_win32_exstyle_noactivate()
    test_win32_apply_noactivate_sets_bits()
    test_win32_apply_noactivate_failure()
    test_present_window_win32_skips_lift()
    test_present_window_win32_lift_on_failure()
    test_present_window_darwin_no_lift()
    test_present_window_other_lifts()
    test_floating_window_show_hide()
    test_floating_show_same_text_noop()
    test_floating_high_frequency_merge()
    test_flush_does_not_reset_topmost_each_frame()
    test_floating_position_tracks_text()
    print("全部通过")
