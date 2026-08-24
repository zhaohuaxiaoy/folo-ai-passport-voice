// main/ble_audio.c —— BLE 直连音频/事件通道实现。
// 上:分片打包纯函数(零全局状态、零分配;任何参数非法只返回错误码,绝不越界、绝不崩溃)。
// 下(仅 ESP_PLATFORM 编译,宿主机测试跳过):NimBLE 外设——GATT 服务 0xA2B0
//    (CTRL 0xA2B1 / EVENT 0xA2B2 / AUDIO 0xA2B3)、AUDIO/EVENT notify
//    (迭代分片 + BLE_GATTS_EVENT_NOTIFY_TX 逐片流控)、CTRL 写回调
//    (app_protocol_parse → 投事件,零阻塞)。
//    广播名 "AI Passport";连接建立后请求 2M PHY + 快连接参数(design.md 吞吐工程)。
#include "ble_audio.h"
#include <stdbool.h>
#include <string.h>

// ==================== 分片打包纯函数(契约见 ble_audio.h) ====================

// 参数合法性:MTU 不低于规范下限 23,帧非空。
static bool frame_valid(size_t frame_len, uint16_t mtu) {
    return mtu >= ATT_MTU_MIN && frame_len > 0;
}

size_t ble_audio_chunk_count(size_t frame_len, uint16_t mtu) {
    if (!frame_valid(frame_len, mtu)) return 0;
    size_t chunk = (size_t)mtu - PAYLOAD_OVERHEAD;
    // 除余方式求 ceil,避免 frame_len 接近 SIZE_MAX 时 (frame_len+chunk-1) 溢出
    return frame_len / chunk + (frame_len % chunk != 0);
}

size_t ble_audio_chunk_len(size_t frame_len, uint16_t mtu, size_t idx) {
    if (!frame_valid(frame_len, mtu)) return 0;
    size_t chunk = (size_t)mtu - PAYLOAD_OVERHEAD;
    size_t off = idx * chunk;
    if (off >= frame_len) return 0;
    size_t remain = frame_len - off;
    return remain < chunk ? remain : chunk;
}

ble_audio_err_t ble_audio_pack_audio(ble_audio_chunk_t *chunks, size_t chunks_cap,
                                     const uint8_t *frame, size_t frame_len, uint16_t mtu,
                                     size_t *chunk_count) {
    if (!chunks || !chunk_count || !frame) return BLE_AUDIO_ERR_NULL;
    if (mtu < ATT_MTU_MIN) return BLE_AUDIO_ERR_MTU;
    if (frame_len == 0) return BLE_AUDIO_ERR_EMPTY;
    size_t count = ble_audio_chunk_count(frame_len, mtu);
    if (count > chunks_cap) return BLE_AUDIO_ERR_SMALL_CAP;
    size_t chunk = (size_t)mtu - PAYLOAD_OVERHEAD;
    for (size_t i = 0; i < count; i++) {
        chunks[i].data = frame + i * chunk;
        chunks[i].len  = ble_audio_chunk_len(frame_len, mtu, i);
    }
    *chunk_count = count;
    return BLE_AUDIO_OK;
}

ble_audio_err_t ble_audio_pack_init(ble_audio_packer_t *p, const uint8_t *frame,
                                    size_t frame_len, uint16_t mtu) {
    if (!p || !frame) return BLE_AUDIO_ERR_NULL;
    if (mtu < ATT_MTU_MIN) return BLE_AUDIO_ERR_MTU;
    if (frame_len == 0) return BLE_AUDIO_ERR_EMPTY;
    p->frame = frame;
    p->frame_len = frame_len;
    p->offset = 0;
    p->mtu = mtu;
    return BLE_AUDIO_OK;
}

ble_audio_err_t ble_audio_pack_next(ble_audio_packer_t *p, const uint8_t **chunk,
                                    size_t *chunk_len) {
    if (!p || !chunk || !chunk_len) return BLE_AUDIO_ERR_NULL;
    if (!p->frame) return BLE_AUDIO_ERR_NULL;          // 未初始化兜底
    if (p->mtu < ATT_MTU_MIN) return BLE_AUDIO_ERR_MTU;
    if (p->offset >= p->frame_len) return BLE_AUDIO_DONE;
    size_t chunk_size = (size_t)p->mtu - PAYLOAD_OVERHEAD;
    size_t remain = p->frame_len - p->offset;
    size_t n = remain < chunk_size ? remain : chunk_size;
    *chunk = p->frame + p->offset;
    *chunk_len = n;
    p->offset += n;
    return BLE_AUDIO_OK;
}

ble_audio_err_t ble_audio_event_chunks(ble_audio_chunk_t *chunks, size_t chunks_cap,
                                       const char *line, size_t line_len, uint16_t mtu,
                                       size_t *chunk_count) {
    if (!chunks || !chunk_count || !line) return BLE_AUDIO_ERR_NULL;
    if (mtu < ATT_MTU_MIN) return BLE_AUDIO_ERR_MTU;
    if (line_len == 0) return BLE_AUDIO_ERR_EMPTY;
    if (line_len > EVENT_LINE_MAX) return BLE_AUDIO_ERR_TOO_LONG;
    return ble_audio_pack_audio(chunks, chunks_cap, (const uint8_t *)line, line_len, mtu,
                                chunk_count);
}

// ==================== NimBLE 外设(GATT 0xA2B0) ====================
// ESP_PLATFORM 由 ESP-IDF 构建系统自动定义;宿主机测试(无 IDF)不编译此段。

#ifdef ESP_PLATFORM

#include "app_events.h"
#include "app_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/store_config.h"

static const char *TAG = "ble_audio";

#define DEVICE_NAME "AI Passport"
#define TX_CHUNK_TIMEOUT_MS 50   // 单片 NOTIFY_TX 等待上限(2M 加密 ~10ms/514B,留余量;超时=拥塞丢帧)

// 服务 128-bit:0000A2B0-0000-1000-8000-00805F9B34FB(BLE_UUID128_INIT 为小端字节序,
// 写法仿旧 ble_provisioning 的 prov_svc_uuid)
static const ble_uuid128_t s_audio_svc_uuid = BLE_UUID128_INIT(
    0xB0, 0xA2, 0x00, 0x00,   // 0000A2B0
    0x00, 0x00,               // -0000-
    0x00, 0x10,               // -1000-
    0x00, 0x80,               // -8000-
    0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB);  // -00805F9B34FB

static uint16_t s_ctrl_val_handle;
static uint16_t s_event_val_handle;
static uint16_t s_audio_val_handle;
static char s_ctrl_buf[CTRL_PAYLOAD_MAX + 1];   // 写回调暂存(静态 2KB,零堆)

// 连接与订阅状态(host task 写,发送任务读;volatile 足够)
static volatile uint16_t s_conn = 0xFFFF;          // 当前连接
static volatile uint16_t s_audio_conn = 0xFFFF;    // 订阅了 AUDIO CCCD 的连接
static volatile bool s_event_subscribed = false;   // EVENT CCCD 已订阅(链路通,link_up 依据)
static volatile uint32_t s_drop_audio = 0;         // 音频 notify 失败丢弃累计
static volatile uint32_t s_drop_event = 0;         // 事件行 notify 失败丢弃累计

// NOTIFY_TX 流控:host 回调每片给一次信号量,发送任务等(超时=链路异常)
static SemaphoreHandle_t s_tx_done_sem;
static volatile uint16_t s_tx_status = 0;

static void start_advertising(void);

// ---- 单片 notify:发完等 NOTIFY_TX 完成再返回(调用方按返回值决定是否继续下一片) ----
static bool notify_one(uint16_t conn_handle, uint16_t val_handle,
                       const void *data, size_t len) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) { ESP_LOGW(TAG, "mbuf 分配失败"); return false; }
    xSemaphoreTake(s_tx_done_sem, 0);   // 清残留完成信号(上片超时遗留)
    int rc = ble_gatts_notify_custom(conn_handle, val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify 失败 %d(handle=%u len=%u)", rc, val_handle, (unsigned)len);
        return false;
    }
    if (xSemaphoreTake(s_tx_done_sem, pdMS_TO_TICKS(TX_CHUNK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "NOTIFY_TX 超时(handle=%u len=%u),按丢帧处理", val_handle, (unsigned)len);
        return false;
    }
    if (s_tx_status != 0) {
        ESP_LOGW(TAG, "NOTIFY_TX 错误 status=%u(handle=%u)", s_tx_status, val_handle);
        return false;
    }
    return true;
}

// ---- GATT server 事件:NimBLE 完成一次 notify 传输后回调(host 任务上下文) ----
static int gatts_event_cb(struct ble_gatts_event *event, void *arg) {
    (void)arg;
    if (event->type == BLE_GATTS_EVENT_NOTIFY_TX) {
        s_tx_status = event->notify_tx.status;
        xSemaphoreGive(s_tx_done_sem);
    }
    return 0;
}

// ---- CTRL 写回调:长度/语法校验 + 投事件,零阻塞(不调任何慢路径) ----
static int ctrl_write(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt) {
    (void)conn_handle;
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > CTRL_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "CTRL 载荷长度非法 %u(上限 %d)", len, CTRL_PAYLOAD_MAX);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;   // 0x0D
    }
    if (os_mbuf_copydata(ctxt->om, 0, len, s_ctrl_buf) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_ctrl_buf[len] = '\0';
    app_event_t ev;
    if (app_protocol_parse(s_ctrl_buf, len, &ev)) {
        app_event_post(&ev);   // 零阻塞
        return 0;
    }
    ESP_LOGW(TAG, "CTRL 行拒绝: %.*s", (int)(len > 80 ? 80 : len), s_ctrl_buf);
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;   // 0x03:畸形/未知 type
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return ctrl_write(conn_handle, ctxt);
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_defs[] = {
    {
        .type = BLE_GATT_SVC_DEF,
        .uuid = &s_audio_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(CTRL_CHR_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_ctrl_val_handle,
            },
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(EVENT_CHR_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_event_val_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(0x2902) }.u,
                      .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, },
                    { 0 },
                },
            },
            {
                .uuid = &(ble_uuid16_t){ BLE_UUID16_INIT(AUDIO_CHR_UUID) }.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_audio_val_handle,
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

// ---- GAP 事件 ----
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "已连接 (handle %u)", s_conn);
            // 吞吐工程:2M PHY + 快连接间隔(15-30ms / latency 0);尽力而为,失败不致命
            int rc = ble_gap_set_preferred_phy(s_conn, BLE_GAP_LE_PHY_2M,
                                               BLE_GAP_LE_PHY_2M, 0);
            if (rc != 0) ESP_LOGW(TAG, "2M PHY 请求失败 %d", rc);
            const struct ble_gap_upd_params upd = {
                .itvl_min = 12,          // 15ms(1.25ms 单位)
                .itvl_max = 24,          // 30ms
                .latency = 0,
                .supervision_timeout = 400,   // 4s(10ms 单位)
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            rc = ble_gap_update_params(s_conn, &upd);
            if (rc != 0) ESP_LOGW(TAG, "连接参数更新失败 %d", rc);
        } else {
            ESP_LOGW(TAG, "连接失败,重开广播");
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开 (reason %d)", event->disconnect.reason);
        if (event->disconnect.conn_handle == s_conn) {
            s_conn = 0xFFFF;
            s_audio_conn = 0xFFFF;
            s_event_subscribed = false;
            app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
            app_event_post(&d);
        }
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // EVENT 特征被订阅 = 链路通(PTT 可用);取消订阅 = 链路断
        if (event->subscribe.attr_handle == s_event_val_handle) {
            s_event_subscribed = event->subscribe.cur_notify;
            if (event->subscribe.cur_notify) {
                ESP_LOGI(TAG, "EVENT 特征已订阅,链路通");
                app_event_t ev = { .type = APP_EV_BLE_CONNECTED };
                app_event_post(&ev);
            } else {
                ESP_LOGW(TAG, "EVENT 订阅取消,链路断");
                app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
                app_event_post(&d);
            }
        }
        if (event->subscribe.attr_handle == s_audio_val_handle) {
            if (event->subscribe.cur_notify) {
                s_audio_conn = event->subscribe.conn_handle;
                ESP_LOGI(TAG, "AUDIO 特征已订阅");
            } else if (event->subscribe.conn_handle == s_audio_conn) {
                s_audio_conn = 0xFFFF;
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

// ---- 广播 ----
static void start_advertising(void) {
    struct ble_gap_adv_params adv_params = { 0 };
    struct ble_hs_adv_fields adv = { 0 };

    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)DEVICE_NAME;
    adv.name_len = strlen(DEVICE_NAME);
    adv.name_is_complete = 1;
    // 服务 UUID 一并广播,便于扫描端按 0xA2B0 过滤(bleak 可按 UUID 发现)
    adv.svc_uuid128 = (ble_uuid128_t *)&s_audio_svc_uuid;
    adv.num_svcs128 = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) { ESP_LOGE(TAG, "adv set_fields 失败 %d", rc); return; }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(gap_event_handler, &adv_params);
    ESP_LOGI(TAG, "广播 %s (rc=%d)", DEVICE_NAME, rc);
}

// ---- 主机同步后开广播 ----
static void on_sync(void) {
    start_advertising();
}

// ---- NimBLE 主机任务 ----
static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ==================== 对外接口 ====================

int ble_audio_init(void) {
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return -1;
    }

    s_tx_done_sem = xSemaphoreCreateBinary();
    if (!s_tx_done_sem) { ESP_LOGE(TAG, "TX 完成信号量创建失败"); return -1; }

    ble_hs_cfg.sync_cb = on_sync;
    // Just Works 无输入输出配对(macOS 主动发起);CONFIG_BT_NIMBLE_SM_SC=y 只允许 SC 配对
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_store_config_init();   // 配合 CONFIG_BT_NIMBLE_NVS_PERSIST=y 持久化配对(bond)
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    int rc = ble_gatts_count_cfg(gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg 失败 %d", rc); return -1; }
    rc = ble_gatts_add_svcs(gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs 失败 %d", rc); return -1; }
    rc = ble_gatts_set_event_cb(gatts_event_cb, NULL);   // NOTIFY_TX 流控事件
    if (rc != 0) { ESP_LOGE(TAG, "set_event_cb 失败 %d", rc); return -1; }
    rc = ble_gatts_start();
    if (rc != 0) { ESP_LOGE(TAG, "gatts_start 失败 %d", rc); return -1; }

    uint8_t addr_type;
    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "id_infer_auto 失败 %d", rc); return -1; }
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr 失败 %d", rc); return -1; }

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE 音频服务就绪(0xA2B0),等待 host sync 后广播 %s", DEVICE_NAME);
    return 0;
}

int ble_audio_notify_audio(const uint8_t *frame, size_t len) {
    if (!frame || len == 0) return -1;
    if (s_conn == 0xFFFF || s_audio_conn == 0xFFFF) {
        s_drop_audio++;
        return -1;   // 订阅者缺失:上层走丢帧
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < ATT_MTU_MIN) mtu = ATT_MTU_MIN;   // 协商未完成兜底

    ble_audio_packer_t p;
    ble_audio_err_t e = ble_audio_pack_init(&p, frame, len, mtu);
    if (e != BLE_AUDIO_OK) return -1;
    const uint8_t *chunk;
    size_t clen;
    while ((e = ble_audio_pack_next(&p, &chunk, &clen)) == BLE_AUDIO_OK) {
        if (!notify_one(s_audio_conn, s_audio_val_handle, chunk, clen)) {
            s_drop_audio++;
            return -1;   // 中段失败:整帧作废(上层丢帧计数)
        }
    }
    return 0;
}

int ble_audio_notify_event(const char *line, size_t len) {
    if (!line || len == 0 || len > EVENT_LINE_MAX) return -1;
    if (s_conn == 0xFFFF || !s_event_subscribed) {
        s_drop_event++;
        return -1;   // 订阅者缺失:上层走丢帧
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < ATT_MTU_MIN) mtu = ATT_MTU_MIN;

    if (len <= (size_t)mtu - PAYLOAD_OVERHEAD) {
        // 单包直发
        if (!notify_one(s_conn, s_event_val_handle, line, len)) {
            s_drop_event++;
            return -1;
        }
        return 0;
    }
    // 跨包:同一分片流控
    ble_audio_packer_t p;
    ble_audio_err_t e = ble_audio_pack_init(&p, (const uint8_t *)line, len, mtu);
    if (e != BLE_AUDIO_OK) return -1;
    const uint8_t *chunk;
    size_t clen;
    while ((e = ble_audio_pack_next(&p, &chunk, &clen)) == BLE_AUDIO_OK) {
        if (!notify_one(s_conn, s_event_val_handle, chunk, clen)) {
            s_drop_event++;
            return -1;
        }
    }
    return 0;
}

bool ble_audio_connected(void) { return s_conn != 0xFFFF; }
bool ble_audio_event_subscribed(void) { return s_event_subscribed; }

uint16_t ble_audio_mtu(void) {
    if (s_conn == 0xFFFF) return 0;
    return ble_att_mtu(s_conn);
}

uint32_t ble_audio_audio_drops(void) { return s_drop_audio; }
uint32_t ble_audio_event_drops(void) { return s_drop_event; }

#endif   // ESP_PLATFORM
