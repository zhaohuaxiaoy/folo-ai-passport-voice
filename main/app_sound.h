// main/app_sound.h —— 提示音模块。
// 方波提示音经 bsp_audio_write 输出(移植自 demo_audio.c 的 play_tone 模式)。
// 与录音严格分时:START 音由 sound_worker 播完 post TONE_DONE,app_task 收到才开流
// (S3 事件化,app_task 不再同步阻塞);其余提示音同样异步走队列。
#pragma once

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 建静态队列 + sound_worker 任务(prio 3, 栈 2KB),并初始化 codec 为 16k/16/1。
esp_err_t app_sound_init(void);

// 异步播放:入队由 sound_worker 播出,返回是否入队成功。
// START 音播完会 post APP_EV_TONE_DONE(开流信号);返回 false 时调用方须自行兜底。
bool app_sound_play(app_tone_t tone);

// 同步播放:在调用者上下文中阻塞播完。仅保留给非 S3 场景的确定性播放
// (当前无调用方,防回归保留;START 已改异步)。
void app_sound_play_sync(app_tone_t tone);

#ifdef __cplusplus
}
#endif
