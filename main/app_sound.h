// main/app_sound.h —— 提示音模块。
// 方波提示音经 bsp_audio_write 输出(移植自 demo_audio.c 的 play_tone 模式)。
// 与录音严格分时:START 音必须同步播完才开流;其余提示音异步走队列。
#pragma once

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 建静态队列 + sound_worker 任务(prio 3, 栈 2KB),并初始化 codec 为 16k/16/1。
esp_err_t app_sound_init(void);

// 异步播放:入队由 sound_worker 播出。用于非采集期的提示音(SEND/APPROVAL/SUCCESS/REJECT/ERROR)。
void app_sound_play(app_tone_t tone);

// 同步播放:在调用者上下文中阻塞播完。仅 app_task 在 START(开流前)使用,
// 保证 80ms 滴声先于录音,避免 esp_codec_dev 单用户冲突。
void app_sound_play_sync(app_tone_t tone);

#ifdef __cplusplus
}
#endif
