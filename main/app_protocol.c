// main/app_protocol.c —— 协议实现。
// 字段契约见 prd.md 协议小节;任何解析失败只返回 false,绝不崩溃。
#include "app_protocol.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

// wire 契约用小写(build/debug/...);UI 显示用 APP_WORKFLOW_NAMES(大写)区分。
static const char *const WF_WIRE_NAMES[APP_WF_COUNT] = {
    [APP_WF_BUILD]   = "build",
    [APP_WF_DEBUG]   = "debug",
    [APP_WF_REVIEW]  = "review",
    [APP_WF_TEST]    = "test",
    [APP_WF_CAPTURE] = "capture",
};

static void str_take(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

// riskLevel: "low" | "medium" | "high",缺省 medium
static uint8_t parse_risk(const cJSON *o) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(o, "riskLevel");
    if (cJSON_IsString(r)) {
        if (strcmp(r->valuestring, "low") == 0)  return APP_RISK_LOW;
        if (strcmp(r->valuestring, "high") == 0) return APP_RISK_HIGH;
    }
    return APP_RISK_MEDIUM;
}

static bool parse_metrics(const cJSON *o, app_event_t *ev) {
    const cJSON *cpu = cJSON_GetObjectItemCaseSensitive(o, "cpu");
    const cJSON *ram = cJSON_GetObjectItemCaseSensitive(o, "ram");
    if (!cJSON_IsNumber(cpu) || !cJSON_IsNumber(ram)) return false;
    ev->u.metrics.cpu    = (uint8_t)(cpu->valuedouble < 0 ? 0 : cpu->valuedouble > 100 ? 100 : cpu->valuedouble);
    ev->u.metrics.ram    = (uint8_t)(ram->valuedouble < 0 ? 0 : ram->valuedouble > 100 ? 100 : ram->valuedouble);
    const cJSON *batt = cJSON_GetObjectItemCaseSensitive(o, "battery");
    ev->u.metrics.battery = cJSON_IsNumber(batt)
        ? (uint8_t)(batt->valuedouble < 0 ? 0 : batt->valuedouble > 100 ? 100 : batt->valuedouble) : 0;
    const cJSON *chg = cJSON_GetObjectItemCaseSensitive(o, "charging");
    ev->u.metrics.charging = cJSON_IsBool(chg) ? cJSON_IsTrue(chg) : false;
    const cJSON *app = cJSON_GetObjectItemCaseSensitive(o, "activeApp");
    str_take(ev->u.metrics.active_app, sizeof(ev->u.metrics.active_app),
             cJSON_IsString(app) ? app->valuestring : "");
    ev->type = APP_EV_MAC_METRICS;
    return true;
}

static bool parse_agent_status(const cJSON *o, app_event_t *ev) {
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "state");
    if (!cJSON_IsString(st)) return false;
    uint8_t s;
    if      (strcmp(st->valuestring, "ready") == 0)    s = APP_AGENT_READY;
    else if (strcmp(st->valuestring, "thinking") == 0) s = APP_AGENT_THINKING;
    else if (strcmp(st->valuestring, "running") == 0)  s = APP_AGENT_RUNNING;
    else if (strcmp(st->valuestring, "error") == 0)    s = APP_AGENT_ERROR;
    else if (strcmp(st->valuestring, "done") == 0)     s = APP_AGENT_DONE;
    else return false;
    ev->u.agent_status.state = s;
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(o, "message");
    str_take(ev->u.agent_status.message, sizeof(ev->u.agent_status.message),
             cJSON_IsString(msg) ? msg->valuestring : "");
    ev->type = APP_EV_AGENT_STATUS;
    return true;
}

static bool parse_approval(const cJSON *o, app_event_t *ev) {
    const cJSON *id  = cJSON_GetObjectItemCaseSensitive(o, "taskId");
    const cJSON *ti  = cJSON_GetObjectItemCaseSensitive(o, "title");
    if (!cJSON_IsString(id) || !cJSON_IsString(ti)) return false;
    str_take(ev->u.approval.task_id, sizeof(ev->u.approval.task_id), id->valuestring);
    str_take(ev->u.approval.title, sizeof(ev->u.approval.title), ti->valuestring);
    const cJSON *tg = cJSON_GetObjectItemCaseSensitive(o, "target");
    str_take(ev->u.approval.target, sizeof(ev->u.approval.target),
             cJSON_IsString(tg) ? tg->valuestring : "");
    const cJSON *df = cJSON_GetObjectItemCaseSensitive(o, "diffSummary");
    str_take(ev->u.approval.diff_summary, sizeof(ev->u.approval.diff_summary),
             cJSON_IsString(df) ? df->valuestring : "");
    ev->u.approval.risk = parse_risk(o);
    ev->type = APP_EV_APPROVAL_REQUEST;
    return true;
}

static bool parse_transcript(const cJSON *o, app_event_t *ev) {
    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(o, "text");
    if (!cJSON_IsString(tx)) return false;
    str_take(ev->u.transcript.text, sizeof(ev->u.transcript.text), tx->valuestring);
    const cJSON *im = cJSON_GetObjectItemCaseSensitive(o, "inject_mode");
    ev->u.transcript.inject_mode = (cJSON_IsString(im) && strcmp(im->valuestring, "paste") == 0)
        ? APP_INJECT_PASTE : APP_INJECT_TYPE;
    // final 缺省 false:旧 Mac 端/旧帧不带该字段一律按预览态处理
    const cJSON *fin = cJSON_GetObjectItemCaseSensitive(o, "final");
    ev->u.transcript.final = cJSON_IsBool(fin) && cJSON_IsTrue(fin);
    ev->type = APP_EV_TRANSCRIPT;
    return true;
}

bool app_protocol_parse(const char *json, size_t len, app_event_t *ev) {
    if (!json || len == 0 || len > APP_PROTO_RX_CAP) return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    bool ok = false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type)) {
        if      (strcmp(type->valuestring, "mac.metrics") == 0)          ok = parse_metrics(root, ev);
        else if (strcmp(type->valuestring, "agent.status") == 0)         ok = parse_agent_status(root, ev);
        else if (strcmp(type->valuestring, "agent.approval_request") == 0) ok = parse_approval(root, ev);
        else if (strcmp(type->valuestring, "transcript") == 0)           ok = parse_transcript(root, ev);
        // 未知 type:丢弃(返回 false,调用方记日志)
    }
    cJSON_Delete(root);
    return ok;
}

// ---- 序列化 ----
static size_t serialize(cJSON *root, char *buf, size_t cap) {
    if (!root || !buf || cap < 2) return 0;   // cap<2 时 (cap-2) 下溢成超大 memcpy
    char *s = cJSON_PrintUnformatted(root);
    if (!s) return 0;                         // 序列化失败(OOM):不发残缺帧
    size_t n = strlen(s);
    if (n + 2 > cap) n = cap - 2;             // 截断保护(本协议帧都远小于 cap)
    memcpy(buf, s, n);
    free(s);
    buf[n++] = '\n';                          // 行分隔
    buf[n] = '\0';
    return n;
}

size_t app_protocol_device_hello(char *buf, size_t cap, int proto) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "device.hello");
    cJSON_AddNumberToObject(o, "proto", proto);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_voice_start(char *buf, size_t cap, app_workflow_t wf) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "voice.start");
    cJSON_AddStringToObject(o, "workflow",
                            wf < APP_WF_COUNT ? WF_WIRE_NAMES[wf] : "build");
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_voice_end(char *buf, size_t cap) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "voice.end");
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_device_status(char *buf, size_t cap, uint32_t drop_count) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "status");
    cJSON_AddNumberToObject(o, "drop", (double)drop_count);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_workflow_switch(char *buf, size_t cap, app_workflow_t wf) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "workflow.switch");
    cJSON_AddStringToObject(o, "current",
                            wf < APP_WF_COUNT ? WF_WIRE_NAMES[wf] : "build");
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_agent_action(char *buf, size_t cap, const char *task_id,
                                 uint8_t decision) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "agent.action");
    cJSON_AddStringToObject(o, "taskId", task_id ? task_id : "");
    const char *act = "details";
    if (decision == APP_ACTION_APPROVE) act = "approve";
    else if (decision == APP_ACTION_REJECT) act = "reject";
    cJSON_AddStringToObject(o, "action", act);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}
