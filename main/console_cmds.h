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
