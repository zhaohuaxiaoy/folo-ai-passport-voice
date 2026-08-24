// main/nvs_settings.c —— 设置存储实现。
// WiFi(SSID/密码)/WS URL/mDNS 模式键已随纯 BLE 架构退役,不再读写。
#include "nvs_settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "settings";
#define APP_NS "app"

esp_err_t nvs_settings_init(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) nvs_close(h);
    return e;
}

void nvs_settings_factory_reset(void) {
    nvs_handle_t h;
    if (nvs_open(APP_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "app 命名空间已清空");
    }
}
