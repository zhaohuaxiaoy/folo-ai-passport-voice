# AI Passport Windows 使用文档

Windows 移植（仅兼容 Windows 10 与 Windows 11）双通道：

| 通道 | 适用 | 连接方式 |
|---|---|---|
| `"ble"`（缺省） | 有蓝牙的 Win10/Win11 | 设备 BLE 广播 "AI Passport"，PC bleak winrt 后端直连 |
| `"wifi"` | **无蓝牙**的 Win10/Win11 | 设备 STA 连家中 WiFi，mDNS 自动发现 PC 的 WS 服务 |

通道由 `companion/config.local.json` 的 `channel` 字段手动选择，不自动判断
（设备侧需对应 `mode ble` / `mode wifi`，见下）。

## 环境要求

- Windows 10 1803 及以上（建议 21H2）/ Windows 11（bleak winrt 后端要求）
- Python 3.10+；首次运行防火墙弹窗需点「允许」（专用网络）

## 安装

```bat
python -m venv companion\.venv
companion\.venv\Scripts\pip install -r companion\requirements.txt
```

`requirements.txt` 用 PEP 508 平台标记按系统自动选包：
Windows 装 `bleak[winrt]`（BLE 通道）+ `pywin32`（注入）+ `pyserial`（串口配网）；
websockets / zeroconf 双系统通用。

## 配置（config.local.json）

```json
{
  "volcano_api_key": "你的火山 API Key",
  "channel": "ble",
  "ws_port": 8765,
  "ws_connect_timeout": 120,
  "inject_focus_delay": 2.0
}
```

| 键 | 缺省 | 说明 |
|---|---|---|
| `channel` | `"ble"` | `"ble"` 或 `"wifi"`；无效值 relay 报错退出 |
| `ws_port` | 8765 | WiFi 通道：本机 WS server 端口（设备 STA 主动连） |
| `ws_connect_timeout` | 120 | WiFi 通道：等设备连上 WS 的超时（秒） |
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

## 注入焦点提示（Windows）

注入 = 剪贴板（CF_UNICODETEXT）+ SendInput Ctrl+V。relay 收到定稿后：
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
- [ ] **省电**：WiFi 模式蓝牙彻底关闭（`st` 无 BLE 广播迹象 / 电流实测）；BLE 模式 WiFi 未启动
- [ ] **模式切换**：`mode wifi` ↔ `mode ble` 往返，NimBLE controller re-sync 后广播自动恢复（固件唯一硬依赖）
- [ ] **配对弹窗**：Windows 首次 BLE 配对 UX（bleak winrt 触发系统弹窗）
- [ ] **注入焦点**：真实粘贴中文/英文到记事本；焦点护栏拦截 relay 控制台；focus_delay 生效
- [ ] **防火墙**：8765 放行后设备可连；未放行时 relay 给出可理解的错误
- [ ] **30 分钟内存**：长跑 WiFi 通道无内存增长（relay 侧 + 固件 `st` 堆水位）
- [ ] **固件侧**：`idf.py build` 双模式构建、WiFi 模式堆栈/功耗实测

## 单元测试（Mac 可跑）

```bash
companion/.venv/bin/python companion/tests/test_relay.py
companion/.venv/bin/python companion/tests/test_ws_transport.py
companion/.venv/bin/python companion/tests/test_inject_win.py
```
