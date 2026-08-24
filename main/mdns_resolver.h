// main/mdns_resolver.h —— mDNS 解析器(设备侧只做 resolver,发布在 Mac Companion)。
// 触发:GOT_IP / WS 断开 / 控制台 `mdns resolve`;结果不同 → APP_EV_WS_TARGET_FOUND。
// 只改运行时 URL 不写 NVS;static 模式(ws_mode)下请求被忽略。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// mdns 初始化 + worker 任务;须在 Wi-Fi/netif 初始化之后、应用启动早期调用。
esp_err_t mdns_resolver_init(void);

// 非阻塞请求一次解析(节流/退避内部处理;static 模式静默忽略)。
void mdns_resolver_request(void);

#ifdef __cplusplus
}
#endif
