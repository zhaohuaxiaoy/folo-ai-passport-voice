// main/mode.h —— 射频模式与链路抽象(Windows 移植:BLE/WiFi 双栈常驻、单射频活)。
// 职责:
//   1. 模式状态机:boot 按 NVS 只启动当前射频;mode_switch 切换时彻底关闭另一射频
//      (WiFi 模式 esp_bt_controller_disable 省电;BLE 模式 esp_wifi_stop)。
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

typedef enum {
    APP_MODE_BLE = 0,   // 缺省:BLE 直连(macOS/Windows 蓝牙)
    APP_MODE_WIFI,      // WiFi 通道(无蓝牙 Windows 电脑)
    APP_MODE_USB,       // USB 有线通道(USB-Serial-JTAG;射频全关,省电)
    APP_MODE_COUNT,
} app_mode_t;

// boot 阶段调用(须在 ble_audio_init 之后):读 NVS 模式、初始化 WiFi/WS/mDNS 栈
// (不 start),按模式只启动当前射频,并注册对应音频发送函数。射频切换会短暂阻塞
// (controller 启停/esp_wifi 启停,≤ 数百 ms)—— boot 期无并发,安全。
esp_err_t mode_init(void);

app_mode_t mode_get(void);
const char *mode_name(app_mode_t m);   // "BLE" / "WiFi"

// 当前通道链路是否已通(BLE:EVENT 已订阅;WiFi:WS 已连接)。
bool mode_link_up(void);

// 事件行上行(BLE:notify EVENT;WiFi:WS 文本帧)。返回 0 = 已入队/已发。
int mode_send_event_line(const char *line, size_t len);

// 切换模式(仅 app_task 上下文调用):
//   1) 先投当前通道断连事件(状态机幂等收束:停流/回 READY/审批保持)
//   2) NVS 写模式(失败中止并回报,射频不动)
//   3) 射频切换:彻底关旧射频 → 启新射频 → 注册新音频发送函数
// 真机风险(NOT RUN):NimBLE controller 启停后 host re-sync 需真机验证。
esp_err_t mode_switch(app_mode_t target);

#ifdef __cplusplus
}
#endif
