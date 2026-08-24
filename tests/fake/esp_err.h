// tests/fake/esp_err.h —— ESP-IDF 错误码的最小宿主替身(仅 host 测试用)。
// 注意:此 fake 头只提供被测代码用到的符号——被测代码若调用了 ESP-IDF 中
// 不存在的 API,编译即失败(这正是 test_audio_streamer 存在的意义)。
#pragma once

typedef int esp_err_t;

#define ESP_OK   0
#define ESP_FAIL (-1)

const char *esp_err_to_name(esp_err_t code);
