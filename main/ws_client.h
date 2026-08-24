// main/ws_client.h —— esp_websocket_client 封装。
// 职责:连/断事件入队、文本/二进制发送、RX 行累积与协议解析。
// 注:voice.end 排空已统一在 main.c 执行器(与 BLE 通道同路径),此处不提供。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_client_init(void);   // 按 NVS 中 URL 创建客户端(不启动)
esp_err_t ws_client_start(void);  // 启动连接(仅在有 IP 后调用)
esp_err_t ws_client_stop(void);
bool ws_client_connected(void);

// 文本帧(带 \n 行分隔);未连接时静默丢弃并记日志。
void ws_client_send_text(const char *s, size_t len);

// 供音频 worker 调用的阻塞二进制发送。返回 ESP_OK 或超时错误。
esp_err_t ws_client_send_bin_blocking(const uint8_t *data, size_t len);

// 重新按新 URL 初始化并写 NVS(控制台 `ws set` 后用,置 static 模式)。
esp_err_t ws_client_reinit(const char *url);

// 运行时改目标 URL,不写 NVS(mDNS 发现结果用;auto 模式下被 WS_TARGET_FOUND 驱动)。
esp_err_t ws_client_retarget(const char *url);

#ifdef __cplusplus
}
#endif
