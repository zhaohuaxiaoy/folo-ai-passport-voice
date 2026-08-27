// 状态机全转移主机测试(纯 C,assert 断言)。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "fake_mode.h"

static uint64_t now = 1000000;              // 单调递增假时钟
static app_state_t s;
static app_action_t out[APP_ACT_MAX];
static uint8_t on;

static void reduce(app_event_type_t t, uint64_t ts) {
    app_event_t ev = { .type = t };
    if (t == APP_EV_KEY_PRESS || t == APP_EV_KEY_RELEASE ||
        t == APP_EV_KEY_CLICK || t == APP_EV_KEY_DOUBLE) {
        ev.u.key.btn = APP_BTN_OK;
    }
    now = ts;
    app_state_reduce(&s, &ev, now, out, &on);
}

static void reduce_btn(app_event_type_t t, app_btn_t b, uint64_t ts) {
    app_event_t ev = { .type = t };
    ev.u.key.btn = b;
    now = ts;
    app_state_reduce(&s, &ev, now, out, &on);
}

static int has_action(app_action_type_t t) {
    for (uint8_t i = 0; i < on; i++) if (out[i].type == t) return 1;
    return 0;
}

static app_action_t *find_action(app_action_type_t t) {
    for (uint8_t i = 0; i < on; i++) if (out[i].type == t) return &out[i];
    return NULL;
}

// 断言 first 动作严格先于 second(顺序契约:STREAM_STOP 先于 SEND_VOICE_END 等)
static void assert_action_order(app_action_type_t first, app_action_type_t second) {
    int a = -1, b = -1;
    for (uint8_t i = 0; i < on; i++) {
        if (a < 0 && out[i].type == first) a = i;
        if (b < 0 && out[i].type == second) b = i;
    }
    assert(a >= 0 && b > a);
}

static void reset(void) {
    app_state_init(&s);
    now = 1000000;
    s.last_key_ms = now;
}

// ---- HOME:● 单击进入 READY;▲(音量加)双击=清空;▼ 单击=回车 ----
static void test_home_nav(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);

    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 10);
    assert(s.state == APP_ST_HOME);                // HOME 不切走
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);

    reset();
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_DOWN, now + 10);
    assert(s.state == APP_ST_HOME);                // DOWN 双击无动作
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    reset();
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_HOME);                // OK 双击不再清空(迁到 UP 双击)
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    reset();
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 10);
    assert(s.state == APP_ST_HOME);                // UP(音量加)双击=清空,不切走
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);

    // ▲ 空闲:无切换、无动作
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 10);
    assert(s.state == APP_ST_HOME);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
}

// ---- READY:OK 已退出 PTT;▲(音量加)长按=说话 / 双击=清空;▼ 单击=回车 ----
static void test_down_enter_clear(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);   // → READY (BUILD)
    assert(s.state == APP_ST_READY);

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 20);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);

    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_DOWN, now + 30);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));    // DOWN 双击已移除

    // UP 空闲
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 40);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // PTT 不受影响(基线缺陷:离线检查引入后此段未设 link_up,PTT 被拒;修正)
    s.link_up = true;
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 50);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
}

// ---- PTT:离线被拒 + 错误音;在线开流 ----
static void test_ptt_offline_online(void) {
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 离线长按 ▲
    assert(s.state == APP_ST_READY);                       // 原地不动
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_ERROR);
    assert(strstr(s.toast, "OFFLINE"));

    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 在线长按 ▲
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_PLAY_TONE));                 // 440Hz 就绪音
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_STREAM_START));             // S3:开流移出 PRESS 产出,由 TONE_DONE 驱动
    assert(!s.stream_started);
    // 顺序契约:就绪音先于 voice.start(动作顺序保证)
    assert_action_order(APP_ACT_PLAY_TONE, APP_ACT_SEND_VOICE_START);
    // 滴声播完 → TONE_DONE → 开流
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 100, out, &on);
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
}

// ---- S3:READY+▲ 长按只产出滴声 + voice.start,开流等 TONE_DONE ----
static void test_tone_press_produce(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);
    assert(s.stream_started == false);
    assert(!has_action(APP_ACT_STREAM_START));
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_START);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert_action_order(APP_ACT_PLAY_TONE, APP_ACT_SEND_VOICE_START);
}

// ---- S3:TONE_DONE 在 LISTENING 且流未开时开流;重复到达幂等忽略 ----
static void test_tone_done_idempotent(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 开录,未开流
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 110, out, &on);        // 滴声播完
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    app_state_reduce(&s, &ev, now + 120, out, &on);        // 重复(异常重复投递)
    assert(on == 0);                                       // 幂等:无动作
}

// ---- S3:松开即发后,滴声期间迟到的 TONE_DONE 必须忽略(不误开流) ----
static void test_tone_done_late_after_send(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 按下 #1:入 LISTENING,未开流
    assert(!has_action(APP_ACT_STREAM_START));
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 30);  // 松开:立即发送
    assert(s.state == APP_ST_TRANSCRIBING);
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 210, out, &on);        // 滴声播完事件此刻才到
    assert(on == 0);                                       // 已离开 LISTENING → 忽略
    assert(!has_action(APP_ACT_STREAM_START));
}

// ---- S3:松开即发完整序列——先 TONE_DONE 开流,RELEASE 立即停流 ----
static void test_tone_release_sends_order(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 开录
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 110, out, &on);        // 滴声播完 → 开流
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000); // 松开:立即停流发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));               // 停流由 RELEASE 产出
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
}

// ---- S3:兜底——TICK 500ms 未收 TONE_DONE → 强制开流(防声音任务异常卡死) ----
static void test_tone_tick_fallback(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 开录
    reduce(APP_EV_TICK, s.state_since_ms + 499);           // 499ms 未到阈值
    assert(!has_action(APP_ACT_STREAM_START));
    assert(!s.stream_started);
    reduce(APP_EV_TICK, s.state_since_ms + 500);           // 500ms 到 → 兜底开流
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    // 兜底后迟到的 TONE_DONE:幂等忽略
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 600, out, &on);
    assert(on == 0);
}

// ---- S3:TONE_DONE 在非 LISTENING 状态(HOME/READY)无动作 ----
static void test_tone_done_ignored_elsewhere(void) {
    reset();
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 10, out, &on);         // HOME
    assert(on == 0);
    s.state = APP_ST_READY;
    s.state_since_ms = now;
    app_state_reduce(&s, &ev, now + 20, out, &on);         // READY
    assert(on == 0);
}

// ---- LISTENING:松开立即结束并发送 ----
static void test_listening_release_sends(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000); // 松开:立即发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_SEND);
}

// ---- UP(音量加)双击 = 清空输入框;OK 双击已移除(录音松开已发送,DOUBLE 全局处理)----
static void test_up_double_clears_input(void) {
    // READY 态完整双击流:两按均短(<0.5s 不触发长按),DOUBLE → clear,无提示音
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 40);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 200);   // 双击第二按
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 220);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 400);  // 双击窗口到期
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(!has_action(APP_ACT_PLAY_TONE));                // 双击无提示音
    assert(!has_action(APP_ACT_SEND_VOICE_START));         // 无录音动作
    assert(s.state == APP_ST_READY);                       // 会话状态不受影响

    // 转写/执行中双击 UP 同样清空(第二按落在 TRANSCRIBING 的典型场景)
    reset();
    s.link_up = true;
    s.state = APP_ST_TRANSCRIBING;
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 10);
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(!has_action(APP_ACT_STREAM_CANCEL));            // 不清会话(无取消语义)
    assert(!has_action(APP_ACT_STREAM_STOP));
    assert(s.state == APP_ST_TRANSCRIBING);                // 会话状态不受影响

    // OK 双击不再清空(功能迁到 UP 双击)
    reset();
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 10);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // 长按松开(PTT 结束)后回弹窗口内的双击是"长按+机械回弹"误判的假双击:
    // 忽略,防止 clear 上行在注入完成后删掉刚注入的文本。
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);     // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 500);     // 开录
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500); // 松开 → TRANSCRIBING
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 100);   // 松开 100ms 后的假双击
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));           // 回弹假双击被吞
    assert(s.state == APP_ST_TRANSCRIBING);                 // 会话不受影响

    // 回弹按压延迟上报:误判双击的第二按在松开后 ~300ms,DOUBLE 事件要再等
    // 双击窗口 300ms,即松开后 ~600ms 才上报 —— 也在防御窗口(700ms)内
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);     // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 500);     // 开录
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500); // 松开 → TRANSCRIBING
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 600);   // 松开 600ms 后的延迟误判
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));           // 仍在窗口内:被吞

    // 录音中(LISTENING,手指在按住)收到 DOUBLE(ADC 阈值穿越抖动):忽略
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 500);     // 开录
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 1200);  // 按住期间的假双击
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    assert(s.state == APP_ST_LISTENING);

    // 松开超过回弹窗口后(注入已完成、用户看到文本)的正常双击清空不受影响
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 500);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 1000);  // 松开 1s 后双击
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
}

// ---- UP(音量加)长按 ≥0.5s = 说话:LONG 开录(滴声在判定时响),LONG_UP 松开发送 ----
static void test_up_long_ptt(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 500);    // 0.5s 判定:开录
    assert(s.state == APP_ST_LISTENING);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_START);              // 滴声在长按判定时响
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_STREAM_STOP));              // 未结束
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500); // 松开:发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
    t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_SEND);               // 发送音保留

    // 离线:长按 UP 被拒,error 音 + toast,不进 LISTENING
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 500);
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_START));
    t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_ERROR);
    assert(strstr(s.toast, "OFFLINE"));

    // 短按(CLICK 而非 LONG)不触发任何动作、无提示音
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 100);
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_PLAY_TONE));
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // OK 键已退出 PTT:按住/松开都不再开录音
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 100);   // 按住 OK
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_PLAY_TONE));
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 200); // 松开 OK
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_END));
    assert(!has_action(APP_ACT_STREAM_STOP));
}

// ---- LISTENING 端:见 test_listening_release_sends / test_up_double_clears_input ----
static void test_listening_end(void) {
    test_listening_release_sends();
    test_up_double_clears_input();
}

// ---- 超时:TRANSCRIBING 30s / AGENT_RUNNING 90s → READY ----
static void test_timeouts(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 3300);  // 单击窗口到期 → TRANSCRIBING
    assert(s.state == APP_ST_TRANSCRIBING);
    reduce(APP_EV_TICK, s.state_since_ms + 29 * 1000);     // 进入后 29s 未到
    assert(s.state == APP_ST_TRANSCRIBING);
    reduce(APP_EV_TICK, s.state_since_ms + 31 * 1000);     // 进入后 31s 超时
    assert(s.state == APP_ST_READY);
    assert(strstr(s.toast, "timeout"));
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));          // 中止路径不该产生上行
}

static void test_agent_running_timeout(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    reduce(APP_EV_TICK, now + 89 * 1000);
    assert(s.state == APP_ST_AGENT_RUNNING);
    reduce(APP_EV_TICK, now + 91 * 1000);
    assert(s.state == APP_ST_READY);
    assert(strstr(s.toast, "timeout"));
}

// ---- Agent 状态流转:TRANSCRIBING → AGENT_RUNNING → DONE ----
static void test_agent_status_flow(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;

    app_event_t ev = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_RUNNING, .message = "unit tests..." } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_AGENT_RUNNING);
    assert(strcmp(s.agent_message, "unit tests...") == 0);
    assert(strcmp(s.agent_state_name, "running") == 0);

    ev = (app_event_t){ .type = APP_EV_AGENT_STATUS,
                        .u.agent_status = { .state = APP_AGENT_DONE, .message = "24 passed" } };
    app_state_reduce(&s, &ev, now, out, &on);
    // 无 DONE 页:done 直接回 READY 待命(用户:不要 done 提示),成功音保留
    assert(s.state == APP_ST_READY);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_SUCCESS);
}

// ---- 审批闭环 ----
static void test_approval(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;

    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "task_9821", .title = "Modify 3 files",
                                       .target = "OrderService.java", .diff_summary = "+128 / -37",
                                       .risk = APP_RISK_HIGH } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_APPROVAL);

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 100);   // ● 批准
    app_action_t *a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_APPROVE);
    assert(strcmp(a->u.agent_action.task_id, "task_9821") == 0);
    assert(s.state == APP_ST_AGENT_RUNNING);

    // 再来一次,▲ 拒绝
    s.state = APP_ST_AGENT_RUNNING;
    ev.u.approval.task_id[0] = 'x'; ev.u.approval.task_id[1] = '\0';
    app_state_reduce(&s, &ev, now, out, &on);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 200);
    a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_REJECT);
    t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_REJECT);

    // ▼ 在 APPROVAL 下 = 回车(与全局 DOWN 语义统一;旧 approval_details
    // 详情视图切换已随状态机演进移除 —— 基线测试过期,按当前语义修正)
    s.state = APP_ST_AGENT_RUNNING;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 300);
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);
}

// ---- 两级息屏/唤醒 ----
// 20s 无键 → 关背光(screen_on=false, 面板仍通电);60s → 面板 SLPIN 断电;
// PRESS 任意键 → 面板上电 + 背光亮。
static void test_screen_off_wake(void) {
    reset();
    const uint64_t t0 = now;   // 息屏计时基准(= last_key_ms;reduce 会改 now,须先取)
    // 20s 级:关背光,面板不动
    reduce(APP_EV_TICK, t0 + APP_IDLE_BACKLIGHT_OFF_MS + 1);
    assert(s.screen_on == false);
    assert(s.panel_on == true);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));
    assert(!has_action(APP_ACT_UI_PANEL_OFF));
    // 60s 边界:59.999s 不触发,恰好 60s(>=)触发面板断电
    reduce(APP_EV_TICK, t0 + APP_IDLE_PANEL_OFF_MS - 1);
    assert(s.panel_on == true);
    reduce(APP_EV_TICK, t0 + APP_IDLE_PANEL_OFF_MS);
    assert(s.panel_on == false);
    assert(has_action(APP_ACT_UI_PANEL_OFF));

    // 任意键唤醒:面板 + 背光一起恢复
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, t0 + APP_IDLE_PANEL_OFF_MS + 2);
    assert(s.panel_on == true);
    assert(s.screen_on == true);
    assert(has_action(APP_ACT_UI_PANEL_ON));
    assert(has_action(APP_ACT_UI_SCREEN_ON));

    // 唤醒后的同一事件流:CLICK 应被忽略(不产生状态跳转)
    s.screen_on = false;
    s.panel_on = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // 息屏时 CLICK 不唤醒
    assert(s.state == APP_ST_HOME);
    assert(s.screen_on == false);
    // 背光灭但面板还通电(20-60s 区间):PRESS 只恢复背光,不再发 PANEL_ON
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);
    assert(s.screen_on == true);
    assert(!has_action(APP_ACT_UI_PANEL_ON));
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 30);
    assert(s.state == APP_ST_READY);
}

// ---- transcript:注入已迁 Mac 端,设备只更新显示(任意状态);final 区分预览/定稿 ----
static void test_transcript_display(void) {
    reset();
    s.state = APP_ST_TRANSCRIBING;
    app_event_t ev = { .type = APP_EV_TRANSCRIPT,
                       .u.transcript = { .text = "fix the bug", .inject_mode = APP_INJECT_TYPE,
                                         .final = false } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(strcmp(s.agent_message, "fix the bug") == 0);
    assert(s.transcript_final == false);                     // final:false = 预览态
    assert(has_action(APP_ACT_UI_REFRESH));
    // 注意:不再产出 INJECT_TEXT 动作(该动作已随 HID 注入退役)

    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_message, "fix the bug") == 0);
    assert(snap.transcript_final == false);                  // 快照区分预览态(UI 加光标)

    // 定稿:final:true → 落定显示,快照标记定稿
    ev.u.transcript.final = true;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(strcmp(s.agent_message, "fix the bug") == 0);
    assert(s.transcript_final == true);
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_message, "fix the bug") == 0);
    assert(snap.transcript_final == true);                   // 定稿态(UI 移除光标)

    // AGENT_RUNNING 同样显示(预览态)
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    ev.u.transcript.text[0] = 'x'; ev.u.transcript.text[1] = '\0';
    ev.u.transcript.final = false;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(strcmp(s.agent_message, "x") == 0);
    assert(s.transcript_final == false);

    // agent.status 替换消息后不再是转写预览(UI 无光标)
    app_event_t st = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_RUNNING, .message = "deploying" } };
    app_state_reduce(&s, &st, now, out, &on);
    assert(strcmp(s.agent_message, "deploying") == 0);
    assert(s.transcript_final == true);

    // 审批决策写入的消息同样非预览
    reset();
    s.state = APP_ST_APPROVAL;
    s.state_since_ms = now;
    app_event_t ap = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1",
                                       .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ap, now, out, &on);
    assert(s.transcript_final == false);                     // 默认未定稿(preview 无从谈起)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 100);     // ● 批准
    assert(strcmp(s.agent_message, "Approved, agent continues...") == 0);
    assert(s.transcript_final == true);                      // 非转写文本,无预览光标
}

// ---- BLE 链路断开:录音中停流回 READY ----
static void test_ble_link_down(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.link_up == false);
    assert(s.ble_connected == false);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));             // 断链无 Mac 可发,清残留防流入下次会话
    assert(!has_action(APP_ACT_SEND_VOICE_END));           // 会话中止,不发 end
    assert(strstr(s.toast, "Mac disconnected"));

    // APPROVAL 下断开:保持状态等待重连后 Mac 重发请求
    reset();
    s.link_up = true;
    s.state = APP_ST_APPROVAL;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
}

// ---- 文本安全:恰好填满缓冲也保证 NUL 结尾(截断上限由协议层测试覆盖) ----
static void test_bounded(void) {
    reset();
    app_event_t ev = { .type = APP_EV_TRANSCRIPT };
    memset(ev.u.transcript.text, 'x', sizeof(ev.u.transcript.text) - 1);
    ev.u.transcript.text[sizeof(ev.u.transcript.text) - 1] = '\0';
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.agent_message[sizeof(s.agent_message) - 1] == '\0');
    assert(has_action(APP_ACT_UI_REFRESH));
}

// ---- 录音中收到审批请求:必须停流,管线不能泄漏 ----
static void test_approval_during_listening(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);

    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1", .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now + 100, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));            // 结束被截断的会话,Mac 侧关闭 ASR
    // APPROVAL 下再松开/双击:忽略,不再触碰管线
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 200);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 300);
    assert(s.state == APP_ST_APPROVAL);
}

// ---- 转写/执行中断开:回 READY,兜底停流 ----
static void test_ble_disconnect_transcribing(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));             // 兜底:停流 + 清残留(幂等)
    assert(strstr(s.toast, "Mac disconnected"));

    // 重连后回 ONLINE
    reset();
    app_event_t c = { .type = APP_EV_BLE_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.link_up == true);
    assert(s.ble_connected == true);
}

// ---- TRANSCRIBING / AGENT_RUNNING 下按键全部忽略 ----
static void test_keys_ignored_in_running(void) {
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);
    assert(s.state == APP_ST_AGENT_RUNNING);
    assert(on == 0);                                       // PRESS/CLICK 无动作

    // UP 双击=全局清空(各态统一,AGENT_RUNNING 也不例外)—— 基线测试过期:
    // 旧断言"任意键全部忽略"未涵盖双击全局语义;OK 双击已迁到 UP
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 30);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(s.state == APP_ST_AGENT_RUNNING);

    s.state = APP_ST_TRANSCRIBING;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 40);
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(on == 0);                                       // 本次 CLICK 无动作
}

// ---- LISTENING 中 ▲/▼ 无效(不打断录音) ----
static void test_listening_arrows_ignored(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 30);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 40);
    assert(s.state == APP_ST_LISTENING);
}

// ---- READY 下无按键同样分级息屏,任意键唤醒 ----
static void test_screen_off_ready(void) {
    reset();
    s.state = APP_ST_READY;
    s.state_since_ms = now;
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);
    assert(s.screen_on == false);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));
    reduce(APP_EV_TICK, now + APP_IDLE_PANEL_OFF_MS + 1);
    assert(s.panel_on == false);
    assert(has_action(APP_ACT_UI_PANEL_OFF));

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + APP_IDLE_PANEL_OFF_MS + 2); // 唤醒
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.state == APP_ST_READY);                        // 只唤醒,不离开 READY
}

// ---- APPROVAL 无超时(安全审批必须等物理按键) ----
static void test_approval_no_timeout(void) {
    reset();
    s.state = APP_ST_APPROVAL;
    s.state_since_ms = now;
    reduce(APP_EV_TICK, now + 120 * 1000);                 // 远超 AGENT_RUNNING 的 90s
    assert(s.state == APP_ST_APPROVAL);
    assert(s.screen_on == true);                           // 也不熄屏
}

// ---- agent.status(error) → READY + 错误提示 ----
static void test_agent_error(void) {
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_ERROR, .message = "build failed" } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_READY);
    assert(strstr(s.toast, "error"));
}

// ---- NET BUSY 横幅:丢帧起、恢复止,边沿触发 ----
static void test_audio_drop_netbusy(void) {
    reset();
    app_event_t st = { .type = APP_EV_AUDIO_DROP_START };
    app_state_reduce(&s, &st, now, out, &on);
    assert(s.net_busy == true);

    reset();
    app_event_t en = { .type = APP_EV_AUDIO_DROP_END };
    app_state_reduce(&s, &en, now, out, &on);
    assert(s.net_busy == false);
}

// ---- BLE 链路事件:订阅=通,断开=断 + toast ----
static void test_ble_events(void) {
    reset();
    app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &d, now, out, &on);
    assert(s.ble_connected == false);
    assert(s.link_up == false);
    assert(strstr(s.toast, "Mac disconnected"));
    app_event_t c = { .type = APP_EV_BLE_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.ble_connected == true);
    assert(s.link_up == true);

    // 事件行丢弃:toast 提示
    app_event_t drop = { .type = APP_EV_BLE_DROP };
    app_state_reduce(&s, &drop, now, out, &on);
    assert(strstr(s.toast, "dropped"));
}

// ---- 快照:agent_state_name 保留真实 status,仅空时兜底 ----
static void test_snapshot_agent_name(void) {
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_THINKING, .message = "" } };
    app_state_reduce(&s, &ev, now, out, &on);
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_state_name, "thinking") == 0);  // 不被覆盖成 "running"

    // 新会话:进入 TRANSCRIBING 时清空,快照兜底显示 "running"
    reset();
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_state_name, "running") == 0);
}

// ---- 快照:link_up 驱动 OFFLINE 横幅 ----
static void test_snapshot_link_up(void) {
    reset();
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.link_up == false);                          // init 默认断(开机无 Mac 不会收到断开事件,不能假设通)

    app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &d, now, out, &on);
    app_state_snapshot(&s, now, &snap);
    assert(snap.link_up == false);
    assert(snap.ble_connected == false);
}

// ---- WiFi/WS 通道(Windows 移植):WS_CONNECTED = 链路通(与 BLE 对等) ----
static void test_ws_link_up(void) {
    reset();
    fake_mode_set(APP_MODE_WIFI);           // 链路事件通道门禁:WS 事件仅 WiFi 模式生效
    app_event_t c = { .type = APP_EV_WS_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.link_up == true);
    assert(s.ble_connected == false);                      // 非 BLE 通道,图标保持灰
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "WiFi") == 0);           // 横幅按通道渲染

    // WiFi 链路下 PTT 可用(HOME 单击进 READY,再按下开录)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
}

// ---- WS 断开:停流回 READY + 触发 mDNS 重查 + 通道名保留 ----
static void test_ws_link_down(void) {
    reset();
    fake_mode_set(APP_MODE_WIFI);           // 链路事件通道门禁:WS 事件仅 WiFi 模式生效
    s.link_up = true;
    s.link_channel = 1;
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_WS_DISCONNECTED };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.link_up == false);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));             // 兜底停流,同 BLE 断连
    assert(!has_action(APP_ACT_SEND_VOICE_END));           // 会话中止,不发 end
    assert(has_action(APP_ACT_RESOLVE_SERVICE));           // mDNS 重查,Companion 重启自动重连
    assert(strstr(s.toast, "offline"));

    // APPROVAL 下断开:保持状态等待重连,但也要重查
    reset();
    s.link_up = true;
    s.link_channel = 1;
    s.state = APP_ST_APPROVAL;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(has_action(APP_ACT_RESOLVE_SERVICE));

    // 快照横幅名:BLE 默认 → WiFi 断线后仍显示 WiFi
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "WiFi") == 0);
}

// ---- USB 有线通道(第三通道):USB_CONNECTED = 链路通(与 BLE/WS 对等) ----
static void test_usb_link_up(void) {
    reset();
    fake_mode_set(APP_MODE_USB);            // 链路事件通道门禁:USB 事件仅 USB 模式生效
    app_event_t c = { .type = APP_EV_USB_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.link_up == true);
    assert(s.ble_connected == false);
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "USB") == 0);              // 横幅按通道渲染

    // USB 链路下 PTT 可用(HOME 单击进 READY,再按下开录)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
}

// ---- USB 断开:停流回 READY + 通道名保留(BLE/WS 断连同路径) ----
static void test_usb_link_down(void) {
    reset();
    fake_mode_set(APP_MODE_USB);            // 链路事件通道门禁:USB 事件仅 USB 模式生效
    s.link_up = true;
    s.link_channel = 2;
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_USB_DISCONNECTED };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.link_up == false);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));               // 兜底停流,同 BLE 断连
    assert(!has_action(APP_ACT_SEND_VOICE_END));             // 会话中止,不发 end
    assert(strstr(s.toast, "USB disconnected"));

    // APPROVAL 下断开:保持状态等待重连
    reset();
    s.link_up = true;
    s.link_channel = 2;
    s.state = APP_ST_APPROVAL;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);

    // 快照横幅名:USB 断线后仍显示 USB
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "USB") == 0);
}

// ---- WiFi 失败 toast 按 reason 去重;链路恢复后允许再报 ----
static void test_wifi_fail_dedup(void) {
    reset();
    app_event_t a = { .type = APP_EV_WIFI_CONNECT_FAIL, .u.wifi_fail = { .reason = 15 } };
    app_state_reduce(&s, &a, now, out, &on);
    assert(strstr(s.toast, "WiFi disconnected"));
    const char *t1 = s.toast;

    app_state_reduce(&s, &a, now + 10, out, &on);          // 同因重连风暴:不再重复
    assert(strcmp(s.toast, t1) == 0);

    app_event_t b = { .type = APP_EV_WIFI_CONNECT_FAIL, .u.wifi_fail = { .reason = 201 } };
    app_state_reduce(&s, &b, now + 20, out, &on);          // 不同原因:允许再报
    assert(strstr(s.toast, "WiFi disconnected"));

    // 链路恢复(WS 连上)后清位:同因新片段允许再报
    reset();
    app_state_reduce(&s, &a, now, out, &on);
    app_event_t c = { .type = APP_EV_WS_CONNECTED };
    app_state_reduce(&s, &c, now + 10, out, &on);
    app_state_reduce(&s, &a, now + 20, out, &on);
    assert(strstr(s.toast, "WiFi disconnected"));
}

// ---- mDNS 发现新目标:产出 WS_RETARGET 动作(带 URL) ----
static void test_ws_target_found(void) {
    reset();
    app_event_t ev = { .type = APP_EV_WS_TARGET_FOUND };
    strcpy(ev.u.ws_target.url, "ws://10.0.0.8:8765");
    app_state_reduce(&s, &ev, now, out, &on);
    app_action_t *rt = find_action(APP_ACT_WS_RETARGET);
    assert(rt != NULL);
    assert(strcmp(rt->u.ws_target.url, "ws://10.0.0.8:8765") == 0);
    // 仅改运行时目标,不置链路状态(WS 尚未连接)
    assert(s.link_up == false);
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "BLE") == 0);
}

// ---- 校时下行:透传动作,状态不变(与链路状态正交) ----
static void test_time_set(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_READY;
    app_event_t ev = { .type = APP_EV_TIME_SET, .u.time_set.epoch = 1767225600LL };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(on == 1);                                        // 仅 TIME_SET 动作
    app_action_t *t = find_action(APP_ACT_TIME_SET);
    assert(t && t->u.time_set.epoch == 1767225600LL);       // epoch 透传无损
    assert(s.state == APP_ST_READY);                        // 状态机不受影响
    assert(s.link_up == true);

    // 负 epoch 同样透传(校时值域不含正负限制)
    reset();
    ev = (app_event_t){ .type = APP_EV_TIME_SET, .u.time_set.epoch = -1 };
    app_state_reduce(&s, &ev, now, out, &on);
    t = find_action(APP_ACT_TIME_SET);
    assert(t && t->u.time_set.epoch == -1);
}


// ---- 链路事件通道门禁(审查 P1):非本模式通道事件不翻转链路状态 ----
static void test_link_event_gate(void) {
    // USB 模式(USB 会话中):BLE 连/断只更新图标,不动 link_up、不掐音频流
    reset();
    fake_mode_set(APP_MODE_USB);
    s.link_up = true;                      // USB 会话进行中
    s.link_channel = 2;
    app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &d, now, out, &on);
    assert(s.ble_connected == false);      // 图标如实更新
    assert(s.link_up == true);             // 链路不被 BLE 事件翻转
    assert(!has_action(APP_ACT_STREAM_CANCEL));   // 不掐 USB 音频流
    assert(s.state == APP_ST_READY || s.state == APP_ST_HOME);

    app_event_t c = { .type = APP_EV_BLE_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.ble_connected == true);
    assert(s.link_up == true);
    assert(s.link_channel == 2);           // 通道名保持 USB

    // 模式切换窗口:投递的旧通道断连事件必须放行(状态机收束)
    fake_mode_set(APP_MODE_BLE);
    fake_mode_set_switching(true);
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;
    s.link_up = true;
    s.link_channel = 2;                    // 切换前仍是 USB 链路
    app_event_t old = { .type = APP_EV_USB_DISCONNECTED };
    app_state_reduce(&s, &old, now, out, &on);
    assert(s.link_up == false);            // 切换收束事件不被门禁挡住
    assert(has_action(APP_ACT_STREAM_CANCEL));
    assert(s.state == APP_ST_READY);
    fake_mode_set_switching(false);

    // 窗口外:非本模式事件仍被挡(防御)
    fake_mode_set(APP_MODE_BLE);
    s.link_up = true;
    app_event_t wd = { .type = APP_EV_WS_DISCONNECTED };
    app_state_reduce(&s, &wd, now, out, &on);
    assert(s.link_up == true);
}

int main(void) {
    test_home_nav();
    test_down_enter_clear();
    test_ptt_offline_online();
    test_tone_press_produce();
    test_tone_done_idempotent();
    test_tone_done_late_after_send();
    test_tone_release_sends_order();
    test_tone_tick_fallback();
    test_tone_done_ignored_elsewhere();
    test_listening_end();
    test_up_long_ptt();
    test_timeouts();
    test_agent_running_timeout();
    test_agent_status_flow();
    test_approval();
    test_approval_during_listening();
    test_screen_off_wake();
    test_screen_off_ready();
    test_transcript_display();
    test_ble_link_down();
    test_ble_disconnect_transcribing();
    test_keys_ignored_in_running();
    test_listening_arrows_ignored();
    test_approval_no_timeout();
    test_agent_error();
    test_audio_drop_netbusy();
    test_ble_events();
    test_snapshot_agent_name();
    test_snapshot_link_up();
    test_ws_link_up();
    test_ws_link_down();
    test_usb_link_up();
    test_usb_link_down();
    test_wifi_fail_dedup();
    test_ws_target_found();
    test_time_set();
    test_bounded();
    test_link_event_gate();
    printf("test_app_state: all assertions passed\n");
    return 0;
}
