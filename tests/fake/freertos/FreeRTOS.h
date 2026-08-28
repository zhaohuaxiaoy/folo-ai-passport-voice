// tests/fake/freertos/FreeRTOS.h —— FreeRTOS 核心类型的最小宿主替身。
// pdMS_TO_TICKS 在 fake 中直接按毫秒映射(fake_rtos.c 的等待实现用毫秒)。
#pragma once

#include <stdint.h>
#include <pthread.h>

typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
typedef int32_t  TickType_t;
// ⚠ 临界区必须是【真实互斥】,不是空操作:audio_streamer 的 s_ring_items++/--
// 由 audio_worker 与 ble_worker 并发执行,真机上是关中断保证原子;fake 若
// 空实现,偶发丢一次递增 → 计数与环失同步 → start() 永久拒绝(实测 3/15,
// 见任务 08-27-ring-desync-diagnose prd.md 的定性结论)。
typedef pthread_mutex_t portMUX_TYPE;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY ((TickType_t)0x7FFFFFFF)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mux)      pthread_mutex_lock(mux)   /* 产品代码传 &mux(与真机一致) */
#define portEXIT_CRITICAL(mux)       pthread_mutex_unlock(mux)
