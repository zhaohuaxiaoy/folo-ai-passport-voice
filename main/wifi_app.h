// main/wifi_app.h —— Wi-Fi STA 初始化与生命周期(WS 客户端在其事件里启停)。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_app_init(void);              // 读 NVS 凭据并开始连接
esp_err_t wifi_app_set_credentials(const char *ssid, const char *pass); // 保存并重连
bool wifi_app_connected(void);
// 返回当前 IP 字符串(未连接时为空串)
const char *wifi_app_ip(void);

#ifdef __cplusplus
}
#endif
