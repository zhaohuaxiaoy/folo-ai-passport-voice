// main/time_sync.h —— wall-clock 校时(校时源仅电脑客户端,双通道 BLE/USB)。
// 设计要点(见 design.md):
//   - s_epoch(单调缓存)为唯一真源:settimeofday 顺带同步系统时间(供未来日志
//     时间戳选项),UI/格式化一律读缓存 —— 宿主测试零系统时间副作用;
//   - 时区仅小时偏移(NVS 存 int8,默认 +8,±12 校验),不做 TZ env;
//   - 重启后未校时:valid=false → UI 显示 "--:--",relay 连接后恢复。
#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t time_sync_init(void);                // 读 NVS 时区(缺省 +8);校时状态=未校时
void time_sync_set_epoch(int64_t epoch_sec);   // 校时(UTC 秒);置 valid + 同步系统时间
bool time_sync_valid(void);                    // 是否已校时(UI "--:--" 判定)
int time_sync_tz_hour(void);                   // 当前时区偏移小时(默认 8)
esp_err_t time_sync_set_tz(int hour);          // 设置时区偏移(±12 校验,NVS 持久化)
int time_sync_format_local(char *buf, size_t cap); // "HH:MM"(校时)或 "--:--"(未校时)

#endif /* TIME_SYNC_H */
