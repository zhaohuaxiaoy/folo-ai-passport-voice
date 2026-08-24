// main/ble_hid_keyboard.c —— BLE HID 键盘外设实现(NimBLE)。
// 标准 boot 键盘报告(8B:修饰键 + 保留 + 6 键码);macOS 无需配对即识别。
// 事件:连接/断开经 app_event_post 上报(BLE_CONNECTED/DISCONNECTED)。
#include "ble_hid_keyboard.h"
#include "ble_provisioning.h"
#include "app_events.h"
#include "app_types.h"
#include "hid_keymap.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/store_config.h"
#include <string.h>

static const char *TAG = "ble_hid";

#define DEVICE_NAME "AI Passport KB"
#define HID_SVC_UUID       0x1812
#define HID_INFO_UUID      0x2A4A
#define HID_REPORT_MAP_UUID 0x2A4B
#define HID_CTRL_PT_UUID   0x2A4C
#define HID_REPORT_UUID    0x2A4D
#define HID_PROTO_UUID     0x2A4E
#define HID_APPEARANCE_KEYBOARD 0x03C1

#define REPORT_LEN 8
#define KEY_DOWN_MS 30    // 键按住时长
#define KEY_GAP_MS  10    // 键间间隔(~40ms/字)
#define PASTE_HOLD_MS 50

// ---- HID 报告描述符(boot 键盘,63B,蓝牙 HID 官方样例) ----
static const uint8_t s_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
};

static const uint8_t s_hid_info[4] = { 0x11, 0x01, 0x00, 0x01 };  // bcdHID 1.11, 无国家码, RemoteWake
static uint8_t s_proto_mode = 0x01;                              // Report Protocol

// 打字队列(静态,深度 4:一次 transcript 至多两块 + 两块余量,再满即丢)
typedef struct {
    char  text[APP_TRANSCRIPT_MAX];
    uint8_t mode;   // APP_INJECT_TYPE / APP_INJECT_PASTE
} hid_job_t;

static StaticQueue_t s_job_queue_struct;
static uint8_t s_job_queue_storage[4 * sizeof(hid_job_t)];
static QueueHandle_t s_job_queue;

// ---- GATT 句柄与连接状态 ----
static uint16_t s_report_val_handle;
static volatile uint8_t s_conn_count;                 // 活跃连接数(≤CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
static volatile uint16_t s_hid_conn = 0xFFFF;         // 订阅了 HID CCCD 的连接(键盘可用即此)
static uint16_t s_conn_handle;
static uint8_t s_report_value[REPORT_LEN];   // 注册值:READ 返回,notify 前同步

static void send_report(const uint8_t report[REPORT_LEN]);
static void start_advertising(void);

// ---- GAP 事件 ----
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            if (s_conn_count == 0) {   // 首连:对外报"键盘已连接"
                app_event_t ev = { .type = APP_EV_BLE_CONNECTED };
                app_event_post(&ev);
            }
            s_conn_count++;
            ESP_LOGI(TAG, "已连接 (handle %u, 连接数 %u)", s_conn_handle, s_conn_count);
            // 双连接:还有空闲槽就继续广播(配网 app 可挤进第二条,AC6)
            if (s_conn_count < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) start_advertising();
        } else {
            ESP_LOGW(TAG, "连接失败,重开广播");
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开 (reason %d)", event->disconnect.reason);
        if (s_conn_count > 0) s_conn_count--;
        if (event->disconnect.conn_handle == s_hid_conn) s_hid_conn = 0xFFFF;
        if (s_conn_count == 0) {   // 全部断开:对外报"键盘已断开"
            app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
            app_event_post(&d);
        }
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // 仅 HID 报告特征的订阅算键盘可用(PROV_RESULT 订阅由 ble_provisioning 处理)
        if (event->subscribe.attr_handle == s_report_val_handle) {
            if (event->subscribe.cur_notify || event->subscribe.cur_indicate) {
                s_hid_conn = event->subscribe.conn_handle;
            } else if (event->subscribe.conn_handle == s_hid_conn) {
                s_hid_conn = 0xFFFF;
            }
            ESP_LOGI(TAG, "HID 订阅连接=%u", s_hid_conn);
        }
        break;

    default:
        break;
    }
    ble_provisioning_gap_event(event);   // 配网服务共享同一 GAP 回调链
    return 0;
}

// ---- 广播 ----
static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = { 0 };
    struct ble_hs_adv_fields adv = { 0 };

    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)DEVICE_NAME;
    adv.name_len = strlen(DEVICE_NAME);
    adv.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) { ESP_LOGE(TAG, "adv set_fields 失败 %d", rc); return; }

    // 外观放扫描响应
    struct ble_hs_adv_fields sr = { 0 };
    sr.appearance = HID_APPEARANCE_KEYBOARD;
    sr.appearance_is_present = 1;
    rc = ble_gap_adv_rsp_set_fields(&sr);
    if (rc != 0) { ESP_LOGE(TAG, "adv rsp set_fields 失败 %d", rc); return; }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(gap_event_handler, &adv_params);
    ESP_LOGI(TAG, "广播 %s (rc=%d)", DEVICE_NAME, rc);
}

// ---- GATT 服务定义 ----
static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    // 描述符(CCCD)访问会带 NULL chr —— 本服务无自定义 dsc 回调,防御即可
    if (!ctxt->chr) return BLE_ATT_ERR_UNLIKELY;
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (uuid == HID_INFO_UUID) {
            os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info));
            return 0;
        }
        if (uuid == HID_REPORT_MAP_UUID) {
            os_mbuf_append(ctxt->om, s_report_map, sizeof(s_report_map));
            return 0;
        }
        if (uuid == HID_PROTO_UUID) {
            os_mbuf_append(ctxt->om, &s_proto_mode, 1);
            return 0;
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && uuid == HID_PROTO_UUID) {
        s_proto_mode = ctxt->om->om_data[0];
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && uuid == HID_CTRL_PT_UUID) {
        return 0;   // 控制点:仅收不发,内容忽略(官方 HID 样例要求该特征存在)
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_defs[] = {
    {
        .type = BLE_GATT_SVC_DEF,
        .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(HID_SVC_UUID) }.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(HID_INFO_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(HID_REPORT_MAP_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(HID_REPORT_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_report_val_handle,
                .value = s_report_value,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(0x2902) }.u,
                      .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, },
                    { 0 },
                },
            },
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(HID_CTRL_PT_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(HID_PROTO_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    { 0 },
};

// ---- 发送 8B 键盘报告(Notifier;发往订阅 HID CCCD 的连接) ----
static void send_report(const uint8_t report[REPORT_LEN])
{
    if (s_hid_conn == 0xFFFF) {
        ESP_LOGW(TAG, "无 HID 键盘连接,丢弃键报");
        return;
    }
    memcpy(s_report_value, report, REPORT_LEN);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, REPORT_LEN);
    if (!om) { ESP_LOGW(TAG, "mbuf 分配失败"); return; }
    int rc = ble_gatts_notify_custom(s_hid_conn, s_report_val_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify 失败 %d", rc);
}

// ---- 打字 worker ----
static void hid_worker(void *arg)
{
    (void)arg;
    hid_job_t job;
    for (;;) {
        if (xQueueReceive(s_job_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (s_hid_conn == 0xFFFF) {
            if (job.mode == APP_INJECT_TYPE) {
                ESP_LOGW(TAG, "BLE 不可用,整段文本丢弃 (%d chars)", (int)strlen(job.text));
            } else {
                ESP_LOGW(TAG, "BLE 不可用,丢弃粘贴");
            }
            app_event_t drop = { .type = APP_EV_BLE_DROP };
            app_event_post(&drop);   // UI toast:注入被丢弃
            continue;
        }
        if (job.mode == APP_INJECT_PASTE) {
            // Cmd+V:按下 Cmd+V → 50ms → 同时松开
            uint8_t paste[REPORT_LEN] = { 0 };
            paste[0] = HID_MOD_LGUI;
            paste[2] = 0x19;   // 'V'
            send_report(paste);
            vTaskDelay(pdMS_TO_TICKS(PASTE_HOLD_MS));
            uint8_t up[REPORT_LEN] = { 0 };
            send_report(up);
            vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
            continue;
        }
        // 逐字键入;非 ASCII 跳过(CJK 走 paste)
        for (const char *p = job.text; *p; p++) {
            if (s_hid_conn == 0xFFFF) {
                // 中途断连:放弃整段,避免对不存在的连接空转 128 字符
                ESP_LOGW(TAG, "BLE 中途断开,中止键入");
                app_event_t drop = { .type = APP_EV_BLE_DROP };
                app_event_post(&drop);
                break;
            }
            hid_key_t k;
            if (!hid_keymap_lookup(*p, &k)) continue;
            uint8_t report[REPORT_LEN] = { 0 };
            report[0] = k.shift ? HID_MOD_LSHIFT : 0;
            report[2] = k.keycode;
            send_report(report);
            vTaskDelay(pdMS_TO_TICKS(KEY_DOWN_MS));
            uint8_t up[REPORT_LEN] = { 0 };
            send_report(up);
            vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
        }
    }
}

// ---- 主机同步后开广播 ----
static void on_sync(void)
{
    start_advertising();
}

// ---- NimBLE 主机任务 ----
static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_hid_keyboard_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_job_queue = xQueueCreateStatic(4, sizeof(hid_job_t),
                                     s_job_queue_storage, &s_job_queue_struct);

    ble_hs_cfg.sync_cb = on_sync;
    // Just Works 无输入输出配对(macOS 主动发起);CONFIG_BT_NIMBLE_SM_SC=y 只允许 SC 配对
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_store_config_init();   // 配合 CONFIG_BT_NIMBLE_NVS_PERSIST=y 持久化配对(bond)
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    int rc = ble_gatts_count_cfg(gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg 失败 %d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs 失败 %d", rc); return ESP_FAIL; }
    rc = ble_provisioning_init();   // 注册配网服务(0xA1B0),统一 gatts_start
    if (rc != 0) { ESP_LOGE(TAG, "配网服务注册失败 %d", rc); return ESP_FAIL; }

    rc = ble_gatts_start();
    if (rc != 0) { ESP_LOGE(TAG, "gatts_start 失败 %d", rc); return ESP_FAIL; }

    uint8_t addr_type;
    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "id_infer_auto 失败 %d", rc); return ESP_FAIL; }
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr 失败 %d", rc); return ESP_FAIL; }

    xTaskCreate(hid_worker, "hid_task", 4096, NULL, 5, NULL);
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "HID 键盘外设就绪,等待 host sync 后广播");
    return ESP_OK;
}

void ble_hid_keyboard_type(const char *text)
{
    hid_job_t job = { .mode = APP_INJECT_TYPE };
    if (!text) return;
    strlcpy(job.text, text, sizeof(job.text));
    if (xQueueSend(s_job_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "打字队列满,丢弃");
    }
}

void ble_hid_keyboard_paste(void)
{
    hid_job_t job = { .mode = APP_INJECT_PASTE };
    if (xQueueSend(s_job_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "打字队列满,丢弃粘贴");
    }
}

bool ble_hid_keyboard_connected(void) { return s_hid_conn != 0xFFFF; }
bool ble_hid_keyboard_ready(void) { return s_hid_conn != 0xFFFF; }   // 连接即订阅(macOS HID 自动订阅)
size_t ble_hid_keyboard_queue_len(void)
{
    return s_job_queue ? (size_t)uxQueueMessagesWaiting(s_job_queue) : 0;
}
