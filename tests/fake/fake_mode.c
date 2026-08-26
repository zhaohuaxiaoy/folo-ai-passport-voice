// tests/fake/fake_mode.c —— mode.h 宿主测试桩(可注入,见 fake_mode.h)。
#include "fake_mode.h"

static app_mode_t s_fake_mode = APP_MODE_BLE;
static bool s_fake_switching = false;

void fake_mode_set(app_mode_t m) { s_fake_mode = m; }
void fake_mode_set_switching(bool v) { s_fake_switching = v; }

app_mode_t mode_get(void) { return s_fake_mode; }
bool mode_switching(void) { return s_fake_switching; }
