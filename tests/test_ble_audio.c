// tests/test_ble_audio.c —— BLE 直连分片/事件打包/CTRL 解析主机测试(assert 风格)。
// 分片与事件打包走 ble_audio 纯函数(重拼后逐字节比对,断言每片 ≤ MTU-3 的 ATT 载荷上限);
// CTRL 下行解析复用 app_protocol_parse,断言 app_types 事件字段。
#include <assert.h>
#include <stdio.h>
#include <stdint.h>   // SIZE_MAX
#include <string.h>
#include "ble_audio.h"
#include "app_protocol.h"
#include "app_types.h"

// 事件行上限必须与协议 TX 上限一致(打包层 512 即 app_protocol 的 APP_PROTO_TX_CAP)
_Static_assert(EVENT_LINE_MAX == APP_PROTO_TX_CAP, "EVENT_LINE_MAX must equal APP_PROTO_TX_CAP");
_Static_assert(AUDIO_FRAME_BYTES == 3200, "audio frame is 100ms @16kHz/16bit/mono");
// 事件 union 尺寸上限:240B 只减不增(approval 最大成员 225B 对齐到 8 ——
// TIME_SET 引入 int64 成员把 union 对齐顶到 8,结构 240B;8 深队列 = 1920B)
_Static_assert(sizeof(app_event_t) == 240, "app_event_t must stay at 240 bytes");

static void test_constants(void) {
    assert(ATT_MTU_MIN == 23);
    assert(PAYLOAD_OVERHEAD == 3);
}

static void test_chunk_count(void) {
    // 3200B @ MTU 247 → ceil(3200/244) = 14(13×244 + 28;任务文里的 13 片 272B 超载荷上限,按 14 片实现)
    assert(ble_audio_chunk_count(3200, 247) == 14);
    // 3200B @ MTU 517 → ceil(3200/514) = 7(6×514 + 116)
    assert(ble_audio_chunk_count(3200, 517) == 7);
    // 整分:488 = 2×244
    assert(ble_audio_chunk_count(488, 247) == 2);
    // 恰好一片
    assert(ble_audio_chunk_count(244, 247) == 1);
    assert(ble_audio_chunk_count(20, 23) == 1);
    assert(ble_audio_chunk_count(21, 23) == 2);   // 20 + 1
    // 非法:空帧 / MTU 低于规范下限 23
    assert(ble_audio_chunk_count(0, 247) == 0);
    assert(ble_audio_chunk_count(3200, 22) == 0);
    assert(ble_audio_chunk_count(3200, 0) == 0);
}

static void test_chunk_len_sequence(void) {
    // MTU 247:前 13 片 244B,末片 3200-13*244 = 28B
    for (size_t i = 0; i < 13; i++) assert(ble_audio_chunk_len(3200, 247, i) == 244);
    assert(ble_audio_chunk_len(3200, 247, 13) == 28);
    assert(ble_audio_chunk_len(3200, 247, 14) == 0);   // 越界
    // MTU 517:前 6 片 514B,末片 116B
    for (size_t i = 0; i < 6; i++) assert(ble_audio_chunk_len(3200, 517, i) == 514);
    assert(ble_audio_chunk_len(3200, 517, 6) == 116);
    // 非法参数
    assert(ble_audio_chunk_len(0, 247, 0) == 0);
    assert(ble_audio_chunk_len(3200, 10, 0) == 0);

    // #6 越界预判:除法求 count,超大 idx 不得经乘法回绕后误判"未越界"返回非零。
    // (旧实现 idx*chunk 在 idx≈SIZE_MAX 时回绕到低地址,off < frame_len 误通过)
    assert(ble_audio_chunk_count(SIZE_MAX, 517) == SIZE_MAX / 514 + 1);   // ceil 无溢出
    assert(ble_audio_chunk_len(SIZE_MAX, 517, SIZE_MAX / 514 + 1) == 0);  // 恰等于 count:越界
    assert(ble_audio_chunk_len(SIZE_MAX, 517, SIZE_MAX) == 0);            // 极端 idx:越界
}

// 一次性打包 → 重拼 → 与原始帧逐字节一致;同时断言每片 ≤ MTU-3、指针零拷贝落在帧内
static void assert_pack_reassembly(const uint8_t *frame, size_t frame_len, uint16_t mtu) {
    ble_audio_chunk_t chunks[256];
    size_t count = 0;
    assert(ble_audio_pack_audio(chunks, 256, frame, frame_len, mtu, &count) == BLE_AUDIO_OK);
    assert(count == ble_audio_chunk_count(frame_len, mtu));
    assert(count > 0);

    uint8_t rebuilt[8192];
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        assert(chunks[i].len == ble_audio_chunk_len(frame_len, mtu, i));
        assert(chunks[i].len <= mtu - PAYLOAD_OVERHEAD);          // ATT 载荷上限
        assert(chunks[i].len > 0);
        assert(chunks[i].data >= frame && chunks[i].data + chunks[i].len <= frame + frame_len);
        memcpy(rebuilt + off, chunks[i].data, chunks[i].len);
        off += chunks[i].len;
    }
    assert(off == frame_len);
    assert(memcmp(rebuilt, frame, frame_len) == 0);
}

static void test_pack_audio(void) {
    uint8_t frame[3200];
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)(i * 13 + 7);

    assert_pack_reassembly(frame, sizeof(frame), 247);   // 14 片(13×244 + 28)
    assert_pack_reassembly(frame, sizeof(frame), 517);   // 7 片(6×514 + 116)
    assert_pack_reassembly(frame, sizeof(frame), 23);    // 最小合法 MTU:20B/片 × 160
    assert_pack_reassembly(frame, 26, 23);               // MTU 边界 23..26 均合法(20B 片 + 余 6B)
    assert_pack_reassembly(frame, 27, 27);               // 任务文提到的 MTU 27:24B 片 + 余 3B

    // 非整分 / 整分 / 极小帧
    assert_pack_reassembly(frame, 100, 247);             // 1 片 100B
    assert_pack_reassembly(frame, 488, 247);             // 整分 2×244
    assert_pack_reassembly(frame, 489, 247);             // 244 + 244 + 1
    assert_pack_reassembly(frame, 1, 247);               // 1 片 1B
}

static void test_pack_audio_small_cap(void) {
    uint8_t frame[3200];
    ble_audio_chunk_t chunks[256];
    size_t count = 99;
    // 3200B @ 247 需 14 片:cap 13 → SMALL_CAP 且不改输出;cap 14 → OK
    assert(ble_audio_pack_audio(chunks, 13, frame, sizeof(frame), 247, &count) == BLE_AUDIO_ERR_SMALL_CAP);
    assert(count == 99);   // 失败不写输出
    assert(ble_audio_pack_audio(chunks, 0, frame, sizeof(frame), 247, &count) == BLE_AUDIO_ERR_SMALL_CAP);
    assert(ble_audio_pack_audio(chunks, 14, frame, sizeof(frame), 247, &count) == BLE_AUDIO_OK);
    assert(count == 14);
}

static void test_pack_audio_errors(void) {
    uint8_t frame[64];
    ble_audio_chunk_t chunks[16];
    size_t count;

    // 空帧
    assert(ble_audio_pack_audio(chunks, 16, frame, 0, 247, &count) == BLE_AUDIO_ERR_EMPTY);
    // 非法 MTU(低于规范下限 23)
    assert(ble_audio_pack_audio(chunks, 16, frame, 64, 22, &count) == BLE_AUDIO_ERR_MTU);
    assert(ble_audio_pack_audio(chunks, 16, frame, 64, 0, &count) == BLE_AUDIO_ERR_MTU);
    // 空指针
    assert(ble_audio_pack_audio(NULL, 16, frame, 64, 247, &count) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_pack_audio(chunks, 16, frame, 64, 247, NULL) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_pack_audio(chunks, 16, NULL, 64, 247, &count) == BLE_AUDIO_ERR_NULL);
}

static void test_pack_next_iterator(void) {
    uint8_t frame[3200];
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)(i * 3 + 1);

    ble_audio_packer_t p;
    assert(ble_audio_pack_init(&p, frame, sizeof(frame), 247) == BLE_AUDIO_OK);

    const uint8_t *chunk;
    size_t len;
    uint8_t rebuilt[3200];
    size_t off = 0;
    size_t pieces = 0;
    ble_audio_err_t e;
    while ((e = ble_audio_pack_next(&p, &chunk, &len)) == BLE_AUDIO_OK) {
        assert(pieces < 14);
        assert(chunk == frame + off);                  // 零拷贝:连续片无缝拼接
        assert(len <= 244);
        assert(len == (pieces < 13 ? 244 : 28));
        memcpy(rebuilt + off, chunk, len);
        off += len;
        pieces++;
    }
    assert(e == BLE_AUDIO_DONE);
    assert(pieces == 14);
    assert(off == sizeof(frame));
    assert(memcmp(rebuilt, frame, sizeof(frame)) == 0);
    // 取完后重复调用仍是 DONE
    assert(ble_audio_pack_next(&p, &chunk, &len) == BLE_AUDIO_DONE);
}

static void test_pack_next_errors(void) {
    uint8_t frame[64];
    ble_audio_packer_t p;
    const uint8_t *chunk;
    size_t len;

    assert(ble_audio_pack_init(NULL, frame, 64, 247) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_pack_init(&p, NULL, 64, 247) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_pack_init(&p, frame, 0, 247) == BLE_AUDIO_ERR_EMPTY);
    assert(ble_audio_pack_init(&p, frame, 64, 22) == BLE_AUDIO_ERR_MTU);

    assert(ble_audio_pack_init(&p, frame, 64, 247) == BLE_AUDIO_OK);
    assert(ble_audio_pack_next(NULL, &chunk, &len) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_pack_next(&p, NULL, &len) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_pack_next(&p, &chunk, NULL) == BLE_AUDIO_ERR_NULL);

    // 零初始化兜底:未 init 的 packer 不得越界读
    ble_audio_packer_t z = {0};
    assert(ble_audio_pack_next(&z, &chunk, &len) == BLE_AUDIO_ERR_NULL);
}

// 事件行打包 → 重拼 → 与原始行逐字节一致(含行分隔 '\n' 随末片到达)
static void assert_reassembly_line(const char *line, size_t line_len, uint16_t mtu) {
    assert(line_len > 0 && line_len <= EVENT_LINE_MAX);
    ble_audio_chunk_t chunks[64];
    size_t count = 0;
    assert(ble_audio_event_chunks(chunks, 64, line, line_len, mtu, &count) == BLE_AUDIO_OK);
    assert(count == ble_audio_chunk_count(line_len, mtu));
    assert(count > 0);

    char rebuilt[EVENT_LINE_MAX + 1];
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        assert(chunks[i].len <= mtu - PAYLOAD_OVERHEAD);
        memcpy(rebuilt + off, chunks[i].data, chunks[i].len);
        off += chunks[i].len;
    }
    assert(off == line_len);
    assert(memcmp(rebuilt, line, line_len) == 0);
    assert(rebuilt[line_len - 1] == '\n');   // 事件行以行分隔结尾
}

static void test_event_chunks(void) {
    char line[EVENT_LINE_MAX];
    size_t n;

    n = app_protocol_voice_start(line, sizeof(line), APP_WF_REVIEW);
    assert_reassembly_line(line, n, 247);
    assert_reassembly_line(line, n, 23);     // 最小 MTU 下跨多片

    n = app_protocol_voice_end(line, sizeof(line));
    assert_reassembly_line(line, n, 247);

    n = app_protocol_key_action(line, sizeof(line), APP_KEY_CLEAR);
    assert_reassembly_line(line, n, 247);

    n = app_protocol_agent_action(line, sizeof(line), "task_9821", APP_ACTION_APPROVE);
    assert_reassembly_line(line, n, 247);

    n = app_protocol_device_hello(line, sizeof(line), 1);
    assert_reassembly_line(line, n, 247);
}

static void test_event_chunks_multipiece(void) {
    // 构造接近上限的真实审批行(满字段):~330B → MTU 247 下跨 2 片
    char line[EVENT_LINE_MAX];
    char task[33], title[65], target[65], diff[65];
    memset(task, 't', 32); task[32] = '\0';
    memset(title, 'T', 64); title[64] = '\0';
    memset(target, 'g', 64); target[64] = '\0';
    memset(diff, 'd', 64); diff[64] = '\0';
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"agent.approval_request\",\"taskId\":\"%s\","
                     "\"title\":\"%s\",\"target\":\"%s\",\"diffSummary\":\"%s\","
                     "\"riskLevel\":\"high\"}\n",
                     task, title, target, diff);
    assert(n > 244 && n <= EVENT_LINE_MAX);   // 确保真的跨片且不超上限
    assert_reassembly_line(line, (size_t)n, 247);
}

static void test_event_chunks_bounds(void) {
    ble_audio_chunk_t chunks[64];
    size_t count;
    char big[EVENT_LINE_MAX + 8];
    memset(big, 'x', sizeof(big));

    // 空行
    assert(ble_audio_event_chunks(chunks, 64, "x", 0, 247, &count) == BLE_AUDIO_ERR_EMPTY);
    // 长度校验先于内容:恰 512B 合法,513B → TOO_LONG
    assert(ble_audio_event_chunks(chunks, 64, big, EVENT_LINE_MAX, 247, &count) == BLE_AUDIO_OK);
    assert(count == ble_audio_chunk_count(EVENT_LINE_MAX, 247));
    assert(ble_audio_event_chunks(chunks, 64, big, EVENT_LINE_MAX + 1, 247, &count) == BLE_AUDIO_ERR_TOO_LONG);
    // 非法 MTU 先于长度校验
    assert(ble_audio_event_chunks(chunks, 64, big, 10, 22, &count) == BLE_AUDIO_ERR_MTU);
    // 空指针
    assert(ble_audio_event_chunks(NULL, 64, "x", 1, 247, &count) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_event_chunks(chunks, 64, "x", 1, 247, NULL) == BLE_AUDIO_ERR_NULL);
    assert(ble_audio_event_chunks(chunks, 64, NULL, 1, 247, &count) == BLE_AUDIO_ERR_NULL);
}

// ---- CTRL 下行解析(对接 app_protocol_parse,假行断言 app_types 事件字段) ----
static void test_ctrl_parse_approval(void) {
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

    // 缺省字段:target/diffSummary 为空,riskLevel 缺省 medium
    const char *j2 = "{\"type\":\"agent.approval_request\",\"taskId\":\"t1\",\"title\":\"x\"}";
    assert(app_protocol_parse(j2, strlen(j2), &ev));
    assert(strcmp(ev.u.approval.target, "") == 0);
    assert(strcmp(ev.u.approval.diff_summary, "") == 0);
    assert(ev.u.approval.risk == APP_RISK_MEDIUM);
}

static void test_ctrl_parse_transcript(void) {
    const char *j = "{\"type\":\"transcript\",\"text\":\"修复了登录 bug\",\"inject_mode\":\"paste\"}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.type == APP_EV_TRANSCRIPT);
    assert(strcmp(ev.u.transcript.text, "修复了登录 bug") == 0);
    assert(ev.u.transcript.inject_mode == APP_INJECT_PASTE);
    assert(ev.u.transcript.final == false);                   // 缺省 false = 预览态

    const char *j2 = "{\"type\":\"transcript\",\"text\":\"no mode\"}";
    assert(app_protocol_parse(j2, strlen(j2), &ev));
    assert(ev.u.transcript.inject_mode == APP_INJECT_TYPE);   // 缺省 type
    assert(ev.u.transcript.final == false);

    const char *j3 = "{\"type\":\"transcript\",\"text\":\"done\",\"final\":true}";
    assert(app_protocol_parse(j3, strlen(j3), &ev));
    assert(ev.u.transcript.final == true);                    // 定稿

    const char *j4 = "{\"type\":\"transcript\",\"text\":\"still live\",\"final\":false}";
    assert(app_protocol_parse(j4, strlen(j4), &ev));
    assert(ev.u.transcript.final == false);                   // 显式 false 仍预览
}

static void test_ctrl_parse_agent_status(void) {
    const char *j = "{\"type\":\"agent.status\",\"state\":\"thinking\",\"message\":\"reading code\"}";
    app_event_t ev;
    assert(app_protocol_parse(j, strlen(j), &ev));
    assert(ev.type == APP_EV_AGENT_STATUS);
    assert(ev.u.agent_status.state == APP_AGENT_THINKING);
    assert(strcmp(ev.u.agent_status.message, "reading code") == 0);
}

static void test_ctrl_parse_rejects(void) {
    app_event_t ev;
    assert(!app_protocol_parse("{\"type\":\"nope\"}", 14, &ev));                    // 未知 type
    assert(!app_protocol_parse("not json", 8, &ev));                                // 畸形
    assert(!app_protocol_parse("{\"type\":\"transcript\"}", 22, &ev));              // 缺必填 text
    assert(!app_protocol_parse("{\"type\":\"agent.status\",\"state\":\"zz\"}", 40, &ev));  // 未知状态
}

int main(void) {
    test_constants();
    test_chunk_count();
    test_chunk_len_sequence();
    test_pack_audio();
    test_pack_audio_small_cap();
    test_pack_audio_errors();
    test_pack_next_iterator();
    test_pack_next_errors();
    test_event_chunks();
    test_event_chunks_multipiece();
    test_event_chunks_bounds();
    test_ctrl_parse_approval();
    test_ctrl_parse_transcript();
    test_ctrl_parse_agent_status();
    test_ctrl_parse_rejects();
    printf("test_ble_audio: all assertions passed\n");
    return 0;
}
