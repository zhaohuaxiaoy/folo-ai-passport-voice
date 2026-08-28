// main/usb_link.h —— USB 有线通道驱动层接口(第三通道)。
// 实现见 usb_link.c(P3),本头文件在 P2 先行定义接口供 mode.c/console 引用。
// 职责:
//   1. USB-Serial-JTAG 驱动 + 读任务:字节流 → 帧协议(usb_link_framing.h)
//      逐字节解析分发;无主机时驱动空闲等待,不占数据面;
//   2. 待命自动接入:usb_link_auto_start 起一个低耗待命任务,主机(SOF)出现
//      即安装驱动 + 日志重定向 + 读任务 —— 插线即用,无需重启/切模式;
//   3. 会话状态:收到 PC 握手 ping → up;拔线(is_connected 翻转,500ms 轮询)→ down;
//   4. 发送:EVENT 行 / AUDIO 帧组帧写 USB(整帧入驱动 ring,部分写入 = 丢帧);
//   5. 日志隔离:esp_log 输出重定向到 RAM 环(经 SYS "log" 取回),不混入数据帧。
// 双通道常开(2026-08-28):驱动归本通道独占,REPL 与其互斥 → 生产版移除 REPL。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 待命自动接入(须在 console_init 之前调用):若主机已在(SOF)立即初始化;
// 否则 250ms 轮询,主机出现即初始化。内部自建低耗待命任务(2KB 栈),初始化
// 完成后自删。无线(电池)开机零 RAM 开销;插线中途插入无需重启。
void usb_link_auto_start(void);

// 日志环提前启用(app_main 早期调用,须在 usb_link_auto_start 之前):建 RAM
// 环 + 安装 esp_log 重定向。无线(电池)开机期间日志也进环 —— 问题在拔线
// 状态发生时,事后插线即可经 SYS "log" 取回现场(2026-08-28 排查:拔线期
// 假按键日志不进环,问题发生时日志全丢)。有主机时仍由本环承接,行为不变。
void usb_link_log_early(void);

// 初始化:安装驱动、drain 陈旧字节、日志重定向到 RAM 环、建读任务。
// 已初始化(驱动已安装)时幂等返回 ESP_OK。由 auto_start 内部调用。
esp_err_t usb_link_init(void);

// 会话是否活跃(收到 PC 握手 ping)。link_up 的 USB 侧充分条件。
bool usb_link_session_active(void);

// 事件行上行(组 EVENT 帧,≤ APP_PROTO_TX_CAP+1)。返回 0 = 已写入 USB。
int usb_link_send_event(const char *line, size_t len);

// 音频帧上行(组 AUDIO 帧,3200B)。返回 0 = 整帧写入;失败 -1(丢帧计数路径)。
int usb_link_send_audio(const uint8_t *frame, size_t len);

// 导出日志环(console `log` 命令:日志常进 RAM 环,经此取回)。
// 返回写入 buf 的字节数(≤ cap-1,保证 NUL 结尾)。未启用时环为空。
// 容量 16KB:拔线运行 1 小时+ 的事件/音频/按键日志量级(~100B/事件),
// 相对 4KB(约 4s 音频流即冲掉)足够覆盖一次完整问题现场。
#define USB_LOG_RING_CAP 16384
size_t usb_link_dump_log(char *buf, size_t cap);
// 游标式分块导出:从逻辑第 offset 字节开始复制 ≤cap 字节,返回实际复制数;
// offset ≥ 已缓冲字节数时返回 0。调用方可持小缓冲(≤512B)循环导出,
// 避免在栈受限任务(usb_read_task 2048B)上声明 USB_LOG_RING_CAP 级数组
// (审查 P0:cmd_log 4096B 栈数组必爆栈)。cap 建议 ≤512。
size_t usb_link_dump_log_at(char *buf, size_t cap, size_t offset);

#ifdef __cplusplus
}
#endif
