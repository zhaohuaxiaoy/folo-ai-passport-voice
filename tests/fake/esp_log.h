// tests/fake/esp_log.h —— ESP_LOG* 宏的宿主替身(打印到 stdout)。
// tag 是运行时变量,不能用相邻字符串字面量拼接,走 %s 格式化;
// fmt 必须是字面量参数(字符串内含逗号无碍,预处理器不拆字符串)。
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[D] %s: " fmt "\n", tag, ##__VA_ARGS__)
