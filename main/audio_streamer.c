// main/audio_streamer.c —— 流式音频管线实现。
#include "audio_streamer.h"
#include "app_events.h"
#include "bsp_audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "adpcm.h"

static const char *TAG = "audio";

// 100ms @ 16kHz/16bit/mono = 3200B(与 ble_audio.h AUDIO_FRAME_BYTES、
// companion/relay.py 的 AUDIO_FRAME_BYTES 三处必须一致)
#define CHUNK_BYTES  3200
// 环内 item = [uint32 会话 token][3200B PCM](3204B,4B 对齐 4×801)。
// token 由 start()/cancel() 递增并写入 item 头,ble_worker 只处理
// token == 当前会话的 item;旧会话残留(环内未消费/在途/迟到块)靠 token
// 失效静默丢弃——取消/断链后无需"排空"门禁,start 永不因残留拒绝
// (任务 08-27-ring-desync-diagnose A1 结论:残留门禁结构性脆弱)。
#define CHUNK_ITEM_BYTES  (4 + CHUNK_BYTES)
// BLE 通道压缩:IMA ADPCM 4:1(3200B PCM → 804B block,8KB/s)。
// 为什么不是 Opus:BLE 上不去的真瓶颈是 mbuf 池(4×256B)+ 首片失败即整帧作废,
// 不是空口带宽;修掉瓶颈后 8KB/s 只需约 0.7 包/连接事件,Opus 多出的压缩率
// 无处可花,却要 38.5KB 静态状态 + 16KB 栈,恰好挤掉修瓶颈所需的堆(见任务
// design.md §3)。ADPCM 状态 4 字节在栈上,无堆、无 alloca、无新组件。
// USB 带宽充裕,不压缩(有损无必要),由 s_compressed 开关区分。
static volatile bool s_compressed = false;    // 压缩开关(mode.c 按通道设置)
// 会话 token:start() 递增开启新会话,cancel() 递增作废迟到块(见 audio_worker
// 快照语义)。worker 把快照写进每个 item 头,ble_worker 只处理匹配当前值的 item。
static volatile uint32_t s_session_token = 0;
static volatile uint32_t s_last_encoded_token = 0;  // 已编码 ADPCM 的会话(token 变=新会话首块)
static adpcm_state_t s_adpcm;                 // 仅 ble_worker 上下文访问(4 字节)
// 环形缓冲:NOSPLIT 两槽(200ms)。
// ⚠ 不能用 BYTEBUF:ESP-IDF v5.5 的 prvGetItemByteBuf 在数据跨尾回绕时只返回
// 到缓冲尾部的那一段,xRingbufferReceiveUpTo 会返回远小于请求的长度(真机实测
// 每轮偏移 16B → 16+2544 两次返回)。压缩路径需要"整块 3200B"语义,短读会
// 直接编出错位音频;NOSPLIT + xRingbufferReceive 由实现保证 item 边界完整。
// 两槽给 BLE 抖动留 100ms 缓冲(旧实现只容一块,一次 notify 卡顿就丢帧)。
#define RING_SLOTS   2
// NOSPLIT 每 item 有 8B 头且按 4B 对齐;xMaxItemSize = ALIGN(xSize/2)-8,
// 要放下 3204 需 xSize/2 ≥ 3212 → 6424,取 6432 留 8B 回绕余量。
#define RING_BYTES   6432
// 信号量等待上限(F1 事件化替代轮询):在途 notify ≤50ms。超时语义:stop/cancel
// 超时直接返回(提示音可能尾叠,非关键路径;cancel 的残留由 token 失效兜底)。
// 等待不是单次 take:信号量只做唤醒加速,wait_worker_exit 循环复查 worker
// 状态终判(陈旧 give 只会造成一次无效唤醒)。最坏等待 ≤200ms,典型 <30ms。
#ifndef WORKER_EXIT_TIMEOUT_MS
#define WORKER_EXIT_TIMEOUT_MS 200   // 等采集 worker 退出阻塞读(原 30×5ms 轮询)
#endif

static StaticRingbuffer_t s_ring_struct;
static uint8_t s_ring_storage[RING_BYTES] __attribute__((aligned(4)));   // NOSPLIT 要求 4B 对齐
static RingbufHandle_t s_ring;
static uint8_t s_chunk[CHUNK_ITEM_BYTES] __attribute__((aligned(4)));  // [token][PCM];+4 处是 bsp_audio_read 的 DMA 目标
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
// worker 栈静态化(.bss):编码器(38.5KB)+ NimBLE controller(~44.8KB)后 heap
// 只剩 ~3KB,动态任务栈分配失败会让 host 任务(4096B)起不来 → 不广播连不上
// (真机侦查定位)。worker 生命周期与 init 同存活(不随 start/stop 删除),静态
// 化安全,唯一例外是 init 失败回滚的 vTaskDelete —— 静态任务可删除。
static StackType_t s_audio_stack[3072 / sizeof(StackType_t)];
static StaticTask_t s_audio_tcb;
// ble_worker 4096:ADPCM 编码是定长表查 + 移位,无 alloca、无大局部变量
// (Opus 时代一路加到 16384 仍崩,根因是堆耗尽而非栈,见任务 design.md §1)。
static StackType_t s_ble_stack[4096 / sizeof(StackType_t)];
static StaticTask_t s_ble_tcb;
// init 全部成功才置位:任一创建失败时公开 API 空转(见各入口检查),绝不访问
// NULL ring/sem —— 主流程(模式切换等)不因音频管线初始化失败而崩溃,音频
// 优雅降级(voice.start 照常,无数据流,审查 P2)。
static volatile bool s_ready = false;

// 音频帧发送函数(通道无关,见 audio_streamer.h)。注册点:main.c boot/模式切换。
// 默认 NULL:发送一律按失败处理(丢帧计数),管线仍健康(第 6/7 轮语义保持)。
static audio_send_fn_t s_send_fn = NULL;

void audio_streamer_set_sender(audio_send_fn_t fn) { s_send_fn = fn; }

void audio_streamer_set_compressed(bool on) { s_compressed = on; }

// 环内块数(待发 + ble_worker 已取未归还的在途块)。send 后 ++、归还后 --,
// 归零 ⇔ 环内无数据且无在途 —— 这正是 cancel/start/drain 需要的"排空"语义。
// 不用 xRingbufferGetCurFreeSize 判空:NOSPLIT 下它返回"当前能放下的最大 item"
// (随读写指针位置变化),空环也不会回到固定值。
static volatile uint32_t s_ring_items = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ring_items(void) {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t n = s_ring_items;
    portEXIT_CRITICAL(&s_ring_mux);
    return n;
}

static void ring_items_inc(void) {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_items++;
    portEXIT_CRITICAL(&s_ring_mux);
}

static void drop_inc(void) {
    portENTER_CRITICAL(&s_drop_mux);
    s_drop_count++;
    portEXIT_CRITICAL(&s_drop_mux);
}

// ble_worker 归还块后调用:环空(无残留且在途已归还)时 give 信号量,
// 唤醒 drain 的等待(voice.end 帧序)。归还是唯一使环空闲的操作,检测点唯一正确。
// 归还块:计数 --,归零则 give(唤醒 drain 的等待)。
static void ring_item_returned(void) {
    bool empty;
    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_items > 0) s_ring_items--;
    empty = (s_ring_items == 0);
    portEXIT_CRITICAL(&s_ring_mux);
    if (empty) xSemaphoreGive(s_ring_empty_sem);
}

// 等环空(最多 ms 毫秒)。信号量只做唤醒加速,环状态做最终判定:
// ①先查环,空 → 立即完成(无事件可等时白等超时);
// ②非空 → 等归还 give(≤50ms 典型)。stale give(上次环空遗留的置位信号量)
// 只会造成一次无效唤醒,循环再查环判定——绝不会因陈旧事件误判排空。
// 返回 true ⇔ 判定时环确实空。回绕安全:deadline 差转 int32,负即到期。
static bool wait_ring_empty(uint32_t ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    for (;;) {
        if (ring_items() == 0) return true;
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
    RingbufHandle_t ring = xRingbufferCreateStatic(RING_BYTES, RINGBUF_TYPE_NOSPLIT,
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
    s_ring_items = 0;
    s_sem = sem;
    s_worker_exit_sem = exit_sem;
    s_ring_empty_sem = empty_sem;

    // 音频优先:采集(6) > 发送(5),麦克风数据永不因发送慢而丢失采集节奏。
    // 栈:audio 3072 字节(深路径 bsp_audio_read→esp_codec_dev_read,驱动 ~1-1.5KB);
    // ble 4096 —— ADPCM 编码只有表查+移位,局部最大是 804B 输出缓冲(见 ble_worker),
    // 加 notify 链路余量充足;真机 HWM 会在 stop 日志里打印,据此再收紧。
    // 创建失败检查:内存不足时静默失败会让 start 后无人消费(信号量空给、管线假活)。
    // 全或无语义:任一失败即回滚已建任务 —— 绝不允许"采集任务在跑、发送任务缺失"
    // 的偏置状态(环缓冲持续堆积,审查 P2-6)。失败时指针保持 NULL/已删,stop 空
    // 指针保护已有;显式报错由调用方优雅降级处理。
    // 任务创建失败同样释放已建信号量(复核 R1):worker 未建/已删,无任务引用它们。
    TaskHandle_t audio_h = xTaskCreateStatic(audio_worker, "audio_worker", 3072, NULL, 6,
                                             s_audio_stack, &s_audio_tcb);
    if (!audio_h) {
        ESP_LOGE(TAG, "audio worker 创建失败");
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        return ESP_FAIL;
    }
    s_audio_task = audio_h;   // 静态任务句柄:用于 HWM 日志与回滚删除
    // 句柄必须存下:uxTaskGetStackHighWaterMark(NULL) 报的是调用者(app_task)的
    // 余量,不是 ble_worker 的 —— 旧代码丢弃返回值导致 stop 日志里 ble 余量是假的。
    TaskHandle_t ble_h = xTaskCreateStatic(ble_worker, "ble_worker", 4096, NULL, 5,
                                           s_ble_stack, &s_ble_tcb);
    if (!ble_h) {
        ESP_LOGE(TAG, "ble worker 创建失败,回滚 audio worker");
        vTaskDelete(audio_h);   // audio worker 阻塞等待中,删除安全(无持有资源)
        s_audio_task = NULL;
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        return ESP_FAIL;
    }
    s_ble_task = ble_h;
    s_ready = true;   // 全部资源就绪:此后公开 API 才可访问 ring/sem/worker
    ESP_LOGI(TAG, "流式管线就绪(静态环 %d B / %d 槽,块 %d B)",
             RING_BYTES, RING_SLOTS, CHUNK_BYTES);
    return ESP_OK;
}

esp_err_t audio_streamer_start(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;   // init 失败:启动被拒(空转,不访问 NULL ring/sem)
    if (s_active) return ESP_OK;                  // 幂等:已在会话中
    // 会话 token:每个新会话取新值,环内残留/在途 item(旧 token)在 ble_worker
    // 被静默丢弃 —— 残留绝不流入新会话,start 永不因残留拒绝(无排空门禁,
    // 见 CHUNK_ITEM_BYTES 注释;cancel 后无需等环空即可立即重开)。
    s_session_token++;
    s_active = true;
    s_peak = 0;
    s_drop_active = false;
    s_drop_count = 0;
    // ADPCM 自适应复位由 ble_worker 消费(token 变化检测,不在本任务碰编码
    // 状态):新会话首块前复位,不携带上一会话的静音/噪声自适应。
    xSemaphoreGive(s_sem);
    ESP_LOGI(TAG, "采集开始 (%s, 会话 %u)", s_compressed ? "ADPCM 4:1" : "PCM",
             (unsigned)s_session_token);
    return ESP_OK;
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
    // 幂等:不检查 s_active——采集可能已停(STOP 后断链),环里残留同样要作废。
    // 语义:①作废会话 token:worker 在 read 阻塞期间已快照旧 token,其迟到块
    // (read 返回后 send)带旧值,被 ble_worker 按失效静默丢弃——残留与在途
    // 帧绝不流入下一次会话,且无需排空等待(cancel 快速返回)。
    // ②停采集(环不再有新写入)③等 audio_worker 退出阻塞读(此后环稳定)。
    // 不用 ring reset API(ESP-IDF 无 xRingbufferReset;且任何"清空"实现都会
    // 与 ble_worker 已取未还的在途块冲突)——token 失效等价于清空,成本为零。
    s_session_token++;
    s_active = false;
    wait_worker_exit(WORKER_EXIT_TIMEOUT_MS);
    ESP_LOGI(TAG, "采集取消(残留由 token 失效丢弃)");
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
#ifdef CONFIG_IDF_TARGET_ESP32C3
        // 诊断(2026-08-28):RTC 取证 —— audio worker 心跳。
        // (仅真机构建;host 测试无 MMIO 映射,SEGFAULT)
        { volatile uint32_t *d = (volatile uint32_t *)0x50001E00; d[9] += 1; d[2] = 'D'; }
#endif
        xSemaphoreTake(s_sem, portMAX_DELAY);    // 等待 start 信号
        s_worker_busy = true;
        while (s_active) {
            // 本块归属的会话 token(进循环快照):read 阻塞期间若发生 cancel,
            // 迟到块(本块)仍带旧 token,被 ble_worker 按失效静默丢弃。
            uint32_t my_token = s_session_token;
            // +4:item 头留给 token;整体 4B 对齐,DMA 目标(+4)仍在 4B 边界
            esp_err_t e = bsp_audio_read(s_chunk + 4, CHUNK_BYTES);
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
            const int16_t *p = (const int16_t *)(s_chunk + 4);
            uint16_t peak = 0;
            for (size_t i = 0; i < CHUNK_BYTES / 2; i++) {
                int32_t v = p[i];
                uint16_t a = (uint16_t)(v < 0 ? -v : v);
                if (a > peak) peak = a;
            }
            s_peak = peak;

            // 盖会话 token(item 头),与块一起入环
            *(uint32_t *)s_chunk = my_token;
            if (xRingbufferSend(s_ring, s_chunk, CHUNK_ITEM_BYTES, 0) == pdTRUE) {
                ring_items_inc();
            } else {
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
#ifdef CONFIG_IDF_TARGET_ESP32C3
        // 诊断(2026-08-28):RTC 取证 —— ble worker 心跳。
        { volatile uint32_t *d = (volatile uint32_t *)0x50001E00; d[2] = 'B'; }
#endif
        size_t len = 0;
        // NOSPLIT:整 item 语义,len 恒等于写入长度(不会像 BYTEBUF 那样短读)
        void *item = xRingbufferReceive(s_ring, &len, pdMS_TO_TICKS(100));
        if (!item) {
            continue;   // 环空:自然态
        }
        int rc = 0;
        if (len != CHUNK_ITEM_BYTES) {
            // 不该发生(NOSPLIT 保证 item 完整);真出现就丢这一块并留证,
            // 绝不按定长解读缓冲(旧 BYTEBUF 路径正是这样越界读的)。
            ESP_LOGW(TAG, "环块长度异常 %u(期望 %d),丢弃",
                     (unsigned)len, CHUNK_ITEM_BYTES);
            rc = -1;
        } else if (*(const uint32_t *)item != s_session_token) {
            // 会话闸门:旧会话残留/迟到块(长度正常但 token 不匹配)静默丢弃
            // —— 取消/断链后环内残留靠此失效,start 无需排空即可立即重开。
            // 静默:正常生命周期(每次取消都会出现),不计数不报警不触发事件。
            vRingbufferReturnItem(s_ring, item);
            ring_item_returned();
            continue;
        } else if (s_compressed) {
            // BLE 压缩路径:整块 3200B PCM → 一个 804B ADPCM block 一次发送。
            // 每块自带 predictor/index 头,单块可解 —— 丢一块只损失该 100ms。
            if (s_last_encoded_token != s_session_token) {
                // 新会话首块(token 变化检测):复位自适应,不携带上一会话
                // 的静音/噪声自适应
                adpcm_state_reset(&s_adpcm);
                s_last_encoded_token = s_session_token;
            }
            uint8_t block[ADPCM_BLOCK_BYTES];
            size_t n = adpcm_encode_block(&s_adpcm, (const int16_t *)(item + 4),
                                          CHUNK_BYTES / 2, block, sizeof(block));
            if (n == 0) {   // 参数/容量不符(编译期常量,理论不可达):丢块留证
                ESP_LOGW(TAG, "ADPCM 编码失败(块 %u B),丢弃", (unsigned)len);
                rc = -1;
            } else {
                // 未注册通道视为发送失败(与旧语义一致)
                rc = s_send_fn ? s_send_fn(block, n) : -1;
            }
        } else {
            // 非压缩路径(USB):PCM 块原样发送(4B token 是环内元数据,
            // 不上链路,载荷 3200B 与旧协议一致)
            rc = s_send_fn ? s_send_fn((const uint8_t *)item + 4, CHUNK_BYTES) : -1;
        }
        vRingbufferReturnItem(s_ring, item);
        ring_item_returned();
        if (rc == 0) {
            // 发送恢复且环已有余量(不再积压)才解除 BLE BUSY——
            // 只靠发送成功不够:若发送慢但一直成功,环满造成的丢帧永远不会被解除
            if (s_drop_active && ring_items() < RING_SLOTS) {
                s_drop_active = false;
                app_event_t ev = { .type = APP_EV_AUDIO_DROP_END };
                app_event_post(&ev);
            }
        } else {
            // 无订阅/断连/编码失败/块长异常:整块丢弃并计数(voice.end 后 status 帧上报)
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
