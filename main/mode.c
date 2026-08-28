// main/mode.c —— 链路通道抽象实现(见 mode.h)。
// 双通道常开(2026-08-28):BLE 广播由 ble_audio_init 无条件常驻,USB 数据
// 通道由 usb_link_auto_start 待命(主机出现即接入)—— 不再有互斥模式、NVS
// 存储、切换/重启。本文件只剩聚合与路由:
//   - link_up / channel_up:两通道的"通"状态聚合;
//   - send/format/sender:按会话通道(APP_CHAN_*)分发到 ble_audio / usb_link。
#include "mode.h"
#include "audio_streamer.h"
#include "ble_audio.h"
#include "usb_link.h"
#include "driver/usb_serial_jtag.h"  // usb_serial_jtag_is_connected:主机在位门禁
#include <string.h>

static bool chan_is_usb(uint8_t chan) { return chan == APP_CHAN_USB; }

// ---- 链路聚合 ----

bool mode_link_up(void)
{
    return ble_audio_event_subscribed() || usb_link_session_active();
}

bool mode_channel_up(uint8_t chan)
{
    if (chan_is_usb(chan)) return usb_link_session_active();
    return ble_audio_event_subscribed();
}

// ---- 上行路由 ----

int mode_send_event_line(uint8_t chan, const char *line, size_t len)
{
    if (chan_is_usb(chan)) return usb_link_send_event(line, len);
    return ble_audio_notify_event(line, len);
}

int mode_send_event_line_important(uint8_t chan, const char *line, size_t len,
                                   uint32_t timeout_ms)
{
    if (chan_is_usb(chan)) {
        // USB 写驱动阻塞式:本就等待完成,无队列可满,语义已"重要"。
        return usb_link_send_event(line, len);
    }
    return ble_audio_notify_event_blocking(line, len, timeout_ms);
}

// ---- 会话音频(格式随通道)----

const char *mode_audio_format(uint8_t chan)
{
    return chan_is_usb(chan) ? "pcm" : "ima_adpcm";
}

void mode_select_audio_sender(uint8_t chan)
{
    if (chan_is_usb(chan)) {
        audio_streamer_set_sender(usb_link_send_audio);
        audio_streamer_set_compressed(false);   // 有线带宽充裕:不压缩
    } else {
        audio_streamer_set_sender(ble_audio_notify_audio);
        audio_streamer_set_compressed(true);    // BLE:IMA ADPCM 4:1(8KB/s)
    }
}

// ---- 有线供电门禁 ----

bool mode_wired(void)
{
    return usb_serial_jtag_is_connected();
}
