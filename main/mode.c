// main/mode.c —— 射频模式与链路抽象实现(见 mode.h)。
// 设计要点:
//   - 双栈常驻、单射频活:boot 两栈都初始化(WiFi 只建不 start),按 NVS 模式只启当前射频。
//   - 切换原子性:NVS 写失败即中止(射频不动,下次 boot 仍按旧模式),不会出现
//     磁盘模式与运行模式不一致。
//   - 切换时序:断连事件先投(app_task 排空循环续跑时状态机收束),再动射频。
//   - 省电:WiFi 模式 esp_bt_controller_disable() 彻底关蓝牙;BLE 模式 esp_wifi_stop()。
#include "mode.h"
#include "app_events.h"
#include "app_types.h"
#include "audio_streamer.h"
#include "ble_audio.h"
#include "mdns_resolver.h"
#include "nvs_settings.h"
#include "usb_link.h"
#include "wifi_app.h"
#include "ws_client.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_restart.h"

static const char *TAG = "mode";

static app_mode_t s_mode = APP_MODE_BLE;

const char *mode_name(app_mode_t m)
{
    if (m == APP_MODE_WIFI) return "WiFi";
    if (m == APP_MODE_USB) return "USB";
    return "BLE";
}

// ---- 链路抽象 ----

bool mode_link_up(void)
{
    if (s_mode == APP_MODE_WIFI) return ws_client_connected();
    if (s_mode == APP_MODE_USB) return usb_link_session_active();
    return ble_audio_event_subscribed();
}

int mode_send_event_line(const char *line, size_t len)
{
    if (s_mode == APP_MODE_WIFI) {
        ws_client_send_text(line, len);   // 尽力而为,内部记日志
        return 0;
    }
    if (s_mode == APP_MODE_USB) return usb_link_send_event(line, len);
    return ble_audio_notify_event(line, len);
}

// ---- 音频发送函数(按模式注册给 audio_streamer)----
static int send_audio_ble(const uint8_t *frame, size_t len)
{
    return ble_audio_notify_audio(frame, len);
}

static int send_audio_wifi(const uint8_t *frame, size_t len)
{
    return (int)ws_client_send_bin_blocking(frame, len);
}

static int send_audio_usb(const uint8_t *frame, size_t len)
{
    return usb_link_send_audio(frame, len);
}

static void register_audio_sender(void)
{
    if (s_mode == APP_MODE_WIFI) {
        audio_streamer_set_sender(send_audio_wifi);
    } else if (s_mode == APP_MODE_USB) {
        audio_streamer_set_sender(send_audio_usb);
    } else {
        audio_streamer_set_sender(send_audio_ble);
    }
}

// ---- 模式状态机 ----

app_mode_t mode_get(void) { return s_mode; }

esp_err_t mode_init(void)
{
    uint8_t m = 0;
    nvs_settings_get_mode(&m);            // 失败按 BLE 兜底(缺省)
    s_mode = (m == 2) ? APP_MODE_USB : ((m == 1) ? APP_MODE_WIFI : APP_MODE_BLE);

    // WiFi/WS/mDNS 栈:双栈常驻、只建不 start(启动与否由模式决定);
    // USB 模式同样初始化(从 USB 切走时 wifi_app_start 依赖已建栈)
    esp_err_t e = wifi_app_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "wifi_app_init 失败: %s", esp_err_to_name(e));
    e = mdns_resolver_init();             // 依赖 wifi_app_init 建的 netif;失败仅警告
    if (e != ESP_OK) ESP_LOGW(TAG, "mdns_resolver_init 失败: %s", esp_err_to_name(e));
    e = ws_client_init();
    if (e != ESP_OK) ESP_LOGW(TAG, "ws_client_init 失败: %s", esp_err_to_name(e));

    // 按模式只启动当前射频;其余射频彻底关闭(省电)。USB 模式:蓝牙+WiFi 全关。
    if (s_mode == APP_MODE_WIFI) {
        e = wifi_app_start();
        if (e != ESP_OK) ESP_LOGE(TAG, "WiFi 启动失败: %s", esp_err_to_name(e));
        // NimBLE host 已随 ble_audio_init 常驻,controller 保持禁用(省电);
        // 切回 BLE 时 esp_bt_controller_enable → host re-sync → 广播自动恢复。
        esp_bt_controller_disable();
        ESP_LOGI(TAG, "模式: WiFi(蓝牙已关闭)");
    } else if (s_mode == APP_MODE_USB) {
        // 射频全关(USB 供电不耗电池):蓝牙 controller 禁用 + WiFi 不 start
        esp_bt_controller_disable();
        e = usb_link_init();              // 驱动 + 读任务 + 日志重定向(console 门禁跳过 REPL)
        if (e != ESP_OK) ESP_LOGE(TAG, "USB 链路初始化失败: %s", esp_err_to_name(e));
        ESP_LOGI(TAG, "模式: USB(蓝牙与 WiFi 已关闭)");
    } else {
        // BLE:controller 由 ble_audio_init 正常启动,WiFi 不 start(不耗射频)
        ESP_LOGI(TAG, "模式: BLE(WiFi 未启动)");
    }
    register_audio_sender();
    return ESP_OK;
}

esp_err_t mode_switch(app_mode_t target)
{
    if (target >= APP_MODE_COUNT || target == s_mode) return ESP_OK;   // 幂等

    // 1. 先投当前通道断连事件:排空循环续跑时状态机幂等收束(停流/回 READY/审批保持)
    app_event_t d = { .type = (s_mode == APP_MODE_WIFI)  ? APP_EV_WS_DISCONNECTED
                             : (s_mode == APP_MODE_USB)  ? APP_EV_USB_DISCONNECTED
                             : APP_EV_BLE_DISCONNECTED };
    app_event_post(&d);

    // 2. NVS 持久化:失败中止,射频不动(模式一致性优先)
    esp_err_t e = nvs_settings_set_mode((uint8_t)target);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "模式写入 NVS 失败(%s),切换中止", esp_err_to_name(e));
        return e;
    }

    // 3. 射频切换:先彻底关旧射频,再启新射频
    if (target == APP_MODE_WIFI) {
        if (s_mode == APP_MODE_USB) usb_link_reset_session();
        esp_bt_controller_disable();      // 省电:WiFi 模式下蓝牙完全关闭
        e = wifi_app_start();
        if (e != ESP_OK) ESP_LOGE(TAG, "WiFi 启动失败: %s", esp_err_to_name(e));
    } else if (target == APP_MODE_USB) {
        // 进入 USB 模式必须重启:REPL 读任务阻塞在驱动 ring 上,driver_uninstall
        // 不唤醒阻塞读者(悬空指针 UB,IDF v5.5 源码证实);重启后 console 门禁
        // 跳过 REPL,USB 数据通道独占串口。
        ESP_LOGI(TAG, "模式切换完成: %s(重启生效)", mode_name(s_mode));
        esp_restart();
        return ESP_OK;                    // 不可达(esp_restart 不返回)
    } else {
        if (s_mode == APP_MODE_USB) {
            usb_link_reset_session();
            // 恢复日志输出到 USB 串口(驱动保持安装,REPL 缺席,无冲突;
            // 重启可恢复控制台 —— 文档注明)
            usb_link_restore_log();
        }
        wifi_app_stop();
        ws_client_stop();                 // 确保 WS 断开事件(与步骤 1 幂等)
        e = esp_bt_controller_enable();   // re-sync → 广播自动恢复(真机必测)
        if (e != ESP_OK) ESP_LOGE(TAG, "蓝牙控制器启动失败: %s", esp_err_to_name(e));
    }

    s_mode = target;
    register_audio_sender();
    ESP_LOGI(TAG, "模式切换完成: %s", mode_name(s_mode));
    return ESP_OK;
}
