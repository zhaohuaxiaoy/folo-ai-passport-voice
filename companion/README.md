# AI Passport Mac Companion

Mac 端配套工具: BLE 配网 + mDNS 发布 + 火山引擎流式 ASR 中转。设备
Wi-Fi WS → 本程序 → 火山 ASR → 回传 transcript(BLE HID 注入)。

## 安装

```bash
python3 -m venv companion/.venv
companion/.venv/bin/pip install -r companion/requirements.txt
```

## 密钥(严禁提交)

- `config.local.json`(已在 .gitignore)保存 `volcano_api_key` —— 火山控制台
  新版 API Key(UUID)。**不要**把真实密钥写进 `config.example.json` 或任何
  会被 git 跟踪的文件。
- 也可用环境变量 `VOLCANO_API_KEY` 覆盖。
- 模板见 `config.example.json`(`companion_port` 是 mDNS 发布的 WS 端口, 默认 8765)。

## BLE 配网(设备免串口)

设备开机后(未配网时屏幕显示 NO WIFI 横幅), 在 Mac 上:

```bash
companion/.venv/bin/python companion/provision.py
```

- 自动识别 Mac 当前 Wi-Fi SSID(`--ssid` 可覆盖), 密码用 `getpass` 交互输入
  (`--pass` 可跳过交互, 但会进 shell 历史, 不推荐)。
- 经 BLE 写入设备(WiFi 凭据只存进程内存, **绝不落盘/打日志**), 结果回显:
  `配网成功! 设备已连上 Wi-Fi, IP = 192.168.x.x`。
- 然后启动 WS 服务并发布 mDNS, 设备免配自动连接:

```bash
companion/.venv/bin/python tools/ws_test_server.py --mdns
```

设备通过 `_ai-passport._tcp` 自动发现本机, 无需 `ws set`。若 mDNS 被路由器
隔离, 逃生通道: 串口 `ws set ws://<Mac IP>:8765` / `ws set auto` 切回自动。

## 本地测试 ASR

## 本地测试 ASR

```bash
pip install websockets
python3 asr_client.py <pcm或wav文件>                       # 豆包ASR 1.0 小时版
python3 asr_client.py <wav> --resource volc.seedasr.sauc.duration  # 豆包ASR 2.0
```

- 音频要求 16kHz / 16bit / 单声道(与设备上行一致), wav 或裸 pcm 均可。
- 分包默认 100ms(3200B), 与固件音频块一致。
- 生成测试音频(macOS 自带 TTS):
  `say -v Tingting "你好世界" -o /tmp/t.aiff && afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/t.aiff /tmp/t.wav`
