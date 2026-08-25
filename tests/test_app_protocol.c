// 协议主机测试:解析 + 序列化(cJSON 内置在 tests/third_party/cJSON,无需 ESP-IDF)。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_protocol.h"
#include "app_types.h"

static void test_parse_metrics(void) {
    const char *j = "{\"type\":\"mac.metrics\",\"cpu\":14.2,\"ram\":38.7,"
                    "\"battery\":87,\"charging\":false,\"activeApp\":\"Code\"}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.type == APP_EV_MAC_METRICS);
    assert(ev.u.metrics.cpu == 14);
    assert(ev.u.metrics.ram == 38);   // 38.7 截断为 38(不四舍五入)
    assert(ev.u.metrics.battery == 87);
    assert(ev.u.metrics.charging == false);
    assert(strcmp(ev.u.metrics.active_app, "Code") == 0);
}

static void test_parse_metrics_bounds(void) {
    const char *j = "{\"type\":\"mac.metrics\",\"cpu\":150,\"ram\":-5}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.u.metrics.cpu == 100);
    assert(ev.u.metrics.ram == 0);
}

static void test_parse_agent_status(void) {
    const char *j = "{\"type\":\"agent.status\",\"state\":\"running\",\"message\":\"unit tests 18/24\"}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.type == APP_EV_AGENT_STATUS);
    assert(ev.u.agent_status.state == APP_AGENT_RUNNING);
    assert(strcmp(ev.u.agent_status.message, "unit tests 18/24") == 0);

    const char *j2 = "{\"type\":\"agent.status\",\"state\":\"done\"}";
    assert(app_protocol_parse(j2, strlen(j2), &ev));
    assert(ev.u.agent_status.state == APP_AGENT_DONE);

    const char *j3 = "{\"type\":\"agent.status\",\"state\":\"nonsense\"}";   // 未知状态丢弃
    assert(!app_protocol_parse(j3, strlen(j3), &ev));
}

static void test_parse_approval(void) {
    const char *j = "{\"type\":\"agent.approval_request\",\"taskId\":\"task_9821\","
                    "\"title\":\"Modify 3 files\",\"target\":\"OrderService.java\","
                    "\"diffSummary\":\"+128 / -37\",\"riskLevel\":\"high\"}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.type == APP_EV_APPROVAL_REQUEST);
    assert(strcmp(ev.u.approval.task_id, "task_9821") == 0);
    assert(strcmp(ev.u.approval.title, "Modify 3 files") == 0);
    assert(strcmp(ev.u.approval.target, "OrderService.java") == 0);
    assert(strcmp(ev.u.approval.diff_summary, "+128 / -37") == 0);
    assert(ev.u.approval.risk == APP_RISK_HIGH);

    const char *j2 = "{\"type\":\"agent.approval_request\",\"taskId\":\"t\",\"title\":\"x\",\"riskLevel\":\"low\"}";
    assert(app_protocol_parse(j2, strlen(j2), &ev));
    assert(ev.u.approval.risk == APP_RISK_LOW);
    assert(strcmp(ev.u.approval.diff_summary, "") == 0);   // 缺省字段为空

    const char *j3 = "{\"type\":\"agent.approval_request\",\"title\":\"no taskId\"}";
    assert(!app_protocol_parse(j3, strlen(j3), &ev));       // 缺必填 → 丢弃
}

static void test_parse_transcript(void) {
    const char *j = "{\"type\":\"transcript\",\"text\":\"fix the bug\",\"inject_mode\":\"type\"}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.type == APP_EV_TRANSCRIPT);
    assert(strcmp(ev.u.transcript.text, "fix the bug") == 0);
    assert(ev.u.transcript.inject_mode == APP_INJECT_TYPE);

    const char *j2 = "{\"type\":\"transcript\",\"text\":\"你好世界\",\"inject_mode\":\"paste\"}";
    assert(app_protocol_parse(j2, strlen(j2), &ev));
    assert(strcmp(ev.u.transcript.text, "你好世界") == 0);
    assert(ev.u.transcript.inject_mode == APP_INJECT_PASTE);

    const char *j3 = "{\"type\":\"transcript\",\"text\":\"no mode\"}";       // 缺省 type
    assert(app_protocol_parse(j3, strlen(j3), &ev));
    assert(ev.u.transcript.inject_mode == APP_INJECT_TYPE);
    assert(ev.u.transcript.final == false);                    // 缺省 final = 预览态

    const char *j4 = "{\"type\":\"transcript\",\"text\":\"定稿\",\"final\":true}";
    assert(app_protocol_parse(j4, strlen(j4), &ev));
    assert(ev.u.transcript.final == true);                     // 定稿标记

    const char *j5 = "{\"type\":\"transcript\",\"text\":\"预览\",\"final\":false}";
    assert(app_protocol_parse(j5, strlen(j5), &ev));
    assert(ev.u.transcript.final == false);                    // 显式 false 仍预览
}

static void test_parse_time_set(void) {
    const char *j = "{\"type\":\"time.set\",\"epoch\":1767225600}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.type == APP_EV_TIME_SET);
    assert(ev.u.time_set.epoch == 1767225600LL);

    // 负 epoch(1970 前,UTC 秒)
    const char *j2 = "{\"type\":\"time.set\",\"epoch\":-1}";
    assert(app_protocol_parse(j2, strlen(j2), &ev));
    assert(ev.type == APP_EV_TIME_SET);
    assert(ev.u.time_set.epoch == -1);

    // 缺 epoch / 非数字 / 超 int64 范围:拒绝
    const char *j3 = "{\"type\":\"time.set\"}";
    assert(!app_protocol_parse(j3, strlen(j3), &ev));
    const char *j4 = "{\"type\":\"time.set\",\"epoch\":\"abc\"}";
    assert(!app_protocol_parse(j4, strlen(j4), &ev));
    const char *j5 = "{\"type\":\"time.set\",\"epoch\":1e300}";
    assert(!app_protocol_parse(j5, strlen(j5), &ev));
}

static void test_parse_rejects(void) {
    app_event_t ev;
    assert(!app_protocol_parse("{\"type\":\"nope\"}", 14, &ev));             // 未知 type
    assert(!app_protocol_parse("not json", 8, &ev));                         // 畸形
    assert(!app_protocol_parse("{\"type\":1}", 11, &ev));                    // type 非字符串
    assert(!app_protocol_parse("", 0, &ev));                                 // 空
}

// 深层嵌套拒绝:解析在 NimBLE host task(4KB 栈)上下文,cJSON 递归无深度限制,
// 预检拦截(见 app_protocol.c json_depth_ok)——否则 2KB 内 [[[[...]]]] 可爆栈。
static void test_parse_deep_nesting_rejected(void) {
    app_event_t ev;

    // 对象 200 层嵌套:拒绝
    char deep_obj[2100];
    size_t n = 0;
    for (int i = 0; i < 200; i++) deep_obj[n++] = '{';
    deep_obj[n++] = '1';
    for (int i = 0; i < 200; i++) deep_obj[n++] = '}';
    deep_obj[n] = '\0';
    assert(!app_protocol_parse(deep_obj, n, &ev));

    // 数组 200 层嵌套:拒绝
    char deep_arr[2100];
    n = 0;
    for (int i = 0; i < 200; i++) deep_arr[n++] = '[';
    deep_arr[n++] = '1';
    for (int i = 0; i < 200; i++) deep_arr[n++] = ']';
    deep_arr[n] = '\0';
    assert(!app_protocol_parse(deep_arr, n, &ev));

    // 32 层内:正常解析不误伤(最深 2 层)
    const char *ok = "{\"type\":\"agent.status\",\"state\":\"running\","
                     "\"message\":\"{\\\"nested\\\":[1,2,{\\\"a\\\":3}]}\"}";
    assert(app_protocol_parse(ok, strlen(ok), &ev));
    assert(ev.type == APP_EV_AGENT_STATUS);
    assert(strcmp(ev.u.agent_status.message, "{\"nested\":[1,2,{\"a\":3}]}") == 0);

    // 字符串内的 { [ 不计数(误判会拒绝正常含大括号文本的载荷)
    const char *str_braces = "{\"type\":\"transcript\",\"text\":\"a { b [ c } d ] e\"}";
    assert(app_protocol_parse(str_braces, strlen(str_braces), &ev));
    assert(ev.type == APP_EV_TRANSCRIPT);
    assert(strcmp(ev.u.transcript.text, "a { b [ c } d ] e") == 0);

    // 转义引号内的 { 也不计数:"{\"a\":\"\\\"{[[[\"} 形式
    const char *esc_quotes = "{\"type\":\"transcript\",\"text\":\"\\\"{[[[\"}";
    assert(app_protocol_parse(esc_quotes, strlen(esc_quotes), &ev));
    assert(ev.type == APP_EV_TRANSCRIPT);
    assert(strcmp(ev.u.transcript.text, "\"{[[[") == 0);

    // 未闭合括号(畸形)不因预检崩溃,仍走 cJSON 拒绝路径
    assert(!app_protocol_parse("{\"type\":\"transcript\",\"text\":\"x\"", 32, &ev));
}

static void test_parse_long_line_truncation(void) {
    // 超长 transcript 行:解析失败(超过 RX 上限),而不是截断破坏内存
    char big[2100];
    memset(big, 'x', sizeof(big) - 2);
    big[0] = '{'; big[1] = '"';
    big[sizeof(big) - 2] = '}';
    big[sizeof(big) - 1] = '\0';
    app_event_t ev;
    assert(!app_protocol_parse(big, strlen(big), &ev));
}

static void test_serialize(void) {
    char buf[APP_PROTO_TX_CAP];
    size_t n;

    n = app_protocol_device_hello(buf, sizeof(buf), 1);
    assert(n > 0 && buf[n - 1] == '\n');
    assert(strstr(buf, "\"event\":\"device.hello\""));
    assert(strstr(buf, "\"proto\":1"));

    n = app_protocol_voice_start(buf, sizeof(buf), APP_WF_BUILD);
    assert(n > 0);
    assert(strstr(buf, "\"event\":\"voice.start\""));
    assert(strstr(buf, "\"workflow\":\"build\""));

    n = app_protocol_voice_end(buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"event\":\"voice.end\""));

    n = app_protocol_key_action(buf, sizeof(buf), APP_KEY_ENTER);
    assert(n > 0);
    assert(strstr(buf, "\"event\":\"key.action\""));
    assert(strstr(buf, "\"action\":\"enter\""));

    n = app_protocol_key_action(buf, sizeof(buf), APP_KEY_CLEAR);
    assert(n > 0);
    assert(strstr(buf, "\"event\":\"key.action\""));
    assert(strstr(buf, "\"action\":\"clear\""));

    n = app_protocol_agent_action(buf, sizeof(buf), "task_9821", APP_ACTION_APPROVE);
    assert(n > 0);
    assert(strstr(buf, "\"taskId\":\"task_9821\""));
    assert(strstr(buf, "\"action\":\"approve\""));

    n = app_protocol_agent_action(buf, sizeof(buf), "t1", APP_ACTION_REJECT);
    assert(strstr(buf, "\"action\":\"reject\""));

    n = app_protocol_agent_action(buf, sizeof(buf), "t1", APP_ACTION_DETAILS);
    assert(strstr(buf, "\"action\":\"details\""));
}

static void test_serialize_small_cap(void) {
    char buf[64];
    // cap < 2:返回 0(不得下溢成超大 memcpy)
    assert(app_protocol_device_hello(buf, 0, 1) == 0);
    assert(app_protocol_device_hello(buf, 1, 1) == 0);
    assert(app_protocol_voice_start(buf, 1, APP_WF_BUILD) == 0);
    // 小 cap:截断但仍以 \n 行分隔结尾、有 NUL
    size_t n = app_protocol_device_hello(buf, 16, 1);
    assert(n > 0 && n < 16);
    assert(buf[n - 1] == '\n');
    assert(buf[n] == '\0');
}

int main(void) {
    test_parse_metrics();
    test_parse_metrics_bounds();
    test_parse_agent_status();
    test_parse_approval();
    test_parse_transcript();
    test_parse_time_set();
    test_parse_rejects();
    test_parse_deep_nesting_rejected();
    test_parse_long_line_truncation();
    test_serialize();
    test_serialize_small_cap();
    printf("test_app_protocol: all assertions passed\n");
    return 0;
}
