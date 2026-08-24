// main/nvs_settings.h —— 持久化设置(NVS 命名空间 "app")。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_settings_init(void);

esp_err_t nvs_settings_get_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
esp_err_t nvs_settings_set_wifi(const char *ssid, const char *pass);

esp_err_t nvs_settings_get_ws_url(char *url, size_t sz);
esp_err_t nvs_settings_set_ws_url(const char *url);

void nvs_settings_factory_reset(void);   // 清 "app" 命名空间

#ifdef __cplusplus
}
#endif
