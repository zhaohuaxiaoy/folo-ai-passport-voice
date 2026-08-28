// main/mode.c —— 射频模式与链路抽象实现(见 mode.h)。
// 设计要点:
//   - 双模式(BLE/USB)抽象:BLE 只启蓝牙;USB 模式射频保持(USB 供电,数据走
//     USB 线,无省电需求)。
//   - 切换原子性:NVS 写失败即中止(射频不动,下次 boot 仍按旧模式),不会出现
//     磁盘模式与运行模式不一致。
//   - 切换时序:断连事件先投(app_task 排空循环续跑时状态机收束),再动射频。
//   - 省电:BLE 模式 controller 常驻;切回 BLE 时按状态守卫重启(USB 模式保持)。
#include "mode.h"
#include "app_events.h"
#include "app_types.h"
#include "audio_streamer.h"
#include "ble_audio.h"
#include "nvs_settings.h"
#include "usb_link.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_system.h"  // IDF >= 5.5: esp_restart 声明移入 esp_system.h

static const char *TAG = "mode";

static app_mode_t s_mode = APP_MODE_BLE;

const char *mode_name(app_mode_t m)
{
    if (m == APP_MODE_USB) return "USB";
    return "BLE";
}

// ---- 链路抽象 ----

bool mode_link_up(void)
{
    if (s_mode == APP_MODE_USB) return usb_link_session_active();
    return ble_audio_event_subscribed();
}

int mode_send_event_line(const char *line, size_t len)
{
    if (s_mode == APP_MODE_USB) return usb_link_send_event(line, len);
    return ble_audio_notify_event(line, len);
}

int mode_send_event_line_important(const char *line, size_t len, uint32_t timeout_ms)
{
    if (s_mode == APP_MODE_BLE) {
        return ble_audio_notify_event_blocking(line, len, timeout_ms);
    }
    // USB 写驱动阻塞式:本就等待完成,无队列可满,语义已"重要"。
    return mode_send_event_line(line, len);
}

// ---- 音频发送函数(按模式注册给 audio_streamer)----
static int send_audio_ble(const uint8_t *frame, size_t len)
{
    return ble_audio_notify_audio(frame, len);
}

static int send_audio_usb(const uint8_t *frame, size_t len)
{
    return usb_link_send_audio(frame, len);
}

static void register_audio_sender(void)
{
    if (s_mode == APP_MODE_USB) {
        audio_streamer_set_sender(send_audio_usb);
        audio_streamer_set_compressed(false);   // 有线带宽充裕:不压缩
    } else {
        audio_streamer_set_sender(send_audio_ble);
        audio_streamer_set_compressed(true);    // BLE:IMA ADPCM 4:1(8KB/s)
    }
}

// 会话音频格式(voice.start 上报,App 据此分发):BLE 压缩,USB PCM。
// 取值必须与实际载荷一致 —— ima_adpcm = 804B block(4B 头 + 800B 4bit 数据),
// Mac 端 companion/adpcm.py 解回 16kHz PCM 后走既有 pcm ASR 路径。
const char *mode_audio_format(void)
{
    return s_mode == APP_MODE_BLE ? "ima_adpcm" : "pcm";
}

// ---- 模式状态机 ----

app_mode_t mode_get(void) { return s_mode; }

esp_err_t mode_init(void)
{
    uint8_t m = 0;
    nvs_settings_get_mode(&m);            // 失败按 BLE 兜底(缺省)
    // 存储语义(BLE=0/USB=2):旧 NVS 值 1(已废弃)越界 → BLE(默认通道)
    s_mode = (m == 2) ? APP_MODE_USB : APP_MODE_BLE;

    if (s_mode == APP_MODE_USB) {
        // 射频保持:蓝牙 controller 已由 ble_audio_init 启动(不 disable);
        // RAM 留给 usb_link(驱动 + 读任务 + 日志环)。
        esp_err_t e = usb_link_init();    // 驱动 + 读任务 + 日志重定向(console 门禁跳过 REPL)
        if (e != ESP_OK) ESP_LOGE(TAG, "USB 链路初始化失败: %s", esp_err_to_name(e));
        ESP_LOGI(TAG, "模式: USB(射频保持,数据走 USB)");
    } else {
        // BLE:controller 由 ble_audio_init 正常启动
        ESP_LOGI(TAG, "模式: BLE");
    }
    register_audio_sender();
    return ESP_OK;
}

// 切换窗口标志:mode_switch 前置投递"旧通道断连事件",而射频切换在本上下文
// 继续执行(≤数百 ms),app_task 消费该事件时 mode_get() 已是新模式 —— 链路
// 事件门禁(app_state)凭此放行窗口内投递的收束事件(审查 P1:USB 模式下
// BLE 连/断不再误掐当前通道音频流;模式切换本身的收束不受门禁影响)。
static volatile bool s_switching = false;

bool mode_switching(void) { return s_switching; }

static esp_err_t mode_switch_impl(app_mode_t target);   // 定义在下方(原函数体)

esp_err_t mode_switch(app_mode_t target)
{
    s_switching = true;
    esp_err_t rc = mode_switch_impl(target);
    s_switching = false;
    return rc;
}

static esp_err_t mode_switch_impl(app_mode_t target)
{
    if (target >= APP_MODE_COUNT || target == s_mode) return ESP_OK;   // 幂等
    // 入参映射(BLE=0 / USB=2 存储语义):值 1(已废弃)不接受
    if (target != APP_MODE_BLE && target != APP_MODE_USB) return ESP_ERR_INVALID_ARG;

    // 0. 切换前同步收束音频管线(停采集+排空残留):不依赖断连事件被 app_task
    //    消费的时序 —— sender 切换前音频已停,杜绝"旧会话帧经新 sender 流出"
    //    (审查 P2-2 时序窗口);cancel 幂等,状态机事件收束为双保险。
    audio_streamer_cancel();

    // 1. 先投当前通道断连事件:排空循环续跑时状态机幂等收束(停流/回 READY/审批保持)。
    //    重要投递:队列满等 app_task 消费(≤500ms)而非丢弃 —— 断连事件丢失则
    //    状态机不收束,录音/审批悬挂。超时 = 事件队列持续满(消费停滞,异常态):
    //    中止切换(此时 NVS 未写、射频未动,状态一致),用户可重试(审查 P2-2)。
    app_event_t d = { .type = (s_mode == APP_MODE_USB)  ? APP_EV_USB_DISCONNECTED
                             : APP_EV_BLE_DISCONNECTED };
    if (app_event_post_important(&d, 500) != ESP_OK) {
        ESP_LOGE(TAG, "断连事件投递超时(队列满),切换中止");
        return ESP_ERR_TIMEOUT;
    }

    // 2. NVS 持久化:失败中止,射频不动(模式一致性优先)
    esp_err_t e = nvs_settings_set_mode((uint8_t)target);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "模式写入 NVS 失败(%s),切换中止", esp_err_to_name(e));
        return e;
    }

    // 3. 射频切换:先彻底关旧射频,再启新射频
    if (target == APP_MODE_USB) {
        // 进入 USB 模式必须重启:REPL 读任务阻塞在驱动 ring 上,driver_uninstall
        // 不唤醒阻塞读者(悬空指针 UB,IDF v5.5 源码证实);重启后 console 门禁
        // 跳过 REPL,USB 数据通道独占串口。
        ESP_LOGI(TAG, "模式切换完成: %s(重启生效)", mode_name(s_mode));
        esp_restart();
        return ESP_OK;                    // 不可达(esp_restart 不返回)
    } else {
        if (s_mode == APP_MODE_USB) {
            usb_link_shutdown();          // 停读任务(自删)+ 会话 down
            // 恢复日志输出到 USB 串口(驱动保持安装,REPL 缺席,无冲突;
            // 重启可恢复控制台 —— 文档注明)
            usb_link_restore_log();
        }
        // USB 模式蓝牙保持开启(未 disable),重复 enable 会报 INVALID_STATE —— 按状态守卫
        if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
            e = esp_bt_controller_enable(ESP_BT_MODE_BLE);   // re-sync → 广播自动恢复(真机必测)
            if (e != ESP_OK) ESP_LOGE(TAG, "蓝牙控制器启动失败: %s", esp_err_to_name(e));
        }
    }

    s_mode = target;
    register_audio_sender();
    ESP_LOGI(TAG, "模式切换完成: %s", mode_name(s_mode));
    return ESP_OK;
}
