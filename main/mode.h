// main/mode.h —— 链路通道抽象(双通道常开架构)。
// 2026-08-28 架构变更:BLE 与 USB 同时常开,旧"互斥模式 + NVS 持久化 + 重启
// 切换"整体退役。本模块只做两件事:
//   1. 链路聚合:link_up = 任一通道通(BLE 订阅 || USB 会话);
//   2. 按会话通道路由:事件行/音频上行、音频格式、发送器注册都跟链路通道走
//      (通道常量见 app_types.h APP_CHAN_*,取值 0=BLE / 2=USB)。
#pragma once

#include "app_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 链路是否可用(任一通道通)。PTT 门禁用。
bool mode_link_up(void);

// 指定通道是否已通(BLE:EVENT 特征已订阅;USB:ping 握手完成)。断链收束
// 判断"另一通道是否还在"用。
bool mode_channel_up(uint8_t chan);

// 事件行上行到指定通道(序列化已完成,含 '\n')。返回 0 = 已发送。
// chan 非法/通道未通时发送失败,与旧语义一致(调用方记日志,不重试)。
int mode_send_event_line(uint8_t chan, const char *line, size_t len);

// 会话边界帧(voice.start/end):阻塞式投递,防 Mac 端会话状态悬挂
// (BLE 走 event_worker 队列,≤timeout_ms;USB 写驱动本就阻塞,语义已"重要")。
int mode_send_event_line_important(uint8_t chan, const char *line, size_t len,
                                   uint32_t timeout_ms);

// 会话音频格式(voice.start 上报,App 据此分发):BLE 压缩(ima_adpcm),
// USB 带宽充裕走原始 PCM。取值必须与实际载荷一致。
const char *mode_audio_format(uint8_t chan);

// 按会话通道注册音频发送器 + 压缩开关。在每次开流(STREAM_START)前调用,
// 由 app_task 串行执行,与 audio_worker 的 token 闸门共同保证切换安全。
void mode_select_audio_sender(uint8_t chan);

// USB 主机是否在位(SOF 检测,驱动无关的寄存器读):有线供电 = 不自动息屏
// 的门禁;主机在场即真(含已连但未 ping 握手)。
bool mode_wired(void);

#ifdef __cplusplus
}
#endif
