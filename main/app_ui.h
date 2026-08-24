// main/app_ui.h —— 产品 UI:7 个状态页 + 常驻顶栏/横幅/Toast。
// 布局:单个基底屏(天空/草地/云) + 每状态一个内容组(切换时显隐) +
// chrome 放 LVGL 顶层(lv_layer_top),避免 7 页重复建状态栏。
// 全部对象 init 时一次建完,render 只改文本/显隐 —— 无运行时堆分配。
#pragma once

#include "app_types.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 建基底屏、7 个状态组与 chrome。须在 bsp_display_init + bsp_lvgl_init 成功后调用,
// 且调用方须持 LVGL 锁(bsp_lvgl_lock)。
esp_err_t app_ui_init(void);

// 按快照刷新 UI。同样须持锁调用(唯一写者:app_task)。
// mic_peak 为麦克风峰值(0..32768),LISTENING 页音量条用。
// 息屏切换由 snapshot.screen_on 驱动(背光 0/100)。
void app_ui_render(const app_ui_snapshot_t *snap, uint16_t mic_peak);

#ifdef __cplusplus
}
#endif
