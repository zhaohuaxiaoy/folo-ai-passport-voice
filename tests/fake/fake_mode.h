// tests/fake/fake_mode.h —— mode.h 宿主测试桩注入接口。
// 默认 mode=BLE、非切换窗口(与现有 BLE 事件用例兼容)。
#pragma once
#include "mode.h"
#include <stdbool.h>

void fake_mode_set(app_mode_t m);
void fake_mode_set_switching(bool v);
