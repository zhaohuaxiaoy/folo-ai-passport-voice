// main/usb_link.h —— USB 有线通道驱动层接口(第三通道)。
// 实现见 usb_link.c(P3),本头文件在 P2 先行定义接口供 mode.c/console 引用。
// 职责:
//   1. USB-Serial-JTAG 驱动(4096/4096 缓冲)+ 读任务:字节流 → 帧协议
//      (usb_link_framing.h)逐字节解析分发;
//   2. 会话状态:收到 PC 握手 ping → up;拔线(is_connected 翻转,500ms 轮询)→ down;
//   3. 发送:EVENT 行 / AUDIO 帧组帧写 USB(整帧入驱动 ring,部分写入 = 丢帧);
//   4. 日志隔离:esp_log 输出重定向到 RAM 环(经 SYS "log" 取回),不混入数据帧。
// 仅 boot USB 模式初始化;进入 USB 模式 = NVS 写入 + esp_restart(REPL 阻塞读删除
// 是 UB,见 design.md),故驱动安装与 REPL 互斥、无二次安装冲突。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化(仅 boot USB 模式调用,须在 console_init 之前——console 门禁跳过 REPL):
// 安装驱动、drain 陈旧字节、日志重定向到 RAM 环、建读任务。
esp_err_t usb_link_init(void);

// 会话是否活跃(收到 ping 且当前模式为 USB)。link_up 的 USB 侧充分条件。
bool usb_link_session_active(void);

// 事件行上行(组 EVENT 帧,≤ APP_PROTO_TX_CAP+1)。返回 0 = 已写入 USB。
int usb_link_send_event(const char *line, size_t len);

// 音频帧上行(组 AUDIO 帧,3200B)。返回 0 = 整帧写入;失败 -1(丢帧计数路径)。
int usb_link_send_audio(const uint8_t *frame, size_t len);

// 会话复位(离开 USB 模式时调用:会话 down,后续发送全部拒绝)。
void usb_link_reset_session(void);

// 恢复 esp_log 输出到 USB 串口(离开 USB 模式时调用;驱动保持安装无冲突)。
void usb_link_restore_log(void);

#ifdef __cplusplus
}
#endif
