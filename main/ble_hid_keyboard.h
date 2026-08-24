// main/ble_hid_keyboard.h —— BLE HID 键盘外设。
// NimBLE 外设:服务 0x1812(HID),Report 特征 8 字节 boot 键盘报告 + Notify。
// 广告名 "AI Passport KB",外观 Keyboard(0x03C1)。MVP 无配对。
// 注入:逐字键入(ASCII,~40ms/字)或 Cmd+V 粘贴(CJK 回退,Mac 端已设剪贴板)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动 NimBLE 主机、注册 HID 服务并开始广播。失败返回错误码。
esp_err_t ble_hid_keyboard_init(void);

// 逐字键入文本(异步排队,后台 hid_task 打字)。非 ASCII 字符跳过。
// 无连接/未订阅时任务丢弃该文本并记日志。
void ble_hid_keyboard_type(const char *text);

// Cmd+V 粘贴(异步排队)。调用方(Mac)须已把内容放入剪贴板。
void ble_hid_keyboard_paste(void);

// 有活跃连接 / 连接且已订阅 Notify(可注入)。
bool ble_hid_keyboard_connected(void);
bool ble_hid_keyboard_ready(void);

// 控制台 `st` 用:当前打字队列长度。
size_t ble_hid_keyboard_queue_len(void);

#ifdef __cplusplus
}
#endif
