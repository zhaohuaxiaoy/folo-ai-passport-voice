// tests/fake/fake_mode.c —— mode.h 宿主测试桩(可注入,见 fake_mode.h)。
#include "fake_mode.h"

static bool s_fake_wired = false;
static bool s_fake_ble_up = false;
static bool s_fake_usb_up = false;

void fake_mode_set_wired(bool v) { s_fake_wired = v; }
void fake_mode_set_channel_up(uint8_t chan, bool up)
{
    if (chan == APP_CHAN_USB) s_fake_usb_up = up;
    else s_fake_ble_up = up;
}

bool mode_wired(void) { return s_fake_wired; }

bool mode_channel_up(uint8_t chan)
{
    return (chan == APP_CHAN_USB) ? s_fake_usb_up : s_fake_ble_up;
}

bool mode_link_up(void)
{
    return s_fake_ble_up || s_fake_usb_up;
}
