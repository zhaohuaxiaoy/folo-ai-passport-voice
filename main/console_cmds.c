// main/console_cmds.c —— esp_console 命令实现。
// USB-Serial-JTAG 控制台:查看系统/BLE 状态、重启与出厂复位。
#include "console_cmds.h"
#include "nvs_settings.h"
#include "ble_audio.h"
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

// ---- st:状态一览 ----
static int cmd_st(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("--- system ---\n");
    printf("heap free: %d B (min %d B)\n",
           (int)esp_get_free_heap_size(), (int)esp_get_minimum_free_heap_size());
    printf("tasks: %d\n", (int)uxTaskGetNumberOfTasks());
    // 栈高水位(真机验证项 #3:event_worker 栈 2048 是否够用,st 可查)
    TaskHandle_t t = xTaskGetHandle("app_task");
    printf("app_task hwm: %u B\n",
           t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0);
    t = xTaskGetHandle("event_worker");
    printf("event_worker hwm: %u B\n",
           t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0);
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
    reg("st", "系统状态一览(BLE 链路/MTU/掉帧/堆)", NULL, cmd_st);
    reg("reboot", "重启设备", NULL, cmd_reboot);
    reg("factory", "清空 NVS 并重启", NULL, cmd_factory);
    return ESP_OK;
}
