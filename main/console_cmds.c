// main/console_cmds.c —— esp_console 命令实现。
// USB-Serial-JTAG 控制台:配置模式、查看系统状态、重启与出厂复位。
// 输出统一走 out()(emit hook,缺省 stdout):REPL 路径行为不变;SYS 命令面
// (USB 模式)经 console_cmds_set_emit 切换为捕获缓冲,输出进 SYS_RESP 帧。
// console_cmds.c 是 main/ 下唯一用 printf 的文件 —— 重构面即全部命令函数。
#include "console_cmds.h"
#include "app_events.h"
#include "mode.h"
#include "nvs_settings.h"
#include "time_sync.h"
#include "ble_audio.h"
#include "audio_streamer.h"
#include "usb_link.h"
#include "bsp_battery.h"
#include "esp_console.h"
#include "host/ble_gap.h"   // bt scan 诊断:ble_gap_disc 主动扫描
#include "host/ble_dtm.h"   // bt dtx 诊断:controller 直接测试模式
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"  // IDF >= 5.5: esp_restart 声明移入 esp_system.h
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_pm_gate_state(uint32_t *idle_ms);   /* main.c:活动门禁状态 */

static const char *TAG = "console";

// ---- 输出出口(emit hook:REPL → stdout;SYS → 捕获缓冲)----
static void (*s_emit)(const char *s, size_t n);

void console_cmds_set_emit(console_emit_fn_t fn)
{
    s_emit = fn;
}

static void out(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;   // 截断(低频调试命令)
    if (s_emit) s_emit(buf, (size_t)n);
    else fwrite(buf, 1, (size_t)n, stdout);
}

// ---- bt scan:射频诊断(BLE RX 前端验证)----
// BLE 广播不可见时区分"RX 坏"与"TX 坏":主动扫描周边广播——
// 扫到设备(Mac/手机/耳机)则天线+晶振+射频 RX 全好,问题锁 TX/adv;
// 扫不到则射频前端/天线/晶振硬件实锤。扫描期间设备自身广播暂停。
#define BT_SCAN_DURATION_MS 8000
#define BT_SCAN_MAX_NAME 24

static volatile int s_bt_seen;      // host task 回调写,命令线程读(单核,无锁)
static volatile bool s_bt_done;
static char s_bt_names[3][BT_SCAN_MAX_NAME + 1];
static int8_t s_bt_rssi[3];

static int bt_scan_listener(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        if (s_bt_seen < 3) {
            struct ble_hs_adv_fields f;
            int i = s_bt_seen;
            s_bt_names[i][0] = '\0';
            if (ble_hs_adv_parse_fields(&f, event->disc.data,
                                        event->disc.length_data) == 0 &&
                f.name != NULL) {
                int n = f.name_len < BT_SCAN_MAX_NAME ? f.name_len : BT_SCAN_MAX_NAME;
                memcpy(s_bt_names[i], f.name, n);
                s_bt_names[i][n] = '\0';
            }
            s_bt_rssi[i] = event->disc.rssi;
        }
        s_bt_seen++;
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_bt_done = true;
    }
    return 0;
}

static int cmd_bt_dtx(int argc, char **argv);   // 定义在下方(bt scan 之后)

static int cmd_bt(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "dtx") == 0) {
        return cmd_bt_dtx(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "scan") != 0) {
        out("usage: bt scan | bt dtx [ch]\n");
        return 1;
    }
    uint8_t own;
    int rc = ble_hs_id_infer_auto(0, &own);
    if (rc != 0) { out("bt: infer addr fail %d\n", rc); return 1; }
    s_bt_seen = 0;
    s_bt_done = false;
    struct ble_gap_disc_params p = {
        .filter_duplicates = 1,   // 每设备一次 DISC 事件
        .passive = 0,             // 主动扫描:收 scan response,名字更全
        .itvl = 0, .window = 0,   // 0 = NimBLE 默认扫描间隔
        .filter_policy = 0, .limited = 0,
    };
    rc = ble_gap_disc(own, BT_SCAN_DURATION_MS, &p, bt_scan_listener, NULL);
    if (rc != 0) {
        out("bt: disc start fail %d\n", rc);
        return 1;
    }
    out("bt scanning %ds...\n", BT_SCAN_DURATION_MS / 1000);
    int waited = 0;
    while (!s_bt_done && waited < BT_SCAN_DURATION_MS + 3000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    ble_gap_disc_cancel();
    out("bt seen: %d\n", s_bt_seen);
    for (int i = 0; i < 3 && i < s_bt_seen; i++) {
        out("  %s rssi %d\n", s_bt_names[i][0] ? s_bt_names[i] : "(unnamed)",
            s_bt_rssi[i]);
    }
    return 0;
}

// ---- bt dtx:射频诊断(controller 直接测试模式,绕过 host 层强制发射)----
// ble_gap_adv_start rc=0 但空中无广播时,用 LE Transmitter Test 裁决:
// controller 在固定信道持续发射测试包(非 adv,普通扫描器不显示),
// 成功且 stop 报大量包 → controller 发射路径正常,问题在 host adv 层;
// start/stop 失败 → controller 层 TX 路径异常。
#define BT_DTM_DURATION_MS 10000

static int cmd_bt_dtx(int argc, char **argv)
{
    int channel = 37;   // 默认 2402MHz(adv 三信道之一,扫频仪/另一板收包用)
    if (argc >= 2) channel = atoi(argv[1]);
    if (channel < 0 || channel > 39) {
        out("bt dtx: channel 0-39\n");
        return 1;
    }
    struct ble_dtm_tx_params tx = {
        .channel = (uint8_t)channel,
        .test_data_len = 37,   // 标准 DTM 长度
        .payload = 0x00,       // PRBS9
        .phy = 1,              // LE 1M
    };
    int rc = ble_dtm_tx_start(&tx);
    if (rc != 0) { out("bt dtx: start fail %d\n", rc); return 1; }
    out("bt dtx: 信道 %d 发射 %ds,旁边设备收包中...\n", channel,
        BT_DTM_DURATION_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(BT_DTM_DURATION_MS));
    uint16_t packets = 0;
    rc = ble_dtm_stop(&packets);
    out("bt dtx: stop rc=%d 发射包数=%u\n", rc, packets);
    return 0;
}

// ---- time:wall-clock 校时(校时源仅电脑客户端;SYS `time set` 供 USB 模式)----
static int cmd_time(int argc, char **argv)
{
    if (argc == 1) {
        char t[16];
        time_sync_format_local(t, sizeof(t));
        out("time: %s (%s, tz %+d)\n", t,
            time_sync_valid() ? "synced" : "unsynced", time_sync_tz_hour());
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        // epoch 为 UTC 秒(int64,负值表示 1970 前);strtoll 拒绝即报错
        char *end = NULL;
        long long epoch = strtoll(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            out("time set: invalid epoch '%s'\n", argv[2]);
            return 1;
        }
        time_sync_set_epoch((int64_t)epoch);
        out("time set: ok\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "tz") == 0) {
        int h = atoi(argv[2]);
        esp_err_t e = time_sync_set_tz(h);
        out("time tz: %s (%+d)\n", e == ESP_OK ? "ok" : esp_err_to_name(e),
            time_sync_tz_hour());
        return 0;
    }
    out("usage: time | time set <epoch> | time tz <±hh>\n");
    return 1;
}

// ---- log:导出 USB 模式日志环(REPL 模式下日志直接上屏,环为空) ----
// 可选 <offset>:SYS_RESP 载荷上限 2048B(USB_RESP_MAX),环(16KB)超限时
// 首块恰在中间截断。`log 2048` / `log 4096` … 按游标分多次取回全量 ——
// 2026-08-28 "长按响两次" 排查:首块在 RELEASE 行截断,第二次 LONG(第二声
// 滴)恰在截断点之后,无 offset 则永远看不到尾部。
static int cmd_log(int argc, char **argv)
{
    size_t start = 0;
    if (argc > 1) {
        char *end = NULL;
        unsigned long v = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') {
            out("usage: log [offset]\n");
            return 1;
        }
        start = (size_t)v;
    }
    // 分块导出:日志环最大 16KB,而本命令在 2048B 栈的 usb_read_task 中执行,
    // 声明整段栈数组必爆栈(审查 P0)。512B 小缓冲循环经 s_emit 透传。
    char buf[512];
    size_t off = start;
    size_t n = 0;
    while ((n = usb_link_dump_log_at(buf, sizeof(buf), off)) > 0) {
        if (s_emit) s_emit(buf, n);   // 原样透传(含换行),不进 out 的截断缓冲
        else fwrite(buf, 1, n, stdout);
        off += n;
        if (n < sizeof(buf) - 1) break;   // 末块(不足一满块),避免尾帧死循环
    }
    if (n == 0) out(start == 0 ? "(log ring empty)\n" : "(end of ring)\n");
    return 0;
}

// ---- st:状态一览 ----
static int cmd_st(int argc, char **argv)
{
    (void)argc; (void)argv;
    out("--- system ---\n");
    out("heap free: %d B (min %d B), largest free block: %d B\n",
        (int)esp_get_free_heap_size(), (int)esp_get_minimum_free_heap_size(),
        (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // 全任务栈高水位枚举(F3:覆盖所有任务,替代只查两个的固定列表;需
    // CONFIG_FREERTOS_USE_TRACE_FACILITY=y 提供 uxTaskGetSystemState)。
    // 控制台低频调试命令,一次性 ~百字节堆分配可接受(不驻留)。
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *sts = heap_caps_malloc((size_t)n * sizeof(TaskStatus_t),
                                         MALLOC_CAP_8BIT);
    if (!sts) {
        out("tasks: %u (状态数组分配失败)\n", (unsigned)n);
    } else {
        n = uxTaskGetSystemState(sts, n, NULL);
        out("tasks: %u\n", (unsigned)n);
        out("%-18s %-10s %10s\n", "name", "state", "hwm(B)");
        static const char *state_name[] = { "running", "ready", "blocked",
                                            "suspended", "deleted", "invalid" };
        for (UBaseType_t i = 0; i < n; i++) {
            const char *s = sts[i].eCurrentState <= eDeleted
                                ? state_name[sts[i].eCurrentState] : "invalid";
            out("%-18s %-10s %10u\n", sts[i].pcTaskName, s,
                (unsigned)sts[i].usStackHighWaterMark);
        }
        free(sts);
    }
    out("--- pm ---\n");
    {
        uint32_t idle_ms = 0;
        bool held = app_pm_gate_state(&idle_ms);
        out("gate: %s  idle: %ums\n", held ? "normal(常态功耗)" : "saving(省电)",
            (unsigned)idle_ms);
    }
    out("--- link ---\n");
    // 双通道常开:聚合链路 + 两通道独立状态。会话路由 = 最近连接/使用通道。
    out("link: %s\n", mode_link_up() ? "up" : "down");
    out("ble: subscribed %s  usb: session %s\n",
        ble_audio_event_subscribed() ? "yes" : "no",
        usb_link_session_active() ? "up" : "down");
    out("--- ble ---\n");
    out("connected: %s event_subscribed: %s mtu: %u\n",
        ble_audio_connected() ? "yes" : "no",
        ble_audio_event_subscribed() ? "yes" : "no",
        (unsigned)ble_audio_mtu());
    out("drops: audio %u event %u\n",
        (unsigned)ble_audio_audio_drops(), (unsigned)ble_audio_event_drops());
    out("--- audio ---\n");
    out("streaming: %s peak: %d\n",
        audio_streamer_active() ? "yes" : "no", (int)audio_streamer_peak());
    out("--- battery ---\n");
    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    out("soc: %d%% mv: %d\n", soc < 0 ? -1 : soc, mv < 0 ? -1 : mv);
    return 0;
}

// ---- rst:复位原因(无线自动重启诊断用) ----
// esp_reset_reason_t(IDF 5.5)顺序, 索引 = 枚举值。名字取自 esp_system.h,
// 不再手抄成中文注释: 这个表错位过一次, 而它的唯一用途就是判因。
static const char *const s_rst_names[] = {
    "unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt",
    "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse",
    "pwr_glitch", "cpu_lockup",
};

const char *console_reset_reason_str(int reason)
{
    if (reason < 0 || (size_t)reason >= sizeof(s_rst_names) / sizeof(s_rst_names[0])) {
        return "unknown";
    }
    return s_rst_names[reason];
}

static int cmd_rst(int argc, char **argv)
{
    (void)argc; (void)argv;
    const int r = (int)esp_reset_reason();
    out("复位原因: %d (%s)\n", r, console_reset_reason_str(r));
    return 0;
}

// ---- reboot / factory ----
static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    out("rebooting...\n");
    esp_restart();
    return 0;
}

static int cmd_factory(int argc, char **argv)
{
    (void)argc; (void)argv;
    out("clearing NVS and rebooting...\n");
    nvs_settings_factory_reset();
    esp_restart();
    return 0;
}

// ---- 命令表(REPL 注册与 SYS 执行共用)----
// 注:模式切换命令(mode)已随双通道常开架构退役(2026-08-28)。
static const struct { const char *name; esp_console_cmd_func_t fn; } s_cmds[] = {
    { "log",     cmd_log },
    { "time",    cmd_time },
    { "st",      cmd_st },
    { "rst",     cmd_rst },
    { "reboot",  cmd_reboot },
    { "factory", cmd_factory },
};
#define CMD_COUNT (sizeof(s_cmds) / sizeof(s_cmds[0]))

// ---- SYS 帧输出捕获(console_cmds_run_line 期间替换 emit)----
static char *s_cap_buf;
static size_t s_cap_len, s_cap_cap;

static void capture_emit(const char *s, size_t n)
{
    if (s_cap_buf == NULL) return;
    size_t room = s_cap_cap - 1 - s_cap_len;   // 留 1 字节给 NUL
    if (n > room) n = room;
    memcpy(s_cap_buf + s_cap_len, s, n);
    s_cap_len += n;
}

int console_cmds_run_line(const char *line, char *out_buf, size_t out_cap)
{
    char tmp[192];
    size_t tl = strlen(line);
    if (tl >= sizeof(tmp)) tl = sizeof(tmp) - 1;
    memcpy(tmp, line, tl);
    tmp[tl] = '\0';

    char *argv[8];
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, " \t\r\n", &save);
         tok && argc < 8; tok = strtok_r(NULL, " \t\r\n", &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        if (out_buf && out_cap > 0) out_buf[0] = '\0';
        return 1;   // 空行:无命令
    }

    const esp_console_cmd_func_t *fn = NULL;
    for (size_t i = 0; i < CMD_COUNT; i++) {
        if (strcmp(argv[0], s_cmds[i].name) == 0) { fn = &s_cmds[i].fn; break; }
    }
    if (fn == NULL) {
        // 先落 NUL:调用方(SYS)以 out_buf[0]=='\0' 判"无输出"回 unknown command ——
        // 不落则读到未初始化堆垃圾(实测 0x14 单字节经 SYS_RESP 上行,时间校不进去)
        if (out_buf && out_cap > 0) out_buf[0] = '\0';
        return 1;   // 未知命令:调用方(SYS)回 "unknown command"
    }

    // 输出捕获进调用方缓冲;执行完恢复(SYS 帧在 usb_link 读任务上下文执行)
    s_cap_buf = out_buf;
    s_cap_len = 0;
    s_cap_cap = out_cap;
    console_cmds_set_emit(capture_emit);
    int rc = (*fn)(argc, argv);
    console_cmds_set_emit(NULL);   // 恢复缺省(stdout)
    s_cap_buf = NULL;
    if (out_buf && out_cap > 0) out_buf[s_cap_len] = '\0';
    return rc;
}

static void reg(const char *name, const char *help, const char *hint,
                esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = fn,
    };
    if (esp_console_cmd_register(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "命令 %s 注册失败", name);
    }
}

esp_err_t console_cmds_register(void)
{
    reg("bt", "BT 射频诊断:bt scan | bt dtx [ch](直接测试模式强制发射)", NULL, cmd_bt);
    reg("log", "导出日志环(有 USB 主机时日志进 RAM 环)", NULL, cmd_log);
    reg("st", "系统状态一览(双链路/MTU/掉帧/堆)", NULL, cmd_st);
    reg("rst", "复位原因(无线自动重启诊断)", NULL, cmd_rst);
    reg("time", "校时:time | time set <epoch> | time tz <±hh>", NULL, cmd_time);
    reg("reboot", "重启设备", NULL, cmd_reboot);
    reg("factory", "清空 NVS 并重启", NULL, cmd_factory);
    return ESP_OK;
}
