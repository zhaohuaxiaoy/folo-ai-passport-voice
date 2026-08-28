// main/nvs_settings.h —— 持久化设置(NVS 命名空间 "app")。
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_settings_init(void);

// 射频模式存储语义:0=BLE(缺省)/ 2=USB(值 1 已废弃,不重排;越界值按 BLE
// 兜底)。NVS 中既存的旧通道键不清理(新固件不再读取)。factory reset 回 BLE。
esp_err_t nvs_settings_get_mode(uint8_t *mode);
esp_err_t nvs_settings_set_mode(uint8_t mode);

// 时区偏移小时(int8,±12;缺省 8)
esp_err_t nvs_settings_get_tz_hour(int8_t *hour);
esp_err_t nvs_settings_set_tz_hour(int8_t hour);

void nvs_settings_factory_reset(void);   // 清 "app" 命名空间

#ifdef __cplusplus
}
#endif
