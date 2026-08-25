// main/usb_link.c —— USB 有线通道驱动层(第三通道,见 usb_link.h / design.md)。
// 驱动部分用 #ifdef ESP_PLATFORM 包裹:帧编解码已在 usb_link_framing(纯 C)独立,
// 宿主测试不编本文件。
// 设计要点:
//   - 仅 boot USB 模式初始化(REPL 门禁跳过):驱动安装与 REPL 互斥,无二次安装冲突;
//   - 单读任务:阻塞 read_bytes(≤500ms) → 帧状态机逐字节分发;拔线由
//     usb_serial_jtag_is_connected()(SOF 检测)轮询,翻转 → 会话 down + 断连事件;
//   - 会话:PC 握手 ping → up(投 USB_CONNECTED)+ pong + device.hello;
//   - 日志隔离:esp_log 输出重定向到 RAM 环(经 console `log` 取回),不混入数据帧;
//   - 发送:整帧一次 write_bytes 入 4096 ring(无 MTU 分片),部分写入/会话 down → -1
//     (走 audio_streamer 既有丢帧计数路径,对齐 BLE)。
#include "usb_link.h"
#include "usb_link_framing.h"
#include "app_events.h"
#include "app_protocol.h"
#include "app_types.h"
#include "console_cmds.h"
#include "mode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM

static const char *TAG = "usb_link";

#define USB_RX_CHUNK          128
#define USB_POLL_INTERVAL_MS  500     /* 拔线轮询周期(read_bytes 阻塞上限) */
#define USB_TX_TIMEOUT_EVENT  pdMS_TO_TICKS(20)
#define USB_TX_TIMEOUT_AUDIO  pdMS_TO_TICKS(100)
#define USB_SHUTDOWN_TIMEOUT_MS 700
#define USB_SYS_LINE_MAX      128     /* SYS 命令文本上限(帧协议契约) */
#define USB_RESP_MAX          2048    /* SYS_RESP 载荷上限 */

/* USB 驱动保持安装(卸载 = 可能阻断读任务并造成悬空 UB),但协议工作缓冲只在
 * USB 模式生命周期内存在。BLE/WiFi 模式不再永久占用这块约 11.5KB 的 RAM。 */
static volatile bool s_running;            /* 离开 USB 模式置 false,读任务 ≤500ms 内自删 */
static volatile bool s_session_up;         /* 收到 ping → true;跨任务读写,单核无撕裂,volatile 显式化 */
static bool s_connected;                   /* is_connected 上次采样 */
static SemaphoreHandle_t s_tx_mux;         /* 发送互斥:串行化三发送方对 s_tx_buf 的组帧+写 */
static SemaphoreHandle_t s_read_exit_sem;  /* shutdown 等待读任务真正退出 */
static usb_frame_ctx_t s_fr_ctx;           /* 帧解码状态机(dir 初始化见 usb_link_init) */
/* 下行缓冲:方向过滤 + 类型上限保证 feed 只写 0..2047;+1 尾字节专容 CTRL
 * 尾 NUL(feed 不写,见 dispatch_frame;审查 P1) */
static uint8_t *s_fr_payload;
static uint8_t *s_tx_buf;                  /* 上行:AUDIO 3200 */
static char *s_sys_cmd;
static char *s_sys_resp;
static volatile bool s_read_exited;

/* 日志环(esp_log 重定向):覆盖写,满则覆盖最旧。
 * 任意任务(全局 esp_log 重定向)可并发写,console `log` 并发读 → 临界区保护。
 * 临界区微秒级,不阻塞日志路径的任何任务。 */
static char *s_log_ring;
static size_t s_log_w, s_log_n;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void release_buffers(void)
{
    char *log_ring;
    heap_caps_free(s_fr_payload);
    heap_caps_free(s_tx_buf);
    heap_caps_free(s_sys_cmd);
    heap_caps_free(s_sys_resp);
    s_fr_payload = NULL;
    s_tx_buf = NULL;
    s_sys_cmd = NULL;
    s_sys_resp = NULL;

    /* esp_log may have an in-flight call when restore_log switches vprintf.
     * Detach the ring under the same critical section used by ring_write so it
     * cannot be freed while a logger still holds the old pointer. */
    portENTER_CRITICAL(&s_log_mux);
    log_ring = s_log_ring;
    s_log_ring = NULL;
    s_log_w = 0;
    s_log_n = 0;
    portEXIT_CRITICAL(&s_log_mux);
    heap_caps_free(log_ring);
}

static esp_err_t alloc_buffers(void)
{
    s_fr_payload = heap_caps_malloc(USB_FRAME_RX_PAYLOAD_MAX + 1, MALLOC_CAP_8BIT);
    s_tx_buf = heap_caps_malloc(USB_FRAME_HEADER + USB_FRAME_TX_PAYLOAD_MAX,
                                MALLOC_CAP_8BIT);
    s_sys_cmd = heap_caps_malloc(USB_SYS_LINE_MAX + 1, MALLOC_CAP_8BIT);
    s_sys_resp = heap_caps_malloc(USB_RESP_MAX, MALLOC_CAP_8BIT);
    s_log_ring = heap_caps_malloc(USB_LOG_RING_CAP, MALLOC_CAP_8BIT);
    if (!s_fr_payload || !s_tx_buf || !s_sys_cmd || !s_sys_resp || !s_log_ring) {
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void ring_write(const char *s, size_t n)
{
    portENTER_CRITICAL(&s_log_mux);
    if (!s_log_ring) {
        portEXIT_CRITICAL(&s_log_mux);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_log_ring[s_log_w] = s[i];
        s_log_w = (s_log_w + 1) % USB_LOG_RING_CAP;
        if (s_log_n < USB_LOG_RING_CAP) s_log_n++;
    }
    portEXIT_CRITICAL(&s_log_mux);
}

static int usb_log_vprintf(const char *fmt, va_list args)
{
    char buf[192];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) return n;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    ring_write(buf, (size_t)n);
    return n;
}

size_t usb_link_dump_log(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0 || !s_log_ring) return 0;
    portENTER_CRITICAL(&s_log_mux);
    if (s_log_n == 0) {
        portEXIT_CRITICAL(&s_log_mux);
        buf[0] = '\0';
        return 0;
    }
    size_t want = s_log_n < cap - 1 ? s_log_n : cap - 1;
    size_t start = (s_log_w + USB_LOG_RING_CAP - s_log_n) % USB_LOG_RING_CAP;
    size_t first = USB_LOG_RING_CAP - start;
    if (first > want) first = want;
    memcpy(buf, s_log_ring + start, first);
    if (want > first) memcpy(buf + first, s_log_ring, want - first);
    portEXIT_CRITICAL(&s_log_mux);
    buf[want] = '\0';
    return want;
}

// ---- 会话 ----

bool usb_link_session_active(void)
{
    return s_session_up && mode_get() == APP_MODE_USB;
}

// 离开 USB 模式:停读任务(≤500ms 内自删,释放 2KB 任务栈)+ 会话 down。
// shutdown 只在读任务确认退出后才允许 restore_log 回收专用缓冲。
void usb_link_shutdown(void)
{
    s_running = false;
    s_session_up = false;
    if (s_read_exit_sem && !s_read_exited) {
        if (xSemaphoreTake(s_read_exit_sem, pdMS_TO_TICKS(USB_SHUTDOWN_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "USB 读任务退出超时,保留专用缓冲避免并发释放");
        }
    }
}

// ---- 发送 ----

static int usb_send_frame(uint8_t type, const uint8_t *payload, size_t len,
                          TickType_t timeout_ticks)
{
    if (!usb_link_session_active()) return -1;
    // 发送互斥:app_task(EVENT)/ble_worker(AUDIO)/读任务(SYS_RESP)三上下文
    // 共享 s_tx_buf,组帧+写必须持锁,否则阻塞写(20-100ms)期间另一任务
    // 覆写缓冲 → 帧交错/校验失败。锁等待有界(200ms ≥ 写超时上限),拿不到
    // → 丢帧(与 BLE 丢帧语义一致)。
    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
    // 一次调用把整帧拷入 TX ring(4096 ≥ 3212B 音频帧);
    // 返回 ≠ 帧长 = 主机未及时读 → 部分写入,丢帧(与 BLE 语义一致)
    size_t n = usb_frame_build(type, payload, len, s_tx_buf,
                               USB_FRAME_HEADER + USB_FRAME_TX_PAYLOAD_MAX);
    int written = 0;
    if (n > 0) {
        written = usb_serial_jtag_write_bytes(s_tx_buf, n, timeout_ticks);
    }
    xSemaphoreGive(s_tx_mux);
    return (n == 0 || written != (int)n) ? -1 : 0;
}

int usb_link_send_event(const char *line, size_t len)
{
    return usb_send_frame(USB_FRAME_EVENT, (const uint8_t *)line, len,
                          USB_TX_TIMEOUT_EVENT);
}

int usb_link_send_audio(const uint8_t *frame, size_t len)
{
    return usb_send_frame(USB_FRAME_AUDIO, frame, len, USB_TX_TIMEOUT_AUDIO);
}

// ---- 下行分发 ----

static void send_sys_resp(const char *text, size_t len)
{
    if (len > USB_RESP_MAX) len = USB_RESP_MAX;   // 超限截断(帧协议契约)
    usb_send_frame(USB_FRAME_SYS_RESP, (const uint8_t *)text, len,
                   USB_TX_TIMEOUT_EVENT);
}

static void send_device_hello(void)
{
    char buf[APP_PROTO_TX_CAP];
    size_t n = app_protocol_device_hello(buf, sizeof(buf), 2);   // proto 2 = USB 通道
    if (n > 0) usb_link_send_event(buf, n);
}

static void handle_sys(const char *line, size_t len)
{
    if (len == 0 || len > USB_SYS_LINE_MAX) return;
    memcpy(s_sys_cmd, line, len);
    s_sys_cmd[len] = '\0';

    if (strcmp(s_sys_cmd, "ping") == 0) {
        // 握手:会话 up + 断连恢复事件 + pong + hello(对齐 ws_client 的 hello 时序)
        if (!s_session_up) {
            s_session_up = true;
            app_event_t c = { .type = APP_EV_USB_CONNECTED };
            app_event_post(&c);
        }
        send_sys_resp("pong", 4);
        send_device_hello();
        return;
    }
    // 其余命令走 console 命令面(与 REPL 同一批 cmd_*,输出捕获进 SYS_RESP)
    int rc = console_cmds_run_line(s_sys_cmd, s_sys_resp, USB_RESP_MAX);
    if (rc != 0 && s_sys_resp[0] == '\0') {
        snprintf(s_sys_resp, USB_RESP_MAX, "unknown command: %s", s_sys_cmd);
    }
    send_sys_resp(s_sys_resp, strlen(s_sys_resp));
}

static void dispatch_frame(uint8_t type, const uint8_t *payload, size_t len)
{
    switch (type) {
    case USB_FRAME_CTRL: {
        // 与 ble_audio.c ctrl_write 同语义:解析 + 投事件,零阻塞
        char *s = (char *)payload;
        if (len == 0 || len > APP_PROTO_RX_CAP) {
            ESP_LOGW(TAG, "CTRL 载荷长度非法 %u(上限 %d)", (unsigned)len,
                     APP_PROTO_RX_CAP);
            return;
        }
        s[len] = '\0';   // 尾 NUL:缓冲 [2048+1] 尾字节专位(feed 只写 0..2047)
        app_event_t ev;
        if (app_protocol_parse(s, len, &ev)) {
            app_event_post(&ev);
        } else {
            ESP_LOGW(TAG, "CTRL 行拒绝: %.*s", (int)(len > 80 ? 80 : len), s);
        }
        break;
    }
    case USB_FRAME_SYS:
        handle_sys((const char *)payload, len);
        break;
    default:
        // EVENT/AUDIO 来自 PC = 协议违约(上行帧类型只应设备发送)
        ESP_LOGW(TAG, "上行帧类型违约(PC 不应发 type %d),丢弃", type);
        break;
    }
}

// ---- 读任务 ----

static void usb_read_task(void *arg)
{
    (void)arg;
    uint8_t buf[USB_RX_CHUNK];
    int64_t last_poll = 0;

    for (;;) {
        if (!s_running) {
            s_read_exited = true;
            if (s_read_exit_sem) xSemaphoreGive(s_read_exit_sem);
            vTaskDelete(NULL);               // 自删(仅本任务,安全;阻塞读 ≤500ms 内返回)
        }
        // 阻塞读(≤500ms);返回实际字节数(0 = 超时无数据)
        int got = usb_serial_jtag_read_bytes(buf, sizeof(buf),
                                             pdMS_TO_TICKS(USB_POLL_INTERVAL_MS));
        for (int i = 0; i < got; i++) {
            uint8_t type = 0;
            size_t plen = 0;
            usb_frame_feed_rc_t rc = usb_frame_feed(&s_fr_ctx, buf[i],
                                                    &type, s_fr_payload, &plen);
            if (rc == USB_FRAME_DONE) {
                dispatch_frame(type, s_fr_payload, plen);
            } else if (rc != USB_FRAME_NONE) {
                ESP_LOGW(TAG, "帧解析丢弃(rc=%d,失同步已重扫)", rc);
            }
        }

        // 拔线轮询(SOF 检测;≥500ms 才采样,避免音频流期间高频空转)
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_poll >= USB_POLL_INTERVAL_MS * 1000) {
            last_poll = now_us;
            bool c = usb_serial_jtag_is_connected();
            if (c != s_connected) {
                s_connected = c;
                if (!c) {
                    // 拔线/主机休眠:会话 down + 断连事件(link_up 收束)
                    ESP_LOGW(TAG, "USB 主机已断开");
                    if (s_session_up) {
                        s_session_up = false;
                        app_event_t d = { .type = APP_EV_USB_DISCONNECTED };
                        app_event_post(&d);
                    }
                }
            }
        }
    }
}

// ---- 初始化 ----

esp_err_t usb_link_init(void)
{
    esp_err_t e = alloc_buffers();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "USB 专用缓冲分配失败");
        return e;
    }
    // 本端接收方向 = DOWN(PC→设备):feed 只收 CTRL/SYS,方向过滤 + 类型上限
    // 保证 s_fr_payload[2048] 永不越界(审查 P1;+1 仅供 CTRL 尾 NUL)
    s_fr_ctx.dir = USB_FRAME_DIR_DOWN;
    s_tx_mux = xSemaphoreCreateMutex();
    if (s_tx_mux == NULL) {
        ESP_LOGE(TAG, "发送互斥锁创建失败");
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    s_read_exit_sem = xSemaphoreCreateBinary();
    if (s_read_exit_sem == NULL) {
        ESP_LOGE(TAG, "读任务退出信号量创建失败");
        vSemaphoreDelete(s_tx_mux);
        s_tx_mux = NULL;
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    s_read_exited = false;
    s_log_w = 0;
    s_log_n = 0;
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 4096,   // 一次 write_bytes 容整 3200B 音频帧
        .rx_buffer_size = 4096,
    };
    e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG 驱动安装失败: %s", esp_err_to_name(e));
        vSemaphoreDelete(s_read_exit_sem);
        s_read_exit_sem = NULL;
        vSemaphoreDelete(s_tx_mux);
        s_tx_mux = NULL;
        release_buffers();
        return e;
    }
    // 清空陈旧字节(boot 期 esptool/启动日志残帧),会话从 PC 握手 ping 开始
    uint8_t drain[64];
    while (usb_serial_jtag_read_bytes(drain, sizeof(drain), 0) > 0) { }
    s_session_up = false;
    s_connected = usb_serial_jtag_is_connected();

    // 日志隔离:esp_log → RAM 环(数据帧独占物理口;经 console `log` 取回)
    esp_log_set_vprintf(usb_log_vprintf);

    if (xTaskCreate(usb_read_task, "usb_link", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "USB 读任务创建失败");
        s_running = false;
        esp_log_set_vprintf(vprintf);
        vSemaphoreDelete(s_read_exit_sem);
        s_read_exit_sem = NULL;
        vSemaphoreDelete(s_tx_mux);
        s_tx_mux = NULL;
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB 链路就绪(等 PC 握手 ping)");
    return ESP_OK;
}

void usb_link_restore_log(void)
{
    esp_log_set_vprintf(vprintf);
    if (!s_read_exited) return;
    release_buffers();
    if (s_read_exit_sem) {
        vSemaphoreDelete(s_read_exit_sem);
        s_read_exit_sem = NULL;
    }
    if (s_tx_mux) {
        vSemaphoreDelete(s_tx_mux);
        s_tx_mux = NULL;
    }
}

#endif /* ESP_PLATFORM */
