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
#include "mdns_resolver.h"
#include "mode.h"
#include "nvs_settings.h"
#include "time_sync.h"
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
static uint32_t s_last_drop_count = 0;   // 最近一次 STREAM_STOP 的会话丢帧数(voice.end 后 status 帧)
static bool s_ui_screen_on = true;       // 上次渲染时的屏幕状态(初始亮,与 app_ui 内 s_last_screen_on 一致)
static int s_batt_soc = -1;              // 电量缓存(至多 1s 读一次真实 I2C,见渲染路径)
static uint64_t s_batt_last_ms = (uint64_t)-1000;   // 上次电量读取时刻(负初值:首帧立即读)
static uint64_t s_last_render_ms = 0;    // 上次渲染时刻(S1 降频:非计时状态 ≥1s 兜底;初值 0 → 首帧立即渲染)

// 事件排空批上限:每 16 条让出 2ms,防洪峰饿死低优先级投递方(F2)
#define APP_EVENT_BATCH_MAX 16

// 经当前链路通道上行一行(序列化已完成,含 '\n')。异步:BLE 走 event_worker 串行
// 发送,WiFi 走 WS 文本帧;失败由通道层计数/记日志,此处仅警告。
static void send_event_line(char *buf, size_t len)
{
    if (len == 0) return;
    if (mode_send_event_line(buf, len) != 0) {
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
        case APP_ACT_TIME_SET:
            // 校时落地:写系统时间 + 置校时标志(app_task 上下文,单写点)
            time_sync_set_epoch(a->u.time_set.epoch);
            break;
        case APP_ACT_WS_RETARGET:
            // mDNS 发现新目标:运行时改 WS URL(不写 NVS,auto 模式语义)
            if (ws_client_retarget(a->u.ws_target.url) != ESP_OK) {
                ESP_LOGW(TAG, "WS retarget 失败: %s", a->u.ws_target.url);
            }
            break;
        case APP_ACT_RESOLVE_SERVICE:
            // WS 断开后触发 mDNS 重查(节流/退避在 mdns_resolver 内部)
            mdns_resolver_request();
            break;
        case APP_ACT_PLAY_TONE:
            // S3:START 改异步播放,开流由 sound_worker 播完后的 TONE_DONE 事件驱动
            // (app_state 归约,分时语义保持:滴声先于采集)。app_task 不再阻塞 80ms。
            // 入队失败(队列满,罕见)兜底:无滴声,立即开流,防 PTT 卡死无声。
            if (a->u.tone == APP_TONE_START) {
                if (!app_sound_play(APP_TONE_START)) {
                    s_state.stream_started = true;   // 同任务上下文,与 reduce 一致
                    audio_streamer_start();
                }
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

        // 阻塞等首个事件,超时 = 距下一心跳(性能轮):按键/转写下行/TONE_DONE/
        // 断连事件由生产者直接唤醒,延迟从最坏 ~100ms(原 vTaskDelay 睡到下一
        // 心跳)降至 ~1ms;心跳 cadence 由阻塞超时兜底,与事件流量独立(息屏/
        // 超时/秒表节奏不变)。关键边界:心跳已到期(sleep<=0)时非阻塞探针
        // 只探不睡 —— 先走下方 TICK 归约再睡,绝不把已到期的心跳再拖 100ms。
        // 排空批量/让出语义在下方循环原样保留。
        int64_t sleep = (int64_t)next_tick - (int64_t)(esp_timer_get_time() / 1000);
        BaseType_t have;
        if (sleep <= 0) {
            have = xQueueReceive(q, &ev, 0);
        } else {
            if (sleep > 200) sleep = APP_TICK_MS;   // esp_timer 回绕兜底(原语义)
            have = xQueueReceive(q, &ev, pdMS_TO_TICKS((uint32_t)sleep));
        }

        // 排空当前积压事件(每次 reduce 立即执行其动作;首事件已取出)。
        // 批上限:持续事件洪峰下(如按住按键/流控风暴)无界排空会饿死
        // 低优先级投递方(event_worker/sound_worker)——每 16 条让出 2ms
        // 给它们调度窗口,事件吞吐不变(第 7 轮 F2)。
        int n_batch = 0;
        while (have == pdTRUE) {
            got = true;
            now_ms = esp_timer_get_time() / 1000;
            if (ev.type == APP_EV_MODE_SWITCH) {
                // 射频模式切换不进归约器:先投的断连事件会由本循环继续消费,
                // 状态机幂等收束后再切射频(避免切换期间渲染/按键被冻结过久)。
                mode_switch((app_mode_t)ev.u.mode_switch.target);
                have = xQueueReceive(q, &ev, 0);
                continue;
            }
            uint8_t n = 0;
            app_state_reduce(&s_state, &ev, now_ms, acts, &n);
            run_actions(acts, n);
            if (++n_batch >= APP_EVENT_BATCH_MAX) {
                vTaskDelay(pdMS_TO_TICKS(2));
                n_batch = 0;
            }
            have = xQueueReceive(q, &ev, 0);
        }

        // 心跳(与事件独立计时,保证息屏/超时/秒表不依赖事件流量)
        bool tick_acted = false;
        now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= next_tick) {
            app_event_t t = { .type = APP_EV_TICK };
            uint8_t n = 0;
            app_state_reduce(&s_state, &t, now_ms, acts, &n);
            run_actions(acts, n);
            tick_acted = (n > 0);      // TICK 产生动作(toast 过期/超时/息屏) → 需渲染
            next_tick = now_ms + APP_TICK_MS;
        }

        // 渲染判定(S1 降频):有事件/动作、LISTENING(计时+音量条持续变化)、
        // 或距上次渲染 ≥1s(顶栏/电量兜底)才渲染。静止状态从每 100ms 降到 ≤1 次/s,
        // 省下 LVGL 锁 + 10+ 次 label set_text 重排/重绘。快照仅一次,判定与渲染共用。
        app_ui_snapshot_t snap;
        app_state_snapshot(&s_state, now_ms, &snap);
        bool need_render = got || tick_acted
                        || snap.state == APP_ST_LISTENING
                        || (now_ms - s_last_render_ms >= 1000);
        if (need_render) {
            // 电量 I2C 读取移出 LVGL 锁(总线事务最长 100ms,不占锁;第 7 轮 F2)。
            // 息屏跳过语义保持:息屏期间不读 I2C、不渲染。读数至多 1s 一次,缓存复用(#9)。
            if (snap.screen_on || s_ui_screen_on) {
                if (now_ms - s_batt_last_ms >= 1000) {
                    s_batt_soc = bsp_battery_soc();
                    s_batt_last_ms = now_ms;
                }
            }
            // 渲染(唯一 LVGL 写者,锁内;电池值在锁外已补真实值)
            if (bsp_lvgl_lock(100)) {
                // 息屏跳过渲染路径:首次转息屏的那帧仍执行(背光 100→0 由它驱动),
                // 之后息屏期间不做快照/LVGL 任何工作(第 5 轮 #10)。
                if (snap.screen_on || s_ui_screen_on) {
                    snap.battery_available = (s_batt_soc >= 0);
                    snap.battery_soc = (s_batt_soc > 0) ? (uint8_t)s_batt_soc : 0;
                    app_ui_render(&snap, audio_streamer_peak());
                    s_ui_screen_on = snap.screen_on;
                }
                bsp_lvgl_unlock();
            }
            s_last_render_ms = now_ms;
        }
        // 睡眠已由循环顶部阻塞 xQueueReceive 承担(超时 = 距下一心跳):
        // 事件即到即醒,心跳到期由超时兜底 —— 无独立睡眠段(性能轮)。
    }
}

// ---- 控制台(USB-Serial-JTAG,避免 UART0 与背光引脚冲突) ----
// USB 模式不启动 REPL:USB 数据通道独占串口(驱动安装与 REPL 互斥);
// 控制台功能由 SYS 命令面经数据协议下行覆盖(relay `!<命令>`)。boot 顺序
// 第 8 步 mode_init 先于此步,可安全按模式门禁。
static void console_init(void)
{
    if (mode_get() == APP_MODE_USB) {
        ESP_LOGI(TAG, "USB 模式:跳过 REPL(控制台走 SYS 命令面)");
        console_cmds_register();   // 命令表仍需注册:SYS 帧走 console_cmds_run_line
        return;
    }
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

    // 8. 多模式(Windows 移植):WiFi/WS/mDNS 按 NVS 模式按需建栈并启动射频;
    //    BLE 冷启动不占 WiFi 栈,WiFi 模式关蓝牙,USB 模式射频保持(数据走 USB 线)。
    //    射频切换会短暂阻塞(≤ 数百 ms),boot 期无并发,安全。
    if (mode_init() != ESP_OK) ESP_LOGW(TAG, "模式初始化失败(按 NVS 兜底)");

    // 9. 控制台与 app 任务(栈 5120:快照 ~250B + 动作 ~544B + TX 512B + LVGL 调用深度;
    //    S3 后 START 音不再同步占栈,同步音 512B 余量转为保险)
    console_init();
    xTaskCreate(app_task, "app_task", 5120, NULL, 4, NULL);

    ESP_LOGI(TAG, "就绪:按键=%s 提示音=%s 电量=%s",
             btn_ok == ESP_OK ? "ok" : "fail",
             snd_ok == ESP_OK ? "ok" : "fail",
             batt_ok == ESP_OK ? "ok" : "fail");
}
