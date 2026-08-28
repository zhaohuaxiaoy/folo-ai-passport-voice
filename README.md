# FoloToy AI Passport · 语音输入与 AI 控制面

[简体中文](README.md) | [English](README.en.md)

**FoloToy AI Passport** 是一台 AI 随身硬件（ESP32-C3）——但它不是"另一个键盘"。按住设备按键说话，候选字**实时显示在 Mac/Windows 桌面的悬浮窗**里，松手后识别完成的文本**自动注入当前输入框**（微信 Mac 端式语音输入）；与此同时，Agent 的思考、执行、审批全过程都在设备屏幕上可视化，关键操作必须由你**物理按键确认**。

一句话：把 AI 从网页搬到桌面输入框，再把它的**耳朵**（语音）、**眼睛**（状态可视）和**手刹**（物理审批）装进你手里的实体设备。

```text
按住说话 ──► 设备录音 ──► 桌面端中转 ──► 火山流式 ASR
                    │  经 BLE / USB
                    ▼  识别中 partial 实时
            悬浮窗显示候选字
                    │
                    ▼  松手 → 定稿 final
            注入当前输入框（仅一次）
```

> 仓库同时包含**固件**（ESP-IDF，设备端）与**桌面端**（companion/，Mac/Windows 中转程序），二者通过 BLE / USB 双通道配合工作。

## 架构总览

| 层 | 组件 | 职责 |
| --- | --- | --- |
| 设备端 | ESP32-C3 固件（ESP-IDF 5.5 + LVGL 9.5） | 状态机、按键/录音、UI 渲染、双通道传输、功耗管理 |
| 桌面端 | companion（macOS / Windows） | 双通道接入、火山流式 ASR 转发、悬浮窗、剪贴板注入、向导、托盘 |
| 云端 | 火山引擎大模型流式 ASR | 音频 → 文本（partial 全量累积 / final 定稿） |

数据流：设备采集 16kHz 音频 → 按 100ms 一帧经 **BLE / USB** 任一通道上行 → 桌面端 relay 流式送入火山 ASR → 中间结果实时回显到悬浮窗与设备屏幕 → 松手定稿后注入一次、收窗。反向通道承载 Agent 状态、审批请求与按键裁决。

## 项目亮点

- **双通道冗余链路**：一套音频/事件协议跑两条物理通道——BLE（Mac 直连，GATT 服务 `0xA2B0`）、USB-Serial-JTAG（有线调试 + 完整控制台命令面）。任意通道断线自动收束回 READY、审批保持等待重连，链路故障不会废掉设备。
- **状态机驱动 + 快照渲染**：核心状态机是纯 C 归约器（`state + event → action`），零 ESP-IDF 依赖，7 组 host 测试直接跑在 PC 上；UI 渲染由快照差异驱动，逻辑与硬件彻底解耦、可单测、可移植。
- **低资源流式音频管线**：在只有 **400KB SRAM** 的 ESP32-C3 上以 3200B/100ms 帧流式上行——静态环形缓冲零动态分配、源端丢帧不整段缓存、掉帧对账防静默丢失，长句不断流。
- **物理安全审批**：Agent 的权限请求（改文件、执行命令）不是推送通知，而是设备屏幕上的审批页——**OK 批准 / UP 拒绝**，真实按键才算数；审批态屏幕永不熄屏。
- **两级功耗管理**：20s 无操作关背光（渲染跳过、面板冻结最后一帧），60s 面板 SLPIN 断电进入 μA 级睡眠，任意键瞬时唤醒、内容免重绘。
- **桌面端体验打磨**：悬浮窗**绝不抢焦点**（Windows `WS_EX_NOACTIVATE` + macOS 热路径零 WindowServer 同步，按住说话不卡桌面）、120ms 帧合并只刷最新帧、5 步向导零音频握手验证 ASR Key、托盘驻留、双平台注入（CJK 走剪贴板通道）。

## 功能特性

### 设备端（固件）

- **按住说话**：READY 态按住**音量+**开始录音（滴声提示），松开自动发送；无取消窗口、无超时残留
- **转写中退出**：TRANSCRIBING 态单击音量+ 立即退出转写场景回 READY，迟到的识别文本不上屏
- **LISTENING 页**：录音中显示麦克风图标 + 计时，替代传统 REC 红点/音量条
- **Agent 工作流可视化**：THINKING / RUNNING / DONE 状态、任务回显、离线横幅——AI 在做什么，屏幕上看得到
- **物理审批**：Agent 审批请求进入审批页，OK 批准 / UP 拒绝 / DOWN 看 Diff 详情（桌面端默认关闭审批）
- **三键交互**：UP/DOWN/OK —— 按住说话、转写中单击退出、DOWN 回车、OK 双击清空输入框（全局语义）
- **完整状态机**：HOME → READY → LISTENING → TRANSCRIBING → AGENT_RUNNING → APPROVAL → DONE
- **提示音**：开始 / 发送 / 审批提醒 / 完成 / 拒绝 / 错误六种
- **双通道传输**：BLE / USB-Serial-JTAG（含完整控制台命令面）
- **低资源音频管线**：3200B/100ms 帧、静态环形缓冲、源端丢帧不整段缓存、掉帧对账
- **两级息屏**：20s 无操作关背光 / 60s 面板 SLPIN 断电 / 任意键唤醒；审批态常亮
- **控制台命令**：`st`（堆/栈水位、连接、掉帧统计）、`log [offset]`（日志环导出）、`rst`（复位原因）、`time`（校时）、`bt scan/dtx`（射频诊断）、`reboot`、`factory`（清 NVS）

### 桌面端（companion/，macOS + Windows）

- **候选字悬浮窗**（核心）：ASR 中间结果**全量累计文本**实时显示在屏幕底部居中的无边框置顶小窗，自动换行、随内容向上生长；松手后定稿文本注入输入框一次、窗口消失
- **不抢焦点**：悬浮窗绝不 focus；Windows 走 `WS_EX_NOACTIVATE` + `SWP_NOACTIVATE`，macOS 热路径不触发 WindowServer 同步（按住说话不卡桌面）
- **高频帧合并**：120ms 合并窗口只刷最新帧，首帧立即渲染，杜绝逐帧重绘卡顿
- **5 步向导**：欢迎 → 自动发现设备（BLE → USB）→ 火山 ASR Key 配置（零音频握手测试，不回显）→ 系统授权引导（macOS）→ 完成状态页
- **托盘驻留**：连接后驻留系统菜单栏/托盘，状态行 + 诊断 + 设置
- **诊断页**：USB 通道完整设备命令面；BLE 通道只读运行状态
- **注入**：macOS 剪贴板 + Cmd+V（需辅助功能授权；中文必须走剪贴板通道）；Windows 独立注入器

## 按键操作

| 按键 | 语境 | 动作 |
| --- | --- | --- |
| **UP（音量+）按住** | READY | 开始录音（PTT，滴声提示），松开自动发送转写 |
| **UP（音量+）单击** | TRANSCRIBING | 退出转写场景，立即回 READY（迟到的识别结果不上屏） |
| OK **双击** | 任意 | 清空当前输入框全部文字 |
| DOWN 单击 | HOME / READY | 输入框回车（提交） |
| OK 单击 | HOME | 进入 READY（工作流就绪） |
| OK 单击 | APPROVAL | 批准 Agent 请求 |
| UP 单击 | APPROVAL | 拒绝 Agent 请求 |
| DOWN 单击 | APPROVAL | 查看 Diff 详情 |

> 长按阈值：UP 长按 300ms 触发录音；OK 长按 500ms 锁屏/解锁（锁定 = 省电息屏，按键照常执行但不亮屏）。

## 快速开始

### 桌面端（Mac / Windows）

**直接使用打包好的客户端（推荐，非开发者无需碰终端）**：

- **macOS**：[下载 AI-Passport-macOS.dmg](https://github.com/zhaohuaxiaoy/folo-ai-passport-voice/releases/latest)（解压拖入 /Applications）
- **Windows**：[下载 AI-Passport-Windows.exe](https://github.com/zhaohuaxiaoy/folo-ai-passport-voice/releases/latest)（双击运行）

Release 由 GitHub Actions 双平台自动构建（`v*` tag 触发，见 `.github/workflows/build-apps.yml`）。

首次启动进入 5 步向导：欢迎 → 自动发现设备（BLE → USB）→ 火山 ASR Key 配置 → 系统授权引导（macOS）→ 状态页；连接后驻留系统托盘。首次运行自动生成 `companion/config.local.json`（火山 API Key，**不入 git**）。

### 获取火山 API Key

语音识别依赖火山引擎流式 ASR，需在 [火山引擎控制台](https://console.volcengine.com/speech/new/setting/apikeys) 申请 API Key：

1. 注册/登录[火山引擎](https://console.volcengine.com)并完成实名认证
2. 打开 [API Key 管理页](https://console.volcengine.com/speech/new/setting/apikeys)（语音技术 → 概览 → API Key 管理）
3. 点击"新建 API Key"，选择 `大模型语音识别` 业务，复制生成的 Key（UUID 格式）
4. 在桌面端 5 步向导的"火山 ASR Key"页粘贴（零音频握手测试，不会回显密钥），或写入 `companion/config.local.json` 的 `volcano_api_key` 字段；也可用环境变量 `VOLCANO_API_KEY` 覆盖

> **安全**：API Key 只存于 `companion/config.local.json`（已被 .gitignore 忽略）或向导本地配置，**严禁提交进 git**。Key 泄露可在控制台随时吊销重建。

> **自构建**：Mac 上 `companion/.venv/bin/python companion/build/pack.py --dmg`；Windows 需在 Windows 构建机运行 `python build/pack.py`（产物 `dist/AI Passport.exe`）。

**从源码运行（开发）**：

```bash
pip install -r companion/requirements.txt
cp companion/config.example.json companion/config.local.json   # 填入火山 API Key
companion/.venv/bin/python companion/fre_app.py                 # 向导 GUI（--dry-run 假链路演练）
companion/.venv/bin/python companion/relay.py                   # CLI 中转（BLE 自动扫描 "AI Passport"）
```

macOS 首次使用需授予**辅助功能**权限（注入剪贴板+Cmd+V 必需）与蓝牙权限。Windows 使用说明见 [`companion/WINDOWS.md`](companion/WINDOWS.md)。

### 固件（ESP32-C3）

需要 ESP-IDF 5.5.x（已知环境 5.5.3）+ Python 3.10+（本机 `.venv` 在 `companion/`）。

**首次构建**（会经 Component Manager 拉取 LVGL、`esp_lvgl_port`、`button`、`esp_codec_dev` 等依赖）：

```bash
git clone <repo-url> && cd ai-passport
python3 -m venv companion/.venv && companion/.venv/bin/pip install -r companion/requirements.txt
source "$HOME/esp/esp-idf-v5.5.3/export.sh"   # 或你的安装路径
idf.py set-target esp32c3
idf.py build
```

**刷机与日志**（设备用 USB 线连电脑，`/dev/cu.usbmodem*` 按实际端口替换）：

```bash
idf.py -p /dev/cu.usbmodem1101 flash         # 烧录 + 自动复位
idf.py -p /dev/cu.usbmodem1101 monitor       # 串口日志（REPL 控制台）
```

烧录后控制台为 USB-Serial-JTAG（GPIO18/19；UART0 默认 TX GPIO21 与本板背光冲突）。

**主机逻辑测试**（固件状态机/协议/音频分片，纯 C，无需硬件）：

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build
```

**USB 控制台命令**（`idf.py monitor` 的 REPL，或桌面端诊断页经 SYS 帧下发）：

| 命令 | 作用 |
| --- | --- |
| `st` | 系统状态：堆/栈水位、BLE/USB 连接、MTU、掉帧、电池 |
| `log [offset]` | 导出日志环（有 USB 主机时日志进 16KB RAM 环）；环超 2048B 时按 `log 2048` / `log 4096` 分段取全量 |
| `rst` | 复位原因（1 上电 / 4 软件 / 11 USB-flash） |
| `time` / `time set <epoch>` | 查看 / 设置 wall-clock（校时源仅电脑客户端） |
| `bt scan` / `bt dtx [ch]` | 射频诊断：主动扫描周边广播 / 固定信道强制发射 |
| `reboot` | 软件重启 |
| `factory` | 清空 NVS 并重启（出厂复位） |

## 仓库结构

```text
main/                    ESP32-C3 固件:状态机、UI、双通道传输、音频流、控制台命令
components/bsp/          板级驱动:显示 / 按键 / 音频 / 电池 / 共享 I2C(bsp_pins.h 为硬件事实唯一来源)
companion/               桌面端:relay 中转(ASR 流式 + 注入)、悬浮窗、向导、托盘、双通道传输
tests/                   可脱离硬件运行的固件逻辑测试(纯 C,ctest)
docs/                    硬件开发指南与验收文档
sdkconfig.defaults       ESP32-C3、USB console、Flash、LVGL 默认配置
partitions.csv           自定义分区表(factory 4MB)
```

## 工作原理

1. 设备 READY 态按住**音量+** → 滴声 → `voice.start` → 3200B/100ms 音频帧经 BLE（GATT NOTIFY）/ USB 上行
2. 桌面端 relay 把音频帧流式送入火山引擎 ASR（`bigmodel_async`，每包结果携带**全量累计文本**）
3. 中间结果（partial）→ 悬浮窗实时显示（120ms 帧合并节流；GUI 挂接时设备端不再预览候选字）
4. 松开音量+ → `voice.end` → ASR 定稿 → **注入当前输入框一次**（剪贴板 + Cmd+V）→ 悬浮窗消失；转写中单击音量+ 可随时退出场景
5. 定稿文本同时回显设备屏幕；Agent 状态（THINKING / RUNNING / DONE）与审批请求下行到设备
6. 审批请求 → 设备进入审批页 → OK/UP 物理按键裁决 → 结果上行 → Agent 继续执行

注入目标是用户当前焦点窗口，悬浮窗绝不抢焦点；任何预览失败只记日志，不影响注入。

## 设计决策

- **为什么双通道**：BLE 覆盖有蓝牙的 Mac；USB 是调试器兼最后保底——任何单一链路失效都可用另一条继续工作，断线事件统一收束，不残留半开会话。
- **为什么状态机是纯 C 归约器**：`state + event → action` 的归约模式让全部转移逻辑**零硬件依赖**，8 组 ctest 直接覆盖状态转移、协议编解码、音频分片、UI 像素计算；UI 按快照差异渲染，加页面不加状态耦合。
- **为什么音频管线做静态环形缓冲**：ESP32-C3 只有 400KB SRAM，动态分配 + 整段缓存一次长录音会直接爆内存。3200B/100ms 帧、源端丢帧、掉帧对账，是"入门级 MCU 也能当 AI 输入设备"的关键。
- **为什么物理审批**：AI 自动改文件、执行命令是有风险的——审批不放通知栏，放设备屏幕，按实体键才算数，且审批态永不熄屏。
- **为什么两级息屏**：20s 关背光（省的是背光 LED 的 mA 级），60s 面板 SLPIN（省的是内部振荡器 + 驱动，μA 级）——分级是因为省电目标不同，唤醒体验一致：任意键瞬亮、内容免重绘（ST7789 DRAM 睡眠期间保留）。

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

设备到货后的完整验收清单见 [`docs/ON_DEVICE.md`](docs/ON_DEVICE.md)。当前状态：**Build PASS、Host tests PASS（8/8）、真机连续实测 PASS**。已真机验证：按住说话多次会话、连续快速长按无丢键、松手后无假滴声/假录音、转写中单击退出、双通道传输、休眠唤醒。仍待重点验收：Windows 焦点不抢占、长句连续十几秒无丢字（如丢字可调 `companion/relay.py` 的 `AUDIO_Q_MAX` 20→30-40）、USB 拔线恢复、电池读数。

## 扩展性

语音输入是这条管道上的**第一个应用**：同一套音频流 + 事件协议（双通道共用）天然可以承载更多能力——转写、翻译、Agent 指令、远程控制。固件与桌面端均在 MIT 许可下开源，换壳、加页面、接新服务都从干净的架构边界开始。

## 许可

MIT © 2026 FoloToy，见 [LICENSE](LICENSE)。

第三方组件：LVGL、esp_lvgl_port、NimBLE、cJSON 等版权归其各自作者。
