// main/wifi_app.c —— Wi-Fi STA 实现(双模式:init 只建栈,start/stop 由 mode 模块驱动)。
// GOT_IP → WIFI_CONNECTED 事件 + mdns_resolver_request() + ws_client_start();
// STA_DISCONNECTED → 按 reason 去重投 WIFI_CONNECT_FAIL + ws_client_stop() + 自动重连。
#include "wifi_app.h"
#include "app_events.h"
#include "app_types.h"
#include "mdns_resolver.h"
#include "mode.h"
#include "nvs_settings.h"
#include "ws_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi_app";

static bool s_started = false;        // esp_wifi_start 是否已生效(幂等 start 用)
// 显式停止意图:stop 前置位,start 清除。esp_wifi_stop 期间/之后的残留
// STA 事件(esp_wifi_stop 会触发 STA_DISCONNECTED)不得触发自动重连
// (审查 P2-3:stop 后按 provisioned 无条件 esp_wifi_connect → 意外重连)。
static bool s_stop_requested = false;
static bool s_connected = false;
static char s_ip[16] = "";
static esp_netif_t *s_sta;
static uint16_t s_last_reason = 0;   // 已上报的失败 reason(0=无);GOT_IP/set_credentials 时清空防风暴

// 失败事件按 reason 去重:同一原因在配网会话/重连风暴中只报一次(PRD AC3 每 op 只报一次)
static void post_wifi_fail(uint16_t reason) {
    if (reason == s_last_reason) return;
    s_last_reason = reason;
    app_event_t ev = { .type = APP_EV_WIFI_CONNECT_FAIL, .u.wifi_fail = { .reason = reason } };
    app_event_post(&ev);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        // 未配置凭据时不连接(否则每次 start 报错;配网后由 wifi_app_set_credentials 触发);
        // 显式 stop 后的残留事件不重连(s_stop_requested,审查 P2-3)
        if (!s_stop_requested && wifi_app_provisioned()) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        ws_client_stop();
        wifi_event_sta_disconnected_t *d = data;
        post_wifi_fail(d ? (uint16_t)d->reason : 0);
        ESP_LOGW(TAG, "Wi-Fi 断开 (reason %u),停止 WS", s_last_reason);
        // 自动重连仅服务运行中状态:显式 stop 后不再重连(审查 P2-3)
        if (!s_stop_requested && wifi_app_provisioned()) esp_wifi_connect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_last_reason = 0;                   // 连上即清:后续新失败允许再次上报
        ESP_LOGI(TAG, "已获取 IP: %s", s_ip);
        app_event_t ev = { .type = APP_EV_WIFI_CONNECTED };
        app_event_post(&ev);
        // 仅 WiFi 模式做 WiFi 通道的事(发现 + 连 WS):USB 模式射频保持但数据走
        // USB 线,不连 WS(防链路状态误翻到 WiFi / 双通道双注入)。
        if (mode_get() == APP_MODE_WIFI) {
            mdns_resolver_request();         // 有网 → mDNS 发现 Companion(auto 模式)
            ws_client_start();               // 有网才起 WS(先按 NVS URL,发现后 retarget)
        }
    }
}

esp_err_t wifi_app_init(void) {
    // 与 main.c 的优雅降级一致:单步失败只记日志返回,不让 ESP_ERROR_CHECK 整机重启
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "netif 初始化失败: %s", esp_err_to_name(e)); return e; }
    e = esp_event_loop_create_default();
    if (e != ESP_OK) { ESP_LOGE(TAG, "事件循环失败: %s", esp_err_to_name(e)); return e; }
    s_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(e)); return e; }
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) { ESP_LOGE(TAG, "Wi-Fi 模式设置失败: %s", esp_err_to_name(e)); return e; }

    e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    if (e != ESP_OK) { ESP_LOGE(TAG, "事件注册失败: %s", esp_err_to_name(e)); return e; }
    e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);
    if (e != ESP_OK) { ESP_LOGE(TAG, "IP 事件注册失败: %s", esp_err_to_name(e)); return e; }
    return ESP_OK;
}

esp_err_t wifi_app_start(void) {
    if (s_started) return ESP_OK;   // 幂等:USB 模式已 start,切到 WiFi 模式不重复 esp_wifi_start
    s_stop_requested = false;      // 新启动恢复自动连接(审查 P2-3)
    char ssid[64] = "", pass[64] = "";
    nvs_settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));   // 失败按空串处理
    if (ssid[0]) {
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (e != ESP_OK) ESP_LOGW(TAG, "凭据写入失败: %s", esp_err_to_name(e));
        ESP_LOGI(TAG, "使用已存凭据连接 %s", ssid);
    } else {
        ESP_LOGW(TAG, "未配置 Wi-Fi —— 请用控制台命令: wifi set <ssid> <pass>");
    }
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK) { ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(e)); return e; }
    s_started = true;
    return ESP_OK;
}

esp_err_t wifi_app_stop(void) {
    s_stop_requested = true;       // 意图:本次 stop 的残留事件不得触发重连
    s_started = false;
    s_connected = false;
    s_ip[0] = '\0';
    esp_err_t e = esp_wifi_stop();
    if (e != ESP_OK) ESP_LOGW(TAG, "Wi-Fi 停止失败: %s", esp_err_to_name(e));
    return e;
}

esp_err_t wifi_app_set_credentials(const char *ssid, const char *pass) {
    s_last_reason = 0;   // 新凭据:旧失败不抑制新上报
    esp_err_t e = nvs_settings_set_wifi(ssid, pass);
    if (e != ESP_OK) return e;

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) return e;
    return esp_wifi_connect();
}

// NVS 是否已有凭据(未配网横幅依据)
bool wifi_app_provisioned(void) {
    char ssid[64] = "", pass[64] = "";
    return nvs_settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && ssid[0];
}

bool wifi_app_connected(void) { return s_connected; }

const char *wifi_app_ip(void) { return s_ip; }
