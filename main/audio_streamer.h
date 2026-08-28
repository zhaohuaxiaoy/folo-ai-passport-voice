// main/audio_streamer.h —— 流式音频管线。
// 静态环形缓冲(NOSPLIT 两槽)+ 双 worker:audio 采集(阻塞 bsp_audio_read) → ring → BLE notify 发送。
// 任何路径无 >4KB 连续堆分配;拥塞时源端丢帧并上报 AUDIO_DROP_* 事件,丢帧数供 voice.end 后
// status 帧对账(design.md 掉帧对账)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 音频帧发送函数(通道无关):BLE → ble_audio_notify_audio;USB → usb_link_send_audio。
// 载荷:压缩模式(ble)为 804B IMA ADPCM block(4B 头 + 800B 数据);非压缩
// (usb)为原始 3200B PCM 块。环内 item 的 4B 会话 token 是环内元数据,
// 不上链路。App 按 voice.start 的 audio 字段分发解析。
// 返回 0 = 已发送;非 0 = 发送失败(丢帧计数路径)。未注册(NULL)时一律按失败处理。
typedef int (*audio_send_fn_t)(const uint8_t *data, size_t len);

// 注册发送函数。调用点:main.c boot 与模式切换后按当前通道注册;切换前链路已断、
// 管线已停,不存在并发改指针(设计见 mode.c)。
void audio_streamer_set_sender(audio_send_fn_t fn);
// 压缩开关(模式驱动,BLE 专属):true → 每 3200B PCM 块编成一个 804B ADPCM
// block 发送(4:1,8KB/s);false → 原样发送(USB 带宽充裕,不压缩)。
void audio_streamer_set_compressed(bool on);

esp_err_t audio_streamer_init(void);   // 建环形缓冲与两个 worker(空闲等待)
// 开始采集与发送(清零会话丢帧计数)。会话以递增 token 标识,环内 item 头携带
// 快照 token;旧会话残留(环内/在途/迟到块)由 ble_worker 按 token 失效静默
// 丢弃——start 永不因残留拒绝,无需排空即可立即重开。
// 返回:ESP_OK 成功;ESP_ERR_INVALID_STATE 管线未就绪。调用方须检查返回值:
// 失败时录音未起来,UI 不得显示录音(REVIEW P2-C:回 READY + toast)。
esp_err_t audio_streamer_start(void);
void audio_streamer_stop(void);        // 停止采集(发送 worker 继续排空,voice.end 前须 drain)
// 取消/断链:作废会话 token + 停采集,环内残留/在途/迟到帧由 ble_worker 按
// token 失效静默丢弃——快速返回(不等环空,无排空门禁)。
// 幂等:采集已停(STOP 后断链)时同样成立(残留 token 已作废)。
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
