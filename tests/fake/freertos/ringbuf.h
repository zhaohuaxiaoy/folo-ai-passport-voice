// tests/fake/freertos/ringbuf.h —— ESP-IDF esp_ringbuf API 的最小宿主替身。
// 声明范围刻意保持"被测代码用到的符号"(audio_streamer.c)。
//
// ⚠ 保真度是这个替身的唯一价值。旧版把 BYTEBUF 的跨尾回绕 memcpy 成连续缓冲,
// 于是"xRingbufferReceiveUpTo 跨尾只返回到缓冲尾部一段"这个真实语义在 host
// 侧不可复现 —— 越界读因此逃过了全部 host 测试(见任务 design.md §2 D-2)。
// 现在两种类型都按 ESP-IDF v5.5 实现建模:
//   BYTEBUF : xRingbufferReceiveUpTo 返回 storage 内指针,长度截到缓冲物理尾部
//             (prvGetItemByteBuf),跨尾必然短读。
//   NOSPLIT : 每 item 带 8B 头 + 4B 对齐,item 不跨尾(尾部不够就整段跳过),
//             xRingbufferReceive 返回完整 item;单 item 上限 size/2 - 8。
// 两者都是"取走=在途,vRingbufferReturnItem 归还后才恢复空闲"。
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

// NOSPLIT/ALLOWSPLIT:取一个完整 item(*pxItemSize = 该 item 的真实长度)。
void *xRingbufferReceive(RingbufHandle_t xRingbuffer, size_t *pxItemSize,
                         TickType_t xTicksToWait);

void vRingbufferReturnItem(RingbufHandle_t xRingbuffer, void *pvItem);

size_t xRingbufferGetCurFreeSize(RingbufHandle_t xRingbuffer);
