// tests/fake/fake_mode.h —— mode.h 宿主测试桩注入接口。
// 双通道常开架构:app_state 只依赖 mode_wired()(息屏门禁)与 mode_channel_up()
// (断链收束时查另一通道)。默认:无线(mode_wired=false)、双通道均断。
#pragma once
#include "mode.h"
#include <stdbool.h>

void fake_mode_set_wired(bool v);
void fake_mode_set_channel_up(uint8_t chan, bool up);
