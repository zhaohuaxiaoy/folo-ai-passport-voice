// main/app_events.c —— 应用事件队列实现。
#include "app_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "app_evt";

#define APP_EVENT_QUEUE_DEPTH 16

// 静态队列:16×~232B ≈ 3.7KB 静态分配,消除启动堆峰值与创建失败分支(M2)
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[APP_EVENT_QUEUE_DEPTH * sizeof(app_event_t)];
static QueueHandle_t s_queue;

esp_err_t app_events_init(void) {
    s_queue = xQueueCreateStatic(APP_EVENT_QUEUE_DEPTH, sizeof(app_event_t),
                                 s_queue_storage, &s_queue_struct);
    if (!s_queue) {   // 静态创建仅在存储对齐异常时失败,防御保留
        ESP_LOGE(TAG, "事件队列创建失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_event_post(const app_event_t *ev) {
    if (!s_queue || !ev) return;
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "事件队列满,丢弃 type=%d", (int)ev->type);
    }
}

esp_err_t app_event_post_important(const app_event_t *ev, uint32_t timeout_ms) {
    if (!s_queue || !ev) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, ev, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "重要事件投递超时(%u ms),type=%d",
                 (unsigned)timeout_ms, (int)ev->type);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

QueueHandle_t app_events_queue(void) { return s_queue; }
