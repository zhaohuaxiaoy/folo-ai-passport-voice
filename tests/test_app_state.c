// 状态机全转移主机测试(纯 C,assert 断言)。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"

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

// ---- HOME:▲/▼ 进入 READY 并轮播;● 单击进入 READY ----
static void test_home_nav(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 10);
    assert(s.state == APP_ST_READY);
    assert(s.workflow == APP_WF_CAPTURE);           // BUILD 上一个是 CAPTURE(回绕)
    assert(has_action(APP_ACT_SEND_WORKFLOW_SWITCH));

    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 10);
    assert(s.state == APP_ST_READY);
    assert(s.workflow == APP_WF_DEBUG);

    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    assert(s.workflow == APP_WF_BUILD);             // OK 进 READY 不改工作流
}

// ---- READY:▲/▼ 轮播 5 工作流并回绕 ----
static void test_workflow_cycle(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);   // → READY (BUILD)
    app_workflow_t wf = APP_WF_BUILD;
    for (int i = 0; i < 5; i++) {
        reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 10 + i);
        wf = (app_workflow_t)((wf + 1) % APP_WF_COUNT);
        assert(s.workflow == wf);
        app_action_t *a = find_action(APP_ACT_SEND_WORKFLOW_SWITCH);
        assert(a && a->u.workflow == (uint8_t)wf);
    }
    // 5 次后回绕到 BUILD;再按 ▲ 应回到 CAPTURE
    assert(s.workflow == APP_WF_BUILD);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 10);
    assert(s.workflow == APP_WF_CAPTURE);
}

// ---- PTT:离线被拒 + 错误音;在线开流 ----
static void test_ptt_offline_online(void) {
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 离线按 ●
    assert(s.state == APP_ST_READY);                       // 原地不动
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_ERROR);
    assert(strstr(s.toast, "OFFLINE"));

    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 在线按 ●
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

// ---- S3:READY+OK PRESS 只产出滴声 + voice.start,开流等 TONE_DONE ----
static void test_tone_press_produce(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 开录
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 开录,未开流
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 110, out, &on);        // 滴声播完
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    app_state_reduce(&s, &ev, now + 120, out, &on);        // 重复(异常重复投递)
    assert(on == 0);                                       // 幂等:无动作
}

// ---- S3:双击取消在滴声期间,迟到的 TONE_DONE 必须忽略(不误开流) ----
static void test_tone_done_late_after_cancel(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 按下 #1:入 LISTENING,未开流
    assert(!has_action(APP_ACT_STREAM_START));
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 30);  // 待定
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 200);   // 按下 #2:取消 → READY
    assert(s.state == APP_ST_READY);
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 210, out, &on);        // 滴声播完事件此刻才到
    assert(on == 0);                                       // 已离开 LISTENING → 忽略
    assert(!has_action(APP_ACT_STREAM_START));
}

// ---- S3:单击发送完整序列——先 TONE_DONE 开流,单击窗口到期才停流 ----
static void test_tone_single_tap_order(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 开录
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 110, out, &on);        // 滴声播完 → 开流
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 3000); // 待定
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 3300);  // 窗口到期 → 发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));               // 停流仍由 CLICK 产出
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
}

// ---- S3:兜底——TICK 500ms 未收 TONE_DONE → 强制开流(防声音任务异常卡死) ----
static void test_tone_tick_fallback(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 开录
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

// ---- LISTENING:松开进入"待定结束",单击到期发送,窗口内再按 = 取消 ----
static void test_listening_single_tap(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);
    // 真实事件序列:按下 → 松开(不立即结束,等单击窗口) → 单击
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 3000);
    assert(s.state == APP_ST_LISTENING);                   // 松开不结束,待定
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 3300);  // 窗口到期 → 正式发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_SEND);
}

// ---- LISTENING:双击取消(真实事件序列,第二次按下即取消) ----
static void test_listening_double_cancel(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 按下 #1:开录
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 30);  // 松开 #1:待定
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 200);   // 按下 #2:取消
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));             // 清残留,防流入下一次会话
    assert(has_action(APP_ACT_SEND_VOICE_END));            // 结束 Mac 端会话(防 ASR 悬挂)
    assert(strstr(s.toast, "cancelled"));
    // 双击窗口后的收尾事件(松开 #2 / DOUBLE)在 READY 下全部忽略
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 500);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 600);
    assert(s.state == APP_ST_READY);
}

// ---- LISTENING 端:见 test_listening_single_tap / test_listening_double_cancel ----
static void test_listening_end(void) {
    test_listening_single_tap();
    test_listening_double_cancel();
}

// ---- 超时:TRANSCRIBING 30s / AGENT_RUNNING 90s → READY ----
static void test_timeouts(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 3000);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 3300);  // 单击窗口到期 → TRANSCRIBING
    assert(s.state == APP_ST_TRANSCRIBING);
    reduce(APP_EV_TICK, s.state_since_ms + 29 * 1000);     // 进入后 29s 未到
    assert(s.state == APP_ST_TRANSCRIBING);
    reduce(APP_EV_TICK, s.state_since_ms + 31 * 1000);     // 进入后 31s 超时
    assert(s.state == APP_ST_READY);
    assert(strstr(s.toast, "timeout"));
    assert(!has_action(APP_ACT_SEND_WORKFLOW_SWITCH));     // 中止路径不该重发工作流
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
    assert(s.state == APP_ST_DONE);
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

    // ▼ 详情视图切换
    s.state = APP_ST_AGENT_RUNNING;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.approval_details == false);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 300);
    assert(s.approval_details == true);
}

// ---- DONE:●/▼ 回 HOME ----
static void test_done_back_home(void) {
    reset();
    s.state = APP_ST_DONE;
    s.state_since_ms = now;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_HOME);

    reset();
    s.state = APP_ST_DONE;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 10);
    assert(s.state == APP_ST_HOME);

    reset();
    s.state = APP_ST_DONE;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 10);    // ▲ 无效
    assert(s.state == APP_ST_DONE);
}

// ---- 息屏/唤醒 ----
static void test_screen_off_wake(void) {
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_SCREENOFF_MS + 1);  // 60s 无键
    assert(s.screen_on == false);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + APP_IDLE_SCREENOFF_MS + 2); // 任意键唤醒
    assert(s.screen_on == true);
    assert(has_action(APP_ACT_UI_SCREEN_ON));

    // 唤醒后的同一事件流:CLICK 应被忽略(不产生状态跳转)
    s.screen_on = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // 息屏时 CLICK 不唤醒
    assert(s.state == APP_ST_HOME);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 唤醒后再按键才生效
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 30);
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

// ---- mac.metrics 快照 ----
static void test_metrics(void) {
    reset();
    app_event_t ev = { .type = APP_EV_MAC_METRICS,
                       .u.metrics = { .cpu = 34, .ram = 58, .battery = 85,
                                      .charging = true, .active_app = "TRAE" } };
    app_state_reduce(&s, &ev, now, out, &on);
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.mac_cpu == 34);
    assert(snap.mac_ram == 58);
    assert(snap.mac_batt == 85);
    assert(snap.mac_charging);
    assert(strcmp(snap.active_app, "TRAE") == 0);
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);

    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1", .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now + 100, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));            // 结束被截断的会话,Mac 侧关闭 ASR
    // APPROVAL 下再松开/双击:忽略,不再触碰管线
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 200);
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
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 30);
    assert(s.state == APP_ST_AGENT_RUNNING);
    assert(on == 0);                                       // 无任何动作

    s.state = APP_ST_TRANSCRIBING;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 40);
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(on == 0);
}

// ---- LISTENING 中 ▲/▼ 无效(不打断录音) ----
static void test_listening_arrows_ignored(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 20);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 30);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 40);
    assert(s.state == APP_ST_LISTENING);
    assert(s.workflow == APP_WF_BUILD);                    // 工作流未变
}

// ---- DONE 下 60s 无按键同样熄屏,任意键唤醒 ----
static void test_screen_off_done(void) {
    reset();
    s.state = APP_ST_DONE;
    s.state_since_ms = now;
    reduce(APP_EV_TICK, now + APP_IDLE_SCREENOFF_MS + 1);
    assert(s.screen_on == false);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + APP_IDLE_SCREENOFF_MS + 2); // 唤醒
    assert(s.screen_on == true);
    assert(s.state == APP_ST_DONE);                        // 只唤醒,不离开 DONE
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_HOME);                        // 再单击才回 HOME
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

int main(void) {
    test_home_nav();
    test_workflow_cycle();
    test_ptt_offline_online();
    test_tone_press_produce();
    test_tone_done_idempotent();
    test_tone_done_late_after_cancel();
    test_tone_single_tap_order();
    test_tone_tick_fallback();
    test_tone_done_ignored_elsewhere();
    test_listening_end();
    test_timeouts();
    test_agent_running_timeout();
    test_agent_status_flow();
    test_approval();
    test_approval_during_listening();
    test_done_back_home();
    test_screen_off_wake();
    test_screen_off_done();
    test_transcript_display();
    test_ble_link_down();
    test_ble_disconnect_transcribing();
    test_keys_ignored_in_running();
    test_listening_arrows_ignored();
    test_approval_no_timeout();
    test_agent_error();
    test_audio_drop_netbusy();
    test_ble_events();
    test_metrics();
    test_snapshot_agent_name();
    test_snapshot_link_up();
    test_bounded();
    printf("test_app_state: all assertions passed\n");
    return 0;
}
