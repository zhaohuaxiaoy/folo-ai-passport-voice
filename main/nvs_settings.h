// main/nvs_settings.h —— 持久化设置(NVS 命名空间 "app")。
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_settings_init(void);

esp_err_t nvs_settings_get_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
esp_err_t nvs_settings_set_wifi(const char *ssid, const char *pass);

esp_err_t nvs_settings_get_ws_url(char *url, size_t sz);
esp_err_t nvs_settings_set_ws_url(const char *url);

// WS 目标策略:true=auto(mDNS 自动发现,缺省)/ false=static(显式 URL 优先)
esp_err_t nvs_settings_get_ws_mode(bool *auto_mode);
esp_err_t nvs_settings_set_ws_mode(bool auto_mode);

// 射频模式:0=BLE(缺省)/ 1=WiFi。factory reset 回 BLE。
esp_err_t nvs_settings_get_mode(uint8_t *mode);
esp_err_t nvs_settings_set_mode(uint8_t mode);

// 时区偏移小时(int8,±12;缺省 8)
esp_err_t nvs_settings_get_tz_hour(int8_t *hour);
esp_err_t nvs_settings_set_tz_hour(int8_t hour);

void nvs_settings_factory_reset(void);   // 清 "app" 命名空间

#ifdef __cplusplus
}
#endif
