// main/audio_streamer.h —— 流式音频管线。
// 4KB 静态环形缓冲 + 双 worker:audio 采集(阻塞 bsp_audio_read) → ring → BLE notify 发送。
// 任何路径无 >4KB 连续堆分配;拥塞时源端丢帧并上报 AUDIO_DROP_* 事件,丢帧数供 voice.end 后
// status 帧对账(design.md 掉帧对账)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_streamer_init(void);   // 建环形缓冲与两个 worker(空闲等待)
void audio_streamer_start(void);       // 开始采集与发送(清零会话丢帧计数)
void audio_streamer_stop(void);        // 停止采集(发送 worker 继续排空)
// 等待环内残留数据发送完(最多 ms)。voice.end 前调用保证帧序。
void audio_streamer_drain(uint32_t ms);
bool audio_streamer_active(void);
uint16_t audio_streamer_peak(void);    // 最近一块的峰值采样(UI 音量条用)
// 读走本会话(自上次 start)累计丢帧数并清零。main.c 在 STREAM_STOP 后取走,
// 随 voice.end 后的 status 帧上报 Mac(掉帧对账)。
uint32_t audio_streamer_take_drops(void);

#ifdef __cplusplus
}
#endif
