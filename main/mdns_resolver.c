// main/mdns_resolver.c —— mDNS 解析器实现。
// 查询 _ai-passport._tcp 取首个 IPv4 → ws://<ip>:<port> → 与缓存不同才投 WS_TARGET_FOUND。
// 节流/退避:最小间隔 15s,失败退避 ×2 封顶 5min,成功重置;查询在信号量驱动的 worker 内,
// 绝不在调用方(事件回调/任务)上下文阻塞。
#include "mdns_resolver.h"
#include "app_events.h"
#include "app_types.h"
#include "nvs_settings.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include <string.h>

static const char *TAG = "mdns";

#define MDNS_SERVICE      "_ai-passport"
#define MDNS_PROTO        "_tcp"
#define MDNS_QUERY_TIMEOUT_MS 3000
#define MDNS_MIN_INTERVAL_MS  15000u    // 最小查询间隔
#define MDNS_MAX_BACKOFF_MS   300000u   // 退避封顶 5min
#define MDNS_WORKER_STACK     4096

static SemaphoreHandle_t s_request;              // 二进制信号量:触发一次查询(满则合并)
static char s_last_url[APP_WS_URL_MAX];          // 去重缓存(相同目标不重投)

// 查询并上报。返回是否成功(成功=找到目标或目标未变,均视为"可接受"以重置退避)。
static bool query_and_report(void)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(MDNS_SERVICE, MDNS_PROTO,
                                   MDNS_QUERY_TIMEOUT_MS, 1, &results);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "查询失败: %s", esp_err_to_name(err));
        return false;
    }
    if (!results) return false;   // 超时无结果

    char url[APP_WS_URL_MAX];
    bool found = false;
    for (mdns_ip_addr_t *a = results->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {   // espressif/mdns 2.x: 字段 ip → addr
            snprintf(url, sizeof(url), "ws://" IPSTR ":%u",
                     IP2STR(&a->addr.u_addr.ip4), results->port);
            found = true;
            break;
        }
    }
    mdns_query_results_free(results);
    if (!found) return false;

    if (strcmp(url, s_last_url) == 0) return true;   // 目标未变:视为成功,不重复投递
    strlcpy(s_last_url, url, sizeof(s_last_url));
    app_event_t ev = { .type = APP_EV_WS_TARGET_FOUND };
    strlcpy(ev.u.ws_target.url, url, sizeof(ev.u.ws_target.url));
    app_event_post(&ev);
    ESP_LOGI(TAG, "发现目标 %s", url);
    return true;
}

static void mdns_worker(void *arg)
{
    (void)arg;
    int64_t next_allowed_us = 0;
    uint32_t backoff_ms = MDNS_MIN_INTERVAL_MS;
    for (;;) {
        xSemaphoreTake(s_request, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        if (now < next_allowed_us) continue;    // 节流:未到最早查询时刻,忽略本次请求
        if (query_and_report()) {
            backoff_ms = MDNS_MIN_INTERVAL_MS;  // 成功重置
            next_allowed_us = now + (int64_t)MDNS_MIN_INTERVAL_MS * 1000;
        } else {
            next_allowed_us = now + (int64_t)backoff_ms * 1000;
            if (backoff_ms * 2 < MDNS_MAX_BACKOFF_MS) backoff_ms *= 2;
            else backoff_ms = MDNS_MAX_BACKOFF_MS;   // 封顶 5min
            ESP_LOGW(TAG, "未找到目标,退避 %ums", backoff_ms);
        }
    }
}

void mdns_resolver_request(void)
{
    if (!s_request) return;
    bool auto_mode;
    nvs_settings_get_ws_mode(&auto_mode);
    if (auto_mode) xSemaphoreGive(s_request);   // static 模式:用户显式 URL,不查询
}

esp_err_t mdns_resolver_init(void)
{
    if (s_request) return ESP_OK;   // 按需初始化 + 幂等(模式切换重试安全)
    esp_err_t e = mdns_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "mdns_init 失败: %s", esp_err_to_name(e)); return e; }
    e = mdns_hostname_set("ai-passport");
    if (e != ESP_OK) { ESP_LOGE(TAG, "hostname 设置失败: %s", esp_err_to_name(e)); return e; }

    s_request = xSemaphoreCreateBinary();
    if (!s_request) return ESP_FAIL;
    xSemaphoreGive(s_request);   // 冷启动即查一次(配网后重启 → 自动发现,AC8)

    xTaskCreate(mdns_worker, "mdns_worker", MDNS_WORKER_STACK, NULL, 3, NULL);
    ESP_LOGI(TAG, "mDNS 解析器就绪(查询 %s.%s)", MDNS_SERVICE, MDNS_PROTO);
    return ESP_OK;
}
