// tests/fake/fake_rtos.c —— FreeRTOS ringbuf/semphr/task 与外围 BSP/事件/发送
// 的最小宿主实现。全部基于 pthread,单进程内模拟 audio_streamer 的并发时序。
// 可控钩子(供 test_audio_streamer.c 断言):
//   - g_notify_rc / g_notify_block_ms: link_send_audio(音频帧发送桩)的返回码与阻塞时长
//   - g_fake_audio_fail: bsp_audio_read 报错(测 AUDIO_ERROR 事件)
//   - g_fake_ring_fail / g_fake_sem_fail: init 失败注入(测 API 空转保护)
//   - g_events[]: app_event_post 投递记录
#include <assert.h>
#include <errno.h>
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
extern uint32_t g_fake_ring_residue_token;
extern int g_fake_sem_fail;
extern int g_fake_task_fail_at;
extern int g_sem_live;

// ==================== 信号量(pthread cond,ticks 按毫秒) ====================
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             n;      // 可用计数
    int             dead;   // vSemaphoreDelete 已标记:等待者立即返回 pdFALSE
} FakeSem;

// 任务"删除即在下一个阻塞点退出"(实现见文件末尾任务段;此处前置声明,
// 因为阻塞原语要在入口检查)。
static void task_exit_if_deleted(void);

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
    task_exit_if_deleted();     // 已被 vTaskDelete 标记 → 在阻塞点自杀(见下)
    pthread_mutex_lock(&s->m);
    if (s->dead) { pthread_mutex_unlock(&s->m); return pdFALSE; }
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
        if (s->dead) { pthread_mutex_unlock(&s->m); return pdFALSE; }
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
// 计数 g_sem_live 供测试断言"失败路径零泄漏"(逻辑存活数,与 malloc 无关)。
// 关键:标记 dead + 广播唤醒等待者,并且【故意不 free】。
// 旧实现直接 free:回滚路径遗留的 audio_worker 正阻塞在这块内存的 cond 上
// (vTaskDelete 是空实现),之后 pthread_cond_destroy 返回 EBUSY、内存被释放
// → 悬垂等待者;下一次 init 的 calloc 可能拿回同一地址,start() 的 give 就把
// 这个僵尸线程唤醒成第二个生产者 → 整个套件偶发多帧/环满丢帧
// (实测 6 次运行有 1~2 次在不同用例上断言失败)。
void vSemaphoreDelete(SemaphoreHandle_t h) {
    FakeSem *s = h;
    if (!s) return;
    g_sem_live--;
    pthread_mutex_lock(&s->m);
    s->dead = 1;
    pthread_cond_broadcast(&s->c);   // 等待者立即醒来,take 返回 pdFALSE
    pthread_mutex_unlock(&s->m);
    // 不 destroy/不 free:等待者可能仍在 cond 上,地址必须保持有效且不被复用。
}

// ==================== 环形缓冲(按 ESP-IDF v5.5 语义建模) ====================
// 保真点(旧替身缺失,导致越界读逃过 host 测试):
//   1) BYTEBUF 的 ReceiveUpTo 返回 storage 内指针,长度截到物理尾部 → 跨尾短读;
//   2) NOSPLIT 每 item 带 8B 头 + 4B 对齐,item 不跨尾(尾部不够整段跳过),
//      单 item 上限 size/2 - 8,超限 Send 失败;
//   3) Receive 阻塞语义:空环等到超时才返回 NULL(worker 不空转)。
#define FAKE_RB_HDR       8
#define FAKE_RB_MAX_ITEMS 16

static size_t rb_align4(size_t n) { return (n + 3u) & ~(size_t)3u; }

typedef struct {
    size_t off;        // 载荷在 storage 内偏移
    size_t len;        // 载荷长度
    size_t consumed;   // 该 item 占用的环空间(跳过的尾段 + 头 + 对齐后的载荷)
    int    in_flight;  // 已取未还
} FakeItem;

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;          // 有新 item / 有归还
    uint8_t  *storage;
    size_t    size;
    RingbufferType_t type;
    size_t    head;             // 读游标(BYTEBUF 用)
    size_t    tail;             // 写游标
    size_t    used;             // 已写未取(含头/对齐/跳过段)
    size_t    in_flight;        // 已取未还
    FakeItem  items[FAKE_RB_MAX_ITEMS];   // NOSPLIT item 队列(FIFO)
    int       it_head, it_count;
    size_t    byte_item_len;    // BYTEBUF 当前在途长度
} FakeRing;

static size_t ring_free(FakeRing *r) {
    return r->size - r->used - r->in_flight;
}

RingbufHandle_t xRingbufferCreateStatic(size_t xBufferSize, RingbufferType_t xBufferType,
                                        uint8_t *pucRingbufferStorage,
                                        StaticRingbuffer_t *pxStaticRingbuffer) {
    (void)pxStaticRingbuffer;
    if (g_fake_ring_fail) return NULL;   // 失败注入钩子(测 init 部分失败保护)
    FakeRing *r = calloc(1, sizeof(*r));
    r->storage = pucRingbufferStorage;
    r->size    = xBufferSize;
    r->type    = xBufferType;
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->c, NULL);
    if (g_fake_ring_residue > 0) {
        // 残留注入钩子:模拟"上一次会话 stop 后环内未消费数据"(stop 只停采集
        // 不排空)。NOSPLIT 下注入成一个短 item,消费者按长度校验丢弃。
        size_t n = (size_t)g_fake_ring_residue;
        if (n > r->size / 2 - FAKE_RB_HDR) n = r->size / 2 - FAKE_RB_HDR;
        memset(r->storage, 0xAA, n);   // 任意字节:丢弃路径不读内容
        if (xBufferType == RINGBUF_TYPE_BYTEBUF) {
            r->used = n;
            r->tail = n;
        } else {
            size_t need = FAKE_RB_HDR + rb_align4(n);
            memmove(r->storage + FAKE_RB_HDR, r->storage, n);
            r->items[0] = (FakeItem){ .off = FAKE_RB_HDR, .len = n, .consumed = need };
            r->it_count = 1;
            r->used = need;
            r->tail = need;
        }
    }
    if (g_fake_ring_residue_token != 0) {
        // 旧 token item 注入钩子:模拟"取消/断链后环内残留的上一会话 item"
        // (token 失效路径,产品语义见 audio_streamer.c CHUNK_ITEM_BYTES 注释)。
        // 长度 = 产品 CHUNK_ITEM_BYTES = 4B token + 3200B PCM(3204B);
        // token 固定 0xDEADBEEF,恒不等于任何测试会话 token(start 自 1 递增)。
        size_t n = 3204;
        if (n > r->size / 2 - FAKE_RB_HDR) n = r->size / 2 - FAKE_RB_HDR;
        memset(r->storage, 0x11, n);                 // PCM 任意字节:丢弃路径不读内容
        *(uint32_t *)r->storage = 0xDEADBEEFu;       // item 数据区开头 4B = token(storage 4B 对齐)
        size_t need = FAKE_RB_HDR + rb_align4(n);
        memmove(r->storage + FAKE_RB_HDR, r->storage, n);
        r->items[r->it_count] = (FakeItem){ .off = FAKE_RB_HDR, .len = n, .consumed = need };
        r->it_count++;
        r->used += need;
        r->tail = r->used;
    }
    return r;
}

BaseType_t xRingbufferSend(RingbufHandle_t h, const void *pvItem,
                           size_t xItemSize, TickType_t xTicksToWait) {
    (void)xTicksToWait;
    task_exit_if_deleted();
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    if (r->type == RINGBUF_TYPE_BYTEBUF) {
        if (xItemSize > ring_free(r)) {
            pthread_mutex_unlock(&r->m);
            return pdFALSE;   // 满:不阻塞(fake 不做超时等待,语义等价于立即超时)
        }
        size_t first = (r->size - r->tail < xItemSize) ? (r->size - r->tail) : xItemSize;
        memcpy(r->storage + r->tail, pvItem, first);
        if (first < xItemSize) {
            memcpy(r->storage, (const uint8_t *)pvItem + first, xItemSize - first);
        }
        r->tail = (r->tail + xItemSize) % r->size;
        r->used += xItemSize;
        pthread_cond_broadcast(&r->c);
        pthread_mutex_unlock(&r->m);
        return pdTRUE;
    }
    // NOSPLIT:单 item 上限 = align(size/2) - 头(真实 xMaxItemSize),超限直接失败
    if (xItemSize == 0 || xItemSize > rb_align4(r->size / 2) - FAKE_RB_HDR ||
        r->it_count >= FAKE_RB_MAX_ITEMS) {
        pthread_mutex_unlock(&r->m);
        return pdFALSE;
    }
    size_t need = FAKE_RB_HDR + rb_align4(xItemSize);
    size_t tail_room = r->size - r->tail;
    size_t skip = (tail_room < need) ? tail_room : 0;   // item 不跨尾:尾段整体跳过
    if (need + skip > ring_free(r)) {
        pthread_mutex_unlock(&r->m);
        return pdFALSE;
    }
    size_t base = skip ? 0 : r->tail;
    memcpy(r->storage + base + FAKE_RB_HDR, pvItem, xItemSize);
    int slot = (r->it_head + r->it_count) % FAKE_RB_MAX_ITEMS;
    r->items[slot] = (FakeItem){ .off = base + FAKE_RB_HDR, .len = xItemSize,
                                 .consumed = need + skip };
    r->it_count++;
    r->tail = (base + need) % r->size;
    r->used += need + skip;
    pthread_cond_broadcast(&r->c);
    pthread_mutex_unlock(&r->m);
    return pdTRUE;
}

// 等到"有可取数据"或超时;调用方持锁。返回 1 = 有数据。
static int rb_wait_data(FakeRing *r, TickType_t ms, int nosplit) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ms / 1000);
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    for (;;) {
        int ready = nosplit
            ? (r->it_count > 0 && !r->items[r->it_head].in_flight)
            : (r->used > 0);
        if (ready) return 1;
        if (ms == 0) return 0;
        if (pthread_cond_timedwait(&r->c, &r->m, &ts) == ETIMEDOUT) {
            return nosplit ? (r->it_count > 0 && !r->items[r->it_head].in_flight)
                           : (r->used > 0);
        }
    }
}

void *xRingbufferReceiveUpTo(RingbufHandle_t h, size_t *pxItemSize,
                             TickType_t xTicksToWait, size_t xMaxSize) {
    task_exit_if_deleted();
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    if (!rb_wait_data(r, xTicksToWait, 0)) {
        pthread_mutex_unlock(&r->m);
        return NULL;
    }
    // ⚠ 真实语义:一次只返回到 storage 物理尾部,跨尾必须再取一次(短读)。
    size_t n = r->used < xMaxSize ? r->used : xMaxSize;
    size_t to_end = r->size - r->head;
    if (n > to_end) n = to_end;
    void *p = r->storage + r->head;
    r->head = (r->head + n) % r->size;
    r->used -= n;
    r->in_flight += n;
    r->byte_item_len = n;
    *pxItemSize = n;
    pthread_mutex_unlock(&r->m);
    return p;
}

void *xRingbufferReceive(RingbufHandle_t h, size_t *pxItemSize, TickType_t xTicksToWait) {
    task_exit_if_deleted();
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    if (!rb_wait_data(r, xTicksToWait, 1)) {
        pthread_mutex_unlock(&r->m);
        return NULL;
    }
    FakeItem *it = &r->items[r->it_head];
    it->in_flight = 1;
    r->used      -= it->consumed;
    r->in_flight += it->consumed;
    *pxItemSize = it->len;             // 完整 item:实现保证边界,不会短读
    void *p = r->storage + it->off;
    pthread_mutex_unlock(&r->m);
    return p;
}

void vRingbufferReturnItem(RingbufHandle_t h, void *pvItem) {
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    if (r->type == RINGBUF_TYPE_BYTEBUF) {
        (void)pvItem;
        r->in_flight -= r->byte_item_len;
        r->byte_item_len = 0;
    } else {
        FakeItem *it = &r->items[r->it_head];
        assert(r->it_count > 0 && it->in_flight &&
               (uint8_t *)pvItem == r->storage + it->off);   // 单消费者按序归还
        r->in_flight -= it->consumed;
        it->in_flight = 0;
        r->it_head = (r->it_head + 1) % FAKE_RB_MAX_ITEMS;
        r->it_count--;
    }
    pthread_cond_broadcast(&r->c);
    pthread_mutex_unlock(&r->m);
}

// 真实语义:返回"当前还能放下的最大 item 载荷",与游标位置有关 —— 因此不能
// 当作判空依据(audio_streamer 改用自己的 item 计数,见该文件注释)。
size_t xRingbufferGetCurFreeSize(RingbufHandle_t h) {
    FakeRing *r = h;
    pthread_mutex_lock(&r->m);
    size_t f = ring_free(r);
    if (r->type != RINGBUF_TYPE_BYTEBUF) {
        size_t tail_room = r->size - r->tail;
        size_t contiguous = f < tail_room ? f : tail_room;
        if (contiguous > FAKE_RB_HDR) {
            size_t wrap = (f > tail_room) ? (f - tail_room) : 0;
            size_t best = contiguous;
            if (wrap > best) best = wrap;
            f = best - FAKE_RB_HDR;
        } else {
            f = (f > tail_room + FAKE_RB_HDR) ? (f - tail_room - FAKE_RB_HDR) : 0;
        }
    }
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
    task_exit_if_deleted();
    usleep((useconds_t)(ticks * 1000));
    s_ticks_ms += ticks;
}

// 任务上下文。deleted 由 vTaskDelete 置位,线程在下一个阻塞原语入口
// (xSemaphoreTake / xRingbufferReceive* / xRingbufferSend / vTaskDelay)
// 自行 pthread_exit —— 与真机"任务被删除后不再运行"等价,且不用 pthread_cancel
// (在 cond_wait 处取消会带着 mutex 恢复,导致 destroy EBUSY / 后续死锁)。
typedef struct { TaskFunction_t fn; void *arg; volatile int deleted; } TaskCtx;

static __thread TaskCtx *s_self = NULL;   // 当前线程的任务上下文

static void task_exit_if_deleted(void) {
    // 在任何锁之外调用:此处退出不会留下 locked mutex。
    if (s_self && s_self->deleted) pthread_exit(NULL);
}

static void *thread_main(void *p) {
    TaskCtx *c = p;
    s_self = c;
    c->fn(c->arg);
    // 不 free(c):句柄(StaticTask_t)可能仍指向它,vTaskDelete 会读 deleted。
    return NULL;
}

// 失败注入钩子:g_fake_task_fail_at = N → 第 N 次任务创建失败(计数递减),
// 0 = 从不失败。测 worker 创建失败:1 = audio_worker 失败,2 = ble_worker
// 失败(回滚路径)(复核 R1)。动态/静态创建共用同一计数。
static int task_create_blocked(void) {
    if (g_fake_task_fail_at > 0) {
        g_fake_task_fail_at--;
        if (g_fake_task_fail_at == 0) return 1;
    }
    return 0;
}

static TaskCtx *task_spawn(TaskFunction_t fn, void *arg) {
    pthread_t tid;
    TaskCtx *c = calloc(1, sizeof(*c));
    c->fn = fn; c->arg = arg;
    if (pthread_create(&tid, NULL, thread_main, c) != 0) {
        free(c);
        return NULL;
    }
    pthread_detach(tid);
    return c;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t priority, void *handle) {
    (void)name; (void)stack_depth; (void)priority; (void)handle;
    if (task_create_blocked()) return pdFAIL;
    return task_spawn(fn, arg) ? pdPASS : pdFAIL;
}

TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                               void *arg, UBaseType_t priority,
                               StackType_t *stack, StaticTask_t *tcb) {
    (void)name; (void)priority;
    assert(stack && tcb && stack_depth > 0);   // 真机 configASSERT 等价:栈/TCB 必须给
    if (task_create_blocked()) return NULL;
    TaskCtx *c = task_spawn(fn, arg);
    if (!c) return NULL;
    tcb->dummy = c;   // 句柄→上下文,供 vTaskDelete 置删除标记
    return tcb;   // 句柄 = TCB 地址(与真机一致,可传给 uxTaskGetStackHighWaterMark)
}

// vTaskDelete 宿主实现:init 失败回滚路径调用(audio_worker 已建、ble_worker 失败)。
// 合作式删除:置 deleted,线程在下一个阻塞原语入口 pthread_exit。
// 为什么不能是空实现(旧版):被"删除"的 audio_worker 仍活着并读全局 s_sem/s_ring
// (回滚时这两个全局已被本次 init 覆盖),下一次成功 init + start 会把它唤醒成
// 第二个生产者 —— 套件里多处精确计数断言随机失败的根源。
// 为什么不用 pthread_cancel:在 cond_wait 取消时会重新获取 mutex,残留 locked
// → vSemaphoreDelete 的 destroy EBUSY / 重试路径死锁。
void vTaskDelete(TaskHandle_t h) {
    if (!h) return;
    TaskCtx *c = ((StaticTask_t *)h)->dummy;
    if (c) c->deleted = 1;   // 唤醒由紧随其后的 vSemaphoreDelete 广播完成
}

// ==================== 外围替身与可控钩子 ====================

const char *esp_err_to_name(esp_err_t code) { return code == 0 ? "ESP_OK" : "ESP_FAIL"; }

// --- bsp_audio_read:按递增序号填充数据(fake 每次调用即一块) ---
int g_fake_audio_fail = 0;   // 非 0 → 返回 ESP_FAIL(测 AUDIO_ERROR 事件)
static unsigned g_audio_seq = 0;

// --- init 失败注入:xRingbufferCreateStatic / xSemaphoreCreateBinary 返回 NULL,
// xTaskCreate 第 N 次调用失败(测 init 部分失败后公开 API 空转保护:
// start/cancel/drain 不访问 NULL;复核 R1 的失败清理/幂等) ---
int g_fake_ring_fail = 0;    // 非 0 → 环创建失败
int g_fake_ring_residue = 0; // 非 0 → 环创建时预置残留字节(stop 残留注入,长度异常路径)
uint32_t g_fake_ring_residue_token = 0; // 非 0 → 环创建时注入一个完整旧 token item(token 失效路径)
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
// 最后一帧的副本:压缩路径用例要把它当 ADPCM block 解回 PCM 校验
// (只看长度无法区分"编出了 804B 垃圾"和"编出了正确 block")。
uint8_t g_notify_last[2048];
int g_notify_last_len = 0;

// 在途发送计数 + 条件变量。用途:fake_reset() 必须等上一个用例的在途 notify
// 真正返回后再清零计数器,否则前一用例(g_notify_block_ms=1000)的在途帧会在
// 下一用例 reset 之后才 g_notify_count++,把"精确计数"断言变成偶发失败。
static pthread_mutex_t s_tx_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_tx_c = PTHREAD_COND_INITIALIZER;
static int s_tx_inflight = 0;

int link_send_audio(const uint8_t *frame, size_t len) {
    pthread_mutex_lock(&s_tx_m);
    s_tx_inflight++;
    pthread_mutex_unlock(&s_tx_m);
    g_notify_count++;   // 调用即计入(发出/在途);阻塞只是模拟在途占用
    if ((int)len < g_notify_min_len) g_notify_min_len = (int)len;
    if ((int)len > g_notify_max_len) g_notify_max_len = (int)len;
    if (frame && len <= sizeof(g_notify_last)) {
        memcpy(g_notify_last, frame, len);
        g_notify_last_len = (int)len;
    }
    if (g_notify_block_ms > 0) usleep((useconds_t)(g_notify_block_ms * 1000));
    pthread_mutex_lock(&s_tx_m);
    s_tx_inflight--;
    pthread_cond_broadcast(&s_tx_c);
    pthread_mutex_unlock(&s_tx_m);
    return g_notify_rc;
}

void fake_reset(void) {
    // 先等在途发送出清(见 s_tx_inflight 注释),再清零——否则跨用例污染。
    pthread_mutex_lock(&s_tx_m);
    while (s_tx_inflight > 0) pthread_cond_wait(&s_tx_c, &s_tx_m);
    pthread_mutex_unlock(&s_tx_m);
    g_fake_audio_fail = 0;
    g_fake_task_fail_at = 0;   // 注入钩子复位(测试显式设置/清零的风格保持)
    g_notify_rc = 0;
    g_notify_block_ms = 0;
    g_notify_count = 0;
    g_notify_min_len = INT_MAX;
    g_notify_max_len = 0;
    g_notify_last_len = 0;
    g_event_count = 0;
    s_ticks_ms = 0;
    memset(g_events, 0, sizeof(g_events));
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h) {
    (void)h;
    return 2048;
}
