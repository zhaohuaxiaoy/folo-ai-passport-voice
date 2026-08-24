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
// 取消排空轮询上限:在途 notify ≤50ms,环内残留 ≤2 块——2s 余量充足。
// 超时不破坏语义:丢帧模式保留,start() 会先等环空再清标志(见两处)。
#ifndef CANCEL_DRAIN_POLLS
#define CANCEL_DRAIN_POLLS 400   // 400 × 5ms = 2s
#endif

static StaticRingbuffer_t s_ring_struct;
static uint8_t s_ring_storage[RING_BYTES];
static RingbufHandle_t s_ring;
static uint8_t s_chunk[CHUNK_BYTES];     // 采集暂存(静态,零堆)
static SemaphoreHandle_t s_sem;          // 二值:空闲时两 worker 同等的等待信号
static volatile bool s_active = false;
static volatile bool s_worker_busy = false;  // audio_worker 是否仍在阻塞读中(供 stop/cancel 等待)
// 丢帧模式:ble_worker 取到块只归还不发送。清除点唯一——start() 在确认环空后清
// (cancel 超时残留时,start 先等 worker 丢完残留再恢复,残留绝不流入下一次会话)。
static volatile bool s_cancel = false;
static volatile uint16_t s_peak = 0;
static bool s_drop_active = false;           // 共享丢帧标志:采集/发送两侧共用一个边沿
// 丢帧计数由两个 worker(采集环满/发送失败)++、app task 取走——单核下 ++ 非原子,
// 用临界区保护(微秒级,不阻塞实时性)。
static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_drop_count = 0;            // 本会话丢帧计数(start 清零;voice.end 后取走上报)

// 发送失败日志限频:拥塞时(无订阅/流控超时)每帧刷 Warning 会反向加重负载。
// 首帧立即打,之后每秒聚合一条(帧数 = 该 1s 窗口内丢弃数,累计统计走 s_drop_count)。
static uint32_t s_fail_log_n = 0;
static TickType_t s_fail_log_at = 0;

static TaskHandle_t s_audio_task = NULL;
static TaskHandle_t s_ble_task = NULL;

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

    // 音频优先:采集(6) > 发送(5),麦克风数据永不因发送慢而丢失采集节奏。
    // 栈:audio 3072 单元(≈12KB;深路径为 bsp_audio_read→esp_codec_dev_read,
    // 驱动调用栈约 1-1.5KB,余量充足);ble 保留 4096(NimBLE notify 调用链最深,
    // 不盲砍——stop() 的 uxTaskGetStackHighWaterMark 日志实测后再缩)。
    xTaskCreate(audio_worker, "audio_worker", 3072, NULL, 6, &s_audio_task);
    xTaskCreate(ble_worker, "ble_worker", 4096, NULL, 5, &s_ble_task);
    ESP_LOGI(TAG, "流式管线就绪(静态环 %d B,块 %d B)", RING_BYTES, CHUNK_BYTES);
    return ESP_OK;
}

void audio_streamer_start(void) {
    // 上一个取消若排空超时,丢帧模式残留:先等 worker 丢完环内残留(环空)再恢复。
    // 这是 s_cancel 的唯一清除点——残留不排空绝不发送(防流入下一次会话)。
    if (s_cancel) {
        bool drained = false;
        for (int i = 0; i < CANCEL_DRAIN_POLLS; i++) {
            if (xRingbufferGetCurFreeSize(s_ring) == RING_BYTES) { drained = true; break; }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!drained) {
            // 残留未排空(notify 在途 >2s / worker 卡死等系统级异常):拒绝启动,
            // 不置 s_active,丢帧模式保持——旧帧绝不流入新会话;下次 start 重试。
            // 若 notify 卡死本也无音频可发,拒绝比泄漏正确。
            ESP_LOGE(TAG, "启动被拒:上一次取消残留未排空(异常)");
            return;
        }
        s_cancel = false;
    }
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
    if (s_audio_task) {   // 首会话后即达深路径;真机据 HWM 再缩栈(见 init 注释)
        ESP_LOGI(TAG, "栈余量: audio %u/3072, ble %u/4096",
                 (unsigned)uxTaskGetStackHighWaterMark(s_audio_task),
                 (unsigned)uxTaskGetStackHighWaterMark(s_ble_task));
    }
    ESP_LOGI(TAG, "采集停止");
}

void audio_streamer_cancel(void) {
    // 幂等:不检查 s_active——采集可能已停(STOP 后断链),环里残留同样要清。
    // 语义:①停采集(环不再有新写入;audio_worker 退出前可能已送出最后一块)
    // ②进丢帧模式(ble_worker 取到块只归还不发送)
    // ③等 audio_worker 退出阻塞读(此后环稳定,不再有新写入)
    // ④等环空(free==RING_BYTES ⇔ 无数据且无在途):残留与在途块已被 worker 归还丢弃。
    // 不用 ring reset API(ESP-IDF 无 xRingbufferReset;且任何"清空"实现都会与
    // ble_worker 已取未还的在途块冲突)。轮询超时后丢帧模式保留,由 start() 收尾
    // (先等环空再清标志)——残留绝不会流入下一次会话(正确性优先于及时返回)。
    s_active = false;
    s_cancel = true;
    for (int i = 0; i < 30 && s_worker_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    bool drained = false;
    for (int i = 0; i < CANCEL_DRAIN_POLLS; i++) {
        if (xRingbufferGetCurFreeSize(s_ring) == RING_BYTES) { drained = true; break; }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!drained) ESP_LOGW(TAG, "取消排空超时,start 前继续丢弃残留");
    ESP_LOGI(TAG, "采集取消(残留已排空)");
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

            if (xRingbufferSend(s_ring, s_chunk, CHUNK_BYTES, 0) != pdTRUE) {
                // 环满:源端立即丢弃(非阻塞——阻塞等空间会拖延下一块采集,
                // 破坏 100ms 采集节奏;麦克风继续跑,I2S 驱动自动落样),报 BLE BUSY
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
        void *item = xRingbufferReceiveUpTo(s_ring, &len, pdMS_TO_TICKS(100),
                                            CHUNK_BYTES);
        if (!item) {
            continue;   // 环空:自然态(丢帧模式由 cancel/start 轮询环空收尾)
        }
        if (s_cancel) {
            // 取消路径:在途帧直接归还(丢弃,不发送),防残留流入下一次会话
            vRingbufferReturnItem(s_ring, item);
            continue;
        }
        int rc = ble_audio_notify_audio(item, len);
        vRingbufferReturnItem(s_ring, item);
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
            s_fail_log_n++;
            if (s_fail_log_at == 0 ||   // 首次失败立即打(之后按 1s 窗口聚合)
                xTaskGetTickCount() - s_fail_log_at >= pdMS_TO_TICKS(1000)) {
                ESP_LOGW(TAG, "音频帧 BLE 发送失败,丢弃(%u 帧/窗口)", s_fail_log_n);
                s_fail_log_n = 0;
                s_fail_log_at = xTaskGetTickCount();
            }
        }
    }
}
