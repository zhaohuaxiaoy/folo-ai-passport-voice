// tests/fake/freertos/FreeRTOS.h —— FreeRTOS 核心类型的最小宿主替身。
// pdMS_TO_TICKS 在 fake 中直接按毫秒映射(fake_rtos.c 的等待实现用毫秒)。
#pragma once

#include <stdint.h>

typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
typedef int32_t  TickType_t;
typedef int      portMUX_TYPE;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY ((TickType_t)0x7FFFFFFF)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define portMUX_INITIALIZER_UNLOCKED 0
/* 引用 mux 参数以"使用"它(否则 -Wunused-variable 误报静态 mux) */
#define portENTER_CRITICAL(mux)      ((void)(mux))
#define portEXIT_CRITICAL(mux)       ((void)(mux))
