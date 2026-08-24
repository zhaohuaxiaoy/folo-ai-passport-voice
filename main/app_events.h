// main/app_events.h —— 全局应用事件队列(唯一事件通道)。
// 按键回调/WS 回调/音频 worker 都只往这里投递,app_task 唯一消费。
#pragma once

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_events_init(void);
void app_event_post(const app_event_t *ev);   // 非阻塞投递(满则丢弃+日志)
void *app_events_queue(void);                 // QueueHandle_t,供 app_task 消费

#ifdef __cplusplus
}
#endif
