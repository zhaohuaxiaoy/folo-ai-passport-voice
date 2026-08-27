# 音量加长按说话 + 双击全选删除 + 麦克风图标重画

## 需求(用户原话)

1. **音量加长按改成说话** —— 长按音量加(UP)键 = PTT 说话。
2. **双击音量加改成全选删除** —— 双击音量加 = 清空输入框。
3. **双击时不要提示音,只有长按 0.5 秒以上才有提示音**。
4. **全选删除在 CodeX 和 Claude Code 里都不能用,只会删 1 个字,要解决**。
5. **录音时设备上的麦克风图标不太像麦克风** —— 重画。

## 现状关键事实(已确认)

- 硬件:三个 ADC 分压键 UP(音量加)/DOWN/OK;iot_button(espressif__button)驱动,long_press_time 默认 600ms。
- OK 键 = PTT(READY 态 PRESS 开始 / LISTENING 态 RELEASE 结束);OK 双击 = 清空输入框(全局分支,app_state.c:127)。
- UP 键目前仅 APPROVAL 态 REJECT 用,长按/双击事件被状态机忽略。
- iot_button 行为(已读源码 iot_button.c):按住超时后松开报 `BUTTON_LONG_PRESS_UP` 而**不再报** `BUTTON_PRESS_UP`(bsp_button.c:64-66 注释同因);长按与单击/双击互斥。
- 提示音:OK PTT 按下即响 START 音(READY→LISTENING);松开响 SEND 音。双击 OK 清空时第一按已进入录音,会响两声——"吵"的来源。
- 注入 clear 序列(inject.py):`Home + Shift+End + Delete`。**Shift+End 在终端模拟器里被拦截为终端级文本选择,TUI 输入框收不到 → Delete 只删 1 字符**。
- CodeX 官方文档:Ctrl+U = Clear current line(输入控制默认键位);Claude Code(Ink 系)/readline 均支持 Ctrl+U(删到行首)。Ctrl 组合键是控制字符,终端不拦截、原样转发。
- LVGL 字体:仅 montserrat 14/20,无符号字体段 → `LV_SYMBOL_MIC` 不可用,图标只能像素画。
- 麦克风图标现状(app_ui.c build_listening):胶囊头(16×26)+ 顶/底水平弧条 + 底口 + 立杆 + 底座上沿 + 底座,视觉偏"立式话筒",用户不认。

## 目标行为

| 按键 | 行为 | 提示音 |
|---|---|---|
| UP 按住 ≥0.5s | 滴声 → 开始录音;松开 → 发送 | 滴声(0.5s 判定时)+ 发送音 |
| UP 双击(每按 <0.5s) | 全选删除(清空输入框) | 无 |
| UP 短按 | 无动作 | 无 |
| OK 单击/按住 | PTT 说话(不变) | 不变 |
| OK 双击 | 移除(原清空功能移到 UP 双击) | — |
| DOWN 单击 | 回车(不变) | — |

- 双击全选删除在 **Claude Code / CodeX(终端 TUI)必须整行清空**,不再只删 1 字。
- 非终端 app(浏览器/微信等)全选删除行为不回归(仍 Home+Shift+End)。
- 录音页麦克风图标更像"麦克风"。

## 验收标准

1. 宿主测试 7/7、`companion` pytest 全绿、`idf.py build` PASS、`python3 -m py_compile companion/*.py`、`git diff --check`。
2. 固件:长按 UP ≥0.5s → START 音 + LISTENING 麦克风页;松开 → SEND 音 + 转写;双击 UP → clear 上行且全程无音;短按 UP → 无动作;OK 双击不再发 clear。
3. Mac:前端 app 为终端类时 clear 注入 Ctrl+U;非终端保留原序列(单元测试覆盖分支选择)。
4. 打包后 dist 无密钥泄露、签名 OK。
5. 真机/真会话为 NOT RUN(计划内不执行,交付后由用户实测):长按手感、双击窗口、Claude Code/CodeX 中 Ctrl+U 实际效果、麦克风图标观感。

## 约束

- 安全:火山密钥/config.local.json 不入包不入库;WiFi 密码不回显;只 push `main`。
- 不引入新依赖(探测前台 app 用现有 osascript/System Events 通道)。
- 长按 PTT 与 OK 键 PTT 并存,OK 键行为不变。
