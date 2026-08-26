# 性能 / 内存后续审查(悬浮窗转圈同类)

> 基线:HEAD 含 macOS 悬浮窗 WindowServer 修复之后。
> 范围:companion 热路径、无界增长、固件常驻 RAM/Flash。只报告不修。
> 同类定义:主线程/事件循环/WindowServer 被热路径同步拖死,或缓冲无上界/配大了却用不上。

---

## 1. 和「转圈」同一类(热路径同步、会卡 UI / 系统)

### P2-1 `phase_q` 无界 + `poll` 永久 100ms 唤醒

- `fre_app.py:98` `queue.Queue()` 无 `maxsize`。ASR partial 从 relay 线程 `put`,UI 每 100ms 才 `get_nowait` 一批。
- UI 一旦卡(字体/托盘/诊断 `log` 大段 insert),队列只增不减,字符串全量累计文本会堆到几十 MB,卡得更死——和转圈是正反馈。
- `poll` 在空闲(已连接、没说话)也 10 次/秒 `after(100)`,macOS 无法 App Nap,托盘常驻时一直有定时器。
- 修法:`Queue(maxsize=64)` 满则丢旧 partial 留最新;空闲拉长到 500ms,有 candidate 再加密。

### P2-2 探测可叠两个线程,WiFi 探测绑死 8765

- `fre_app.py:302` `start_discover` 不查旧线程是否还活着。连点「重新设置」→ 两个 `asyncio.run(probe_channels)`。
- `probe.py:54-60` WiFi 探测 `connect` 等设备,绑 `ws_port`(默认 8765)。第二个探测 bind 失败被吞,界面显示「未发现」。
- 修法:入口 `is_alive()` 拒绝或先停旧探测;探测用临时端口。

### P2-3 每次说话都跑审批演示

- GUI `Relay(...)` 未关 `do_approval`,默认 `True`(`relay.py:191,574`)。
- 每次 `voice.end` 后台发 `agent.approval_request`(假任务 task-001),设备切审批页。
- 影响:多一次 CTRL 往返 + 设备 UI 重绘;和语音输入产品路径无关,还抢事件队列。
- 修法:GUI 默认 `do_approval=False`;CLI 才开 `--approval`。

### P2-4 托盘每次 phase 重建整棵菜单

- `_tray_phase` 在 `session_start` / `session_end` / connected 都 `update_menu` → `pystray` 重建 NSStatusItem 菜单。
- 说话开始/结束各一次,macOS 菜单栏同步更新,和悬浮窗同一拍,加重主线程。
- 修法:只在 connected/disconnected 变的时候重建;listening 用标题字符串原地改,或节流。

### P3-1 unicode 注入逐字符 `CGEventPost`

- `inject.py:59-65` 每个码点两次 Post,无间隔。长句 100 字 = 200 次用户输入事件,目标 App 主线程会卡。
- 已在 `to_thread`,不冻 companion 事件循环,但用户会感到「注入那一下系统顿一下」。
- 修法:短文本保持 unicode;超 N 字走剪贴板。或批量 Unicode 字符串一次 SetUnicodeString。

---

## 2. 内存:无上界 / 配太大

### P2-5 ASR WebSocket `max_size=None` + `gzip.decompress` 无上限

- `asr_client.py:195` `websockets.connect(..., max_size=None)`。
- `parse_server`(`asr_client.py:70-74`)按帧头 `size` 切片后直接 `gzip.decompress`,没有 `max_length`。
- 畸形/异常服务端帧可解出任意大对象,companion 进程 OOM。语音结果 JSON 通常 <10KB。
- 修法:`max_size=256*1024`;`gzip.decompress(payload, max_length=64*1024)`。

### P2-6 诊断页 Text 无限 `insert`

- `fre_app.py:678-682` `_diag_append` 只追加。USB `log` 一次最多 4KB,连点或长时间开着诊断页只涨不裁。
- 审计 P2-9,仍未修。
- 修法:超 500 行删头部,或 `log` 输出覆盖而非追加。

### P3-2 音频队列 10 秒偏大

- `AUDIO_Q_MAX=100` × 3200B ≈ **320KB** PCM 常驻峰值(`relay.py:61`)。
- ASR 卡顿时宁可丢帧,2s(20 帧,64KB)足够抖动。
- `RESULTS_Q_MAX=256`(`asr_client.py:153`)每条是全量累计文本,偏肥;32 足够预览。

### P3-3 `StreamingASR(cfg={})` 空 dict 陷阱

- `asr_client.py:173` `self.cfg = cfg or load_config()`。传入 `{}` 会读真实配置(可能含 Key)。
- 审计 P3,测试/调用方行为意外,不是热路径泄漏。

---

## 3. 固件 RAM / Flash 可砍(C3 无 PSRAM)

### P2-7 当前 `sdkconfig` 仍编进 LVGL 示例和 Demo

- `sdkconfig:2850` `CONFIG_LV_BUILD_EXAMPLES=y`
- `sdkconfig:2856` `CONFIG_LV_BUILD_DEMOS=y`
- `sdkconfig.defaults` **没有**关掉,menuconfig 默认把示例链进固件。Flash 多占约 100KB+,部分静态数据进 RAM。
- 修法:defaults 里显式 `CONFIG_LV_BUILD_EXAMPLES=n`、`CONFIG_LV_BUILD_DEMOS=n`,再 `idf.py fullclean && build`。

### P2-8 NimBLE 全角色仍开

- defaults 只写了 `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y`,没关其它角色。
- 生效值(`sdkconfig:578-581`):CENTRAL / BROADCASTER / OBSERVER 全是 y。设备只做 Peripheral。
- 修法:defaults 写 `CENTRAL=n` `OBSERVER=n`(广播仍可能要 BROADCASTER,需对一下 Kconfig 依赖)。

### P2-9 `LV_DRAW_LAYER_SIMPLE_BUF_SIZE=24576` 与 LVGL 池一样大

- `sdkconfig:2507`。简单层缓冲从 24KB 池里再切 24KB,半透明/变换一次就能把池抽干(断言模式直接崩)。
- 当前 UI 无半透明层,风险是以后加特效。可降到 4–8KB。

### P3-4 WiFi 静态 RX 在 BLE 模式仍占 BSS

- `ws_client.c:22` `static char s_rx[2049]`。BLE 冷启动不建 WiFi 栈,但这块 BSS 常驻。
- 可接受;要省就改成按需堆缓冲。

### P3-5 吉祥物眨眼动画息屏后仍跑

- `ui_pixel.c:113-126` `LV_ANIM_REPEAT_INFINITE`。息屏跳过 `app_ui_render`,但 LVGL 动画定时器仍在 taskLVGL 里跑。
- 微小功耗/CPU;息屏时 `lv_anim_del` 或暂停 LVGL。

### P3-6 `app_sound_play_sync` 无调用者

- `app_sound.h:23` 死代码,可删。

---

## 4. 建议修的顺序(若继续做)

| 批次 | 项 | 收益 | 真机 |
|---|---|---|---|
| A 立刻 | P2-1 有界 phase_q;P2-3 GUI 关审批演示;P2-5 gzip/WS 上限 | 防 OOM、少卡、少干扰语音 | 否 |
| A | P2-6 诊断 Text 截断;P2-2 探测互斥 | 防泄漏/端口冲突 | 否 |
| B | P2-7/P2-8 关 LVGL 示例 + NimBLE 多余角色 | Flash/RAM | 需重编固件 |
| B | P3-2 音频队列 100→20 | companion ~250KB 峰值 | 否 |
| C | P2-4 托盘菜单节流;P2-9 SIMPLE_BUF;P3-5 息屏停动画 | 手感/功耗 | 建议真机看一眼 |

不建议本轮动:BLE 环扩容(仍等真机吞吐)、I2S 超时、WDT(行为变化大)。
