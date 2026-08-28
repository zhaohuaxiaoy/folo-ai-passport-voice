// main/nvs_settings.c —— 设置存储实现。
#include "nvs_settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "settings";
#define APP_NS "app"

// 注:rf_mode 键已随双通道常开架构退役(2026-08-28,不再有互斥模式);旧键
// 残留于 NVS 无读取方,无害(factory 清空可除)。
static const char *K_TZ_HOUR   = "tz_hour";

esp_err_t nvs_settings_init(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) nvs_close(h);
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
