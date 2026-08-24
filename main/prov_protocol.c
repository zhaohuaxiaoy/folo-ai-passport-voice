// main/prov_protocol.c —— BLE 配网协议实现。
// 契约见 design.md 协议小节;任何解析失败只返回错误码,绝不崩溃。
#include "prov_protocol.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static const char *const ERR_TEXTS[PROV_ERR_COUNT] = {
    [PROV_ERR_NVS_ERROR]  = "nvs_error",
    [PROV_ERR_NO_AP]      = "no_ap",
    [PROV_ERR_AUTH_FAIL]  = "auth_fail",
    [PROV_ERR_ASSOC_FAIL] = "assoc_fail",
    [PROV_ERR_TIMEOUT]    = "timeout",
    [PROV_ERR_OTHER]      = "other",
};

const char *prov_error_text(prov_error_t code) {
    if ((unsigned)code < PROV_ERR_COUNT) return ERR_TEXTS[code];
    return "other";
}

prov_parse_result_t prov_protocol_parse(const char *json, size_t len, prov_wifi_set_t *out) {
    if (!json || !out || len == 0) return PROV_PARSE_BAD_JSON;
    if (len > PROV_PAYLOAD_MAX) return PROV_PARSE_TOO_LONG;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return PROV_PARSE_BAD_JSON;

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd)) { cJSON_Delete(root); return PROV_PARSE_BAD_JSON; }
    if (strcmp(cmd->valuestring, "wifi_set") != 0) {
        cJSON_Delete(root);
        return PROV_PARSE_UNKNOWN_CMD;
    }

    // ssid 必填且 1..32
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(ssid)) { cJSON_Delete(root); return PROV_PARSE_FIELD_INVALID; }
    size_t ssid_len = strlen(ssid->valuestring);
    if (ssid_len == 0 || ssid_len > PROV_SSID_MAX) {
        cJSON_Delete(root);
        return PROV_PARSE_FIELD_INVALID;
    }

    // pass 可选,0..63;缺省 = 开放网络
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
    const char *pass_str = cJSON_IsString(pass) ? pass->valuestring : "";
    if (strlen(pass_str) > PROV_PASS_MAX) {
        cJSON_Delete(root);
        return PROV_PARSE_FIELD_INVALID;
    }

    strncpy(out->ssid, ssid->valuestring, sizeof(out->ssid) - 1);
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    strncpy(out->pass, pass_str, sizeof(out->pass) - 1);
    out->pass[sizeof(out->pass) - 1] = '\0';
    cJSON_Delete(root);
    return PROV_PARSE_OK;
}

// ---- 序列化(与 app_protocol.c 同款截断模式) ----
static size_t serialize(cJSON *root, char *buf, size_t cap) {
    if (!root || !buf || cap < 2) return 0;   // cap<2 时 (cap-2) 下溢成超大 memcpy
    char *s = cJSON_PrintUnformatted(root);
    if (!s) return 0;                         // 序列化失败(OOM):不发残缺帧
    size_t n = strlen(s);
    if (n + 2 > cap) n = cap - 2;             // 截断保护(结果帧远小于 cap)
    memcpy(buf, s, n);
    free(s);
    buf[n++] = '\n';                          // 行分隔
    buf[n] = '\0';
    return n;
}

size_t prov_protocol_result_ok(char *buf, size_t cap, const char *ip) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "wifi_set");
    cJSON_AddStringToObject(o, "status", "ok");
    if (ip && ip[0]) cJSON_AddStringToObject(o, "ip", ip);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t prov_protocol_result_error(char *buf, size_t cap, prov_error_t code, const char *detail) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "wifi_set");
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "code", prov_error_text(code));
    if (detail && detail[0]) cJSON_AddStringToObject(o, "detail", detail);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}
