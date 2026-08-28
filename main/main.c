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
#include "mode.h"
#include "nvs_settings.h"
#include "usb_link.h"
#include "time_sync.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "lvgl.h"                       // lv_obj_invalidate:面板唤醒后全屏重绘
#include "esp_pm.h"                     // 空闲 light sleep + 录音会话 PM 锁
#include "driver/usb_serial_jtag.h"     // usb_serial_jtag_is_connected:USB 在位检测
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "main";

// ---- 动作执行:与协议/音频/BLE 的边界都在这里 ----
static app_state_t s_state;   // app_task 独占,无需锁

// ---- 电源管理:录音会话 PM 锁(2026-08-28 功耗优化 #1)----
// 空闲(无锁)时 esp_pm 变频 + light sleep;录音期间必须阻止睡眠并保持
// 高频:① I2S 采集由 APB 驱动,APB 降频/停摆会毁音频;② ADPCM 编码与
// BLE 发送要 CPU 余量。锁在 APP_ACT_STREAM_START 获取、STOP/CANCEL 释放
// (残留在环内的帧由 worker 在 80MHz 下仍可编码发送,best-effort)。
static esp_pm_lock_handle_t s_pm_cpu, s_pm_apb;
static esp_pm_lock_handle_t s_pm_no_ls;   // USB 主机在位:禁 light sleep
// ⚠ 单位陷阱:IDF 的 RISC-V port 里 portSTACK_TYPE = uint8_t(portmacro.h),
//    即 sizeof(StackType_t)==1,xTaskCreateStatic 的 depth 参数就是字节数。
//    这里曾写 [2048/4] = 512B 数组却按 2048B 登记 → FreeRTOS 把栈窗口开到
//    数组后 1536B,压住 s_state 与 app_events 的 s_queue_storage:初始上下文帧
//    落在 0x3fca15d0(第 3 号事件槽内),BLE 连接后事件投递把它覆写 →
//    本任务下次被调度恢复出全零寄存器、mret 到 PC=0 → Instruction access
//    fault 复位循环(2026-08-28 真机根因)。数组元素数 = 字节数,别再除 4。
// 2048B 时实测高水位仅剩 368B(用量 ~1.7KB:ESP_LOGI 的 192B 栈缓冲 + 日志环
// 路径),18% 余量太薄 → 3072B。
static StackType_t s_usb_presence_stack[3072];
static StaticTask_t s_usb_presence_tcb;

static void pm_session_lock(void)
{
    if (s_pm_cpu) esp_pm_lock_acquire(s_pm_cpu);
    if (s_pm_apb) esp_pm_lock_acquire(s_pm_apb);
}

static void pm_session_unlock(void)
{
    if (s_pm_apb) esp_pm_lock_release(s_pm_apb);
    if (s_pm_cpu) esp_pm_lock_release(s_pm_cpu);
}

// ---- 电源管理:活动门禁(2026-08-28 用户要求)----
// 用户语义:"什么都不动超过 1 分钟再进入省电,任何操作之后立即恢复正常功耗"。
// 活动锁 = CPU_FREQ_MAX + NO_LIGHT_SLEEP,等价于功耗优化前的常态(160MHz、
// 不睡);任意队列事件(按键/BLE 连断/USB 连断/下行转写/TONE_DONE)都算"操作",
// 立即取锁并刷新时刻;app_task 心跳检查,连续 PM_IDLE_GATE_MS 无事件才释放,
// 这时才交给 esp_pm 变频 + light sleep。开机也算一次活动(先给 1 分钟常态)。
// 录音会话另有 pm_session_lock(APB 也锁,I2S 不能降频),两者独立叠加。
#define PM_IDLE_GATE_MS 60000
static esp_pm_lock_handle_t s_pm_act_cpu, s_pm_act_ls;
static bool s_pm_act_held;
static uint64_t s_pm_act_ms;

// 哪些事件算"链路/主机活动"(与按键无关,天然无假事件问题)
static bool pm_ev_is_link(uint8_t t)
{
    switch (t) {
    case APP_EV_BLE_CONNECTED: case APP_EV_BLE_DISCONNECTED:
    case APP_EV_USB_CONNECTED: case APP_EV_USB_DISCONNECTED:
    case APP_EV_TRANSCRIPT:    case APP_EV_AGENT_STATUS:
    case APP_EV_APPROVAL_REQUEST: case APP_EV_TIME_SET:
    case APP_EV_AUDIO_ERROR:   case APP_EV_BLE_DROP:
        return true;
    default:
        return false;
    }
}

static void pm_activity_mark(uint64_t now_ms, uint8_t ev_type)
{
    // 诊断:静默 >5s 后又被唤活,记下是哪类事件把门禁踢回常态(排查"永不进入
    // 省电"时唯一有效的线索;按构造罕见,常量输出量)。
    if (s_pm_act_held && now_ms - s_pm_act_ms > 5000) {
        ESP_LOGI(TAG, "活动刷新: ev=%d 静默=%llums", (int)ev_type,
                 (unsigned long long)(now_ms - s_pm_act_ms));
    }
    s_pm_act_ms = now_ms;
    if (s_pm_act_held) return;
    if (s_pm_act_cpu) esp_pm_lock_acquire(s_pm_act_cpu);
    if (s_pm_act_ls)  esp_pm_lock_acquire(s_pm_act_ls);
    s_pm_act_held = true;
    ESP_LOGI(TAG, "活动:恢复常态功耗(不降频、不 light sleep)");
}

static void pm_idle_gate_check(uint64_t now_ms)
{
    if (!s_pm_act_held || now_ms - s_pm_act_ms < PM_IDLE_GATE_MS) return;
    if (s_pm_act_ls)  esp_pm_lock_release(s_pm_act_ls);
    if (s_pm_act_cpu) esp_pm_lock_release(s_pm_act_cpu);
    s_pm_act_held = false;
    ESP_LOGI(TAG, "空闲 %ds:进入省电(变频 + light sleep)", PM_IDLE_GATE_MS / 1000);
}

// 供 console `st` 展示门禁状态(诊断用;app_task 单写,读侧容忍撕裂)
bool app_pm_gate_state(uint32_t *idle_ms)
{
    if (idle_ms) {
        uint64_t now = esp_timer_get_time() / 1000;
        *idle_ms = (uint32_t)(now - s_pm_act_ms);
    }
    return s_pm_act_held;
}

// ---- USB 在位检测:插 USB 时禁 light sleep ----
// USB-Serial-JTAG 在 light sleep 期间不服务数据流:枚举仍在但串口 0 字节
// (2026-08-28 真机实测),有线通道/调试/日志全失联。1s 轮询检测主机在位
// (SOF 检测,同 usb_link.c 语义),在位时持 NO_LIGHT_SLEEP 锁;拔线(电池
// 运行)自动释放,恢复空闲省电。响应延迟 ≤1s,可接受。
static void usb_presence_task(void *arg)
{
    bool locked = false;
    for (;;) {
        bool conn = usb_serial_jtag_is_connected();
        if (conn != locked) {
            if (conn) {
                if (s_pm_no_ls) esp_pm_lock_acquire(s_pm_no_ls);
                ESP_LOGI(TAG, "USB 主机在位:禁用 light sleep");
            } else {
                if (s_pm_no_ls) esp_pm_lock_release(s_pm_no_ls);
                ESP_LOGI(TAG, "USB 断开:恢复 light sleep");
            }
            locked = conn;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
static uint32_t s_last_drop_count = 0;   // 最近一次 STREAM_STOP 的会话丢帧数(voice.end 后 status 帧)
static bool s_ui_screen_on = true;       // 上次渲染时的屏幕状态(初始亮,与 app_ui 内 s_last_screen_on 一致)
static bool s_ui_panel_on  = true;       // 上次渲染时的面板供电状态(init 已完成 DISPON)
static int s_batt_soc = -1;              // 电量缓存(至多 1s 读一次真实 I2C,见渲染路径)
static uint64_t s_batt_last_ms = (uint64_t)-1000;   // 上次电量读取时刻(负初值:首帧立即读)
static uint64_t s_last_render_ms = 0;    // 上次渲染时刻(S1 降频:非计时状态 ≥1s 兜底;初值 0 → 首帧立即渲染)

// 事件排空批上限:每 16 条让出 2ms,防洪峰饿死低优先级投递方(F2)
#define APP_EVENT_BATCH_MAX 16

// 经当前链路通道上行一行(序列化已完成,含 '\n')。会话路由:双通道常开下
// 所有上行跟 s_state.link_channel 走(最近连接/使用通道;活动会话期间通道
// 连接事件不夺路,见 app_state.c)。BLE 走 event_worker 串行发送,USB 走
// SYS 帧;失败由通道层计数/记日志,此处仅警告。
static void send_event_line(char *buf, size_t len)
{
    if (len == 0) return;
    if (mode_send_event_line(s_state.link_channel, buf, len) != 0) {
        ESP_LOGW(TAG, "EVENT 行发送失败,丢弃: %.*s", (int)(len > 64 ? 64 : len), buf);
    }
}

// 会话边界帧(voice.start/end):BLE 阻塞入队 ≤200ms 不丢,防 Mac 端会话状态悬挂
static void send_event_line_important(char *buf, size_t len)
{
    if (len == 0) return;
    if (mode_send_event_line_important(s_state.link_channel, buf, len, 200) != 0) {
        ESP_LOGW(TAG, "EVENT 行发送失败(重要),丢弃: %.*s", (int)(len > 64 ? 64 : len), buf);
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
        case APP_ACT_UI_PANEL_OFF:
        case APP_ACT_UI_PANEL_ON:
            break;                       // 渲染/背光/面板电源由主循环末尾统一做(快照驱动)
        case APP_ACT_SEND_VOICE_START:
            // 会话通道决定音频格式(voice.start 上报,App 据此分发解码)
            len = app_protocol_voice_start(buf, sizeof(buf),
                                           mode_audio_format(s_state.link_channel));
            send_event_line_important(buf, len);   // 会话边界帧:不丢(审查 P2)
            break;
        case APP_ACT_SEND_VOICE_END:
            // 帧序保证:等残留音频块先出环,再发 voice.end;随后补发 status 帧(会话丢帧对账)。
            // drain 之后采集已停(前面 STOP/CANCEL 已执行)且环已排空,丢帧计数稳定,
            // 此刻取走才是本会话完整计数(此前取会漏掉 worker 最后一次丢帧)。
            audio_streamer_drain(500);
            s_last_drop_count = audio_streamer_take_drops();
            len = app_protocol_voice_end(buf, sizeof(buf));
            send_event_line_important(buf, len);   // 会话边界帧:不丢(审查 P2)
            len = app_protocol_device_status(buf, sizeof(buf), s_last_drop_count);
            send_event_line(buf, len);             // 对账帧:尽力而为即可
            break;
        case APP_ACT_SEND_KEY_ACTION:
            len = app_protocol_key_action(buf, sizeof(buf),
                                          (app_key_action_t)a->u.key_action.action);
            // 诊断(2026-08-28 "双击清空还是不行"):进日志环,与 key: 的
            // btn/ev/mv 时间线对齐,直接分辨"状态机没判出双击"还是"判出来了
            // 但 Mac 侧没注入"——两侧各有一行日志,不必猜。
            ESP_LOGW("key", "key.action=%s -> %s",
                     a->u.key_action.action == APP_KEY_CLEAR ? "clear" : "enter",
                     len ? "sent" : "encode-fail");
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
            // 双通道常开:音频发送器按会话通道注册(USB=PCM/BLE=IMA ADPCM)。
            // 在开流前切换 —— 残留旧 token 帧由 worker 静默丢弃,不会错道。
            mode_select_audio_sender(s_state.link_channel);
            pm_session_lock();   // 录音会话:阻止 light sleep + 保持 CPU/APB 高频
            if (audio_streamer_start() != ESP_OK) {
                // 启动被拒(残留未排空/未就绪):录音没起来——投 AUDIO_ERROR
                // 让状态机回 READY + toast,UI 不得停在 LISTENING(REVIEW P2-C)。
                // 入队失败(罕见)仅日志:下次 PTT 可重试。
                pm_session_unlock();   // 流没起来,锁不滞留
                app_event_t ev = { .type = APP_EV_AUDIO_ERROR };
                app_event_post(&ev);
            }
            break;
        case APP_ACT_STREAM_STOP:
            // 正常结束:只停采集,残留帧由后续 SEND_VOICE_END 的 drain 排空并取计数
            audio_streamer_stop();
            pm_session_unlock();   // 残留帧 80MHz 下照常编码发送(best-effort)
            break;
        case APP_ACT_STREAM_CANCEL:
            // 取消/断链:停采集 + 清残留 + 丢弃在途帧(残留不流入下一次会话)
            audio_streamer_cancel();
            pm_session_unlock();
            break;
        case APP_ACT_TIME_SET:
            // 校时落地:写系统时间 + 置校时标志(app_task 上下文,单写点)
            time_sync_set_epoch(a->u.time_set.epoch);
            break;
        case APP_ACT_PLAY_TONE:
            // S3:START 改异步播放,开流由 sound_worker 播完后的 TONE_DONE 事件驱动
            // (app_state 归约,分时语义保持:滴声先于采集)。app_task 不再阻塞 80ms。
            // 入队失败(队列满,罕见)兜底:无滴声,立即开流,防 PTT 卡死无声。
            if (a->u.tone == APP_TONE_START) {
                if (!app_sound_play(APP_TONE_START)) {
                    s_state.stream_started = true;   // 同任务上下文,与 reduce 一致
                    mode_select_audio_sender(s_state.link_channel);   // 兜底路径同样按通道选发送器
                    pm_session_lock();   // 与 STREAM_START 同语义(静音兜底开流)
                    if (audio_streamer_start() != ESP_OK) {
                        // 兜底路径同样检查:开流失败 → 回 READY + toast(REVIEW P2-C)
                        pm_session_unlock();
                        app_event_t ev = { .type = APP_EV_AUDIO_ERROR };
                        app_event_post(&ev);
                    }
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
    case BSP_BTN_LONG_UP: e.type = APP_EV_KEY_LONG_UP; break;
    default:              return;
    }
    e.u.key.btn = (uint8_t)btn;
    // 回调时刻 ADC 读数:真实按压 3-5mV,假按(射频腐蚀)2890mV=松开电平。
    // 诊断日志与 app_state 真假判定共用同一读数(一次采样两个消费者)。
    int mv = bsp_button_read_mv();
    e.u.key.mv = (uint16_t)mv;
    // 诊断日志(2026-08-28 排查"无人按键却采集开始/停止"):WARN 级进日志环,
    // 带 ADC 电压与时间戳,还原假按形状(哪个键、mv 阈值穿越、时序)。
    ESP_LOGW("key", "btn=%d ev=%d mv=%d t=%llu", (int)btn, (int)ev, mv,
             (unsigned long long)esp_timer_get_time());
    // 重要投递(审查 P2-1):PRES/RELEASE/CLICK/DOUBLE 是 PTT 状态机驱动事件,
    // 按键风暴期队列满丢弃会卡死录音/发送。队列不满时零阻塞(唯一开销是满时
    // 等 ≤100ms);按键回调在 button 任务,可容忍。
    app_event_post_important(&e, 100);
}

// ---- 应用任务:事件 → 归约器 → 动作 → 渲染;100ms 心跳驱动超时/秒表 ----
static void app_task(void *arg)
{
    (void)arg;
    app_state_init(&s_state);
    QueueHandle_t q = app_events_queue();
    uint64_t next_tick = esp_timer_get_time() / 1000;
    uint64_t now_ms = next_tick;
    pm_activity_mark(now_ms, 255);   // 开机 = 活动,先跑 1 分钟常态功耗

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
            uint8_t n = 0;
            // 活动判定必须在归约之后:射频腐蚀 ADC 会凭空投出 KEY_PRESS
            // (2026-08-28 实测 31s 处一次),若"任意事件即活动",这些假事件
            // 会让门禁永远回不到省电(实测 51s 静默被两次噪声打断)。真操作的
            // 判据 = 归约产出了动作、或状态/屏幕/锁定态变了、或是链路事件;
            // 被 mv 门禁吞掉的假按三者皆不满足 → 不算活动。
            const uint8_t st_before = (uint8_t)s_state.state;
            const bool scr_before = s_state.screen_on, lock_before = s_state.locked;
            app_state_reduce(&s_state, &ev, now_ms, acts, &n);
            if (n > 0 || (uint8_t)s_state.state != st_before
                || s_state.screen_on != scr_before || s_state.locked != lock_before
                || pm_ev_is_link(ev.type)) {
                pm_activity_mark(now_ms, ev.type);
            }
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
            pm_idle_gate_check(now_ms);   // 满 1 分钟无事件才放手省电
        }

        // 渲染判定(S1 降频):有事件/动作、或距上次渲染 ≥1s(顶栏/电量/
        // LISTENING 计时兜底)才渲染。LISTENING 页是静态内容(麦克风图标 +
        // 1Hz 计时),不再强制每 100ms 重绘——省下 LVGL 锁 + label 重排/重绘。
        app_ui_snapshot_t snap;
        app_state_snapshot(&s_state, now_ms, &snap);
        bool need_render = got || tick_acted
                        || (now_ms - s_last_render_ms >= 1000);
        if (need_render) {
            // 面板电源(快照驱动):状态变化帧执行——60s 级 SLPIN 断电;
            // 唤醒(任意键)SLPOUT+120ms+DISPON 在此阻塞一次(可接受)。
            // 必须在渲染跳过判断之前:断电后渲染路径被跳过,此处不能跟着跳。
            if (snap.panel_on != s_ui_panel_on) {
                bsp_display_power(snap.panel_on);
                if (snap.panel_on) {
                    // 唤醒重绘:SLPIN 断电清空面板 RAM,DISPON 后 LVGL 只重绘
                    // 脏区域 —— 无变化时屏幕整片黑(背光亮但内容黑 = 不亮屏)。
                    // 全屏标脏,等 LVGL task 下一轮渲染。
                    lv_obj_invalidate(lv_screen_active());
                }
                s_ui_panel_on = snap.panel_on;
            }
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
                    app_ui_render(&snap);
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

// ---- 控制台(命令面) ----
// 双通道常开(2026-08-28):USB-Serial-JTAG 驱动由 usb_link 无条件占用(数据
// 通道插线即用,不再有模式/重启)。REPL 与该驱动互斥 → 生产版整体移除:
//   - 命令面 = SYS 帧经数据协议下行(relay `!<命令>`,见 usb_link handle_sys);
//   - 日志:有主机时进 RAM 环(`!log` 导出);无主机时走 vprintf(无人读取,丢弃);
//   - 附带收益:电池开机 REPL 空转饿死空闲任务 → TWDT 复位循环的根因(真机
//     取证 prev_rst=6/console_repl)随 REPL 一起消失。
// 如需恢复 REPL(仅开发):去掉 usb_link_auto_start 的调用后,此函数改回
// esp_console_new_repl_usb_serial_jtag + 主机门禁启动(旧实现见 git 历史)。
static void console_init(void)
{
    console_cmds_register();
}

void app_main(void)
{
    // 日志环最先启用:esp_log → RAM 环(16KB)。拔线(电池)开机期间也记录,
    // 问题在无线态发生时,插线后经 SYS "log" 取回现场。置于所有日志之前,
    // 使"固件启动"/"复位原因"也进环 —— 设备重启后即可判因(brownout/
    // 软件/看门狗),无须依赖串口在线(2026-08-28:插线瞬间设备重启,
    // RAM 环被清,拔线期现场丢失;复位原因必须能自证)。
    usb_link_log_early();

    // 日志级别不在代码里覆盖:交付版本用 sdkconfig 的 INFO。
    // (抓 HCI 包时临时把 CONFIG_BT_NIMBLE_LOG_LEVEL 调到 DEBUG,别留在代码里——
    //  NimBLE DEBUG 每个 HCI 字节一行,音频流期间会压满串口和 CPU。)
    ESP_LOGI(TAG, "AI Passport 固件启动");
    // 复位原因(1上电 2外部 4软件 5panic 6int_wdt 7task_wdt 8wdt 9深度睡眠
    // 10brownout 掉电 11sdio):用户反馈异常时第一行日志即可判断电源/软件。
    ESP_LOGI(TAG, "复位原因: %d", (int)esp_reset_reason());

    // 0. 电源管理:空闲自动 light sleep + CPU 变频(功耗优化 #1)。
    //    max 160MHz(录音/渲染峰值),min 80MHz(空闲省 CPU 电流);
    //    light_sleep 由会话 PM 锁(录音期间)阻止,见 pm_session_lock。
    //    按键 5ms 轮询与 BLE 连接事件会限制单次睡眠长度(~5-30ms 段),
    //    真机实测电流后再做轮询降频(#3)进一步拉长睡眠。
    const esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    ESP_LOGI(TAG, "PM: %dMHz→%dMHz, light_sleep=%d",
             pm_cfg.max_freq_mhz, pm_cfg.min_freq_mhz, pm_cfg.light_sleep_enable);
    esp_err_t e = esp_pm_configure(&pm_cfg);
    if (e == ESP_OK) {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ptt_cpu", &s_pm_cpu);
        esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "ptt_apb", &s_pm_apb);
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_ls", &s_pm_no_ls);
        // 活动门禁锁(见 pm_activity_mark):空闲 1 分钟前一直持有
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "act_cpu", &s_pm_act_cpu);
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "act_ls", &s_pm_act_ls);
    } else {
        ESP_LOGW(TAG, "esp_pm_configure 失败 %s(无 PM,照常运行)", esp_err_to_name(e));
    }
    if (e == ESP_OK && s_pm_no_ls) {
        TaskHandle_t usb_h = xTaskCreateStatic(usb_presence_task, "usb_presence",
                                               sizeof(s_usb_presence_stack), NULL, 1,
                                               s_usb_presence_stack,
                                               &s_usb_presence_tcb);
        if (!usb_h) ESP_LOGW(TAG, "usb_presence 任务创建失败(USB 在位时通道可能失联)");
    }

    // 1. NVS(设置持久化)
    e = nvs_flash_init();
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
    // 背光随显示初始化点亮(2026-08-28 曾尝试"首帧刷完再亮"消除底部黑条,
    // 回退:点亮延迟会使异常/无线启动路径下整屏全黑 —— 黑条可接受,全黑不可)。
    bsp_display_backlight(100);

    // 3. 事件队列(先于一切生产者)
    if (app_events_init() != ESP_OK) {
        ESP_LOGE(TAG, "事件队列初始化失败,终止");
        return;
    }

    // 4. 按键 / 提示音 / 电池(单项失败不阻塞)
    esp_err_t btn_ok = bsp_button_init(on_key, NULL);
    if (btn_ok != ESP_OK) ESP_LOGW(TAG, "按键初始化失败,PTT 不可用");
    // ⚠ 启动期按键门禁必须紧贴 bsp_button_init(2026-08-28 真机取证修正):
    //    button 任务随 bsp_button_init 启动即开始 tick,而后面的 UI/音频/
    //    BLE 初始化(实测 ~1s)期间 ADC 受外设/射频初始化腐蚀 → 假 UP 按下
    //    → 假 LONG(滴声)→ 假 LONG_UP。原实现把门禁放在 ble_audio_init 前
    //    (≈1.9s),而腐蚀在 1.06s 已发生,门禁形同虚设。此处紧随 button
    //    创建,3s 覆盖腐蚀期加安全余量;bsp_button.c 内 600ms 面板窗会被
    //    本设置覆盖(后者更长,语义一致)。
    // 注:以下两行是门禁的实体调用(2026-08-28 修正 —— 上一轮编辑误删调用
    // 只留注释,导致 SW 复位后启动假按未被拦:复位原因 4 时 1.24s 假 PRESS
    // → 1.47s 假 LONG → 2.02s 假 LONG_UP,而窗口本应覆盖到 3.78s)。
    extern void button_adc_set_ignore_until(int64_t until_us);
    button_adc_set_ignore_until(esp_timer_get_time() + 3 * 1000 * 1000);
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
    // 2026-08-28 无线 INT_WDT 二分结论:临时禁用 BLE 后无线仍重启 → 与 radio 无关。
    if (ble_audio_init() != 0) ESP_LOGW(TAG, "BLE 音频服务初始化失败");

    // 8. USB 有线通道待命(双通道常开,2026-08-28):BLE 广播已在第 7 步常驻;
    //    本步只起一个低耗待命任务,主机(SOF)出现即接入 —— 无线开机零 RAM
    //    开销,插线中途接入无需重启。旧"互斥模式 + NVS + 切换重启"已退役。
    usb_link_auto_start();

    // 9. app 任务(栈 5120:快照 ~250B + 动作 ~544B + TX 512B + LVGL 调用深度;
    //    S3 后 START 音不再同步占栈,同步音 512B 余量转为保险)
    // ⚠ 必须先于 console_init:按键/电量/UI 定时器在 console_init 期间就已在投递
    //    重要事件,消费者未起来 → 队列满 → 每条等满 100ms
    //    (真机日志 app_evt: 重要事件投递超时 ×3)。
    // ⚠ 静态栈:堆紧张时动态 xTaskCreate 栈分配会失败(且历史上无返回值检查)
    //    → 事件无人消费 + 全机无反应。静态栈 .bss 免疫。
    //    注意:栈参数单位是字节,且 IDF RISC-V 的 StackType_t 是 uint8_t(1B),
    //    故数组元素数 = 字节数(sizeof 写法自证,勿手写 /4,见 s_usb_presence_stack)。
    static StackType_t s_app_stack[5120 / sizeof(StackType_t)];
    static StaticTask_t s_app_tcb;
    if (!xTaskCreateStatic(app_task, "app_task", 5120, NULL, 4,
                           s_app_stack, &s_app_tcb)) {
        ESP_LOGE(TAG, "app_task 创建失败");
    }

    // 10. 控制台(REPL 走 USB-Serial-JTAG;失败只降级,不阻塞主流程)
    console_init();

    ESP_LOGI(TAG, "就绪:按键=%s 提示音=%s 电量=%s",
             btn_ok == ESP_OK ? "ok" : "fail",
             snd_ok == ESP_OK ? "ok" : "fail",
             batt_ok == ESP_OK ? "ok" : "fail");
}
