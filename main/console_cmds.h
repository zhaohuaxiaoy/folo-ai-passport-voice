// main/console_cmds.h —— esp_console 命令注册与无 REPL 执行入口。
// 命令实现(st/reboot/factory 等)见 console_cmds.c。双执行路径:
//   1. REPL(BLE 模式):esp_console_cmd_register 注册,esp_console 逐行执行,
//      输出经 emit hook 写 stdout(行为不变);
//   2. SYS 命令面(USB 模式,无 REPL):console_cmds_run_line 分词执行同一批
//      cmd_* 函数,输出捕获进调用方缓冲(经 console_cmds_set_emit 切换)。
#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t console_cmds_register(void);

// 复位原因枚举 → 可读名(esp_reset_reason 的 esp_reset_reason_t)。
// 越界/未知返回 "unknown"。main.c 的启动日志与 rst 命令共用一份表 ——
// 之前两处各写了一份注释版枚举, 且都比 IDF 5.x 的实际取值错位一位
// (真机 2026-08-29: USB 复位报 11, 注释表把 11 写成 SDIO)。
const char *console_reset_reason_str(int reason);

// 输出回调(SYS 帧捕获用;NULL = 缺省写 stdout)。
typedef void (*console_emit_fn_t)(const char *s, size_t n);
void console_cmds_set_emit(console_emit_fn_t fn);

// 无 REPL 执行入口:空格分词 ≤8 参数,查表执行同一批命令。
// 输出写入 out(最多 out_cap-1 字节,超限截断,NUL 结尾)。
// 返回 0 = 命令已执行(含用法错误);1 = 未知命令(未写入 out 或写入错误提示)。
int console_cmds_run_line(const char *line, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
