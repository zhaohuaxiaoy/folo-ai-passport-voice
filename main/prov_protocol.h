// main/prov_protocol.h —— BLE 配网协议(纯 C + cJSON,宿主机可直接测试)。
// 契约见 .trellis/tasks/08-24-ai-passport-ble-provisioning/design.md 协议小节。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROV_PAYLOAD_MAX 512   // PROV_CMD 载荷上限
#define PROV_SSID_MAX    32    // ssid 1..32(不含 NUL)
#define PROV_PASS_MAX    63    // pass 0..63,空 = 开放网络

// 解析结果:OK 之外全是错误;BAD_JSON/UNKNOWN_CMD/FIELD_INVALID/TOO_LONG。
typedef enum {
    PROV_PARSE_OK = 0,
    PROV_PARSE_BAD_JSON,       // 畸形 JSON / 空载荷 / 非对象根
    PROV_PARSE_UNKNOWN_CMD,    // cmd 不是已知命令
    PROV_PARSE_FIELD_INVALID,  // 字段缺失/类型错/ssid 超 32/pass 超 63
    PROV_PARSE_TOO_LONG,       // 载荷超 512B
} prov_parse_result_t;

// 解析成功后的凭据(自包含,可直接进事件队列)。
typedef struct {
    char ssid[PROV_SSID_MAX + 1];
    char pass[PROV_PASS_MAX + 1];
} prov_wifi_set_t;

// 配网结果错误码(wire 文本见 prov_error_text)。
typedef enum {
    PROV_ERR_NVS_ERROR = 0,
    PROV_ERR_NO_AP,
    PROV_ERR_AUTH_FAIL,
    PROV_ERR_ASSOC_FAIL,
    PROV_ERR_TIMEOUT,
    PROV_ERR_OTHER,
    PROV_ERR_COUNT,
} prov_error_t;

// 解析 PROV_CMD 载荷。仅返回 PROV_PARSE_OK 时 out 有效。
prov_parse_result_t prov_protocol_parse(const char *json, size_t len, prov_wifi_set_t *out);

// 结果序列化(沿用 app_protocol 的截断模式:行分隔 + NUL)。返回写入字节数(不含 NUL);失败返回 0。
// ok:  {"cmd":"wifi_set","status":"ok","ip":"192.168.1.5"}(ip 为空则省略)
// err: {"cmd":"wifi_set","status":"error","code":"auth_fail","detail":"reason=202"}(detail 空则省略)
size_t prov_protocol_result_ok(char *buf, size_t cap, const char *ip);
size_t prov_protocol_result_error(char *buf, size_t cap, prov_error_t code, const char *detail);

// 错误码 → wire 文本("nvs_error"/"no_ap"/"auth_fail"/"assoc_fail"/"timeout"/"other")。
const char *prov_error_text(prov_error_t code);

#ifdef __cplusplus
}
#endif
