// main/main.c —— AI Passport 固件入口。
// 引导顺序:NVS → BSP(显示/按键/电池) → 事件队列 → 提示音/UI → BLE 音频服务 → 音频流 → 控制台。
// 架构:按键/BLE 回调只投递事件,app_task 唯一消费并驱动状态机 → 动作执行 → LVGL 渲染(锁内)。
// 音频/事件上行经 ble_audio 的 GATT notify(0xA2B0);下行(Mac 命令)经 CTRL 写回调投事件。
#include "app_events.h"
#include "app_state.h"
#include "app_sound.h"
#include "app_ui.h"
#include "app_protocol.h"
#include "audio_streamer.h"
#include "ble_audio.h"
#include "console_cmds.h"
#include "nvs_settings.h"
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
static uint32_t s_last_drop_count = 0;   // 最近一次 STREAM_STOP 的会话丢帧数(voice.end 后 status 帧)

// 经 EVENT 特征上行一行(序列化已完成,含 '\n');无订阅时由 ble_audio 计数丢弃。
static void send_event_line(char *buf, size_t len)
{
    if (len == 0) return;
    if (ble_audio_notify_event(buf, len) != 0) {
        ESP_LOGW(TAG, "EVENT 行发送失败,丢弃: %.*s", (int)(len > 64 ? 64 : len), buf);
    }
}

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
            send_event_line(buf, len);
            break;
        case APP_ACT_SEND_VOICE_END:
            // 帧序保证:等残留音频块先出环,再发 voice.end;随后补发 status 帧(会话丢帧对账)。
            // drain 之后采集已停(前面 STOP/CANCEL 已执行)且环已排空,丢帧计数稳定,
            // 此刻取走才是本会话完整计数(此前取会漏掉 worker 最后一次丢帧)。
            audio_streamer_drain(500);
            s_last_drop_count = audio_streamer_take_drops();
            len = app_protocol_voice_end(buf, sizeof(buf));
            send_event_line(buf, len);
            len = app_protocol_device_status(buf, sizeof(buf), s_last_drop_count);
            send_event_line(buf, len);
            break;
        case APP_ACT_SEND_WORKFLOW_SWITCH:
            len = app_protocol_workflow_switch(buf, sizeof(buf),
                                               (app_workflow_t)a->u.workflow);
            send_event_line(buf, len);
            break;
        case APP_ACT_SEND_AGENT_ACTION:
            len = app_protocol_agent_action(buf, sizeof(buf),
                                            a->u.agent_action.task_id,
                                            a->u.agent_action.decision);
            send_event_line(buf, len);
            break;
        case APP_ACT_STREAM_START:
            // 新会话:清零对账计数(上一次会话若断链无 voice.end,计数在此丢弃)
            s_last_drop_count = 0;
            audio_streamer_start();
            break;
        case APP_ACT_STREAM_STOP:
            // 正常结束:只停采集,残留帧由后续 SEND_VOICE_END 的 drain 排空并取计数
            audio_streamer_stop();
            break;
        case APP_ACT_STREAM_CANCEL:
            // 取消/断链:停采集 + 清残留 + 丢弃在途帧(残留不流入下一次会话)
            audio_streamer_cancel();
            break;
        case APP_ACT_PLAY_TONE:
            // START 音同步播完才开流(动作顺序保证),其余异步
            if (a->u.tone == APP_TONE_START) {
                app_sound_play_sync(APP_TONE_START);
            } else {
                app_sound_play((app_tone_t)a->u.tone);
            }
            break;
        }
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
            uint8_t n = 0;
            app_state_reduce(&s_state, &ev, now_ms, acts, &n);
            run_actions(acts, n);
        }

        // 心跳(与事件独立计时,保证息屏/超时/秒表不依赖事件流量)
        now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= next_tick) {
            app_event_t t = { .type = APP_EV_TICK };
            uint8_t n = 0;
            app_state_reduce(&s_state, &t, now_ms, acts, &n);
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

    // 1. NVS(设置持久化)
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

    // 6. 音频流式管线(双 worker 空闲等待)
    if (audio_streamer_init() != ESP_OK) ESP_LOGW(TAG, "音频流管线初始化失败");

    // 7. BLE 音频服务(NimBLE 外设 + GATT 0xA2B0,host sync 后自动开广播)
    if (ble_audio_init() != 0) ESP_LOGW(TAG, "BLE 音频服务初始化失败");

    // 8. 控制台与 app 任务(栈 5120:快照 ~250B + 动作 ~544B + TX 512B + 同步音 512B + LVGL 调用深度)
    console_init();
    xTaskCreate(app_task, "app_task", 5120, NULL, 4, NULL);

    ESP_LOGI(TAG, "就绪:按键=%s 提示音=%s 电量=%s",
             btn_ok == ESP_OK ? "ok" : "fail",
             snd_ok == ESP_OK ? "ok" : "fail",
             batt_ok == ESP_OK ? "ok" : "fail");
}
