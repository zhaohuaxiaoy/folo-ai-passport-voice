// main/time_sync.c —— wall-clock 校时(校时源仅电脑客户端)。
// 三通道下发统一入口:BLE/WS 协议行 time.set、USB SYS 命令 time set、REPL 手动。
// 状态:重启后未校时(valid=false);set_epoch 置位后本会话有效。
// 时区:仅小时偏移,NVS 键 tz_hour(int8,默认 +8,±12 校验),不引入 TZ env。
#include "time_sync.h"
#include "nvs_settings.h"
#include "esp_log.h"
#include <time.h>
#include <stdio.h>

static const char *TAG = "time_sync";

#define TZ_DEFAULT_HOUR 8
#define TZ_MAX_HOUR     12

// s_epoch 为唯一真源(单写点:set_epoch);settimeofday 仅顺带同步系统时间
// (未来日志 wall-clock 时间戳选项用),失败忽略 —— valid/UI 不依赖系统时间。
// 宿主测试读此缓存,零系统时间副作用。
static int64_t s_epoch = 0;
static bool s_valid = false;
static int s_tz_hour = TZ_DEFAULT_HOUR;

esp_err_t time_sync_init(void)
{
    int8_t tz = 0;
    nvs_settings_get_tz_hour(&tz);   // 内部已兜底:打不开/损坏/缺省 → 8
    s_tz_hour = (tz >= -TZ_MAX_HOUR && tz <= TZ_MAX_HOUR) ? tz : TZ_DEFAULT_HOUR;
    s_valid = false;
    return ESP_OK;
}

void time_sync_set_epoch(int64_t epoch_sec)
{
    s_epoch = epoch_sec;
    s_valid = true;
#ifdef ESP_PLATFORM
    struct timeval tv = { .tv_sec = (time_t)epoch_sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);   // 失败忽略:UI 走 s_epoch,系统时间仅日志时间戳备用
#endif
}

bool time_sync_valid(void) { return s_valid; }

int time_sync_tz_hour(void) { return s_tz_hour; }

esp_err_t time_sync_set_tz(int hour)
{
    if (hour < -TZ_MAX_HOUR || hour > TZ_MAX_HOUR) {
        ESP_LOGW(TAG, "时区偏移越界 %d(±%d)", hour, TZ_MAX_HOUR);
        return ESP_ERR_INVALID_ARG;
    }
    s_tz_hour = hour;
    return nvs_settings_set_tz_hour((int8_t)hour);
}

// 本地时间 = epoch + tz*3600,按 UTC 读(gmtime_r 手动偏移,不依赖 TZ env)。
// 返回写入长度(未校时固定 "--:--" 5 字符)。
int time_sync_format_local(char *buf, size_t cap)
{
    if (!s_valid) return snprintf(buf, cap, "--:--");
    time_t t = (time_t)(s_epoch + (int64_t)s_tz_hour * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    return snprintf(buf, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}
