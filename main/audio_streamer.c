// main/audio_streamer.c —— 流式音频管线实现。
#include "audio_streamer.h"
#include "app_events.h"
#include "ble_audio.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

// 100ms @ 16kHz/16bit/mono = 3200B(与 ble_audio AUDIO_FRAME_BYTES 对齐)
#define CHUNK_BYTES  3200
#define RING_BYTES   4096   // 静态环,约可容一块

static StaticRingbuffer_t s_ring_struct;
static uint8_t s_ring_storage[RING_BYTES];
static RingbufHandle_t s_ring;
static uint8_t s_chunk[CHUNK_BYTES];     // 采集暂存(静态,零堆)
static SemaphoreHandle_t s_sem;          // 二值:空闲时两 worker 同等的等待信号
static volatile bool s_active = false;
static volatile bool s_worker_busy = false;  // audio_worker 是否仍在阻塞读中(供 stop 等待)
static volatile bool s_ble_busy = false;     // ble_worker 是否持有在途块(供 cancel 等待)
static volatile bool s_cancel = false;       // cancel 请求:ble_worker 跳过发送直接归还
static volatile uint16_t s_peak = 0;
static bool s_drop_active = false;           // 共享丢帧标志:采集/发送两侧共用一个边沿
// 丢帧计数由两个 worker(采集环满/发送失败)++、app task 取走——单核下 ++ 非原子,
// 用临界区保护(微秒级,不阻塞实时性)。
static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_drop_count = 0;            // 本会话丢帧计数(start 清零;voice.end 后取走上报)

static void drop_inc(void) {
    portENTER_CRITICAL(&s_drop_mux);
    s_drop_count++;
    portEXIT_CRITICAL(&s_drop_mux);
}

static void audio_worker(void *arg);
static void ble_worker(void *arg);

esp_err_t audio_streamer_init(void) {
    s_ring = xRingbufferCreateStatic(RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                     s_ring_storage, &s_ring_struct);
    if (!s_ring) return ESP_FAIL;
    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) return ESP_FAIL;

    // 音频优先:采集(6) > 发送(5),麦克风数据永不因发送慢而丢失采集节奏
    xTaskCreate(audio_worker, "audio_worker", 4096, NULL, 6, NULL);
    xTaskCreate(ble_worker, "ble_worker", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "流式管线就绪(静态环 %d B,块 %d B)", RING_BYTES, CHUNK_BYTES);
    return ESP_OK;
}

void audio_streamer_start(void) {
    if (s_active) return;
    s_active = true;
    s_peak = 0;
    s_drop_active = false;
    s_drop_count = 0;
    xSemaphoreGive(s_sem);
    ESP_LOGI(TAG, "采集开始");
}

void audio_streamer_stop(void) {
    if (!s_active) return;
    s_active = false;             // audio worker 在下一次循环退出
    // 等 worker 退出阻塞读(≤100ms),避免 SEND 提示音与采集尾部重叠写码片
    for (int i = 0; i < 30 && s_worker_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "采集停止");
}

void audio_streamer_cancel(void) {
    if (!s_active) return;
    s_active = false;             // audio worker 在下一次循环退出
    s_cancel = true;              // ble_worker 跳过发送直接归还(丢弃在途帧)
    xRingbufferReset(s_ring);     // 清空未发送残留
    // 等两个 worker 退出(≤150ms):audio 退出阻塞读、ble 归还已取出的块。
    // 之后环空且在途块已丢弃,残留不会流入下一次会话。
    for (int i = 0; i < 30 && (s_worker_busy || s_ble_busy); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_cancel = false;
    ESP_LOGI(TAG, "采集取消(残留已清)");
}

bool audio_streamer_active(void) { return s_active; }

uint16_t audio_streamer_peak(void) { return s_peak; }

uint32_t audio_streamer_take_drops(void) {
    portENTER_CRITICAL(&s_drop_mux);
    uint32_t d = s_drop_count;
    s_drop_count = 0;
    portEXIT_CRITICAL(&s_drop_mux);
    return d;
}

// 等环空(最多 ms 毫秒),用于 voice.end 前的帧序保证。
// 只轮询不消费:环满即还有数据(含 ble_worker 已取走未归还的在飞块),取走不发送=丢队尾。
void audio_streamer_drain(uint32_t ms) {
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (xTaskGetTickCount() < until) {
        if (xRingbufferGetCurFreeSize(s_ring) == RING_BYTES) return;  // 环已空(含在飞块已归还)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "drain 超时,可能仍有残帧在飞");
}

static void audio_worker(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_sem, portMAX_DELAY);    // 等待 start 信号
        s_worker_busy = true;
        while (s_active) {
            esp_err_t e = bsp_audio_read(s_chunk, CHUNK_BYTES);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "bsp_audio_read 失败 (%s),停流",
                         esp_err_to_name(e));
                app_event_t ev = { .type = APP_EV_AUDIO_ERROR };  // 语义独立的硬件错误事件
                app_event_post(&ev);
                s_active = false;
                break;
            }
            // 峰值采样(UI 音量条)。先升 int32 再取负:
            // int16 的 -32768 直接取负是有符号溢出(UB)。
            const int16_t *p = (const int16_t *)s_chunk;
            uint16_t peak = 0;
            for (size_t i = 0; i < CHUNK_BYTES / 2; i++) {
                int32_t v = p[i];
                uint16_t a = (uint16_t)(v < 0 ? -v : v);
                if (a > peak) peak = a;
            }
            s_peak = peak;

            if (xRingbufferSend(s_ring, s_chunk, CHUNK_BYTES,
                                pdMS_TO_TICKS(50)) != pdTRUE) {
                // 环满:源端丢弃(麦克风继续跑,I2S 驱动自动落样),报 BLE BUSY
                drop_inc();
                if (!s_drop_active) {
                    s_drop_active = true;
                    app_event_t ev = { .type = APP_EV_AUDIO_DROP_START };
                    app_event_post(&ev);
                }
            }
        }
        s_worker_busy = false;
    }
}

static void ble_worker(void *arg) {
    (void)arg;
    for (;;) {
        size_t len = 0;
        s_ble_busy = true;                       // 在途块持有标记(cancel 等待用)
        void *item = xRingbufferReceiveUpTo(s_ring, &len, pdMS_TO_TICKS(100),
                                            CHUNK_BYTES);
        if (!item) {
            s_ble_busy = false;
            continue;
        }
        if (s_cancel) {
            // 取消路径:直接归还不发送(在途帧丢弃,防残留流入下一次会话)
            vRingbufferReturnItem(s_ring, item);
            s_ble_busy = false;
            continue;
        }
        int rc = ble_audio_notify_audio(item, len);
        vRingbufferReturnItem(s_ring, item);
        s_ble_busy = false;
        if (rc == 0) {
            // 发送恢复且环已有余量(不再积压)才解除 BLE BUSY——
            // 只靠发送成功不够:若发送慢但一直成功,环满造成的丢帧永远不会被解除
            if (s_drop_active &&
                xRingbufferGetCurFreeSize(s_ring) >= CHUNK_BYTES) {
                s_drop_active = false;
                app_event_t ev = { .type = APP_EV_AUDIO_DROP_END };
                app_event_post(&ev);
            }
        } else {
            // 无订阅/断连/流控超时:整帧丢弃并计数(voice.end 后 status 帧上报)
            drop_inc();
            if (!s_drop_active) {
                s_drop_active = true;
                app_event_t ev = { .type = APP_EV_AUDIO_DROP_START };
                app_event_post(&ev);
            }
            ESP_LOGW(TAG, "音频帧 BLE 发送失败,丢弃");
        }
    }
}
