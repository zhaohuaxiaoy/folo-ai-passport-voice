// tests/fake/freertos/task.h —— 任务 API 的最小宿主替身(pthread 线程)。
#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *arg);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t priority, void *handle);

// ticks 按毫秒解释(usleep 实现)。
void vTaskDelay(TickType_t ticks);

// 单调递增毫秒计数(vTaskDelay 推进;供 drain 类轮询用)。
TickType_t xTaskGetTickCount(void);

typedef void *TaskHandle_t;

// 最小宿主桩:返回固定余量(测试仅保证符号可链;真机由 ESP-IDF 实测)。
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h);
