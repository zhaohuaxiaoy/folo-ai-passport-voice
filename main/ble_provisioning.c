// main/ble_provisioning.c —— BLE 配网服务实现。
// 服务 0xA1B0(0000A1B0-...-00805F9B34FB):
//   PROV_CMD(0xA1B1, WRITE|WRITE_ENC) 载荷 ≤512B JSON;畸形/未知命令/字段非法 → ATT 0x03,超长 → ATT 0x0D
//   PROV_RESULT(0xA1B2, READ|READ_ENC|NOTIFY + CCCD) ok/error 结果,永不回显密码
// 写回调零阻塞:校验 → APP_EV_PROV_CMD → app_event_post;绝不调 WiFi 驱动。
// 结果上报从 app_task 调用(跨任务 notify 先例:ble_hid_keyboard send_report)。
#include "ble_provisioning.h"
#include "app_events.h"
#include "app_types.h"
#include "prov_protocol.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include <string.h>

static const char *TAG = "ble_prov";

#define PROV_CMD_UUID      0xA1B1
#define PROV_RESULT_UUID   0xA1B2
#define PROV_RESULT_BUF    160

// 服务 128-bit:0000A1B0-0000-1000-8000-00805F9B34FB(BLE_UUID128_INIT 为小端字节序)
static const ble_uuid128_t prov_svc_uuid = BLE_UUID128_INIT(
    0xB0, 0xA1, 0x00, 0x00,   // 0000A1B0
    0x00, 0x00,               // -0000-
    0x00, 0x10,               // -1000-
    0x00, 0x80,               // -8000-
    0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB);  // -00805F9B34FB

// ---- 连接与订阅状态(host task 写,app_task 读;volatile 足够) ----
static uint16_t s_result_val_handle;
static volatile uint16_t s_prov_conn = 0xFFFF;   // 配网连接(写入者)
static volatile bool s_result_subscribed = false;
static char s_result_buf[PROV_RESULT_BUF];       // READ 缓存(尽力而为,竞态无害)
static const char s_result_idle[] = "{\"cmd\":\"wifi_set\",\"status\":\"idle\"}";

// ---- 写 PROV_CMD:长度/语法校验 + 投事件,零阻塞 ----
static int prov_write(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt)
{
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > PROV_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "PROV_CMD 超长 %u > %u", len, PROV_PAYLOAD_MAX);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;   // 0x0D
    }
    char payload[PROV_PAYLOAD_MAX + 1];
    if (os_mbuf_copydata(ctxt->om, 0, len, payload) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    payload[len] = '\0';

    prov_wifi_set_t w;
    prov_parse_result_t r = prov_protocol_parse(payload, len, &w);
    if (r != PROV_PARSE_OK) {
        ESP_LOGW(TAG, "PROV_CMD 拒绝(parse=%d)", (int)r);
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;      // 0x03:畸形/未知命令/字段非法
    }
    s_prov_conn = conn_handle;
    ESP_LOGI(TAG, "PROV_CMD 接受: ssid=%s", w.ssid);   // 日志只打 ssid,绝不打 pass
    app_event_t ev = { .type = APP_EV_PROV_CMD };
    strlcpy(ev.u.prov.ssid, w.ssid, sizeof(ev.u.prov.ssid));
    strlcpy(ev.u.prov.pass, w.pass, sizeof(ev.u.prov.pass));
    app_event_post(&ev);
    return 0;
}

static int prov_read(struct ble_gatt_access_ctxt *ctxt)
{
    os_mbuf_append(ctxt->om, s_result_buf, strlen(s_result_buf));
    return 0;
}

static int prov_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return prov_write(conn_handle, ctxt);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)  return prov_read(ctxt);
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def prov_gatt_defs[] = {
    {
        .type = BLE_GATT_SVC_DEF,
        .uuid = &prov_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(PROV_CMD_UUID) }.u,
                .access_cb = prov_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(PROV_RESULT_UUID) }.u,
                .access_cb = prov_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_result_val_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(0x2902) }.u,
                      .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    { 0 },
};

esp_err_t ble_provisioning_init(void)
{
    strlcpy(s_result_buf, s_result_idle, sizeof(s_result_buf));
    int rc = ble_gatts_count_cfg(prov_gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg 失败 %d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(prov_gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs 失败 %d", rc); return ESP_FAIL; }
    ESP_LOGI(TAG, "配网服务已注册(等待统一 gatts_start)");
    return ESP_OK;
}

void ble_provisioning_gap_event(struct ble_gap_event *event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_SUBSCRIBE:
        // PROV_RESULT 的 CCCD 订阅(与 HID 订阅按 attr_handle 区分)
        if (event->subscribe.attr_handle == s_result_val_handle) {
            s_result_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "PROV_RESULT 订阅=%d", (int)s_result_subscribed);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        // 配网 app 断开:清状态回归单连接常态(AC6)
        if (event->disconnect.conn_handle == s_prov_conn) {
            s_prov_conn = 0xFFFF;
            s_result_subscribed = false;
        }
        break;
    default:
        break;
    }
}

// ---- 结果上报(仅 app_task 调用) ----
static void notify_result(const char *json)
{
    if (s_prov_conn == 0xFFFF || !s_result_subscribed) {
        ESP_LOGW(TAG, "无配网连接/订阅,结果丢弃");
        return;
    }
    strlcpy(s_result_buf, json, sizeof(s_result_buf));   // 同步 READ 缓存
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (!om) { ESP_LOGW(TAG, "mbuf 分配失败,结果丢弃"); return; }
    int rc = ble_gatts_notify_custom(s_prov_conn, s_result_val_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify 失败 %d", rc);
}

void ble_provisioning_notify_ok(const char *ip)
{
    char buf[PROV_RESULT_BUF];
    size_t n = prov_protocol_result_ok(buf, sizeof(buf), ip);
    if (n) notify_result(buf);
}

void ble_provisioning_notify_error(prov_error_t code, const char *detail)
{
    char buf[PROV_RESULT_BUF];
    size_t n = prov_protocol_result_error(buf, sizeof(buf), code, detail);
    if (n) notify_result(buf);
}
