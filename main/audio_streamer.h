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
// 开始采集与发送(清零会话丢帧计数)。若上一次取消的残留仍未排空(系统级异常),
// 拒绝启动并保持丢帧模式——下次 start 重试,旧帧绝不流入新会话。
void audio_streamer_start(void);
void audio_streamer_stop(void);        // 停止采集(发送 worker 继续排空,voice.end 前须 drain)
// 取消/断链:停采集 + 丢弃环内残留与在途帧(防残留流入下一次会话)。
// 幂等:采集已停(STOP 后断链)时同样清残留。实现为"丢帧模式"——ble_worker 取到块
// 只归还不发送,直至环空才自清;本函数等待排空完成(上限 CANCEL_DRAIN_TIMEOUT_MS),
// 超时返回后丢帧模式仍持续,残留绝不会流入下一次会话。
void audio_streamer_cancel(void);
// 等待环内残留数据发送完(最多 ms)。voice.end 前调用保证帧序。
void audio_streamer_drain(uint32_t ms);
bool audio_streamer_active(void);
uint16_t audio_streamer_peak(void);    // 最近一块的峰值采样(UI 音量条用)
// 读走本会话(自上次 start)累计丢帧数并清零。main.c 在 SEND_VOICE_END 的 drain 之后
// 取走(采集已停 + 环已排空,计数稳定),随 voice.end 后的 status 帧上报 Mac(掉帧对账)。
uint32_t audio_streamer_take_drops(void);

#ifdef __cplusplus
}
#endif
