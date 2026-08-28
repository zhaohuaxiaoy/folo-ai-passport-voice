// main/nvs_settings.c —— 设置存储实现。
#include "nvs_settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "settings";
#define APP_NS "app"

static const char *K_MODE      = "rf_mode";
static const char *K_TZ_HOUR   = "tz_hour";

esp_err_t nvs_settings_init(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) nvs_close(h);
    return e;
}

// 模式映射(存储语义 0=BLE/2=USB,见 mode.h):旧 NVS 值 1(已废弃)与
// 其他越界值一律按 BLE 兜底 —— 只删中间值不重排,存 2 的设备升级后仍为 USB。
esp_err_t nvs_settings_get_mode(uint8_t *mode) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { if (mode) *mode = 0; return ESP_OK; }  // 打不开按 BLE 兜底
    uint8_t v = 0;   // 缺省 BLE
    e = nvs_get_u8(h, K_MODE, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) v = 0;   // 首次使用:BLE
    if (mode) *mode = (v == 2) ? 2 : 0;      // 2→USB;旧值 1(已废弃)/其他 → BLE
    return ESP_OK;
}

esp_err_t nvs_settings_set_mode(uint8_t mode) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, K_MODE, mode);         // 原样存三值(旧实现"非零写 1",v3 改为原样)
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "rf_mode = %d", (int)mode);
    return e;
}

// 时区偏移小时(int8 支持负偏移,±12);缺省 8(Asia/Shanghai)
esp_err_t nvs_settings_get_tz_hour(int8_t *hour) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { if (hour) *hour = 8; return ESP_OK; }   // 打不开按默认兜底
    int8_t v = 8;
    e = nvs_get_i8(h, K_TZ_HOUR, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) v = 8;   // 首次使用:默认 +8
    if (hour) *hour = (v >= -12 && v <= 12) ? v : 8;   // 损坏值兜底
    return ESP_OK;
}

esp_err_t nvs_settings_set_tz_hour(int8_t hour) {
    if (hour < -12 || hour > 12) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_i8(h, K_TZ_HOUR, hour);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "tz_hour = %d", (int)hour);
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
