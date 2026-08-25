// main/ws_client.c —— WebSocket 客户端实现。
// 注:voice.end 的排空逻辑已统一在 main.c 执行器(APP_ACT_SEND_VOICE_END),此处不再持有。
#include "ws_client.h"
#include "app_events.h"
#include "app_protocol.h"
#include "mode.h"
#include "nvs_settings.h"
#include "wifi_app.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t s_client;
static bool s_connected = false;
static SemaphoreHandle_t s_tx_mutex;   // 保护 s_client 句柄生命周期与发送的互斥

// RX 行累积器:静态 2KB(+1 为 NUL),按 \n 切行解析;超限丢弃本行剩余部分。
static char s_rx[APP_PROTO_RX_CAP + 1];
static size_t s_rx_len = 0;
static bool s_discard_line = false;    // 超长行:丢弃到下一个 \n,不再当作新行解析

static void handle_line(char *line, size_t len) {
    app_event_t ev;
    if (app_protocol_parse(line, len, &ev)) {
        app_event_post(&ev);
    } else {
        ESP_LOGW(TAG, "协议解析失败,丢弃: %.*s", (int)(len > 80 ? 80 : len), line);
    }
}

static void feed_rx(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (!s_discard_line) {
                s_rx[s_rx_len] = '\0';
                handle_line(s_rx, s_rx_len);
            }
            s_discard_line = false;
            s_rx_len = 0;
        } else if (s_discard_line) {
            continue;                          // 超长行剩余部分:丢弃
        } else if (s_rx_len < sizeof(s_rx) - 1) {
            s_rx[s_rx_len++] = data[i];
        } else {
            // 行超长:整行作废,直到下一个换行(不能只清零——残留段会被误当作新行解析)
            ESP_LOGW(TAG, "RX 行超限,丢弃到行尾");
            s_discard_line = true;
            s_rx_len = 0;
        }
    }
}

static void on_ws_event(void *handler_arg, esp_event_base_t base, int32_t id, void *data) {
    (void)handler_arg; (void)base;
    esp_websocket_event_data_t *e = data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        // 上一会话 RX 残留(半行/超长丢弃态)作废:断连期间无 feed,静态状态
        // 原样保留,不重置则旧半行接新数据被误解析成垃圾行(审查 P2-4)。
        s_rx_len = 0;
        s_discard_line = false;
        s_connected = true;
        ESP_LOGI(TAG, "WS 已连接");
        app_event_t ev = { .type = APP_EV_WS_CONNECTED };
        app_event_post(&ev);
        char buf[APP_PROTO_TX_CAP];
        size_t n = app_protocol_device_hello(buf, sizeof(buf), 1);
        ws_client_send_text(buf, n);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        if (s_connected) {
            s_connected = false;
            ESP_LOGW(TAG, "WS 断开 (ev=%d)", (int)id);
            app_event_t ev = { .type = APP_EV_WS_DISCONNECTED };
            // 收束关键事件:important 投递,队列满时不丢(性能轮队列 16→8 后
            // 更关键;与 mode.c 断连投递语义对齐)。事件回调在 WS 客户端任务
            // 上下文,非 ISR,阻塞 ≤100ms 安全。
            app_event_post_important(&ev, 100);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x1 && e->data_len > 0) {      // 仅文本帧
            feed_rx(e->data_ptr, e->data_len);
        }
        break;
    default:
        break;
    }
}

static esp_websocket_client_config_t default_config(const char *url) {
    return (esp_websocket_client_config_t){
        .uri = url,
        .reconnect_timeout_ms = 1000,
        .network_timeout_ms = 5000,
    };
}

esp_err_t ws_client_init(void) {
    if (s_client && s_tx_mutex) return ESP_OK;   // 按需初始化 + 幂等
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_FAIL;
    char url[128];
    nvs_settings_get_ws_url(url, sizeof(url));
    ESP_LOGI(TAG, "WS 目标: %s", url);
    s_client = esp_websocket_client_init(&default_config(url));
    if (!s_client) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    return ESP_OK;
}

// 重建客户端句柄:persist=true 同时写 NVS(reinit),false 只改运行时(retarget)
static esp_err_t rebuild_client(const char *url, bool persist) {
    esp_err_t e = persist ? nvs_settings_set_ws_url(url) : ESP_OK;
    if (e != ESP_OK) return e;
    // BLE 冷启动没有建立 WS 句柄:持久化配置仍可成功,运行时 retarget 则必须
    // 拒绝,避免把未初始化的互斥量当作可用对象。
    if (!s_client || !s_tx_mutex) return persist ? ESP_OK : ESP_ERR_INVALID_STATE;
    // 重建句柄必须与 ws_worker 的在飞发送互斥,否则 send_bin 可能撞上 destroy(use-after-free)
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "等待发送互斥超时,放弃重建");
        return ESP_ERR_TIMEOUT;
    }
    ws_client_stop();
    if (s_client) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    s_rx_len = 0;
    s_discard_line = false;
    s_client = esp_websocket_client_init(&default_config(url));
    if (!s_client) {
        xSemaphoreGive(s_tx_mutex);
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    if (wifi_app_connected()) e = ws_client_start();
    xSemaphoreGive(s_tx_mutex);
    return e;
}

esp_err_t ws_client_reinit(const char *url) { return rebuild_client(url, true); }

esp_err_t ws_client_retarget(const char *url) { return rebuild_client(url, false); }

esp_err_t ws_client_start(void) {
    if (!s_client) return ESP_FAIL;
    if (mode_get() == APP_MODE_USB) {
        // USB 模式射频保持但数据走 USB 线:WS 通道不建立(防链路状态误翻到
        // WiFi / 双通道双注入;含 `ws set` 手动 reinit 路径,见 rebuild_client)。
        // 切回 WiFi 模式后由 mode_switch 补触发或下次 GOT_IP 驱动。
        return ESP_OK;
    }
    ESP_LOGI(TAG, "WS 启动");
    return esp_websocket_client_start(s_client);
}

esp_err_t ws_client_stop(void) {
    if (!s_client) return ESP_ERR_INVALID_STATE;
    // 主动停(断网/重配)也要走一次"断开"通知:
    // esp_websocket_client_stop 不保证派发 WEBSOCKET_EVENT_DISCONNECTED,
    // 否则 UI 会一直显示 ONLINE、录音中还会留下不停流改发的僵尸流。
    if (s_connected) {
        s_connected = false;
        ESP_LOGW(TAG, "WS 主动停止");
        app_event_t ev = { .type = APP_EV_WS_DISCONNECTED };
        app_event_post_important(&ev, 100);   // 收束关键事件不丢(见上)
    }
    return esp_websocket_client_stop(s_client);
}

bool ws_client_connected(void) { return s_connected; }

void ws_client_send_text(const char *s, size_t len) {
    if (!s || len == 0 || !s_tx_mutex) return;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    if (s_connected && s_client) {
        if (esp_websocket_client_send_text(s_client, s, len, 200) < 0) {
            ESP_LOGW(TAG, "文本帧发送失败");
        }
    }
    xSemaphoreGive(s_tx_mutex);
}

esp_err_t ws_client_send_bin_blocking(const uint8_t *data, size_t len) {
    if (!s_tx_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (s_connected && s_client) {
        if (esp_websocket_client_send_bin(s_client, (const char *)data, len, 200) < 0) {
            e = ESP_ERR_TIMEOUT;
        } else {
            e = ESP_OK;
        }
    }
    xSemaphoreGive(s_tx_mutex);
    return e;
}
