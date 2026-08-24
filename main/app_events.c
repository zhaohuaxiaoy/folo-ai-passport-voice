// main/app_events.c —— 应用事件队列实现。
#include "app_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "app_evt";

#define APP_EVENT_QUEUE_DEPTH 16

static QueueHandle_t s_queue;

esp_err_t app_events_init(void) {
    s_queue = xQueueCreate(APP_EVENT_QUEUE_DEPTH, sizeof(app_event_t));
    if (!s_queue) {
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

QueueHandle_t app_events_queue(void) { return s_queue; }
