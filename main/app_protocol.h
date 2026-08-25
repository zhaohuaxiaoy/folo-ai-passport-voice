// main/app_protocol.h —— 设备↔Mac JSON 协议(行分隔)。
// 纯 C + cJSON(ESP-IDF 自带,宿主机测试直接编 $IDF_PATH/components/json)。
#pragma once

#include "app_types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PROTO_TX_CAP 512   // 序列化缓冲上限
#define APP_PROTO_RX_CAP 2048  // 解析行上限

// 解析 Mac→设备 的 JSON 行。成功填 ev 并返回 true;未知 type / 畸形 / 非法字段 → false。
bool app_protocol_parse(const char *json, size_t len, app_event_t *ev);

// 设备→Mac 序列化。返回写入字节数(不含 NUL);失败返回 0。
size_t app_protocol_device_hello(char *buf, size_t cap, int proto);
size_t app_protocol_voice_start(char *buf, size_t cap, app_workflow_t wf);
size_t app_protocol_voice_end(char *buf, size_t cap);
// 上行按键动作(enter/clear,PC client 执行注入;见 key-remap 任务)
size_t app_protocol_key_action(char *buf, size_t cap, app_key_action_t action);
// decision: app_approval_decision_t (approve/reject/details)
size_t app_protocol_agent_action(char *buf, size_t cap, const char *task_id,
                                 uint8_t decision);
// voice.end 后补发的会话对账帧: {"event":"status","drop":n}(掉帧对账,见 design.md)
size_t app_protocol_device_status(char *buf, size_t cap, uint32_t drop_count);

#ifdef __cplusplus
}
#endif
