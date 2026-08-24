// main/main.c —— AI Passport 固件入口。
// 引导顺序:NVS → BSP(显示/按键/电池) → 事件队列 → 提示音/UI → Wi-Fi/WS → 音频流 → BLE HID → 控制台。
// 架构:按键/WS/BLE/音频回调只投递事件,app_task 唯一消费并驱动状态机 → 动作执行 → LVGL 渲染(锁内)。
#include "app_events.h"
#include "app_state.h"
#include "app_sound.h"
#include "app_ui.h"
#include "app_protocol.h"
#include "audio_streamer.h"
#include "ble_hid_keyboard.h"
#include "ble_provisioning.h"
#include "prov_protocol.h"
#include "console_cmds.h"
#include "mdns_resolver.h"
#include "nvs_settings.h"
#include "wifi_app.h"
#include "ws_client.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "main";

// ---- 动作执行:与协议/音频/BLE 的边界都在这里 ----
static app_state_t s_state;   // app_task 独占,无需锁

static void run_actions(const app_action_t *acts, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        const app_action_t *a = &acts[i];
        char buf[APP_PROTO_TX_CAP];
        size_t len = 0;

        switch (a->type) {
        case APP_ACT_NONE:
        case APP_ACT_UI_REFRESH:
        case APP_ACT_UI_SCREEN_OFF:
        case APP_ACT_UI_SCREEN_ON:
            break;                       // 渲染/背光由主循环末尾统一做(快照驱动)
        case APP_ACT_SEND_VOICE_START:
            len = app_protocol_voice_start(buf, sizeof(buf), s_state.workflow);
            ws_client_send_text(buf, len);
            break;
        case APP_ACT_SEND_VOICE_END:
            ws_client_send_voice_end();
            break;
        case APP_ACT_SEND_WORKFLOW_SWITCH:
            len = app_protocol_workflow_switch(buf, sizeof(buf),
                                               (app_workflow_t)a->u.workflow);
            ws_client_send_text(buf, len);
            break;
        case APP_ACT_SEND_AGENT_ACTION:
            len = app_protocol_agent_action(buf, sizeof(buf),
                                            a->u.agent_action.task_id,
                                            a->u.agent_action.decision);
            ws_client_send_text(buf, len);
            break;
        case APP_ACT_STREAM_START:
            audio_streamer_start();
            break;
        case APP_ACT_STREAM_STOP:
            audio_streamer_stop();
            break;
        case APP_ACT_PLAY_TONE:
            // START 音同步播完才开流(动作顺序保证),其余异步
            if (a->u.tone == APP_TONE_START) {
                app_sound_play_sync(APP_TONE_START);
            } else {
                app_sound_play((app_tone_t)a->u.tone);
            }
            break;
        case APP_ACT_INJECT_TEXT:
            if (a->u.inject.inject_mode == APP_INJECT_PASTE) {
                ble_hid_keyboard_paste();
            } else {
                ble_hid_keyboard_type(a->u.inject.text);
            }
            break;
        case APP_ACT_PROV_WIFI:
            // 配网凭据 → 存 NVS + set_config + 立即连接(wifi_app 内自包含)
            wifi_app_set_credentials(a->u.prov.ssid, a->u.prov.pass);
            break;
        case APP_ACT_WS_RETARGET:
            ws_client_retarget(a->u.ws_target.url);   // 运行时改目标,不写 NVS
            break;
        case APP_ACT_RESOLVE_SERVICE:
            mdns_resolver_request();                  // WS 断开/手动 → 重查(auto 模式)
            break;
        }
    }
}

// WiFi 断开 reason → 配网错误码(与 app_state 的 toast 映射一致)
static prov_error_t wifi_reason_to_prov(uint16_t reason)
{
    switch (reason) {
    case 201: return PROV_ERR_NO_AP;
    case 202: case 15: case 2: return PROV_ERR_AUTH_FAIL;
    case 203: return PROV_ERR_ASSOC_FAIL;
    default:  return PROV_ERR_OTHER;
    }
}

// ---- 按键回调(button 任务上下文):只投事件,零阻塞 ----
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    app_event_t e = { 0 };
    switch (ev) {
    case BSP_BTN_PRESS:   e.type = APP_EV_KEY_PRESS;   break;
    case BSP_BTN_RELEASE: e.type = APP_EV_KEY_RELEASE; break;
    case BSP_BTN_CLICK:   e.type = APP_EV_KEY_CLICK;   break;
    case BSP_BTN_DOUBLE:  e.type = APP_EV_KEY_DOUBLE;  break;
    case BSP_BTN_LONG:    e.type = APP_EV_KEY_LONG;    break;
    default:              return;
    }
    e.u.key.btn = (uint8_t)btn;
    app_event_post(&e);
}

// ---- 应用任务:事件 → 归约器 → 动作 → 渲染;100ms 心跳驱动超时/秒表 ----
static void app_task(void *arg)
{
    (void)arg;
    app_state_init(&s_state);
    // 启动注入:NVS 已有凭据则不显示"NO WIFI"横幅(配网后由事件流接管)
    s_state.wifi_configured = wifi_app_provisioned();
    QueueHandle_t q = app_events_queue();
    uint64_t next_tick = esp_timer_get_time() / 1000;
    uint64_t now_ms = next_tick;

    for (;;) {
        app_event_t ev;
        app_action_t acts[APP_ACT_MAX];
        bool got = false;

        // 排空当前积压事件(每次 reduce 立即执行其动作)
        while (xQueueReceive(q, &ev, 0) == pdTRUE) {
            got = true;
            now_ms = esp_timer_get_time() / 1000;
            // 配网结果编排:reduce 会清 provisioning,先快照会话标志(每 op 只报一次)
            const bool prov_active = s_state.provisioning;
            uint8_t n = 0;
            app_state_reduce(&s_state, &ev, now_ms, acts, &n);
            if (prov_active) {
                if (ev.type == APP_EV_WIFI_CONNECTED) {
                    ble_provisioning_notify_ok(wifi_app_ip());
                } else if (ev.type == APP_EV_WIFI_CONNECT_FAIL) {
                    ble_provisioning_notify_error(
                        wifi_reason_to_prov(ev.u.wifi_fail.reason), NULL);
                }
            }
            run_actions(acts, n);
        }

        // 心跳(与事件独立计时,保证息屏/超时/秒表不依赖事件流量)
        now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= next_tick) {
            app_event_t t = { .type = APP_EV_TICK };
            // 超时判断在 reduce 前求值(reduce 会清 deadline)
            const bool prov_timeout = s_state.provisioning &&
                                      now_ms >= s_state.prov_deadline_ms;
            uint8_t n = 0;
            app_state_reduce(&s_state, &t, now_ms, acts, &n);
            if (prov_timeout) {
                ble_provisioning_notify_error(PROV_ERR_TIMEOUT, NULL);
            }
            run_actions(acts, n);
            next_tick = now_ms + APP_TICK_MS;
        }

        // 渲染(唯一 LVGL 写者,锁内;电池值在此补真实值)
        if (bsp_lvgl_lock(100)) {
            app_ui_snapshot_t snap;
            app_state_snapshot(&s_state, now_ms, &snap);
            int soc = bsp_battery_soc();
            snap.battery_available = (soc >= 0);
            snap.battery_soc = (soc > 0) ? (uint8_t)soc : 0;
            app_ui_render(&snap, audio_streamer_peak());
            bsp_lvgl_unlock();
        }

        // 无事件时睡到下一个心跳,避免空转
        if (!got) {
            int64_t sleep = (int64_t)next_tick - (int64_t)(esp_timer_get_time() / 1000);
            if (sleep <= 0 || sleep > 200) sleep = APP_TICK_MS;
            vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep));
        }
    }
}

// ---- 控制台(USB-Serial-JTAG,避免 UART0 与背光引脚冲突) ----
static void console_init(void)
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.max_cmdline_length = 128;
    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    if (esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl) != ESP_OK) {
        ESP_LOGW(TAG, "控制台 REPL 初始化失败");
        return;
    }
    console_cmds_register();
    esp_console_start_repl(repl);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 固件启动");

    // 1. NVS(凭据/设置持久化)
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(e));
        return;
    }
    nvs_settings_init();

    // 2. BSP:总线 + 显示(失败则无法继续 —— UI 是唯一出口)
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,终止");
        return;
    }
    bsp_display_backlight(100);

    // 3. 事件队列(先于一切生产者)
    if (app_events_init() != ESP_OK) {
        ESP_LOGE(TAG, "事件队列初始化失败,终止");
        return;
    }

    // 4. 按键 / 提示音 / 电池(单项失败不阻塞)
    esp_err_t btn_ok = bsp_button_init(on_key, NULL);
    if (btn_ok != ESP_OK) ESP_LOGW(TAG, "按键初始化失败,PTT 不可用");
    esp_err_t snd_ok = app_sound_init();
    if (snd_ok != ESP_OK) ESP_LOGW(TAG, "提示音初始化失败");
    esp_err_t batt_ok = bsp_battery_init();
    if (batt_ok != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败(UI 显示 --)");

    // 5. UI(锁内建全部页面)
    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bsp_lvgl_unlock();
    }

    // 6. 网络:Wi-Fi(GOT_IP 后自动起 WS)→ WS 客户端
    wifi_app_init();
    if (mdns_resolver_init() != ESP_OK) ESP_LOGW(TAG, "mDNS 解析器初始化失败");
    if (ws_client_init() != ESP_OK) ESP_LOGW(TAG, "WS 客户端初始化失败");

    // 7. 音频流式管线(双 worker 空闲等待)
    if (audio_streamer_init() != ESP_OK) ESP_LOGW(TAG, "音频流管线初始化失败");

    // 8. BLE HID 键盘(外设广播,host sync 后自动开广播)
    if (ble_hid_keyboard_init() != ESP_OK) ESP_LOGW(TAG, "BLE HID 初始化失败");

    // 9. 控制台与 app 任务(栈 5120:快照 ~250B + 动作 ~544B + TX 512B + 同步音 512B + LVGL 调用深度)
    console_init();
    xTaskCreate(app_task, "app_task", 5120, NULL, 4, NULL);

    ESP_LOGI(TAG, "就绪:按键=%s 提示音=%s 电量=%s",
             btn_ok == ESP_OK ? "ok" : "fail",
             snd_ok == ESP_OK ? "ok" : "fail",
             batt_ok == ESP_OK ? "ok" : "fail");
}
