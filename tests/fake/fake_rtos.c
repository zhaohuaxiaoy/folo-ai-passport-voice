// tests/fake/fake_rtos.c —— FreeRTOS ringbuf/semphr/task 与外围 BSP/事件/发送
// 的最小宿主实现。全部基于 pthread,单进程内模拟 audio_streamer 的并发时序。
// 可控钩子(供 test_audio_streamer.c 断言):
//   - g_notify_rc / g_notify_block_ms: link_send_audio(音频帧发送桩)的返回码与阻塞时长
//   - g_fake_audio_fail: bsp_audio_read 报错(测 AUDIO_ERROR 事件)
//   - g_fake_ring_fail / g_fake_sem_fail: init 失败注入(测 API 空转保护)
//   - g_events[]: app_event_post 投递记录
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_events.h"
#include "bsp_audio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* init 失败注入钩子(定义于下方"外围替身与可控钩子"节,stub 引用前置声明) */
extern int g_fake_ring_fail;
extern int g_fake_ring_residue;
extern int g_fake_sem_fail;
extern int g_fake_task_fail_at;
extern int g_sem_live;

// ==================== 信号量(pthread cond,ticks 按毫秒) ====================
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             n;   // 可用计数
} FakeSem;

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    if (g_fake_sem_fail) return NULL;   // 失败注入钩子(测 init 部分失败保护)
    FakeSem *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    g_sem_live++;                       // 存活计数(vSemaphoreDelete 递减)
    return s;
}

static void timedwait(FakeSem *s, int64_t ms) {
    // pthread_cond_timedwait 的绝对超时按 CLOCK_REALTIME 解释(macOS 不支持
    // MONOTONIC condattr);xTaskGetTickCount 才用 MONOTONIC。用错时钟会让
    // 时间戳差出"系统启动至今",超时立即失效——F1 引入带超时 take 后必现。
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    ts.tv_sec  += (time_t)(ms / 1000) + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&s->c, &s->m, &ts);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t ticks) {
    FakeSem *s = h;
    pthread_mutex_lock(&s->m);
    while (s->n == 0) {
        if (ticks == 0) {
            pthread_mutex_unlock(&s->m);
            return pdFALSE;
        }
        if (ticks == portMAX_DELAY) {
            pthread_cond_wait(&s->c, &s->m);
        } else {
            timedwait(s, (int64_t)ticks);
            if (s->n == 0) {
                pthread_mutex_unlock(&s->m);
                return pdFALSE;
            }
        }
    }
    s->n--;
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    FakeSem *s = h;
    pthread_mutex_lock(&s->m);
    s->n = 1;   // 二值
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}

// vSemaphoreDelete 宿主实现:init 失败清理路径调用(复核 R1)。信号量存活
// 计数 g_sem_live 供测试断言"失败路径零泄漏"。有等待者时 macOS pthread
// destroy 返回 EBUSY 不销毁 —— 泄漏的 worker 线程(回滚路径 vTaskDelete
// 空实现)永久阻塞在旧 cond wait,无唤醒者,不碰全局状态,测试内可接受。
void vSemaphoreDelete(SemaphoreHandle_t h) {
    FakeSem *s = h;
    if (!s) return;
    g_sem_live--;
    pthread_mutex_destroy(&s->m);
    pthread_cond_destroy(&s->c);
    free(s);
}

// ==================== 字节环(BYTEBUF:取走=在途,归还才恢复) ====================
typedef struct {
    pthread_mutex_t m;
    uint8_t  *storage;
    size_t    size;
    size_t    head;      // 消费位置
    size_t    tail;      // 生产位置
    size_t    used;      // 已写未取字节
    size_t    in_flight; // 已取未还字节(不含在 used 中)
    size_t    item_len;  // 当前在途 item 长度(单消费者,ReturnItem 用)
    uint8_t   out[3200]; // ReceiveUpTo 返回缓冲(单消费者复用,worker 归还前不再取)
} FakeRing;

RingbufHandle_t xRingbufferCreateStatic(size_t xBufferSize, RingbufferType_t xBufferType,
                                        uint8_t *pucRingbufferStorage,
                                        StaticRingbuffer_t *pxStaticRingbuffer) {
    (void)xBufferType;
    (void)pxStaticRingbuffer;
    if (g_fake_ring_fail) return NULL;   // 失败注入钩子(测 init 部分失败保护)
    FakeRing *r = calloc(1, sizeof(*r));
    r->storage = pucRingbufferStorage;
    r->size    = xBufferSize;
    if (g_fake_ring_residue > 0) {
        // 残留注入钩子:模拟"上一次会话 stop 后环内未消费字节"(stop 只停采集
        // 不排空,s_cancel=false)。worker 取到后按 start 的丢帧模式归还不发送。
        size_t n = (size_t)g_fake_ring_residue;
        if (n > r->size - 1) n = r->size - 1;
        memset(r->storage, 0xAA, n);   // 任意字节:丢弃路径不读内容
        r->used = n;
        r->tail = n;
    }
    pthread_mutex_init(&r->m, NULL);
    return r;
}

static size_t ring_free(FakeRing *r) {
    return r->size - r->used - r->in_flight;
}

BaseType_t xRingbufferSend(RingbufHandle_t h, const void *pvItem,
                           size_t xItemSize, TickType_t xTicksToWait) {
    (void)xTicksToWait;
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    if (xItemSize > ring_free(r)) {
        pthread_mutex_unlock(&r->m);
        return pdFALSE;   // 满:不阻塞(fake 不做超时等待,语义等价于立即超时)
    }
    size_t first = (r->size - r->tail < xItemSize) ? (r->size - r->tail) : xItemSize;
    memcpy(r->storage + r->tail, pvItem, first);
    if (first < xItemSize) memcpy(r->storage, (const uint8_t *)pvItem + first, xItemSize - first);
    r->tail = (r->tail + xItemSize) % r->size;
    r->used += xItemSize;
    pthread_mutex_unlock(&r->m);
    return pdTRUE;
}

void *xRingbufferReceiveUpTo(RingbufHandle_t h, size_t *pxItemSize,
                             TickType_t xTicksToWait, size_t xMaxSize) {
    (void)xTicksToWait;
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    if (r->used == 0) {
        pthread_mutex_unlock(&r->m);
        return NULL;   // 空:立即返回(不等超时,测试里等价于环空采样)
    }
    size_t n = r->used < xMaxSize ? r->used : xMaxSize;
    size_t first = (r->size - r->head < n) ? (r->size - r->head) : n;
    memcpy(r->out, r->storage + r->head, first);
    if (first < n) memcpy(r->out + first, r->storage, n - first);
    r->head = (r->head + n) % r->size;
    r->used -= n;
    r->in_flight += n;
    r->item_len = n;
    *pxItemSize = n;
    pthread_mutex_unlock(&r->m);
    return r->out;
}

void vRingbufferReturnItem(RingbufHandle_t h, void *pvItem) {
    (void)pvItem;
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    r->in_flight -= r->item_len;
    r->item_len = 0;
    pthread_mutex_unlock(&r->m);
}

size_t xRingbufferGetCurFreeSize(RingbufHandle_t h) {
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    size_t f = ring_free(r);
    pthread_mutex_unlock(&r->m);
    return f;
}

// ==================== 任务(线程) ====================
// 真实单调毫秒:测试进程内限频类逻辑(如发送失败日志 1s 窗口)需要真实时间;
// 虚拟 tick(vTaskDelay 推进)在 worker spin 窗口内冻结,会让窗口判定失效。
static int64_t s_ticks_ms = 0;   // 仅 vTaskDelay 推进(兼容旧语义,不再被读取)

TickType_t xTaskGetTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (TickType_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void vTaskDelay(TickType_t ticks) {
    usleep((useconds_t)(ticks * 1000));
    s_ticks_ms += ticks;
}

typedef struct { TaskFunction_t fn; void *arg; } TaskCtx;

static void *thread_main(void *p) {
    TaskCtx *c = p;
    c->fn(c->arg);
    free(c);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t priority, void *handle) {
    (void)name; (void)stack_depth; (void)priority; (void)handle;
    // 失败注入钩子:g_fake_task_fail_at = N → 第 N 次调用失败(计数递减),
    // 0 = 从不失败。测 worker 创建失败:1 = audio_worker 失败,2 = ble_worker
    // 失败(回滚路径)(复核 R1)。
    if (g_fake_task_fail_at > 0) {
        g_fake_task_fail_at--;
        if (g_fake_task_fail_at == 0) return pdFAIL;
    }
    pthread_t tid;
    TaskCtx *c = malloc(sizeof(*c));
    c->fn = fn; c->arg = arg;
    return (pthread_create(&tid, NULL, thread_main, c) == 0) ? pdPASS : pdFAIL;
}

// vTaskDelete 宿主桩:init 失败回滚路径调用(audio_worker 已建、ble_worker 失败)。
// 空实现 = 遗留一个阻塞在旧信号量 cond_wait 上的挂起线程(无唤醒者,不碰全局
// 状态),测试进程退出时回收 —— 宿主限制,不代表固件运行时泄漏(真机 vTaskDelete
// 由内核立即回收任务)。不做"真删除"的原因:被删任务阻塞在 pthread_cond_wait,
// pthread_cancel 在 cond_wait 是取消点且取消恢复时重新获取 mutex → 信号量
// mutex 残留 locked,vSemaphoreDelete 的 pthread_mutex_destroy 返回 EBUSY,
// 重试路径 start() 死锁;join 不退出线程只会挂起调用方。
void vTaskDelete(TaskHandle_t h) { (void)h; }

// ==================== 外围替身与可控钩子 ====================

const char *esp_err_to_name(esp_err_t code) { return code == 0 ? "ESP_OK" : "ESP_FAIL"; }

// --- bsp_audio_read:按递增序号填充数据(fake 每次调用即一块) ---
int g_fake_audio_fail = 0;   // 非 0 → 返回 ESP_FAIL(测 AUDIO_ERROR 事件)
static unsigned g_audio_seq = 0;

// --- init 失败注入:xRingbufferCreateStatic / xSemaphoreCreateBinary 返回 NULL,
// xTaskCreate 第 N 次调用失败(测 init 部分失败后公开 API 空转保护:
// start/cancel/drain 不访问 NULL;复核 R1 的失败清理/幂等) ---
int g_fake_ring_fail = 0;    // 非 0 → 环创建失败
int g_fake_ring_residue = 0; // 非 0 → 环创建时预置残留字节(stop 残留注入)
int g_fake_sem_fail = 0;     // 非 0 → 信号量创建失败
int g_fake_task_fail_at = 0; // N>0 → 第 N 次 xTaskCreate 失败(0 = 从不)
int g_sem_live = 0;          // 存活信号量计数(断言 init 失败路径零泄漏)

esp_err_t bsp_audio_read(void *pcm, size_t bytes) {
    if (g_fake_audio_fail) return ESP_FAIL;
    memset(pcm, (int)(0x40 + (g_audio_seq % 64)), bytes);   // 数据随序号变化
    g_audio_seq++;
    return ESP_OK;
}

// --- app_event_post:记录投递(供断言取消/断链/丢帧事件) ---
#define FAKE_EVENT_CAP 64
app_event_t g_events[FAKE_EVENT_CAP];
int g_event_count = 0;

void app_event_post(const app_event_t *ev) {
    if (g_event_count < FAKE_EVENT_CAP) g_events[g_event_count++] = *ev;
}

// --- app_event_post_important:宿主无队列满语义,与普通投递等价(记录供断言)。
esp_err_t app_event_post_important(const app_event_t *ev, uint32_t timeout_ms) {
    (void)timeout_ms;
    app_event_post(ev);
    return ESP_OK;
}

// --- link_send_audio:音频帧发送桩(通道抽象后改名,语义同旧 ble_audio_notify_audio)。
// 可配置返回码与阻塞(模拟在途发送被流控占用);由测试经 audio_streamer_set_sender 注册。
int g_notify_count = 0;     // 成功到达发送层的帧数(cancel 丢弃的不算)
int g_notify_rc = 0;        // 返回码(非 0 走丢帧计数路径)
int g_notify_block_ms = 0;  // 阻塞毫秒(模拟在途发送被流控占用)

int g_notify_min_len = INT_MAX;   // 发送帧长范围:非 0 发送后 min/max 相等
int g_notify_max_len = 0;          // ⇔ 全部帧为完整块(无残留残片泄漏)
int link_send_audio(const uint8_t *frame, size_t len) {
    (void)frame;
    g_notify_count++;   // 调用即计入(发出/在途);阻塞只是模拟在途占用
    if ((int)len < g_notify_min_len) g_notify_min_len = (int)len;
    if ((int)len > g_notify_max_len) g_notify_max_len = (int)len;
    if (g_notify_block_ms > 0) usleep((useconds_t)(g_notify_block_ms * 1000));
    return g_notify_rc;
}

void fake_reset(void) {
    g_fake_audio_fail = 0;
    g_fake_task_fail_at = 0;   // 注入钩子复位(测试显式设置/清零的风格保持)
    g_notify_rc = 0;
    g_notify_block_ms = 0;
    g_notify_count = 0;
    g_notify_min_len = INT_MAX;
    g_notify_max_len = 0;
    g_event_count = 0;
    s_ticks_ms = 0;
    memset(g_events, 0, sizeof(g_events));
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h) {
    (void)h;
    return 2048;
}
