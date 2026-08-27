// main/time_sync.c —— wall-clock 校时(校时源仅电脑客户端)。
// 三通道下发统一入口:BLE/WS 协议行 time.set、USB SYS 命令 time set、REPL 手动。
// 状态:重启后未校时(valid=false);set_epoch 置位后本会话有效。
// 时区:仅小时偏移,NVS 键 tz_hour(int8,默认 +8,±12 校验),不引入 TZ env。
#include "time_sync.h"
#include "nvs_settings.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>  // settimeofday
#include <stdio.h>

static const char *TAG = "time_sync";

#define TZ_DEFAULT_HOUR 8
#define TZ_MAX_HOUR     12

// s_epoch 为校时基线(单写点:set_epoch);settimeofday 同步系统时钟并持续
// 走时 —— 设备端显示读 time(NULL)(见 format_local),否则永远冻结在校时
// 时刻(真机实测 13:46 不再走动)。宿主测试无系统时钟,按 s_epoch 直接格式化。
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

// 本地时间 = 当前系统时钟 + tz*3600,按 UTC 读(gmtime_r 手动偏移,不依赖
// TZ env)。设备端系统时钟 set_epoch 时已 settimeofday,持续走时;宿主测试
// 无系统时钟,按 s_epoch 基线直接格式化(确定性基准)。返回写入长度
// (未校时固定 "--:--" 5 字符)。
int time_sync_format_local(char *buf, size_t cap)
{
    if (!s_valid) return snprintf(buf, cap, "--:--");
    time_t t;
#ifdef ESP_PLATFORM
    time_t now = time(NULL);
    if (now < s_epoch) now = (time_t)s_epoch;   // settimeofday 未生效兜底:冻结在基线
    t = now + (int64_t)s_tz_hour * 3600;
#else
    t = (time_t)(s_epoch + (int64_t)s_tz_hour * 3600);
#endif
    struct tm tm;
    gmtime_r(&t, &tm);
    return snprintf(buf, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}
