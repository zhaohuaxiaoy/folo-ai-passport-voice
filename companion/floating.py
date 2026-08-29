"""候选字悬浮窗(微信语音输入式, macOS/Windows)。

按住设备 PTT 说话时, ASR 中间结果(partial)实时显示在屏幕底部居中的
无边框置顶小窗里; 识别完成(final)后文本自动注入输入框、窗口消失。

平台差异与已知局限:
- 圆角: macOS/Windows 都无法原生逐像素圆角 → 统一方形面板(macOS
  半透明深色面板)。
- 焦点(macOS): Tk Toplevel 底层是 NSWindow, 系统拒绝 nonactivating
  panel 位, Tk 映射窗口时无条件激活 app —— transient/accessory
  都拦不住抢焦点(用户实测)。macOS 改走原生 NSPanel +
  NSNonactivatingPanelMask(PyObjC): 系统层面禁止激活 app 与成为
  key window, 悬浮窗出现/消失都不碰输入焦点(真机验证 frontmost
  不变)。NSPanel 构建失败时降级 Tk 实现(抢焦点, 已知局限)。
  Windows: Tk deiconify/lift/-topmost 会抢前台, 改走 WS_EX_NOACTIVATE
  + SetWindowPos(SWP_NOACTIVATE)。
- 线程: 本模块全部方法只允许在 tk 主线程调用(fre_app 经 poll()
  驱动), 不跨线程触碰控件。

模块顶层不创建任何 tk 对象, 无显示环境可安全 import。
"""
import sys
import time
import tkinter as tk
import tkinter.font as tkfont

# 平台字体候选链: 取第一个在系统中存在的(纯函数 _pick_family 可无显示单测)
_WIN_FONTS = ("Microsoft YaHei UI", "Microsoft YaHei", "SimHei")
_MAC_FONTS = ("PingFang SC", "Hiragino Sans GB", "STHeiti", "Helvetica Neue")

_PANEL_BG = "#202124"      # 深色面板
_PANEL_FG = "#FFFFFF"      # 白字
_WIDTH = 360               # 面板宽度上限
_WRAP = 320                # 文本换行宽度(留出加大后的左右间距)
_PAD_X = 20
_PAD_Y = 18
_FONT_SIZE = 14
_FLUSH_MS = 120               # 高频 partial 帧合并窗口(首帧立即渲染)

# Windows 不激活置顶(REVIEW P2-D): GWL_EXSTYLE 位 + SetWindowPos 标志。
# 抽出纯函数便于无显示 / 非 Windows 单测。
GWL_EXSTYLE = -20
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOOLWINDOW = 0x00000080   # 不进 Alt-Tab, 降低被当成前台的概率
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_FRAMECHANGED = 0x0020


def _pick_family(available, candidates):
    """按候选链返回第一个可用字体族; 全缺返回 None(调用方回退默认)。

    available: 可迭代的字体族名(如 tkfont.families()); candidates: 有序候选。
    """
    for fam in candidates:
        if fam in available:
            return fam
    return None


def _anchor(sw, sh, w, h):
    """屏幕尺寸 + 窗口尺寸 → 底部居中坐标 (x, y)。

    Tk 口径: 原点左上, y = 窗口上边距屏幕顶边(geometry +x+y 的语义)。
    窗口底边落在屏幕 80% 高度线(距屏幕底部向上 20%,微信语音输入式):
    y = int(sh * 0.8) - h。
    """
    x = max(0, (sw - w) // 2)
    y = max(0, int(sh * 0.8) - h)
    return x, y


def _anchor_cocoa(sw, sh, w, h):
    """同一落点换算到 Cocoa 口径 (x, y)。

    NSWindow setFrameOrigin_ 的原点在屏幕左下, y = 窗口下边距屏幕底边 ——
    与 Tk 的上下颠倒。直接套 _anchor 的 y 会把窗口顶到屏幕上部(sh=1080
    时下边缘落在 y=864, 离顶只剩 ~100px), 与 Tk 分支的"中下部"不是一个
    位置。所以统一由 _anchor 出落点再翻转一次, 两个平台永远一致。
    """
    x, y_top = _anchor(sw, sh, w, h)
    return x, max(0, int(sh) - y_top - int(h))


def _win32_exstyle_noactivate(existing):
    """在已有扩展样式上加上 NOACTIVATE|TOOLWINDOW(纯函数)。"""
    return existing | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW


def _win32_user32():
    """懒加载 user32, 并钉死 64 位 HWND/LONG_PTR 原型, 避免截断。"""
    import ctypes
    u = ctypes.windll.user32
    u.GetParent.argtypes = [ctypes.c_void_p]
    u.GetParent.restype = ctypes.c_void_p
    get = getattr(u, "GetWindowLongPtrW", u.GetWindowLongW)
    set_ = getattr(u, "SetWindowLongPtrW", u.SetWindowLongW)
    get.argtypes = [ctypes.c_void_p, ctypes.c_int]
    get.restype = ctypes.c_ssize_t
    set_.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_ssize_t]
    set_.restype = ctypes.c_ssize_t
    u.SetWindowPos.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint]
    u.SetWindowPos.restype = ctypes.c_int
    return u


def _win32_long_fns(user32):
    get = getattr(user32, "GetWindowLongPtrW", None) or user32.GetWindowLongW
    set_ = getattr(user32, "SetWindowLongPtrW", None) or user32.SetWindowLongW
    return get, set_


def _win32_toplevel_hwnd(win, user32=None):
    """Tk winfo_id 常是客户区 HWND; 有父窗口则用父(真正的 toplevel)。"""
    hwnd = int(win.winfo_id())
    try:
        user32 = user32 or _win32_user32()
        parent = user32.GetParent(hwnd)
        return int(parent) if parent else hwnd
    except Exception:  # noqa: BLE001
        return hwnd


def _win32_apply_noactivate(hwnd, user32=None):
    """给 HWND 加上 WS_EX_NOACTIVATE|TOOLWINDOW, 并以 SWP_NOACTIVATE 置顶。

    失败返回 False(调用方降级 lift, 窗仍能显示)。user32 可注入(单测)。
    """
    if not hwnd:
        return False
    try:
        user32 = user32 or _win32_user32()
        get_long, set_long = _win32_long_fns(user32)
        ex = get_long(hwnd, GWL_EXSTYLE) or 0
        set_long(hwnd, GWL_EXSTYLE, _win32_exstyle_noactivate(ex))
        user32.SetWindowPos(
            hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED)
        return True
    except Exception:  # noqa: BLE001 ctypes/权限/句柄失效
        return False


def _mac_accessory():
    """把本 app 激活策略降为 accessory(菜单栏常驻 app 的标准做法)。

    macOS 上 Tk 映射窗口(deiconify)时会激活 app、抢走用户输入窗口的
    前台焦点 —— transient 拦不住(用户实测仍失焦)。accessory 策略下
    app 激活不抢占其他 app 的 key window,悬浮窗出现时输入框保持焦点。
    失败静默降级(不阻断主流程)。Tk 初始化后才能调,故放 _build。
    """
    try:
        import ctypes
        import ctypes.util
        objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.sel_registerName.restype = ctypes.c_void_p
        msg = objc.objc_msgSend
        msg.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        msg.restype = ctypes.c_void_p
        ns_app = msg(objc.objc_getClass(b"NSApplication"),
                     objc.sel_registerName(b"sharedApplication"))
        if not ns_app:
            return False
        msg.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long]
        msg(ns_app, objc.sel_registerName(b"setActivationPolicy:"), 1)  # Accessory
        return True
    except Exception:  # noqa: BLE001 ctypes/权限失败:降级,悬浮窗仍能显示
        return False


def _present_window(win):
    """把已 withdraw 的 Toplevel 映出来, 不抢前台焦点。

    Windows: 先写 NOACTIVATE 再 deiconify, SetWindowPos 置顶; 失败才 lift。
    macOS: 只 deiconify, 不 lift(lift 会让 WindowServer 转圈)。
    其他: deiconify + lift。
    """
    if sys.platform == "win32":
        hwnd = _win32_toplevel_hwnd(win)
        _win32_apply_noactivate(hwnd)
        win.deiconify()
        if not _win32_apply_noactivate(hwnd):
            win.lift()
        return
    win.deiconify()
    if sys.platform == "darwin":
        # overrideredirect 在 withdraw 时设 -topmost 不生效, 映出后设一次
        win.attributes("-topmost", True)
        return
    win.lift()


class _TkFloat:
    """Tk 实现(Windows 主用;macOS 仅 NSPanel 降级路径)。懒创建 Toplevel,
    会话间复用(show/hide 不销毁)。

    - show(text): 幂等 —— 更新文本 + 映出 + 置顶 + 重新定位。
    - hide(): withdraw(不销毁)。
    - destroy(): 显式销毁(应用退出/relay_done 兜底; 随 root.destroy 自动回收)。
    """

    def __init__(self, root):
        self._root = root
        self._win = None
        self._label = None
        self._font = None
        self._last_text = None      # show() 幂等短路: 重复 partial 零 Tcl 往返
        self._sw = self._sh = None  # 屏幕尺寸缓存(会话内恒定, 省每帧往返)
        self._pending = None        # 高频帧合并: 待刷文本
        self._flush_after = None    # 合并定时器 id
        self._last_flush_at = 0.0   # 上次渲染时间戳(合并窗口判定)
        self._geo_cache = None      # 上次 geometry: 尺寸未变不重设
        self._visible = False       # 自管映射态: 不信 macOS winfo_ismapped 延迟

    # ---- 显示/隐藏 ----

    def show(self, text):
        """显示候选文本。首次调用懒创建窗口。

        高频 partial 帧合并: 首帧立即渲染(窗口不延迟出现), 之后
        _FLUSH_MS 窗口内的帧只保留最新 —— macOS 上窗口 resize + 透明
        合成开销不低, 逐帧更新会让 UI 线程卡顿(用户实测)。
        """
        if self._win is None:
            self._build()
        if text == self._last_text and self._visible:
            return
        # 距上次渲染已过合并窗口 → 立即渲染; 否则挂 pending 等定时器刷最新
        # (monotonic 是秒, _FLUSH_MS 是毫秒, 比较前换算)
        if self._flush_after is None \
                and time.monotonic() - self._last_flush_at >= _FLUSH_MS / 1000:
            self._flush(text)
            return
        self._pending = text        # 高频帧合并: 定时器只刷最新
        if self._flush_after is None:
            self._flush_after = self._root.after(_FLUSH_MS, self._flush_pending)

    def _flush_pending(self):
        self._flush_after = None
        text = self._pending
        self._pending = None
        if text is not None:
            self._flush(text)

    def _flush(self, text):
        """渲染一帧(文本 + 定位)。

        macOS 热路径禁止: 每帧 -topmost / lift / update_idletasks。
        这三项都会同步进 WindowServer, ASR partial 频率下会全桌面转圈。
        """
        self._last_flush_at = time.monotonic()
        self._last_text = text
        self._label.configure(text=text)
        if not self._visible:
            _present_window(self._win)
            self._visible = True
        # 尺寸 = label 请求尺寸 + 面板内边距。label.reqw 在 configure 后
        # 同步更新, 而 win.reqw 要等 idle 布局传播——热路径不 update_idletasks
        # (同步 WindowServer 会转圈), 故不能用窗口层 req 尺寸(长文本时
        # 锚点按旧尺寸算, 窗口错位)。
        w = self._label.winfo_reqwidth() + 2 * _PAD_X
        h = self._label.winfo_reqheight() + 2 * _PAD_Y
        x, y = _anchor(self._sw if self._sw is not None
                       else self._win.winfo_screenwidth(),
                       self._sh if self._sh is not None
                       else self._win.winfo_screenheight(), w, h)
        if (x, y, w, h) != self._geo_cache:
            self._win.geometry(f"+{x}+{y}")
            self._geo_cache = (x, y, w, h)

    def hide(self):
        if self._win is not None:
            if self._flush_after is not None:
                self._root.after_cancel(self._flush_after)
                self._flush_after = None
            self._pending = None
            self._win.withdraw()
            self._visible = False
            # 清幂等缓存 + 重置合并窗口: 新会话首帧立即渲染,
            # 且若与上会话末帧同文也不得被短路吞
            self._last_text = None
            self._last_flush_at = 0.0

    def destroy(self):
        if self._win is not None:
            if self._flush_after is not None:
                self._root.after_cancel(self._flush_after)
                self._flush_after = None
            self._win.destroy()
            self._win = None
            self._label = None
            self._last_text = None
            self._visible = False

    # ---- 构建 ----

    def _build(self):
        win = tk.Toplevel(self._root)
        win.withdraw()                      # 立刻藏: 默认映射会抢焦, 且空窗会闪
        win.overrideredirect(True)          # 无标题栏、无关闭/最小化/最大化
        win.resizable(False, False)
        if sys.platform == "darwin":
            # macOS 抢焦点根因是 Tk 映射窗口时激活 app(用户实测 transient
            # 拦不住)。accessory 激活策略下 app 激活不抢其他 app 前台;
            # transient 保留:窗口不能成为 key window,双保险。
            win.transient(self._root)
            _mac_accessory()
        if sys.platform == "win32":
            win.update_idletasks()
            _win32_apply_noactivate(_win32_toplevel_hwnd(win))
        else:
            win.attributes("-topmost", True)    # 只设一次, 热路径不再碰
        # 绝不 focus/focus_force: 注入目标是用户焦点窗口(见模块 docstring)

        panel = tk.Frame(win, bg=_PANEL_BG,
                         padx=_PAD_X, pady=_PAD_Y)
        panel.pack(fill="both", expand=True)

        # 极简单块: 深色面板 + 白字候选文本, 无任何装饰
        label_kw = dict(text="", bg=_PANEL_BG, fg=_PANEL_FG,
                        wraplength=_WRAP, justify="left", anchor="w")
        font = self._pick_font()
        if font is not None:
            label_kw["font"] = font
        self._label = tk.Label(panel, **label_kw)
        self._label.pack(anchor="w")

        self._win = win
        # 屏幕尺寸会话内恒定: 缓存, show() 不再逐帧查询
        self._sw = win.winfo_screenwidth()
        self._sh = win.winfo_screenheight()
        win.update_idletasks()
        # 初始定位(底部居中), 之后每次 show() 按内容尺寸重定位
        x, y = _anchor(self._sw, self._sh,
                       win.winfo_reqwidth(), win.winfo_reqheight())
        win.geometry(f"+{x}+{y}")
        self._geo_cache = (x, y, win.winfo_reqwidth(), win.winfo_reqheight())

    def _pick_font(self):
        if self._font is None:
            # 禁止 tkfont.families(): macOS 枚举全表会卡住主线程数秒(转圈)。
            # 按候选链逐个创建, actual(family) 对得上才用。
            candidates = _WIN_FONTS if sys.platform == "win32" else _MAC_FONTS
            for fam in candidates:
                try:
                    f = tkfont.Font(self._root, family=fam, size=_FONT_SIZE)
                    if f.actual("family") == fam:
                        self._font = f
                        break
                except tk.TclError:
                    continue
            if self._font is None:
                try:
                    self._font = tkfont.nametofont("TkDefaultFont")
                except tk.TclError:
                    self._font = None
        return self._font


class _MacPanelFloat:
    """macOS 原生 NSPanel 悬浮窗(PyObjC)。

    Tk Toplevel 底层是 NSWindow —— 系统拒绝 nonactivating panel 位
    (实测 styleMask 不变化), Tk 映射窗口时无条件激活 app, 抢走输入框
    焦点(transient/accessory 均拦不住, 用户实测)。NSPanel +
    NSNonactivatingPanelMask 从系统层面禁止窗口激活 app 与成为 key
    window: 悬浮窗出现/消失都不碰输入焦点(真机验证 frontmost 不变)。

    高频 partial 帧沿用 _FLUSH_MS 合并(换行测量 + 面板重排不便宜)。
    """

    def __init__(self, root):
        self._root = root
        # 懒 import + 立刻探测: PyObjC 缺失/损坏时抛错, 门面降级 Tk。
        from AppKit import (NSPanel, NSBorderlessWindowMask,
                            NSNonactivatingPanelMask, NSBackingStoreBuffered,
                            NSStatusWindowLevel, NSColor, NSTextField,
                            NSMakeRect, NSFont, NSLineBreakByWordWrapping,
                            NSFontAttributeName, NSScreen,
                            NSStringDrawingUsesLineFragmentOrigin)
        from Foundation import NSString, NSMakeSize, NSMakePoint
        self._panel = None
        self._label = None
        self._font = None
        self._last_text = None
        self._pending = None
        self._flush_after = None
        self._last_flush_at = 0.0
        self._visible = False

    # ---- 显示/隐藏 ----

    def show(self, text):
        """显示候选文本。首次调用懒创建窗口(接口与 _TkFloat 对齐)。"""
        if self._panel is None:
            self._build()
        if text == self._last_text and self._visible:
            return
        if self._flush_after is None \
                and time.monotonic() - self._last_flush_at >= _FLUSH_MS / 1000:
            self._flush(text)
            return
        self._pending = text
        if self._flush_after is None:
            self._flush_after = self._root.after(_FLUSH_MS, self._flush_pending)

    def _flush_pending(self):
        self._flush_after = None
        text = self._pending
        self._pending = None
        if text is not None:
            self._flush(text)

    def _flush(self, text):
        self._last_flush_at = time.monotonic()
        self._last_text = text
        self._label.setStringValue_(text)
        self._relayout(text)
        if not self._visible:
            self._panel.orderFrontRegardless()   # 不激活 app、不成 key window
            self._visible = True

    def hide(self):
        if self._panel is None:
            return
        if self._flush_after is not None:
            self._root.after_cancel(self._flush_after)
            self._flush_after = None
        self._pending = None
        if self._visible:
            self._panel.orderOut_(None)
            self._visible = False
        self._last_text = None
        self._last_flush_at = 0.0

    def destroy(self):
        if self._flush_after is not None:
            self._root.after_cancel(self._flush_after)
            self._flush_after = None
        if self._panel is not None:
            self._panel.orderOut_(None)
            self._panel = None
            self._label = None
            self._font = None
            self._last_text = None
            self._visible = False

    # ---- 构建 ----

    def _build(self):
        from AppKit import (NSPanel, NSBorderlessWindowMask,
                            NSNonactivatingPanelMask, NSBackingStoreBuffered,
                            NSStatusWindowLevel, NSColor, NSTextField,
                            NSMakeRect, NSFont, NSLineBreakByWordWrapping)
        panel = NSPanel.alloc().initWithContentRect_styleMask_backing_defer_(
            NSMakeRect(0, 0, _WIDTH, 60),
            NSBorderlessWindowMask | NSNonactivatingPanelMask,
            NSBackingStoreBuffered, False)
        panel.setLevel_(NSStatusWindowLevel)
        panel.setBackgroundColor_(NSColor.colorWithCalibratedWhite_alpha_(0.13, 0.96))
        panel.setOpaque_(False)
        panel.setHidesOnDeactivate_(False)
        self._panel = panel
        self._font = NSFont.fontWithName_size_("PingFangSC-Regular", _FONT_SIZE)
        if self._font is None:
            self._font = NSFont.systemFontOfSize_(_FONT_SIZE)
        label = NSTextField.alloc().initWithFrame_(
            NSMakeRect(_PAD_X, _PAD_Y, _WIDTH - 2 * _PAD_X, 24))
        label.setBezeled_(False)
        label.setDrawsBackground_(False)
        label.setEditable_(False)
        label.setSelectable_(False)
        label.setStringValue_("")
        label.setFont_(self._font)
        label.setTextColor_(NSColor.whiteColor())
        label.cell().setWraps_(True)
        label.cell().setLineBreakMode_(NSLineBreakByWordWrapping)
        panel.contentView().addSubview_(label)
        self._label = label

    def _relayout(self, text):
        """按文本换行尺寸重设面板并定位(底部居中, 底边 60% 屏幕高,
        微信语音输入式的中下部悬浮)。"""
        from AppKit import (NSFontAttributeName, NSScreen,
                            NSStringDrawingUsesLineFragmentOrigin)
        from Foundation import NSString, NSMakeSize, NSMakePoint, NSMakeRect
        screen = NSScreen.mainScreen().frame()
        attrs = {NSFontAttributeName: self._font}
        # boundingRect 是 NSString 的方法, Python str 需桥接
        size = NSString.stringWithString_(text).boundingRectWithSize_options_attributes_(
            NSMakeSize(_WRAP, 1e9),
            NSStringDrawingUsesLineFragmentOrigin, attrs).size
        w = min(max(size.width, 80.0), float(_WRAP)) + 2 * _PAD_X
        h = max(size.height, 16.0) + 2 * _PAD_Y + 4
        sw, sh = screen.size.width, screen.size.height
        x, y = _anchor_cocoa(int(sw), int(sh), int(w), int(h))
        self._panel.setContentSize_(NSMakeSize(w, h))
        self._label.setFrame_(NSMakeRect(_PAD_X, _PAD_Y,
                                         w - 2 * _PAD_X, h - 2 * _PAD_Y))
        self._panel.setFrameOrigin_(NSMakePoint(x, y))


class FloatingCandidate:
    """候选字悬浮窗门面: macOS 用原生 NSPanel(不抢焦点), 其他平台用 Tk。

    - show(text): 幂等 —— 更新文本 + 映出 + 置顶 + 重新定位。
    - hide(): withdraw/orderOut(不销毁)。
    - destroy(): 显式销毁(应用退出/relay_done 兜底)。
    macOS 下 NSPanel 构建失败(缺 PyObjC)时降级 Tk 实现(抢焦点, 已知局限)。
    """

    def __init__(self, root):
        self._root = root
        if sys.platform == "darwin":
            try:
                self._impl = _MacPanelFloat(root)
            except Exception as e:  # noqa: BLE001 缺 PyObjC: 降级 Tk(抢焦点)
                # 降级必须留痕: Tk 悬浮窗会抢焦点, 抢走之后注入就打进空处
                # (表现正是"明明有焦点却没输入上去"), 而原来这里是静默的。
                print(f"[悬浮窗] NSPanel 不可用, 降级 Tk(会抢焦点): {e}",
                      file=sys.stderr)
                self._impl = _TkFloat(root)
        else:
            self._impl = _TkFloat(root)

    def show(self, text):
        self._impl.show(text)

    def hide(self):
        self._impl.hide()

    def destroy(self):
        self._impl.destroy()
