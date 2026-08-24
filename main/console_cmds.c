// main/console_cmds.c —— esp_console 命令实现。
// USB-Serial-JTAG 控制台:配置 Wi-Fi / WS 目标、查看系统状态、重启与出厂复位。
#include "console_cmds.h"
#include "nvs_settings.h"
#include "wifi_app.h"
#include "ws_client.h"
#include "ble_hid_keyboard.h"
#include "audio_streamer.h"
#include "bsp_battery.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_restart.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "console";

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
        nvs_settings_get_ws_url(url, sizeof(url));
        printf("url: %s\n", url);
        printf("connected: %s\n", ws_client_connected() ? "yes" : "no");
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 3) {
        esp_err_t e = ws_client_reinit(argv[2]);
        printf("ws set: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
        return 0;
    }
    printf("usage: ws set <url> | ws status\n");
    return 1;
}

// ---- st:状态一览 ----
static int cmd_st(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("--- system ---\n");
    printf("heap free: %d B (min %d B)\n",
           (int)esp_get_free_heap_size(), (int)esp_get_minimum_free_heap_size());
    printf("tasks: %d\n", (int)uxTaskGetNumberOfTasks());
    printf("--- wifi ---\n");
    printf("connected: %s ip: %s\n", wifi_app_connected() ? "yes" : "no",
           wifi_app_ip()[0] ? wifi_app_ip() : "(none)");
    printf("--- ws ---\n");
    printf("connected: %s\n", ws_client_connected() ? "yes" : "no");
    printf("--- ble ---\n");
    printf("connected: %s ready(subscribed): %s queue: %d\n",
           ble_hid_keyboard_connected() ? "yes" : "no",
           ble_hid_keyboard_ready() ? "yes" : "no",
           (int)ble_hid_keyboard_queue_len());
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
    reg("wifi", "Wi-Fi 设置/状态",
        "set <ssid> <pass> | get | status", cmd_wifi);
    reg("ws", "WebSocket 目标设置/状态",
        "set <url> | status", cmd_ws);
    reg("st", "系统状态一览", NULL, cmd_st);
    reg("reboot", "重启设备", NULL, cmd_reboot);
    reg("factory", "清空 NVS 并重启", NULL, cmd_factory);
    return ESP_OK;
}
