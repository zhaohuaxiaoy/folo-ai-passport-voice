# AI Passport Windows 使用文档

Windows 移植（仅兼容 Windows 10 与 Windows 11）三通道：

| 通道 | 适用 | 连接方式 |
|---|---|---|
| `"ble"`（缺省） | 有蓝牙的 Win10/Win11 | 设备 BLE 广播 "AI Passport"，PC bleak winrt 后端直连 |
| `"wifi"` | **无蓝牙**的 Win10/Win11 | 设备 STA 连家中 WiFi，mDNS 自动发现 PC 的 WS 服务 |
| `"usb"` | 任意电脑（无蓝牙/不便配 WiFi 的场合） | USB 线直连（设备 USB-Serial-JTAG），PC 自动扫描端口 |

通道由 `companion/config.local.json` 的 `channel` 字段手动选择，不自动判断
（设备侧需对应 `mode ble` / `mode wifi` / `mode usb`，见下）。

## 环境要求

- Windows 10 1803 及以上（建议 21H2）/ Windows 11（bleak winrt 后端要求）
- Python 3.10+；首次运行防火墙弹窗需点「允许」（专用网络）

## 安装

```bat
python -m venv companion\.venv
companion\.venv\Scripts\pip install -r companion\requirements.txt
```

`requirements.txt` 用 PEP 508 平台标记按系统自动选包：
Windows 装 `bleak[winrt]`（BLE 通道）+ `pywin32`（注入）；
`pyserial` 全平台安装（USB 通道数据收发 + Windows 串口配网）；
websockets / zeroconf 双系统通用。

## 配置（config.local.json）

```json
{
  "volcano_api_key": "你的火山 API Key",
  "channel": "ble",
  "ws_port": 8765,
  "ws_connect_timeout": 120,
  "usb_port": "",
  "inject_focus_delay": 2.0
}
```

| 键 | 缺省 | 说明 |
|---|---|---|
| `channel` | `"ble"` | `"ble"`、`"wifi"` 或 `"usb"`；无效值 relay 报错退出 |
| `ws_port` | 8765 | WiFi 通道：本机 WS server 端口（设备 STA 主动连） |
| `ws_connect_timeout` | 120 | WiFi 通道：等设备连上 WS 的超时（秒） |
| `usb_port` | `""` | USB 通道：串口路径；留空 = 自动扫描（VID 0x303A / PID 0x1001）；多设备时填指定端口（如 `COM5`） |
| `inject_focus_delay` | 2.0 | 注入前等用户切换到目标窗口的秒数 |

密钥安全：`config.local.json` 已被 git 忽略；`volcano_api_key` 也可用环境变量
`VOLCANO_API_KEY` 提供。WiFi 密码只存在于设备 NVS（串口 `wifi set` 不回显），
**绝不**写入任何配置文件。

## 使用

```bat
companion\.venv\Scripts\python companion\relay.py
```

### 通道：BLE（有蓝牙电脑）

设备开机（屏幕显示 BLE 状态）→ 运行 relay → 首次连接会弹系统配对窗口，
确认即可。其余流程与 Mac 一致：按住设备 ● 说话，设备屏幕实时预览转写，
松手后定稿文本注入当前聚焦输入框。

### 通道：WiFi（无蓝牙电脑）

设备侧 USB 串口控制台（Windows 可用任意串口终端，或 `pyserial`）配网：

```
mode wifi                        # 切 WiFi 模式(蓝牙关闭省电)
wifi set <你的SSID> <密码>       # 密码只写 NVS, 不回显
ws set auto                      # 缺省即 auto: mDNS 自动发现 PC
st                               # 确认 mode: WiFi / ws: connected
```

PC 侧把 `channel` 改为 `"wifi"` 后运行 relay：本机起 WS server（端口
8765）并发布 `_ai-passport._tcp`，设备经 mDNS 自动找到并连入。

- **防火墙**：首次监听 8765 会弹「允许访问」——勾选专用网络并允许。
- **同一局域网**：设备和 PC 必须在同一网段；路由器开启 AP 隔离会阻断发现。

切回 Mac 使用：设备控制台 `mode ble` 即可（BLE 模式会停掉 WiFi 射频）。

### 通道：USB（有线）

任何电脑都能用 USB 线直连（ESP32-C3 原生 USB-Serial-JTAG 口）。USB 供电
不耗电池，此模式下**蓝牙与 WiFi 全部关闭**（省电）。

首次启用（设备侧，任意模式的控制台或经 USB 的 relay `!mode usb`）：

```
mode usb                        # 切 USB 模式 = 写 NVS + 重启(约 1-2s)
```

设备重启后插 USB 线连电脑。PC 侧把 `channel` 改为 `"usb"` 后运行 relay：
自动扫描 ESP32-C3 串口（多设备时打印清单取第一个，可用 `usb_port` 指定），
握手成功后与 BLE 流程一致：按住设备 ● 说话 → 设备屏幕实时预览 → 松手
定稿注入当前输入框。

**USB 模式没有设备控制台**（REPL 与数据通道独占串口，启动时已跳过）。
配网/状态经 relay 的 stdin 交互（`!<命令>` 下行 SYS 命令面，与设备 console
同一批命令）：

```
!mode wifi                      # 切回 WiFi 模式(重启生效)
!wifi set <你的SSID> <密码>     # 密码明文经本地 USB(与串口控制台一致), 不落盘
!ws set auto                    # 自动发现本机 WS server
!log                            # 取回设备日志环(esp_log 已重定向 RAM 环)
!st                             # 会话状态
!reboot / !factory              # 重启 / 恢复出厂
```

> 注意：`mode usb` 切换 = 设备重启（运行时摘除 REPL 阻塞读不安全，设计上
> 杜绝）；从 USB 切走（`!mode ble/wifi`）同样重启生效。USB 模式下设备日志
> 不再实时上屏，进 4KB RAM 环，经 `!log` 取回。
> `wifi set` 密码经本地 USB 明文传输——与既有串口控制台一致，本地线缆可接受。

## 注入焦点提示（Windows）

注入 = 剪贴板（CF_UNICODETEXT）+ SendInput Ctrl+V（实现为 pywin32
`keybd_event`，Win10/11 均有效）。relay 收到定稿后：
1. 先检查前台窗口——若还是 relay 自己的控制台（标题含 python/cmd/PowerShell
   等）→ **拒绝注入**并给出指引；
2. 等 `inject_focus_delay` 秒（缺省 2s），此时请点击目标输入窗口；
3. 复查前台——仍停在控制台则中止（不往控制台乱粘）。

不自动置顶/抢焦点（Windows 前台锁定使 SetForegroundWindow 不可靠）。
独立测试注入：

```bat
companion\.venv\Scripts\python companion\inject_win.py "你好世界 123"
companion\.venv\Scripts\python companion\inject_win.py "你好" --dry-run
```

## 验收清单（Windows 真机，NOT RUN）

> 以下项目需 Windows 机器实机验证，本仓库开发环境（macOS）无法执行。
> 固件侧验证同样未跑（本机无 ESP-IDF 构建环境）。

- [ ] **Win10 21H2 + Win11 各一台**：BLE 通道全流程（配对弹窗 → 录音 → 转写 → 注入）
- [ ] **Win10 21H2 + Win11 各一台**：WiFi 通道全流程（串口配网 → mDNS 发现 → WS 连上 → 全流程）
- [ ] **无蓝牙电脑**：WiFi 通道可用；`st` 显示 mode: WiFi、ws: connected
- [ ] **USB 通道**：`mode usb`（重启）→ relay 自动扫描端口 → 握手 → 全流程；`!mode` / `!log` / `!st` 往返；拔线 → relay 报断连收束
- [ ] **USB 串口枚举**：Windows COM 自动识别（VID/PID 匹配）；多设备时 `usb_port` 指定生效；端口被占用给可理解报错
- [ ] **USB 省电**：USB 模式蓝牙 + WiFi 均关闭（`st` 无射频迹象 / 电流实测）
- [ ] **USB 音频流**：32KB/s 持续流 100ms 写超时不异常掉帧（瓶颈在主机读侧）
- [ ] **省电**：WiFi 模式蓝牙彻底关闭（`st` 无 BLE 广播迹象 / 电流实测）；BLE 模式 WiFi 未启动
- [ ] **模式切换**：`mode wifi` ↔ `mode ble` ↔ `mode usb` 往返，NimBLE controller re-sync 后广播自动恢复（固件唯一硬依赖）
- [ ] **配对弹窗**：Windows 首次 BLE 配对 UX（bleak winrt 触发系统弹窗）
- [ ] **注入焦点**：真实粘贴中文/英文到记事本；焦点护栏拦截 relay 控制台；focus_delay 生效
- [ ] **防火墙**：8765 放行后设备可连；未放行时 relay 给出可理解的错误
- [ ] **30 分钟内存**：长跑 WiFi/USB 通道无内存增长（relay 侧 + 固件 `st` 堆水位）
- [ ] **固件侧**：`idf.py build` 三模式构建、`esp_driver_usb_serial_jtag` REQUIRES、USB 模式堆栈/功耗实测

## 单元测试（Mac 可跑）

```bash
companion/.venv/bin/python companion/tests/test_relay.py
companion/.venv/bin/python companion/tests/test_ws_transport.py
companion/.venv/bin/python companion/tests/test_serial_frame.py
companion/.venv/bin/python companion/tests/test_serial_transport.py
companion/.venv/bin/python companion/tests/test_serial_relay.py
companion/.venv/bin/python companion/tests/test_inject_win.py
```
