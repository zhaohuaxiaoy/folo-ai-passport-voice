// main/audio_streamer.c —— 流式音频管线实现。
#include "audio_streamer.h"
#include "app_events.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

// 100ms @ 16kHz/16bit/mono = 3200B(与 ble_audio AUDIO_FRAME_BYTES 对齐)
#define CHUNK_BYTES  3200
// 静态环恰容一块(3200)+16B 余量:ESP-IDF v5.5 BYTEBUF 零头开销(v5.4+ 重写
// 实现,xMaxItemSize=xSize、无对齐要求);余量避开"满/空歧义"路径,且兼容旧
// 实现 8B 头+8B wrap 算术 —— 3216 对两代实现都安全。放一块后 free=16 < 3200,
// 第二块 send 失败 → 丢帧,与 4096(余 896,同样 < 3200)行为逐字节一致:环
// 始终恰容一块。省 880B 静态 RAM(性能轮)。
#define RING_BYTES   3216
// 信号量等待上限(F1 事件化替代轮询):在途 notify ≤50ms,环内残留 ≤2 块——
// 200ms 余量充足。超时语义:stop 超时直接返回(提示音可能尾叠,非关键路径),
// cancel 超时保留丢帧模式,start() 会先等环空再清标志——残留绝不流入新会话。
// 等待不是单次 take:信号量只做唤醒加速,wait_worker_exit 循环复查 worker
// 状态终判(陈旧 give 只会造成一次无效唤醒)。最坏等待 ≤400ms,典型 <30ms。
#ifndef WORKER_EXIT_TIMEOUT_MS
#define WORKER_EXIT_TIMEOUT_MS 200   // 等采集 worker 退出阻塞读(原 30×5ms 轮询)
#endif
#ifndef CANCEL_DRAIN_TIMEOUT_MS
#define CANCEL_DRAIN_TIMEOUT_MS 200  // 等环空(原 400×5ms 轮询)
#endif

static StaticRingbuffer_t s_ring_struct;
static uint8_t s_ring_storage[RING_BYTES];
static RingbufHandle_t s_ring;
static uint8_t s_chunk[CHUNK_BYTES];     // 采集暂存(静态,零堆)
static SemaphoreHandle_t s_sem;          // 二值:空闲时两 worker 同等的等待信号
// F1 事件化等待(替代轮询,取消/收尾不再忙等):
// s_worker_exit_sem —— audio_worker 退出阻塞读时 give(stop/cancel 等它);
// s_ring_empty_sem  —— ble_worker 归还块后环空时 give(cancel/start/drain 等它)。
// 二值信号量记住状态:give 先于 take 则 take 立即返回;多次 give 不计数,无副作用。
// s_worker_exit_sem 的消费端是 wait_worker_exit:give 只做唤醒加速,退出与否
// 以 s_worker_busy 状态终判——stale give(上次退出未消费)不构成误判。
static SemaphoreHandle_t s_worker_exit_sem;
static SemaphoreHandle_t s_ring_empty_sem;
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
// init 全部成功才置位:任一创建失败时公开 API 空转(见各入口检查),绝不访问
// NULL ring/sem —— 主流程(模式切换等)不因音频管线初始化失败而崩溃,音频
// 优雅降级(voice.start 照常,无数据流,审查 P2)。
static volatile bool s_ready = false;

// 音频帧发送函数(通道无关,见 audio_streamer.h)。注册点:main.c boot/模式切换。
// 默认 NULL:发送一律按失败处理(丢帧计数),管线仍健康(第 6/7 轮语义保持)。
static audio_send_fn_t s_send_fn = NULL;

void audio_streamer_set_sender(audio_send_fn_t fn) { s_send_fn = fn; }

static void drop_inc(void) {
    portENTER_CRITICAL(&s_drop_mux);
    s_drop_count++;
    portEXIT_CRITICAL(&s_drop_mux);
}

// ble_worker 归还块后调用:环空(无残留且在途已归还)时 give 信号量,
// 唤醒 cancel/start/drain 的等待。归还是唯一使环空闲的操作,检测点唯一正确。
static void ring_empty_give_if(void) {
    if (xRingbufferGetCurFreeSize(s_ring) == RING_BYTES) {
        xSemaphoreGive(s_ring_empty_sem);
    }
}

// 等环空(最多 ms 毫秒)。信号量只做唤醒加速,环状态做最终判定:
// ①先查环,空 → 立即完成(无事件可等时白等超时);
// ②非空 → 等归还 give(≤50ms 典型)。stale give(上次环空遗留的置位信号量)
// 只会造成一次无效唤醒,循环再查环判定——绝不会因陈旧事件误判排空。
// 返回 true ⇔ 判定时环确实空。回绕安全:deadline 差转 int32,负即到期。
static bool wait_ring_empty(uint32_t ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    for (;;) {
        if (xRingbufferGetCurFreeSize(s_ring) == RING_BYTES) return true;
        TickType_t remain = (TickType_t)(deadline - xTaskGetTickCount());
        if ((int32_t)remain <= 0) return false;
        xSemaphoreTake(s_ring_empty_sem, remain);
    }
}

// 等采集 worker 退出阻塞读(最多 ms 毫秒)。信号量只做唤醒加速,worker 状态做
// 最终判定(对齐 wait_ring_empty 模式):循环复查 s_worker_busy,直到确实退出。
// stale give(上次 worker 退出 give 无人消费的置位信号量)只会造成一次无效
// 唤醒,循环再查状态——绝不会因陈旧 token 误判已退出(worker 仍在阻塞读时,
// stop 提前返回会导致提示音与采集尾重叠)。返回 true ⇔ 判定时 worker 确实已退出。
static bool wait_worker_exit(uint32_t ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (s_worker_busy) {
        TickType_t remain = (TickType_t)(deadline - xTaskGetTickCount());
        if ((int32_t)remain <= 0) return false;
        xSemaphoreTake(s_worker_exit_sem, remain);
    }
    return true;
}

static void audio_worker(void *arg);
static void ble_worker(void *arg);

esp_err_t audio_streamer_init(void) {
    // 幂等:成功态重入直接返回(复核 R1)——重建会覆盖全局句柄,旧 worker
    // (读全局 s_ring/s_sem 指针)转读新资源 → 双 worker 并发/资源错乱。
    // 失败态重试:全或无保证失败时无存活 worker、全局句柄未被覆盖(初始
    // NULL 或上次有效残留),重试从干净状态重建。
    if (s_ready) return ESP_OK;
    s_ready = false;   // 先重置:失败路径保持 false,公开 API 空转(审查 P2)
    // 全或无:资源全部创建成功才落全局句柄 —— 失败路径绝不覆盖旧句柄
    // (重复 init 时并发读者/遗留线程仍见有效句柄,不因部分失败暴露 NULL;
    // 真实固件首次 init 失败时全局句柄保持初始 NULL,由 s_ready 空转保护)。
    // 失败路径同时释放已创建的堆信号量(复核 R1:任一失败点不得泄漏;
    // ring 为静态存储,无需释放)。
    RingbufHandle_t ring = xRingbufferCreateStatic(RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                                   s_ring_storage, &s_ring_struct);
    if (!ring) return ESP_FAIL;
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) return ESP_FAIL;
    SemaphoreHandle_t exit_sem = xSemaphoreCreateBinary();
    if (!exit_sem) {
        vSemaphoreDelete(sem);
        return ESP_FAIL;
    }
    SemaphoreHandle_t empty_sem = xSemaphoreCreateBinary();
    if (!empty_sem) {
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        return ESP_FAIL;
    }
    s_ring = ring;
    s_sem = sem;
    s_worker_exit_sem = exit_sem;
    s_ring_empty_sem = empty_sem;

    // 音频优先:采集(6) > 发送(5),麦克风数据永不因发送慢而丢失采集节奏。
    // 栈:audio 3072 单元(≈12KB;深路径为 bsp_audio_read→esp_codec_dev_read,
    // 驱动调用栈约 1-1.5KB,余量充足);ble 保留 4096(NimBLE notify 调用链最深,
    // 不盲砍——stop() 的 uxTaskGetStackHighWaterMark 日志实测后再缩)。
    // 创建失败检查:内存不足时静默失败会让 start 后无人消费(信号量空给、管线假活)。
    // 全或无语义:任一失败即回滚已建任务 —— 绝不允许"采集任务在跑、发送任务缺失"
    // 的偏置状态(环缓冲持续堆积,审查 P2-6)。失败时指针保持 NULL/已删,stop 空
    // 指针保护已有;显式报错由调用方优雅降级处理。
    // 任务创建失败同样释放已建信号量(复核 R1):worker 未建/已删,无任务引用它们。
    if (xTaskCreate(audio_worker, "audio_worker", 3072, NULL, 6, &s_audio_task) != pdPASS) {
        ESP_LOGE(TAG, "audio worker 创建失败(内存不足?)");
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        return ESP_FAIL;
    }
    if (xTaskCreate(ble_worker, "ble_worker", 4096, NULL, 5, &s_ble_task) != pdPASS) {
        ESP_LOGE(TAG, "ble worker 创建失败,回滚 audio worker");
        vTaskDelete(s_audio_task);     // audio worker 阻塞等待中,删除安全(无持有资源)
        s_audio_task = NULL;
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        return ESP_FAIL;
    }
    s_ready = true;   // 全部资源就绪:此后公开 API 才可访问 ring/sem/worker
    ESP_LOGI(TAG, "流式管线就绪(静态环 %d B,块 %d B)", RING_BYTES, CHUNK_BYTES);
    return ESP_OK;
}

void audio_streamer_start(void) {
    if (!s_ready) return;   // init 失败:启动被拒(空转,不访问 NULL ring/sem)
    // 残留门禁(审查 P1):任何残留帧绝不流入新会话。
    // 残留来源:①cancel 排空超时的丢帧模式残留(s_cancel 保持)②正常 stop 后环内
    // 未消费残留(stop 只停采集不排空,环非空但 s_cancel 已复位)。两种都先置
    // 丢帧模式,等 worker 丢完环内残留(环空)再恢复。
    if (s_cancel || xRingbufferGetCurFreeSize(s_ring) != RING_BYTES) {
        s_cancel = true;   // 统一走丢帧模式:worker 取块只归还不发送,直至环空
        // 等环空(残留/在途被 worker 归还时 give 唤醒;环已空则直过)。
        // 超时 → 拒绝启动,丢帧模式保持——旧帧绝不流入新会话。
        bool drained = wait_ring_empty(CANCEL_DRAIN_TIMEOUT_MS);
        if (!drained) {
            // 残留未排空(notify 在途 >200ms / worker 卡死等系统级异常):拒绝启动,
            // 不置 s_active,丢帧模式保持——旧帧绝不流入新会话;下次 start 重试。
            // 若 notify 卡死本也无音频可发,拒绝比泄漏正确。
            ESP_LOGE(TAG, "启动被拒:上一次会话残留未排空(异常)");
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
    // 等 worker 退出阻塞读(信号量唤醒 + 状态终判,见 wait_worker_exit),
    // 避免 SEND 提示音与采集尾部重叠写码片。
    wait_worker_exit(WORKER_EXIT_TIMEOUT_MS);
    if (s_audio_task) {   // 首会话后即达深路径;真机据 HWM 再缩栈(见 init 注释)
        ESP_LOGI(TAG, "栈余量: audio %u/3072, ble %u/4096",
                 (unsigned)uxTaskGetStackHighWaterMark(s_audio_task),
                 (unsigned)uxTaskGetStackHighWaterMark(s_ble_task));
    }
    ESP_LOGI(TAG, "采集停止");
}

void audio_streamer_cancel(void) {
    if (!s_ready) return;   // init 失败:幂等空转(不访问 NULL ring,审查 P2)
    // 幂等:不检查 s_active——采集可能已停(STOP 后断链),环里残留同样要清。
    // 语义:①停采集(环不再有新写入;audio_worker 退出前可能已送出最后一块)
    // ②进丢帧模式(ble_worker 取到块只归还不发送)
    // ③等 audio_worker 退出阻塞读(此后环稳定,不再有新写入)
    // ④等环空(free==RING_BYTES ⇔ 无数据且无在途):残留与在途块已被 worker 归还丢弃。
    // 不用 ring reset API(ESP-IDF 无 xRingbufferReset;且任何"清空"实现都会与
    // ble_worker 已取未还的在途块冲突)。
    // 等待为事件驱动(worker 退出信号 + 归还时环空信号),典型 <30ms 返回。
    // 环可能本就为空(无残留无在途):没有归还 give 事件可等,先查实际状态直过,
    // 否则会白等超时(信号量是事件不是状态)。排空成功 → 清 s_cancel:残留已
    // 物理清除,丢弃模式使命完成;仅超时残留时保留,由 start() 兜底收尾——
    // 残留绝不会流入下一次会话(正确性优先于及时返回)。
    s_active = false;
    s_cancel = true;
    wait_worker_exit(WORKER_EXIT_TIMEOUT_MS);
    bool drained = wait_ring_empty(CANCEL_DRAIN_TIMEOUT_MS);
    if (drained) {
        s_cancel = false;
    } else {
        ESP_LOGW(TAG, "取消排空超时,start 前继续丢弃残留");
    }
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
    if (!s_ready) return;   // init 失败:空转(不访问 NULL ring)
    // 等环空(信号量事件驱动 + 环状态判定,见 wait_ring_empty)。超时仅记日志
    // ——voice.end 帧序由调用方保证(drain 后发 end),残帧在飞时 end 仍会发,
    // status 对账帧兜底。
    if (!wait_ring_empty(ms)) {
        ESP_LOGW(TAG, "drain 超时,可能仍有残帧在飞");
    }
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
        xSemaphoreGive(s_worker_exit_sem);   // F1: 通知 stop/cancel 等此处的等待者
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
            ring_empty_give_if();
            continue;
        }
        int rc = s_send_fn ? s_send_fn(item, len) : -1;   // 未注册通道视为发送失败
        vRingbufferReturnItem(s_ring, item);
        ring_empty_give_if();
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
                ESP_LOGW(TAG, "音频帧发送失败,丢弃(%u 帧/窗口)", s_fail_log_n);
                s_fail_log_n = 0;
                s_fail_log_at = xTaskGetTickCount();
            }
        }
    }
}
