# FoloToy AI Passport · 语音输入

[简体中文](README.md) | [English](README.en.md)

面向 **FoloToy AI Passport**（ESP32-C3）的语音输入方案：按住设备按键说话，转写候选字**实时显示在 Mac/Windows 桌面的悬浮窗**里；松手后识别完成的文本**自动注入当前输入框**——微信 Mac 端语音输入体验。

```text
按住 OK 说话 ──► 设备录音 ──► BLE / WiFi / USB ──► 桌面端中转 ──► 火山流式 ASR
                                                                     │
   当前输入框 ◄── 注入定稿(仅一次) ◄── 松手 ──► 悬浮窗实时显示候选字 ◄──┘
```

> 仓库同时包含**固件**（ESP-IDF，设备端）与**桌面端**（companion/，Mac/Windows 中转程序），二者通过 BLE / WiFi / USB 三通道配合工作。

## 功能特性

### 设备端（固件）

- **按住说话**：READY 态按住 OK 开始录音（滴声提示），松开自动发送；无取消窗口、无超时残留
- **LISTENING 页**：录音中显示麦克风图标 + 计时，替代传统 REC 红点/音量条
- **三键交互**：UP/DOWN/OK —— 按住说话、DOWN 回车、OK 双击清空输入框（全局语义）
- **完整状态机**：HOME → READY → LISTENING → TRANSCRIBING → AGENT_RUNNING → APPROVAL → DONE
- **物理审批**：Agent 审批请求时进入审批页，OK 批准 / UP 拒绝 / DOWN 看 Diff 详情（桌面端默认关闭审批）
- **提示音**：开始 / 发送 / 审批提醒 / 完成 / 拒绝 / 错误六种
- **三通道传输**：BLE（Mac 直连，GATT 服务 `0xA2B0`）/ WiFi+WebSocket+mDNS（Windows 无蓝牙 PC）/ USB（USB-Serial-JTAG 有线，含完整控制台命令面）
- **低资源音频管线**：3200B/100ms 帧、静态环形缓冲、源端丢帧不整段缓存、掉帧对账
- **控制台命令**：`st`（堆/栈水位、连接、掉帧统计）、`mode`、`wifi set`（密码不回显）、`ws`、`mdns`、`logs`、`system`、`factory reset`、`reboot`

### 桌面端（companion/，macOS + Windows）

- **候选字悬浮窗**（核心）：ASR 中间结果**全量累计文本**实时显示在屏幕底部居中的无边框置顶小窗，自动换行、随内容向上生长；松手后定稿文本注入输入框一次、窗口消失
- **不抢焦点**：悬浮窗绝不 focus；Windows 走 `WS_EX_NOACTIVATE` + `SWP_NOACTIVATE`，macOS 热路径不触发 WindowServer 同步（按住说话不卡桌面）
- **高频帧合并**：120ms 合并窗口只刷最新帧，首帧立即渲染，杜绝逐帧重绘卡顿
- **5 步向导**：欢迎 → 自动发现设备（BLE → WiFi → USB）→ 火山 ASR Key 配置（零音频握手测试，不回显）→ 系统授权引导（macOS）→ 完成状态页
- **托盘驻留**：连接后驻留系统菜单栏/托盘，状态行 + 诊断 + 设置
- **诊断页**：USB 通道完整设备命令面；BLE/WiFi 通道只读运行状态
- **注入**：macOS 剪贴板 + Cmd+V（需辅助功能授权；中文必须走剪贴板通道）；Windows 独立注入器

## 按键操作

| 按键 | 语境 | 动作 |
| --- | --- | --- |
| OK **按住** | READY | 开始录音（PTT），松开发送 |
| OK **双击** | 任意 | 清空当前输入框全部文字 |
| DOWN 单击 | HOME / READY | 输入框回车（提交） |
| OK 单击 | HOME | 进入 READY（工作流就绪） |
| OK 单击 | APPROVAL | 批准 Agent 请求 |
| UP 单击 | APPROVAL | 拒绝 Agent 请求 |
| DOWN 单击 | APPROVAL | 查看 Diff 详情 |

## 快速开始

### 桌面端（Mac / Windows）

**直接使用打包好的客户端（推荐，非开发者无需碰终端）**：

- **macOS**：`companion/dist/AI Passport.app`（`companion/build/pack.py` 构建，或使用发布产物），双击启动
- **Windows**：需在 Windows 构建机上运行 `python3 companion/build/pack.py` 生成 `dist/AI Passport.exe`（或使用发布产物）

首次启动进入 5 步向导：欢迎 → 自动发现设备（BLE → WiFi → USB）→ 火山 ASR Key 配置 → 系统授权引导（macOS）→ 状态页；连接后驻留系统托盘。首次运行自动生成 `companion/config.local.json`（火山 API Key，**不入 git**）。

**从源码运行（开发）**：

```bash
pip install -r companion/requirements.txt
cp companion/config.example.json companion/config.local.json   # 填入火山 API Key
companion/.venv/bin/python companion/fre_app.py                 # 向导 GUI（--dry-run 假链路演练）
companion/.venv/bin/python companion/relay.py                   # CLI 中转（BLE 自动扫描 "AI Passport"）
```

macOS 首次使用需授予**辅助功能**权限（注入剪贴板+Cmd+V 必需）与蓝牙权限。Windows 使用说明见 [`companion/WINDOWS.md`](companion/WINDOWS.md)。

### 固件（ESP32-C3）

需要 ESP-IDF 5.5.x（已知环境 5.5.3）：

```bash
source "$HOME/esp/esp-idf-v5.5.3/export.sh"   # 或你的安装路径
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

首次构建会经 Component Manager 拉取 LVGL、`esp_lvgl_port`、`button`、`esp_codec_dev` 等依赖。烧录后控制台为 USB-Serial-JTAG（GPIO18/19；UART0 默认 TX GPIO21 与本板背光冲突）。

## 仓库结构

```text
main/                    ESP32-C3 固件:状态机、UI、三通道传输、音频流、控制台命令
components/bsp/          板级驱动:显示 / 按键 / 音频 / 电池 / 共享 I2C(bsp_pins.h 为硬件事实唯一来源)
companion/               桌面端:relay 中转(ASR 流式 + 注入)、悬浮窗、向导、托盘、三通道传输
tests/                   可脱离硬件运行的固件逻辑测试(纯 C,ctest)
docs/                    硬件开发指南与验收文档
sdkconfig.defaults       ESP32-C3、USB console、Flash、LVGL 默认配置
partitions.csv           自定义分区表(factory 4MB)
```

## 工作原理

1. 设备 READY 态按住 OK → 滴声 → `voice.start` → 3200B/100ms 音频帧经 BLE（GATT NOTIFY）/ WS / USB 上行
2. 桌面端 relay 把音频帧流式送入火山引擎 ASR（`bigmodel_async`，每包结果携带**全量累计文本**）
3. 中间结果（partial）→ 悬浮窗实时显示（120ms 帧合并节流；GUI 挂接时设备端不再预览候选字）
4. 松开 OK → `voice.end` → ASR 定稿 → **注入当前输入框一次**（剪贴板 + Cmd+V）→ 悬浮窗消失

注入目标是用户当前焦点窗口，悬浮窗绝不抢焦点；任何预览失败只记日志，不影响注入。

## 开发

固件逻辑测试（无硬件，cmake + ctest）：

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build
```

桌面端单测（pytest）：

```bash
companion/.venv/bin/python -m pytest companion/tests/ -q -o asyncio_mode=auto
```

- 固件状态机、协议、音频分片、UI 像素计算均有 host 侧测试覆盖；构建通过 ≠ 硬件验证通过
- 添加新页面/功能时保持硬件逻辑在 `components/bsp`、应用逻辑在 `main`；可测试的纯逻辑与 ESP-IDF/LVGL 分离
- LVGL 非线程安全；按键回调只派发轻量事件；慢操作放工作任务

### 真机验收状态

设备到货后的完整验收清单见 [`docs/ON_DEVICE.md`](docs/ON_DEVICE.md)。当前状态：**Build PASS、Host tests PASS、Device tests NOT RUN（待真机）**。重点验证项：悬浮窗真实会话（长按 PTT → 实时候选 → 松手注入一次）、Windows 焦点不抢占、连说十几秒无丢字（如丢字可调 `companion/relay.py` 的 `AUDIO_Q_MAX` 20→30-40）、BLE 吞吐/掉帧率、USB 拔线恢复、电池读数。

## 许可

MIT © 2026 FoloToy，见 [LICENSE](LICENSE)。

第三方组件：LVGL、esp_lvgl_port、NimBLE、cJSON 等版权归其各自作者。
