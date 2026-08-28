# AI Passport Companion（macOS / Windows）

中转程序（BLE 中央 + 火山 ASR 流式 + 输入注入）。设备只做录音终端，按住
● 说话 → 本程序收音频流 → 火山转写 → 设备屏幕实时预览（下行 TRANSCRIPT
帧）→ 定稿文本注入当前聚焦输入框。

双通道常开（2026-08-28 起设备端 BLE/USB 同时可用，不再有 mode 切换命令）：
- `"ble"`（缺省）：BLE 直连，macOS / 有蓝牙的 Windows 通用；
- `"usb"`：USB 有线直连（设备经 USB 线连电脑即可，蓝牙保持开启——USB
  供电无省电需求，数据传输走 USB 线）。relay 提供 stdin 交互 `!<命令>`
  （`!st` / `!log` / `!rst` / `!reboot` 等）经 SYS 命令面下行查状态。

通道与注入参数见 `config.local.json` 的 `channel` 等字段（`usb_port`
非空 = USB 直连端口，留空自动扫描）。**Windows 安装、防火墙、
注入焦点提示见 [WINDOWS.md](WINDOWS.md)。**

## 安装

```bash
python3 -m venv companion/.venv
companion/.venv/bin/pip install -r companion/requirements.txt
```

## 密钥（严禁提交）

- `config.local.json`（已在 .gitignore）保存 `volcano_api_key` —— 火山控制台
  新版 API Key（UUID）。**不要**把真实密钥写进 `config.example.json` 或任何
  会被 git 跟踪的文件。
- 也可用环境变量 `VOLCANO_API_KEY` 覆盖。
- 模板见 `config.example.json`（`volcano_model=bigmodel_async` + `enable_nonstream`
  二遍识别：流式实时上屏 + 每句 VAD 定稿纠错）。

## 权限（首次使用必须）

输入注入走「剪贴板 + Cmd+V」，需要 macOS **辅助功能**授权：

1. 系统设置 → 隐私与安全性 → 辅助功能 → 勾选运行终端（或 iTerm）。
2. 未授权时 relay/inject 会明确报错（-1743/-25211），按提示操作即可。

## 使用

设备开机（屏幕显示 BLE 状态）→ 运行：

```bash
companion/.venv/bin/python companion/relay.py
```

- 自动扫描 "AI Passport" 并连接（首次自动配对）；按住设备 ● 说话，
  屏幕实时预览转写，松手后定稿文本注入当前输入框（TextEdit 实测中文/英文）。
  **行为契约**：中间结果只下行设备屏幕预览（`final:false`），输入框只落
  定稿一次（`final:true`）——剪贴板粘贴是插入语义，中间多次注入会叠加文本。
- `--device AA:BB:CC:DD:EE:FF` 跳过扫描直连指定设备。
- `--no-inject` 只转写（设备屏幕预览照常下行），不注入输入框。
- `--no-approval` 关闭审批演示；默认发 `agent.approval_request` → 设备按键决策 → 回传 `agent.action`。
- `--timeout <秒>` 无设备超时（默认 60）。
- `--dry-run` 等价 `--no-inject`，且注入动作只打印将执行的命令（不真执行）。

单独测试注入：

```bash
companion/.venv/bin/python companion/inject.py "你好世界 123"      # 粘贴到当前输入框
companion/.venv/bin/python companion/inject.py "你好" --dry-run     # 只打印命令
```

## 本地测试 ASR（无需真机）

```bash
companion/.venv/bin/python companion/asr_client.py /tmp/t.wav                 # 流式
companion/.venv/bin/python companion/asr_client.py /tmp/t.wav --nonstream     # 二遍非流式
```

- 音频要求 16kHz / 16bit / 单声道（与设备上行一致），wav 或裸 pcm 均可，
  分包默认 100ms（3200B）。
- 生成测试音频（macOS 自带 TTS）：
  `say -v Tingting "你好世界" -o /tmp/t.aiff && afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/t.aiff /tmp/t.wav`

## 单元测试

```bash
companion/.venv/bin/python companion/tests/test_relay.py
companion/.venv/bin/python companion/tests/test_serial_frame.py
companion/.venv/bin/python companion/tests/test_serial_transport.py
companion/.venv/bin/python companion/tests/test_serial_relay.py
companion/.venv/bin/python companion/tests/test_inject_win.py
```

- `test_relay.py`：FakeTransport + FakeASR + FakeInjector 注入 —— 分片重组
  跨界 / voice 状态机 / 注入序列 / 掉帧对账 / 审批闭环 / 转写下行（final
  标记与 128B 切分）/ 超时。
- `test_serial_frame.py`：USB 帧协议编解码（与固件 `usb_link_framing.c`
  逐字节同构，共享测试意图）—— 回环 / 垃圾前缀 / 假锚点 / 超长 / 坏校验 /
  噪声混流恢复。
- `test_serial_transport.py`：`os.openpty` 假设备侧 —— 握手 ping/pong /
  超时 / 订阅前缓冲 / EVENT+AUDIO 分发 / CTRL 到达 / SYS 往返 / 拔线断连。
- `test_serial_relay.py`：pty 假设备 + relay 集成全流程（hello → voice.start
  → 音频帧 → voice.end → 注入 + 下行预览/定稿 → `!st` syscmd 往返 → 断连收束）。
- `test_inject_win.py`（Mac 可跑，不触碰 win32）：dry_run 跨平台 / 平台与
  pywin32 缺失指引 / 前台护栏判定。真实粘贴为 Windows 真机项（见 WINDOWS.md）。

## 协议契约

- 上行 EVENT 行 ≤512B（含 '\n'）：`hello / voice.start / voice.end / agent.action / status`（`status` 带 drop 计数）。
- 上行 AUDIO：3200B 裸 PCM 帧（100ms @16kHz/16bit/mono）。
- 下行 CTRL 行 ≤2048B（JSON 行）：`transcript`（`final:false` 预览 / `final:true` 定稿，
  超 128 字符分多条）、`agent.approval_request`、`agent.status`、`mac.metrics`。
- USB 通道额外有二进制帧层（`serial_frame.py` ↔ 固件 `usb_link_framing.c`
  逐字节同构）：EVENT/AUDIO 上行、CTRL/SYS 下行、SYS_RESP 上行（命令输出；
  SYS 命令面 = 设备 console 同一批命令，USB 模式替代控制台）。
