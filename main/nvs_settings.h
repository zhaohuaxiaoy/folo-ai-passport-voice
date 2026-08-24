// main/nvs_settings.h —— 持久化设置(NVS 命名空间 "app")。
// WiFi/WS 键已随纯 BLE 架构退役;现仅保留命名空间初始化与出厂复位。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_settings_init(void);

void nvs_settings_factory_reset(void);   // 清 "app" 命名空间

#ifdef __cplusplus
}
#endif
