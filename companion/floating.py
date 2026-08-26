"""候选字悬浮窗(微信语音输入式, macOS/Windows)。

按住设备 PTT 说话时, ASR 中间结果(partial)实时显示在屏幕底部居中的
无边框置顶小窗里; 识别完成(final)后文本自动注入输入框、窗口消失。

平台差异与已知局限:
- 圆角: tkinter 在 macOS/Windows 都无法原生逐像素圆角
  (macOS 无 -transparentcolor; Windows 的 -transparentcolor 在部分
  合成器下有重影风险) → 统一方形面板; macOS 仅以 -alpha 0.95 柔化。
- 焦点: 悬浮窗绝不调用 focus/focus_force —— 注入目标是用户当前
  焦点窗口, 悬浮窗不得抢焦点(macOS overrideredirect 天然不获焦,
  Windows 需克制)。
- 线程: 本模块全部方法只允许在 tk 主线程调用(fre_app 经 poll()
  驱动), 不跨线程触碰控件。

模块顶层不创建任何 tk 对象, 无显示环境可安全 import。
"""
import sys
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

    窗口底边落在屏幕 80% 高度线: y = int(sh * 0.8) - h。
    """
    x = max(0, (sw - w) // 2)
    y = max(0, int(sh * 0.8) - h)
    return x, y


class FloatingCandidate:
    """候选字悬浮窗。懒创建 Toplevel, 会话间复用(show/hide 不销毁)。

    - show(text): 幂等 —— 更新文本 + deiconify + lift + 置顶 + 重新定位。
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

    # ---- 显示/隐藏 ----

    def show(self, text):
        """显示候选文本。首次调用懒创建窗口。"""
        if self._win is None:
            self._build()
        # 幂等短路: ASR partial 常在同一段文本反复发帧(静音段/重发),
        # 已显示同文本时零 Tcl 往返。
        if text == self._last_text and self._win.winfo_ismapped():
            return
        self._last_text = text
        self._label.configure(text=text)
        if not self._win.winfo_ismapped():
            # 仅首次/被外部 withdraw(托盘等)后补发: 已映射窗口上是 no-op
            self._win.deiconify()
            self._win.lift()
        # -topmost 每次重发(1 次往返, 廉价): macOS 上窗口 realize 前
        # 设置不生效, 且重发防系统/合成器漂移
        self._win.attributes("-topmost", True)
        self._win.update_idletasks()
        x, y = _anchor(self._sw if self._sw is not None
                       else self._win.winfo_screenwidth(),
                       self._sh if self._sh is not None
                       else self._win.winfo_screenheight(),
                       self._win.winfo_width(), self._win.winfo_height())
        self._win.geometry(f"+{x}+{y}")

    def hide(self):
        if self._win is not None:
            self._win.withdraw()
            # 清幂等缓存: 新会话首帧若与上会话末帧同文, 不得被短路吞
            self._last_text = None

    def destroy(self):
        if self._win is not None:
            self._win.destroy()
            self._win = None
            self._label = None
            self._last_text = None

    # ---- 构建 ----

    def _build(self):
        win = tk.Toplevel(self._root)
        win.overrideredirect(True)          # 无边框
        win.attributes("-topmost", True)    # 置顶
        win.resizable(False, False)
        if sys.platform == "darwin":
            win.attributes("-alpha", 0.95)  # macOS 柔化(Windows 保持不透明)
        # 绝不 focus/focus_force: 注入目标是用户焦点窗口(见模块 docstring)

        panel = tk.Frame(win, bg=_PANEL_BG,
                         padx=_PAD_X, pady=_PAD_Y)
        panel.pack(fill="both", expand=True)

        # 极简单块: 深色面板 + 白字候选文本, 无任何装饰
        self._label = tk.Label(panel, text="", bg=_PANEL_BG, fg=_PANEL_FG,
                               font=self._pick_font(), wraplength=_WRAP,
                               justify="left", anchor="w")
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

    def _pick_font(self):
        if self._font is None:
            try:
                fam = _pick_family(tkfont.families(self._root),
                                   _WIN_FONTS if sys.platform == "win32"
                                   else _MAC_FONTS)
                if fam is None:
                    fam = tkfont.nametofont("TkDefaultFont").actual("family")
                self._font = tkfont.Font(family=fam, size=_FONT_SIZE)
            except tk.TclError:
                # 极端环境(字体表不可用): 回退默认字体
                self._font = tkfont.nametofont("TkDefaultFont")
        return self._font
