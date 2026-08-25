// main/console_cmds.c —— esp_console 命令实现。
// USB-Serial-JTAG 控制台:配置模式/Wi-Fi/WS 目标、查看系统状态、重启与出厂复位。
#include "console_cmds.h"
#include "app_events.h"
#include "mode.h"
#include "nvs_settings.h"
#include "ble_audio.h"
#include "audio_streamer.h"
#include "mdns_resolver.h"
#include "wifi_app.h"
#include "ws_client.h"
#include "bsp_battery.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_restart.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "console";

// ---- mode:射频模式(手动切换;NVS 持久化)----
static int cmd_mode(int argc, char **argv)
{
    if (argc == 1) {
        printf("mode: %s\n", mode_name(mode_get()));
        printf("link: %s\n", mode_link_up() ? "up" : "down");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ble") == 0) {
        if (mode_get() == APP_MODE_BLE) { printf("already BLE\n"); return 0; }
        app_event_t e = { .type = APP_EV_MODE_SWITCH, .u.mode_switch = { .target = APP_MODE_BLE } };
        app_event_post(&e);
        printf("switching to BLE...\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "wifi") == 0) {
        if (mode_get() == APP_MODE_WIFI) { printf("already WiFi\n"); return 0; }
        app_event_t e = { .type = APP_EV_MODE_SWITCH, .u.mode_switch = { .target = APP_MODE_WIFI } };
        app_event_post(&e);
        printf("switching to WiFi...\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "usb") == 0) {
        if (mode_get() == APP_MODE_USB) { printf("already USB\n"); return 0; }
        app_event_t e = { .type = APP_EV_MODE_SWITCH, .u.mode_switch = { .target = APP_MODE_USB } };
        app_event_post(&e);
        printf("switching to USB (reboot)...\n");
        return 0;
    }
    printf("usage: mode | mode ble | mode wifi | mode usb\n");
    return 1;
}

// ---- wifi ----
static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        char ssid[64] = "", pass[64] = "";
        if (nvs_settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
            printf("stored ssid: (unreadable)\n");
        } else {
            printf("stored ssid: %s\n", ssid[0] ? ssid : "(none)");
        }
        printf("connected: %s\n", wifi_app_connected() ? "yes" : "no");
        printf("ip: %s\n", wifi_app_ip());
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 4) {
        // 密码只写 NVS,绝不回显到控制台
        esp_err_t e = wifi_app_set_credentials(argv[2], argv[3]);
        printf("wifi set: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
        return 0;
    }
    if (strcmp(argv[1], "get") == 0) {
        char ssid[64] = "", pass[64] = "";
        if (nvs_settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
            printf("ssid: (unreadable)\n");
            return 0;
        }
        printf("ssid: %s pass: %s\n", ssid[0] ? ssid : "(none)",
               pass[0] ? "***" : "(none)");
        return 0;
    }
    printf("usage: wifi set <ssid> <pass> | wifi get | wifi status\n");
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
        printf("url: %s\n", url);
        printf("mode: %s\n", auto_mode ? "auto (mDNS)" : "static");
        printf("connected: %s\n", ws_client_connected() ? "yes" : "no");
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 3) {
        if (strcmp(argv[2], "auto") == 0) {
            // 切回自动发现:mDNS 可覆盖运行时 URL(不写回 NVS URL)
            esp_err_t e = nvs_settings_set_ws_mode(true);
            printf("ws mode: %s\n", e == ESP_OK ? "auto (mDNS)" : esp_err_to_name(e));
            return 0;
        }
        nvs_settings_set_ws_mode(false);   // 显式 URL → static(用户显式优先)
        esp_err_t e = ws_client_reinit(argv[2]);
        printf("ws set: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
        return 0;
    }
    printf("usage: ws set <url> | ws set auto | ws status\n");
    return 1;
}

// ---- mdns:手动触发解析 ----
static int cmd_mdns(int argc, char **argv)
{
    (void)argc; (void)argv;
    mdns_resolver_request();
    printf("mDNS 解析已触发(auto 模式);目标变化时自动重连 WS\n");
    return 0;
}

// ---- st:状态一览 ----
static int cmd_st(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("--- system ---\n");
    printf("heap free: %d B (min %d B), largest free block: %d B\n",
           (int)esp_get_free_heap_size(), (int)esp_get_minimum_free_heap_size(),
           (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // 全任务栈高水位枚举(F3:覆盖所有任务,替代只查两个的固定列表;需
    // CONFIG_FREERTOS_USE_TRACE_FACILITY=y 提供 uxTaskGetSystemState)。
    // 控制台低频调试命令,一次性 ~百字节堆分配可接受(不驻留)。
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *sts = heap_caps_malloc((size_t)n * sizeof(TaskStatus_t),
                                         MALLOC_CAP_8BIT);
    if (!sts) {
        printf("tasks: %u (状态数组分配失败)\n", (unsigned)n);
    } else {
        n = uxTaskGetSystemState(sts, n, NULL);
        printf("tasks: %u\n", (unsigned)n);
        printf("%-18s %-10s %10s\n", "name", "state", "hwm(B)");
        static const char *state_name[] = { "running", "ready", "blocked",
                                            "suspended", "deleted", "invalid" };
        for (UBaseType_t i = 0; i < n; i++) {
            const char *s = sts[i].eCurrentState <= eDeleted
                                ? state_name[sts[i].eCurrentState] : "invalid";
            printf("%-18s %-10s %10u\n", sts[i].pcTaskName, s,
                   (unsigned)sts[i].usStackHighWaterMark);
        }
        free(sts);
    }
    printf("--- link ---\n");
    printf("mode: %s link: %s\n", mode_name(mode_get()), mode_link_up() ? "up" : "down");
    printf("wifi: %s ip: %s\n", wifi_app_connected() ? "connected" : "disconnected",
           wifi_app_ip()[0] ? wifi_app_ip() : "(none)");
    printf("ws: %s\n", ws_client_connected() ? "connected" : "disconnected");
    printf("--- ble ---\n");
    printf("connected: %s event_subscribed: %s mtu: %u\n",
           ble_audio_connected() ? "yes" : "no",
           ble_audio_event_subscribed() ? "yes" : "no",
           (unsigned)ble_audio_mtu());
    printf("drops: audio %u event %u\n",
           (unsigned)ble_audio_audio_drops(), (unsigned)ble_audio_event_drops());
    printf("--- audio ---\n");
    printf("streaming: %s peak: %d\n",
           audio_streamer_active() ? "yes" : "no", (int)audio_streamer_peak());
    printf("--- battery ---\n");
    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    printf("soc: %d%% mv: %d\n", soc < 0 ? -1 : soc, mv < 0 ? -1 : mv);
    return 0;
}

// ---- reboot / factory ----
static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting...\n");
    esp_restart();
    return 0;
}

static int cmd_factory(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("clearing NVS and rebooting...\n");
    nvs_settings_factory_reset();
    esp_restart();
    return 0;
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
    reg("mode", "射频模式:mode | mode ble | mode wifi(切换需几秒,期间链路中断)", NULL, cmd_mode);
    reg("wifi", "Wi-Fi 配置:wifi status | wifi get | wifi set <ssid> <pass>", NULL, cmd_wifi);
    reg("ws", "WS 目标:ws status | ws set <url> | ws set auto", NULL, cmd_ws);
    reg("mdns", "手动触发 mDNS 解析", NULL, cmd_mdns);
    reg("st", "系统状态一览(模式/双链路/MTU/掉帧/堆)", NULL, cmd_st);
    reg("reboot", "重启设备", NULL, cmd_reboot);
    reg("factory", "清空 NVS 并重启", NULL, cmd_factory);
    return ESP_OK;
}
