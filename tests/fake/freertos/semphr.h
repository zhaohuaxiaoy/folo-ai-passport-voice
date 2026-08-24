// tests/fake/freertos/semphr.h —— 信号量 API 的最小宿主替身(基于 pthread cond)。
#pragma once

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateBinary(void);

// ticks 按毫秒解释(见 FreeRTOS.h 的 pdMS_TO_TICKS);0 = 非阻塞尝试。
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
