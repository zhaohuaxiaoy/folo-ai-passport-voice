// tests/fake/fake_events.c —— 轻量事件投递桩(仅 app_protocol 链的宿主测试用:
// test_app_protocol / test_ble_audio 链接 app_protocol.c,其中
// app_protocol_dispatch_event 引用这两个符号;test_audio_streamer 用
// fake_rtos.c 内同语义实现,不与此文件同链)。宿主无队列满语义:
// important 与普通投递等价,只记录到 g_events 供断言。
#include "app_events.h"
#include "esp_err.h"

#define FAKE_EVENT_CAP 64
app_event_t g_events[FAKE_EVENT_CAP];
int g_event_count = 0;

void app_event_post(const app_event_t *ev) {
    if (g_event_count < FAKE_EVENT_CAP) g_events[g_event_count++] = *ev;
}

esp_err_t app_event_post_important(const app_event_t *ev, uint32_t timeout_ms) {
    (void)timeout_ms;
    app_event_post(ev);
    return ESP_OK;
}
