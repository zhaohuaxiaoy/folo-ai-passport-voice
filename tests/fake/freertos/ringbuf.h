// tests/fake/freertos/ringbuf.h —— ESP-IDF esp_ringbuf API 的最小宿主替身。
// 声明范围刻意保持"被测代码用到的符号"(audio_streamer.c)。真实语义:
// BYTEBUF 字节环,取走=在途(xRingbufferReceiveUpTo 返回的指针持有中),
// vRingbufferReturnItem 归还后才恢复空闲;空闲大小不含在途块。
#pragma once

#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>

typedef void *RingbufHandle_t;

typedef struct xSTATIC_RINGBUFFER {
    int dummy;
} StaticRingbuffer_t;

typedef enum {
    RINGBUF_TYPE_NOSPLIT   = 0,
    RINGBUF_TYPE_ALLOWSPLIT = 1,
    RINGBUF_TYPE_BYTEBUF   = 2,
} RingbufferType_t;

RingbufHandle_t xRingbufferCreateStatic(size_t xBufferSize, RingbufferType_t xBufferType,
                                        uint8_t *pucRingbufferStorage,
                                        StaticRingbuffer_t *pxStaticRingbuffer);

BaseType_t xRingbufferSend(RingbufHandle_t xRingbuffer, const void *pvItem,
                           size_t xItemSize, TickType_t xTicksToWait);

void *xRingbufferReceiveUpTo(RingbufHandle_t xRingbuffer, size_t *pxItemSize,
                             TickType_t xTicksToWait, size_t xMaxSize);

void vRingbufferReturnItem(RingbufHandle_t xRingbuffer, void *pvItem);

size_t xRingbufferGetCurFreeSize(RingbufHandle_t xRingbuffer);
