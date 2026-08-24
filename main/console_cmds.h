// main/console_cmds.h —— esp_console 命令注册。
// wifi set/get/status、ws set/status、st(状态一览)、reboot、factory。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t console_cmds_register(void);

#ifdef __cplusplus
}
#endif
