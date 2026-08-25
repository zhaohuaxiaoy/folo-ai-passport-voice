// main/console_cmds.c —— esp_console 命令实现。
// USB-Serial-JTAG 控制台:配置模式/Wi-Fi/WS 目标、查看系统状态、重启与出厂复位。
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
#include "mdns_resolver.h"
#include "usb_link.h"
#include "wifi_app.h"
#include "ws_client.h"
#include "bsp_battery.h"
#include "esp_console.h"
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

// ---- mode:射频模式(手动切换;NVS 持久化)----
static int cmd_mode(int argc, char **argv)
{
    if (argc == 1) {
        out("mode: %s\n", mode_name(mode_get()));
        out("link: %s\n", mode_link_up() ? "up" : "down");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ble") == 0) {
        if (mode_get() == APP_MODE_BLE) { out("already BLE\n"); return 0; }
        app_event_t e = { .type = APP_EV_MODE_SWITCH, .u.mode_switch = { .target = APP_MODE_BLE } };
        app_event_post(&e);
        out("switching to BLE...\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "wifi") == 0) {
        if (mode_get() == APP_MODE_WIFI) { out("already WiFi\n"); return 0; }
        app_event_t e = { .type = APP_EV_MODE_SWITCH, .u.mode_switch = { .target = APP_MODE_WIFI } };
        app_event_post(&e);
        out("switching to WiFi...\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "usb") == 0) {
        if (mode_get() == APP_MODE_USB) { out("already USB\n"); return 0; }
        app_event_t e = { .type = APP_EV_MODE_SWITCH, .u.mode_switch = { .target = APP_MODE_USB } };
        app_event_post(&e);
        out("switching to USB (reboot)...\n");
        return 0;
    }
    out("usage: mode | mode ble | mode wifi | mode usb\n");
    return 1;
}

// ---- wifi ----
static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        char ssid[64] = "", pass[64] = "";
        if (nvs_settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
            out("stored ssid: (unreadable)\n");
        } else {
            out("stored ssid: %s\n", ssid[0] ? ssid : "(none)");
        }
        out("connected: %s\n", wifi_app_connected() ? "yes" : "no");
        out("ip: %s\n", wifi_app_ip());
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 4) {
        // 密码只写 NVS,绝不回显到控制台(SYS_RESP 同样不回显)
        esp_err_t e = wifi_app_set_credentials(argv[2], argv[3]);
        out("wifi set: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
        return 0;
    }
    if (strcmp(argv[1], "get") == 0) {
        char ssid[64] = "", pass[64] = "";
        if (nvs_settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
            out("ssid: (unreadable)\n");
            return 0;
        }
        out("ssid: %s pass: %s\n", ssid[0] ? ssid : "(none)",
            pass[0] ? "***" : "(none)");
        return 0;
    }
    out("usage: wifi set <ssid> <pass> | wifi get | wifi status\n");
    return 1;
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

// ---- ws ----
static int cmd_ws(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        char url[128] = "";
        bool auto_mode;
        nvs_settings_get_ws_url(url, sizeof(url));
        nvs_settings_get_ws_mode(&auto_mode);
        out("url: %s\n", url);
        out("mode: %s\n", auto_mode ? "auto (mDNS)" : "static");
        out("connected: %s\n", ws_client_connected() ? "yes" : "no");
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 3) {
        if (strcmp(argv[2], "auto") == 0) {
            // 切回自动发现:mDNS 可覆盖运行时 URL(不写回 NVS URL)
            esp_err_t e = nvs_settings_set_ws_mode(true);
            out("ws mode: %s\n", e == ESP_OK ? "auto (mDNS)" : esp_err_to_name(e));
            return 0;
        }
        nvs_settings_set_ws_mode(false);   // 显式 URL → static(用户显式优先)
        esp_err_t e = ws_client_reinit(argv[2]);
        out("ws set: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
        return 0;
    }
    out("usage: ws set <url> | ws set auto | ws status\n");
    return 1;
}

// ---- mdns:手动触发解析 ----
static int cmd_mdns(int argc, char **argv)
{
    (void)argc; (void)argv;
    mdns_resolver_request();
    out("mDNS 解析已触发(auto 模式);目标变化时自动重连 WS\n");
    return 0;
}

// ---- log:导出 USB 模式日志环(REPL 模式下日志直接上屏,环为空) ----
static int cmd_log(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[USB_LOG_RING_CAP];
    size_t n = usb_link_dump_log(buf, sizeof(buf));
    if (n == 0) out("(log ring empty)\n");
    else if (s_emit) s_emit(buf, n);   // 原样透传(含换行),不进 out 的截断缓冲
    else fwrite(buf, 1, n, stdout);
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
    out("--- link ---\n");
    out("mode: %s link: %s\n", mode_name(mode_get()), mode_link_up() ? "up" : "down");
    out("wifi: %s ip: %s\n", wifi_app_connected() ? "connected" : "disconnected",
        wifi_app_ip()[0] ? wifi_app_ip() : "(none)");
    out("ws: %s\n", ws_client_connected() ? "connected" : "disconnected");
    out("usb: session %s\n", usb_link_session_active() ? "up" : "down");
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
static const struct { const char *name; esp_console_cmd_func_t fn; } s_cmds[] = {
    { "mode",    cmd_mode },
    { "wifi",    cmd_wifi },
    { "ws",      cmd_ws },
    { "mdns",    cmd_mdns },
    { "log",     cmd_log },
    { "st",      cmd_st },
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
    if (argc == 0) return 1;   // 空行:无命令

    const esp_console_cmd_func_t *fn = NULL;
    for (size_t i = 0; i < CMD_COUNT; i++) {
        if (strcmp(argv[0], s_cmds[i].name) == 0) { fn = &s_cmds[i].fn; break; }
    }
    if (fn == NULL) return 1;   // 未知命令:调用方(SYS)回 "unknown command"

    // 输出捕获进调用方缓冲;执行完恢复(REPL 线程与 USB 读任务互斥 ——
    // REPL 存在时 USB 驱动未装、usb_link 读任务不存在,反之亦然)
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
    reg("mode", "射频模式:mode | mode ble | mode wifi | mode usb(切换需几秒,期间链路中断)", NULL, cmd_mode);
    reg("wifi", "Wi-Fi 配置:wifi status | wifi get | wifi set <ssid> <pass>", NULL, cmd_wifi);
    reg("ws", "WS 目标:ws status | ws set <url> | ws set auto", NULL, cmd_ws);
    reg("mdns", "手动触发 mDNS 解析", NULL, cmd_mdns);
    reg("log", "导出 USB 模式日志环(REPL 模式下为空)", NULL, cmd_log);
    reg("st", "系统状态一览(模式/双链路/MTU/掉帧/堆)", NULL, cmd_st);
    reg("time", "校时:time | time set <epoch> | time tz <±hh>", NULL, cmd_time);
    reg("reboot", "重启设备", NULL, cmd_reboot);
    reg("factory", "清空 NVS 并重启", NULL, cmd_factory);
    return ESP_OK;
}
