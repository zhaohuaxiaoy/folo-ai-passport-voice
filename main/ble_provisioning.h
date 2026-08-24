// main/ble_provisioning.h —— BLE 配网服务(0xA1B0)提供方。
// PROV_CMD 写回调零阻塞(校验→事件);结果经 PROV_RESULT notify,READ 可轮询最近结果。
#pragma once

#include "esp_err.h"
#include "prov_protocol.h"

struct ble_gap_event;   // 前置声明,避免本头引 NimBLE 头(host 侧转发的薄接口)

#ifdef __cplusplus
extern "C" {
#endif

// 注册配网服务(count_cfg + add_svcs);须在 ble_gatts_start() 前调用。
// 由 ble_hid_keyboard_init 统一注册后统一 gatts_start。
esp_err_t ble_provisioning_init(void);

// 转发 GAP 事件(ble_hid_keyboard 的 gap_event_handler 内调用):
// 跟踪配网连接归属与 PROV_RESULT 的 CCCD 订阅状态。
void ble_provisioning_gap_event(struct ble_gap_event *event);

// 从 app_task 上报配网结果(无配网连接/未订阅则静默丢弃)。
void ble_provisioning_notify_ok(const char *ip);
void ble_provisioning_notify_error(prov_error_t code, const char *detail);

#ifdef __cplusplus
}
#endif
