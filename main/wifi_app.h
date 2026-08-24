// main/wifi_app.h —— Wi-Fi STA 生命周期(双模式:init 建栈、start/stop 由 mode 模块驱动)。
// WS 客户端在其事件里启停;GOT_IP 后触发 mDNS 解析。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_app_init(void);              // 建 netif/esp_wifi/事件注册(不 start)
esp_err_t wifi_app_start(void);             // 应用 NVS 凭据并启动连接(WiFi 模式进入时)
esp_err_t wifi_app_stop(void);              // 完全停止(BLE 模式进入时,省电)
esp_err_t wifi_app_set_credentials(const char *ssid, const char *pass); // 保存并重连
bool wifi_app_connected(void);
// 返回当前 IP 字符串(未连接时为空串)
const char *wifi_app_ip(void);
bool wifi_app_provisioned(void);            // NVS 是否已有凭据(未配网横幅依据)

#ifdef __cplusplus
}
#endif
