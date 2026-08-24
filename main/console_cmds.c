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
#include <stdlib.h>
#include <string.h>

static const char *TAG = "console";

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
