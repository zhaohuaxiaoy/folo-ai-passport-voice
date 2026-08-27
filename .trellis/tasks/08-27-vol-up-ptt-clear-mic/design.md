# 设计:UP 长按 PTT + 双击 clear + Ctrl+U 注入 + 麦克风图标

## 1. 事件流(iot_button 语义,已读源码验证)

espressif__button 组件(iot_button.c,10ms tick):
- 按下 → `BUTTON_PRESS_DOWN`(立即)
- 按住 ≥ `long_press_ticks` → `BUTTON_LONG_PRESS_START`(一次)+ 周期 HOLD
- 松开分两种(PRESS_UP_CHECK 分支,i 141-156):
  - 按住时长 < long_press_ticks → `BUTTON_PRESS_UP`;随后按双击窗口(300ms)判定 SINGLE/DOUBLE_CLICK
  - 按住时长 ≥ long_press_ticks → 进 PRESS_LONG_PRESS_UP_CHECK → `BUTTON_LONG_PRESS_UP`(不报 PRESS_UP)
- 互斥性天然成立:双击的两按均 <500ms;第二按若按住 ≥500ms 则转为长按(可接受)。

**UP 键配置**:`button_config_t.long_press_time = 500`(50 ticks,即用户阈值 0.5s)。
**OK 键**:维持 10 分钟等效禁用长按(PTT 按住不超时)。
**DOWN 键**:不动(默认 600ms 长按事件被状态机忽略)。

## 2. 固件链路改动

```
bsp_button.h   : BSP_BTN_LONG_UP 枚举(追加,不插中间)
bsp_button.c   : cb_long_up() 注册 BUTTON_LONG_PRESS_UP → s_cb(btn, BSP_BTN_LONG_UP)
main.c on_key  : BSP_BTN_LONG_UP → APP_EV_KEY_LONG_UP
app_types.h    : APP_EV_KEY_LONG_UP 枚举 + 按键语义注释
app_state.c    : handle_key:
                   READY 态   b==UP && LONG     → 开始录音(与 OK PRESS 同序列:
                                                  START 音 + SEND_VOICE_START + LISTENING)
                   LISTENING  b==UP && LONG_UP  → 结束录音(与 OK RELEASE 同序列:
                                                  STREAM_STOP + SEND_VOICE_END + SEND 音 + TRANSCRIBING)
                   全局       b==UP && DOUBLE   → send_key_action(CLEAR)
                   删除 OK 双击 clear 分支(app_state.c:127-130)
```

细节:
- 长按录音期间只响两声:进入时 START(0.5s 判定),松开 SEND。双击流程无任何 tone(第一按 <0.5s 不会触发 START)。
- LISTENING 态结束事件按 btn 区分:OK RELEASE / UP LONG_UP 都要处理。
- PRESS 唤醒(息屏)对所有键通用,不变;UP 长按在息屏时第一按 PRESS 先唤醒(不录音),再按住 0.5s 进 LONG 才开始——行为自然,无需特判。
- `app_state_reduce` 的事件 switch 已按类型分发,`APP_EV_KEY_LONG_UP` 并入按键组(last_key_ms 刷新)。

## 3. Mac 注入 clear 修复(inject.py)

**根因**:`Home + Shift+End + Delete` 中 Shift+End 被终端模拟器拦截为终端文本选择,不转发给 TUI。

**方案**:按前台 app 分两条序列。

```python
_TERMINAL_APPS = {   # bundle id 优先,进程名兜底
    "com.apple.Terminal": "Terminal",
    "com.googlecode.iterm2": "iTerm2",
    "dev.warp.Warp-Stable": "Warp",
    "com.mitchellh.ghostty": "Ghostty",
    "net.kovidgoyal.kitty": "kitty",
    "org.alacritty": "Alacritty",
    "com.wezterm.wezterm": "WezTerm",
    "com.microsoft.VSCode": "Code",      # 内置终端跑 Claude Code/CodeX
    "com.todesktop.230313mzl4w4p92": "Cursor",
    "dev.zed.Zed": "Zed",
    "com.hyper": "Hyper",
}
```
- `_frontmost_is_terminal()`:osascript `tell application "System Events" to get bundle identifier of first application process whose frontmost is true`;取不到 bundle id 时用进程名;两次 osascript 失败时按"非终端"降级(不恶化现状)。
- clear 序列:
  - 终端 → `keystroke "u" using control down`(Ctrl+U:CodeX 官方 Clear current line;Claude Code/Ink 支持删到行首;readline 标准;控制字符终端不拦截)
  - 非终端 → 原 `Home + Shift+End + Delete`(Chromium 里 Ctrl+U = 查看源码,必须避开)
- 已知局限(记录,不阻塞):终端内多行输入仅清当前行(CodeX 官方同限);光标不在行尾时仅删光标前(语音注入后光标必在末尾,主流程无碍);tmux prefix 若被用户改成 Ctrl+U 则失效(罕见,可后续做配置)。
- `inject_win.py` 本轮不动(用户场景 macOS;Windows 终端同样支持 Ctrl+U,后续需要时同法)。

## 4. 麦克风图标(app_ui.c build_listening)

经典麦克风造型,块状像素画(block(),中心 x=120,屏 240×320):

```
圆头:  y58-90 三段圆(24/32/24 宽)
   y58-66: x108-132
   y66-82: x104-136
   y82-90: x108-132
支架弧: y90-118,左右各一条从圆头下缘收拢:
   y90-98:  x106-110 / 130-134
   y98-106: x108-112 / 128-132
   y106-118: x110-114 / 126-130  → 收拢到立杆
立杆:  x116-124(8 宽),y118-126
底座:  y126-132: x104-136 横条;y132-138: x112-128
```

(以实际渲染微调;目标:一眼认出是麦克风,去掉"立式话筒"感。)

## 5. 测试与门禁

- `tests/test_app_state.c`:UP 长按流(READY→LONG→LISTENING→LONG_UP→TRANSCRIBING)、UP 双击发 clear、OK 双击不再发 clear、短按无动作。宿主测试 7/7。
- `companion/tests/test_inject.py`(或既有注入测试):clear 分支选择——伪造前台探测函数,终端清单内 → ctrl+u 命令;清单外 → 原序列。
- `companion/.venv/bin/python -m pytest tests/ -q -o asyncio_mode=auto` 全绿。
- `idf.py build` PASS;打包(需用户退出 App 后烧录)。
- NOT RUN(真机实测留用户):长按 0.5s 手感/滴声时机、双击窗口内快速两按、Claude Code/CodeX 中 Ctrl+U 清空、麦克风图标观感。
