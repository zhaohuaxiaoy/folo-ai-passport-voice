// main/mode.h —— 射频模式与链路抽象(按模式启停射频)。
// 职责:
//   1. 模式状态机:boot 按 NVS 启动对应射频;mode_switch 切换时按模式启停射频
//      (BLE ↔ USB 双模式;USB 模式射频保持,数据走 USB 线)。
//   2. 链路抽象:link_up / send_event_line / send_audio —— main.c 执行器、audio_streamer、
//      UI 不再感知模式(音频帧发送经 audio_streamer_set_sender 按模式注册)。
//   3. NimBLE host 常驻 + controller 启停:enable 后 on_sync 触发 → 广播自动恢复。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 存储语义(BLE=0 / USB=2,与旧 NVS 值兼容):值 1 已废弃(旧通道),解析为
// 越界 → 回退 BLE。只删中间值不重排 —— 旧 NVS 存 2(USB)的设备升级后仍为 USB。
typedef enum {
    APP_MODE_BLE = 0,   // 缺省:BLE 直连(macOS/Windows 蓝牙)
    APP_MODE_USB = 2,   // USB 有线通道(USB-Serial-JTAG;射频保持,数据走 USB)
    APP_MODE_COUNT = 3, // 保留空洞:值 1 已废弃(旧通道),不得重排枚举
} app_mode_t;

// boot 阶段调用(须在 ble_audio_init 之后):读 NVS 模式,按模式启动当前射频,
// 并注册对应音频发送函数。射频切换会短暂阻塞(controller 启停,≤ 数百 ms)
// —— boot 期无并发,安全。
esp_err_t mode_init(void);

app_mode_t mode_get(void);
const char *mode_name(app_mode_t m);   // "BLE" / "USB"
// 会话音频格式(voice.start 上报,App 据此分发解析):BLE → "ima_adpcm"
// (每帧一个独立 IMA ADPCM block,4B 首部 + 800B 数据 = 804B);
// USB → "pcm"(3200B PCM 块直传)。
const char *mode_audio_format(void);

// 是否处于 mode_switch 切换窗口(仅 app_task 上下文,切换期间置位)。
// 供链路事件门禁使用:mode_switch 在切换前投递"旧通道断连事件"(app_task
// 消费时 mode_get() 已是新模式),切换窗口内放行该事件,状态机才能收束。
bool mode_switching(void);

// 当前通道链路是否已通(BLE:EVENT 已订阅;USB:USB 会话已 up)。
bool mode_link_up(void);

// 事件行上行(BLE:notify EVENT;USB:SYS/EVENT 帧)。返回 0 = 已入队/已发。
int mode_send_event_line(const char *line, size_t len);

// 会话边界帧的可靠上行(voice.start/end):BLE 通道走阻塞入队(满等 ≤timeout_ms,
// 审查 P2:边界帧丢失 → Mac 端会话状态悬挂);USB 发送本就同步阻塞,无差别。
int mode_send_event_line_important(const char *line, size_t len, uint32_t timeout_ms);

// 切换模式(仅 app_task 上下文调用):
//   1) 先投当前通道断连事件(状态机幂等收束:停流/回 READY/审批保持)
//   2) NVS 写模式(失败中止并回报,射频不动)
//   3) 射频切换:关旧射频 → 启新射频 → 注册新音频发送函数
// 真机风险(NOT RUN):NimBLE controller 启停后 host re-sync 需真机验证。
esp_err_t mode_switch(app_mode_t target);

#ifdef __cplusplus
}
#endif
