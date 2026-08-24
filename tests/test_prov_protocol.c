// 配网协议主机测试:解析 + 序列化 + 512B 上限 + 截断(与 test_app_protocol 同构)。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "prov_protocol.h"

static void test_parse_ok(void) {
    const char *j = "{\"cmd\":\"wifi_set\",\"ssid\":\"MyNet\",\"pass\":\"wpa2\"}";
    prov_wifi_set_t w;
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_OK);
    assert(strcmp(w.ssid, "MyNet") == 0);
    assert(strcmp(w.pass, "wpa2") == 0);
}

static void test_parse_open_network(void) {
    // 空 pass = 开放网络;pass 字段缺省等同空
    const char *j = "{\"cmd\":\"wifi_set\",\"ssid\":\"Cafe-Guest\",\"pass\":\"\"}";
    prov_wifi_set_t w;
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_OK);
    assert(w.pass[0] == '\0');

    const char *j2 = "{\"cmd\":\"wifi_set\",\"ssid\":\"Cafe-Guest\"}";
    assert(prov_protocol_parse(j2, strlen(j2), &w) == PROV_PARSE_OK);
    assert(w.pass[0] == '\0');
}

static void test_parse_length_bounds(void) {
    prov_wifi_set_t w;

    // ssid 32 恰好合法;33 超限
    char j[256];
    char ssid33[34];
    memset(ssid33, 's', 33);
    ssid33[33] = '\0';
    snprintf(j, sizeof(j), "{\"cmd\":\"wifi_set\",\"ssid\":\"%s\",\"pass\":\"\"}", ssid33);
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_FIELD_INVALID);

    char ssid32[33];
    memset(ssid32, 's', 32);
    ssid32[32] = '\0';
    snprintf(j, sizeof(j), "{\"cmd\":\"wifi_set\",\"ssid\":\"%s\",\"pass\":\"\"}", ssid32);
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_OK);
    assert(strlen(w.ssid) == 32);

    // pass 63 恰好合法;64 超限
    char pass64[65];
    memset(pass64, 'p', 64);
    pass64[64] = '\0';
    snprintf(j, sizeof(j), "{\"cmd\":\"wifi_set\",\"ssid\":\"n\",\"pass\":\"%s\"}", pass64);
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_FIELD_INVALID);

    char pass63[64];
    memset(pass63, 'p', 63);
    pass63[63] = '\0';
    snprintf(j, sizeof(j), "{\"cmd\":\"wifi_set\",\"ssid\":\"n\",\"pass\":\"%s\"}", pass63);
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_OK);
    assert(strlen(w.pass) == 63);
}

static void test_parse_empty_ssid(void) {
    const char *j = "{\"cmd\":\"wifi_set\",\"ssid\":\"\",\"pass\":\"x\"}";
    prov_wifi_set_t w;
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_FIELD_INVALID);
}

static void test_parse_unknown_cmd(void) {
    const char *j = "{\"cmd\":\"firmware_wipe\",\"ssid\":\"n\"}";
    prov_wifi_set_t w;
    assert(prov_protocol_parse(j, strlen(j), &w) == PROV_PARSE_UNKNOWN_CMD);
}

static void test_parse_malformed(void) {
    prov_wifi_set_t w;
    assert(prov_protocol_parse("not json at all", 15, &w) == PROV_PARSE_BAD_JSON);
    assert(prov_protocol_parse("{\"cmd\":", 8, &w) == PROV_PARSE_BAD_JSON);   // 截断
    assert(prov_protocol_parse("", 0, &w) == PROV_PARSE_BAD_JSON);            // 空载荷
    assert(prov_protocol_parse("{\"cmd\":\"wifi_set\"}", 19, &w) == PROV_PARSE_FIELD_INVALID);  // 缺 ssid
    assert(prov_protocol_parse("{\"cmd\":\"wifi_set\",\"ssid\":123}", 31, &w) == PROV_PARSE_FIELD_INVALID);  // ssid 非字符串
    assert(prov_protocol_parse(NULL, 10, &w) == PROV_PARSE_BAD_JSON);
}

static void test_parse_too_long(void) {
    // 长度检查先于 JSON 解析:513B → TOO_LONG(内容无关)
    char j[600];
    memset(j, 'x', sizeof(j));
    prov_wifi_set_t w;
    assert(prov_protocol_parse(j, 513, &w) == PROV_PARSE_TOO_LONG);

    // 恰好 512B 且语法完整的载荷 → 长度放行,进入字段校验(ssid 473 字符超长)
    memset(j, 'a', sizeof(j));
    memcpy(j, "{\"cmd\":\"wifi_set\",\"ssid\":\"", 27);
    memcpy(j + 27 + 473, "\",\"pass\":\"\"}", 12);   // 闭合串,ssid 内容恰 473B
    size_t n = 27 + 473 + 12;
    assert(n == 512);
    assert(prov_protocol_parse(j, n, &w) == PROV_PARSE_FIELD_INVALID);
}

static void test_serialize_ok(void) {
    char buf[128];
    size_t n = prov_protocol_result_ok(buf, sizeof(buf), "192.168.1.5");
    assert(n > 0);
    assert(strstr(buf, "\"status\":\"ok\"") != NULL);
    assert(strstr(buf, "\"ip\":\"192.168.1.5\"") != NULL);
    assert(buf[n - 1] == '\n');
    assert(buf[n] == '\0');

    // ip 为空 → 省略 ip 字段
    n = prov_protocol_result_ok(buf, sizeof(buf), "");
    assert(strstr(buf, "\"ip\"") == NULL);
}

static void test_serialize_error(void) {
    char buf[128];
    size_t n = prov_protocol_result_error(buf, sizeof(buf), PROV_ERR_AUTH_FAIL, "reason=202");
    assert(n > 0);
    assert(strstr(buf, "\"status\":\"error\"") != NULL);
    assert(strstr(buf, "\"code\":\"auth_fail\"") != NULL);
    assert(strstr(buf, "\"detail\":\"reason=202\"") != NULL);
    assert(buf[n - 1] == '\n');

    // 全部错误码文本
    assert(strcmp(prov_error_text(PROV_ERR_NVS_ERROR), "nvs_error") == 0);
    assert(strcmp(prov_error_text(PROV_ERR_NO_AP), "no_ap") == 0);
    assert(strcmp(prov_error_text(PROV_ERR_AUTH_FAIL), "auth_fail") == 0);
    assert(strcmp(prov_error_text(PROV_ERR_ASSOC_FAIL), "assoc_fail") == 0);
    assert(strcmp(prov_error_text(PROV_ERR_TIMEOUT), "timeout") == 0);
    assert(strcmp(prov_error_text(PROV_ERR_OTHER), "other") == 0);
    assert(strcmp(prov_error_text((prov_error_t)99), "other") == 0);   // 越界兜底
}

static void test_serialize_small_cap(void) {
    char buf[16];
    size_t n = prov_protocol_result_error(buf, sizeof(buf), PROV_ERR_TIMEOUT, "slow");
    assert(n > 0 && n < sizeof(buf));
    assert(buf[n - 1] == '\n');   // 截断仍保行分隔
    assert(buf[n] == '\0');
    assert(prov_protocol_result_ok(buf, 0, "ip") == 0);   // cap 0 → 0
}

int main(void) {
    test_parse_ok();
    test_parse_open_network();
    test_parse_length_bounds();
    test_parse_empty_ssid();
    test_parse_unknown_cmd();
    test_parse_malformed();
    test_parse_too_long();
    test_serialize_ok();
    test_serialize_error();
    test_serialize_small_cap();
    printf("test_prov_protocol: all assertions passed\n");
    return 0;
}
