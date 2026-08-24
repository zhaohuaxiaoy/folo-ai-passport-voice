// main/nvs_settings.c —— 设置存储实现。
#include "nvs_settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "settings";
#define APP_NS "app"

static const char *K_WIFI_SSID = "wifi_ssid";
static const char *K_WIFI_PASS = "wifi_pass";
static const char *K_WS_URL    = "ws_url";
static const char *K_WS_MODE   = "ws_mode";

// 默认 WS 地址(通过控制台 `ws set` 修改)
static const char *DEFAULT_WS_URL = "ws://192.168.1.100:8765";

esp_err_t nvs_settings_init(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) nvs_close(h);
    return e;
}

static esp_err_t get_str(const char *key, char *buf, size_t sz) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_str(h, key, buf, &sz);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) { buf[0] = '\0'; return ESP_OK; }
    if (e != ESP_OK) { buf[0] = '\0'; }  // 超长/损坏:保证调用方拿到的字符串有 NUL 结尾
    return e;
}

static esp_err_t set_str(const char *key, const char *val) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t nvs_settings_get_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    esp_err_t e = get_str(K_WIFI_SSID, ssid, ssid_sz);
    if (e != ESP_OK) return e;
    return get_str(K_WIFI_PASS, pass, pass_sz);
}

esp_err_t nvs_settings_set_wifi(const char *ssid, const char *pass) {
    esp_err_t e = set_str(K_WIFI_SSID, ssid);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SSID 写入失败: %s", esp_err_to_name(e)); return e; }
    e = set_str(K_WIFI_PASS, pass);
    if (e != ESP_OK) { ESP_LOGE(TAG, "密码写入失败: %s", esp_err_to_name(e)); return e; }
    ESP_LOGI(TAG, "Wi-Fi 已保存: %s", ssid);
    return ESP_OK;
}

esp_err_t nvs_settings_get_ws_url(char *url, size_t sz) {
    esp_err_t e = get_str(K_WS_URL, url, sz);
    if (e != ESP_OK) return e;
    if (url[0] == '\0') {                 // 首次使用写默认值
        strlcpy(url, DEFAULT_WS_URL, sz);
        (void)set_str(K_WS_URL, DEFAULT_WS_URL);
    }
    return ESP_OK;
}

esp_err_t nvs_settings_set_ws_url(const char *url) {
    esp_err_t e = set_str(K_WS_URL, url);
    if (e != ESP_OK) { ESP_LOGE(TAG, "WS URL 写入失败: %s", esp_err_to_name(e)); return e; }
    ESP_LOGI(TAG, "WS URL 已保存: %s", url);
    return ESP_OK;
}

esp_err_t nvs_settings_get_ws_mode(bool *auto_mode) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { if (auto_mode) *auto_mode = true; return ESP_OK; }  // 打不开按 auto 兜底
    uint8_t v = 1;   // 缺省 auto
    e = nvs_get_u8(h, K_WS_MODE, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) v = 1;   // 首次使用:auto
    if (auto_mode) *auto_mode = (v != 0);
    return ESP_OK;
}

esp_err_t nvs_settings_set_ws_mode(bool auto_mode) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, K_WS_MODE, auto_mode ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "ws_mode = %s", auto_mode ? "auto" : "static");
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
