// 状态机全转移主机测试(纯 C,assert 断言)。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "fake_mode.h"

// 通道常量契约(双通道常开,2026-08-28):BLE=0/USB=2 —— 与旧 NVS 存储语义
// 保持一致(只删中间值不重排),状态机 link_channel 直接沿用。
_Static_assert(APP_CHAN_BLE == 0, "APP_CHAN_BLE must be 0");
_Static_assert(APP_CHAN_USB == 2, "APP_CHAN_USB must be 2");

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

// 带 ADC 读数的按键事件(mv 默认 0 = 真实按压;≥2000 = 松开电平 = 幽灵事件)
static void reduce_btn_mv(app_event_type_t t, app_btn_t b, uint16_t mv, uint64_t ts) {
    app_event_t ev = { .type = t };
    ev.u.key.btn = b;
    ev.u.key.mv = mv;
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

// ---- HOME:● 单击进入 READY;▼ 单击=回车 / 长按=清空;▲ 无空闲语义 ----
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
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 10);
    assert(s.state == APP_ST_HOME);                // DOWN 长按=清空,不切走
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);

    // 三颗键的双击一律无语义(清空已从 UP 双击迁到 DOWN 长按,2026-08-29)
    for (int i = 0; i < 3; i++) {
        const app_btn_t b[] = { APP_BTN_UP, APP_BTN_DOWN, APP_BTN_OK };
        reset();
        reduce_btn(APP_EV_KEY_DOUBLE, b[i], now + 10);
        assert(s.state == APP_ST_HOME);
        assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    }

    // ▲ 空闲:无切换、无动作
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 10);
    assert(s.state == APP_ST_HOME);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
}

// ---- READY:OK 已退出 PTT;▲ 长按=说话;▼ 单击=回车 / 长按=清空 ----
static void test_down_enter_clear(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);   // → READY (BUILD)
    assert(s.state == APP_ST_READY);

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 20);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 30);  // DOWN 长按 = 清空
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    // 长按态松开只报 LONG_UP,不补 CLICK —— 清空之后绝不会再补一次回车
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_DOWN, now + 40);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    assert(s.state == APP_ST_READY);

    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_DOWN, now + 30);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));    // DOWN 双击无语义

    // UP 空闲
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 40);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // PTT 不受影响(基线缺陷:离线检查引入后此段未设 link_up,PTT 被拒;修正)
    s.link_up = true;
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 50);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
}

// ---- 键位改版回归(2026-08-29:清空 UP 双击 → DOWN 长按)----
// 旧版为了让"双击清空"和"长按说话"挤在 UP 一颗键上,堆了四层补偿:自建轻点链
// (UP_TAP_CHAIN_MS 1800)、挂起确认(PTT_CONFIRM_MS 250)、回弹假双击防御、驱动
// 补报去重。清空搬到 DOWN 长按后这四层全部删除,对应的五个用例
// (test_double_click_not_ptt / test_tap_chain_clear / test_slow_double_clear /
// test_real_device_rhythm_clear / test_single_tap_no_clear)一并删除,换成下面两个
// 反向断言:UP 上再没有轻点语义,长按说话不再被任何前置轻点拖慢。

// UP 的轻点/单击/双击一律无清空语义。按下即录之后(2026-08-29)轻点会开一次
// 会话并在松开时当场收束(< PTT_MIN_TALK_MS),但绝不产生任何按键上行动作
static void test_up_taps_never_clear(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);      // → READY

    // 连续三次轻点(旧版第二次就会判双击清空)
    for (int i = 0; i < 3; i++) {
        reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 20);
        reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 120);
        assert(!has_action(APP_ACT_SEND_KEY_ACTION));
        assert(s.state == APP_ST_READY);                 // 轻点开的会话已收束
        reduce_btn(APP_EV_KEY_CLICK,   APP_BTN_UP, now + 180);
        assert(!has_action(APP_ACT_SEND_KEY_ACTION));
        assert(on == 0);                                  // 补报单击在 READY 无语义
    }
    // 驱动补报 DOUBLE 也不清空
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 50);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    assert(s.state == APP_ST_READY);
}

// 轻点之后立刻按住说话:必须当场开录音(按下即录,更不存在 250ms 挂起确认)
static void test_ptt_no_confirm_delay(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);      // → READY

    reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 20);    // 轻点
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 120);
    reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 60);    // 紧接着按住(旧版判"链内")
    assert(s.state == APP_ST_LISTENING);                      // 按下当场开录
    assert(has_action(APP_ACT_SEND_VOICE_START));
    reduce_btn(APP_EV_KEY_LONG,    APP_BTN_UP, now + 500);    // 驱动到点补报,已在录音中
    assert(s.state == APP_ST_LISTENING);
    assert(on == 0);                                          // 补报长按不再产生动作
    // 松手正常发送(按住够久,不触发误触收口)
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000);
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_SEND_VOICE_END));

    // 误碰级短按(< PTT_MIN_TALK_MS)收口:丢音频、不进转写。短按不到长按阈值,
    // 松开只报 RELEASE(没有 LONG_UP),结束分支必须同时认这一种。
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 120);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    // 驱动随后补报的单击落在 READY:无语义,零动作
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 180);
    assert(on == 0);
}

// ---- PTT:离线被拒 + 错误音;在线开流 ----
static void test_ptt_offline_online(void) {
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 离线按下 ▲
    assert(s.state == APP_ST_READY);                       // 原地不动
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_ERROR);
    assert(strstr(s.toast, "OFFLINE"));

    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 在线按下 ▲
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录,未开流
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 按下 #1:入 LISTENING,未开流
    assert(!has_action(APP_ACT_STREAM_START));
    // 松开:立即发送(≥ PTT_MIN_TALK_MS,否则算阈值误触被取消,见 test_ptt_no_confirm_delay)
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 500);
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 110, out, &on);        // 滴声播完 → 开流
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000); // 松开:立即停流发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));               // 停流由 RELEASE 产出
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
}

// ---- S3:兜底——TICK 满 APP_TONE_PENDING_TIMEOUT_MS 未收 TONE_DONE → 强制开流 ----
// 阈值用常量表达(2026-08-29 由 500 降到 200):这条断言卡的是"到点才开、早一毫秒
// 不开",与具体毫秒值无关,常量再调本测试自动跟随。
static void test_tone_tick_fallback(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    reduce(APP_EV_TICK, s.state_since_ms + APP_TONE_PENDING_TIMEOUT_MS - 1);   // 差 1ms
    assert(!has_action(APP_ACT_STREAM_START));
    assert(!s.stream_started);
    reduce(APP_EV_TICK, s.state_since_ms + APP_TONE_PENDING_TIMEOUT_MS);       // 到点兜底
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000); // 松开:立即发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
    // 转写期静音(用户需求 2026-08-28):松开不再播发送音
    assert(!has_action(APP_ACT_PLAY_TONE));
}

// ---- DOWN(音量减)长按 0.5s = 清空输入框(2026-08-29 从 UP 双击迁来)----
static void test_down_long_clears_input(void) {
    // READY:长按判定当场清空 + 一声确认音;不碰会话状态
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_DOWN, now + 20);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));          // 按下还不算
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 500);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    app_action_t *tone = find_action(APP_ACT_PLAY_TONE);
    assert(tone && tone->u.tone == APP_TONE_REJECT);       // 阈值到点的确认音
    assert(!has_action(APP_ACT_SEND_VOICE_START));         // 不开录音
    assert(s.state == APP_ST_READY);
    // 松开:长按态只报 LONG_UP,不补 CLICK → 不会再发一次回车
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_DOWN, now + 300);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // 全局语义:转写中 / Agent 运行中同样清空,且不动会话
    const app_stage_t st_list[] = { APP_ST_TRANSCRIBING, APP_ST_AGENT_RUNNING,
                                    APP_ST_HOME, APP_ST_APPROVAL };
    for (unsigned i = 0; i < sizeof(st_list) / sizeof(st_list[0]); i++) {
        reset();
        s.link_up = true;
        s.state = st_list[i];
        s.state_since_ms = now;
        reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 10);
        a = find_action(APP_ACT_SEND_KEY_ACTION);
        assert(a && a->u.key_action.action == APP_KEY_CLEAR);
        assert(!has_action(APP_ACT_STREAM_CANCEL));        // 不清会话
        assert(!has_action(APP_ACT_STREAM_STOP));
        assert(s.state == st_list[i]);                     // 状态不变
    }

    // 幽灵长按(回调 mv 回到松开电平 2890 = 无人按键,射频腐蚀残留)必须丢弃 ——
    // 清空是破坏性动作,不能被假事件触发
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_DOWN, 2890, now + 500);
    assert(on == 0);                                       // 零动作
    // 真实按压读数(DOWN 档 150-447mV)照常清空
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_DOWN, 300, now + 500);
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);

    // UP 双击不再清空(语义已迁走);OK 双击本来就没有
    reset();
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 10);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 10);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // PTT 结束后立刻长按 DOWN 清空:不再有回弹防御窗口把它吞掉(回弹扫过 DOWN 档
    // 只是掠过,凑不出 500ms 长按,不需要靠时间窗防)
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 500);     // 开录
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500); // 松开 → TRANSCRIBING
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 100);   // 松开 100ms 后就清空
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(s.state == APP_ST_TRANSCRIBING);                 // 会话不受影响
}

// ---- UP(音量加)= 按住说话:PRESS 当场开录(滴声同时响),松开发送 ----
// 2026-08-29:开录事件由 LONG(0.5s 阈值)改为 PRESS —— 真机实测按下到 `采集开始`
// 要 ~1.01s(428ms 阈值 + 578ms 滴声门禁),用户第一秒的话被吞掉。
static void test_up_press_ptt(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 500);    // 按下即录
    assert(s.state == APP_ST_LISTENING);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_START);              // 滴声在按下时响
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_STREAM_STOP));              // 未结束
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500); // 松开:发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
    // 转写期静音(用户需求 2026-08-28):松开不再播发送音
    assert(!has_action(APP_ACT_PLAY_TONE));

    // 离线:按下 UP 被拒,error 音 + toast,不进 LISTENING
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 500);
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_START));
    t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_ERROR);
    assert(strstr(s.toast, "OFFLINE"));

    // 只有补报单击、没有 PRESS(测试构造)时不触发任何动作、无提示音
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

// ---- 按下即录的三道防线(2026-08-29)----
// 1) 幽灵 PRESS 判假:UP 侧用分压档上限(≤150mV)而不是通用的 2000mV —— 真机环里
//    抓到过 `btn=0 ev=0 mv=598`(OK 键按住期间冒出的跨档假 UP 按下),2000 放它过去。
// 2) 短按收口:< PTT_MIN_TALK_MS 松手 → 丢音频 + 收束回 READY。
// 3) 不变量 PTT_MIN_TALK_MS ≥ 驱动 long_press_time(500ms):短于阈值的一按事后会被
//    驱动补报 SINGLE_CLICK,而 UP 单击在 TRANSCRIBING 是"退出转写"。取等号让二者互斥
//    —— 能进转写的一按必然到过阈值,到过阈值的一按永不补报单击。
static void test_ptt_press_start_guards(void) {
    // 幽灵按下(松开电平 2890)不开会话
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);          // → READY
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 2890, now + 20);
    assert(s.state == APP_ST_READY);
    assert(on == 0);

    // 跨档幽灵按下(598mV 落在 OK 档)同样不开会话
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 598, now + 30);
    assert(s.state == APP_ST_READY);
    assert(on == 0);

    // 真实按下(3mV)当场开会话
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 3, now + 40);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));

    // 不变量:差一毫秒到阈值(499ms)必须收束回 READY,绝不能进 TRANSCRIBING ——
    // 否则紧随的补报单击会把这次转写撤掉。
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 499);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 180);         // 驱动补报
    assert(s.state == APP_ST_READY);
    assert(on == 0);

    // 刚好到阈值(500ms):正常发送进转写,此时驱动只报 LONG_UP,不补单击
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 3, now + 20);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 500);
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_SEND_VOICE_END));
    // 长按态松开后驱动还会补一条 RELEASE:已不在 LISTENING,零动作
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 1);
    assert(on == 0);
    assert(s.state == APP_ST_TRANSCRIBING);
}

// ---- LISTENING 端:见 test_listening_release_sends / test_down_long_clears_input ----
static void test_listening_end(void) {
    test_listening_release_sends();
    test_down_long_clears_input();
}

// ---- 超时:TRANSCRIBING 30s / AGENT_RUNNING 90s → READY ----
static void test_timeouts(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
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
    // 无 DONE 页:done 直接回 READY 待命(用户:不要 done 提示),
    // 成功音一并取消(用户要求转写→ready 静音,2026-08-28)
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_PLAY_TONE));
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
// PRESS 任意键 → 面板上电 + 背光亮。自动息屏只是省电显示态:唤醒后事件
// 照常放行执行(不吞按键;锁定态的全键忽略见锁屏用例,两回事)。
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

    // 自动息屏只省显示,不吞按键(过修修正):PRESS 唤醒后事件照常放行执行。
    // 真实事件流 CLICK 前必有 PRESS,设备在 CLICK 到达时已亮屏。
    s.screen_on = false;
    s.panel_on = true;
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 10);    // PRESS:恢复背光
    assert(s.screen_on == true);
    assert(!has_action(APP_ACT_UI_PANEL_ON));              // 面板已通电,不发 PANEL_ON
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);    // 同一次按键的 CLICK
    assert(s.state == APP_ST_READY);                       // 照常执行:HOME ● 单击 → READY
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

// ---- TRANSCRIBING:音量+单击退出转写场景(2026-08-28 用户需求);迟到文本丢弃 ----
static void reduce_up_press(uint16_t mv, uint64_t ts) {
    app_event_t ev = { .type = APP_EV_KEY_PRESS };
    ev.u.key.btn = APP_BTN_UP;
    ev.u.key.mv = mv;
    now = ts;
    app_state_reduce(&s, &ev, now, out, &on);
}

static void test_transcribing_exit(void) {
    // 真实单击退出:PRESS(mv=3,手指在键上)→ RELEASE → CLICK → READY
    reset();
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    reduce_up_press(3, now + 100);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 200);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 250);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_UI_REFRESH));

    // 退出后(READY)迟到的识别结果丢弃,不再上屏
    app_event_t ev = { .type = APP_EV_TRANSCRIPT,
                       .u.transcript = { .text = "late result", .inject_mode = APP_INJECT_TYPE,
                                         .final = true } };
    app_state_reduce(&s, &ev, now + 300, out, &on);
    assert(strcmp(s.agent_message, "late result") != 0);

    // 假按风暴不退出:PRESS(mv=2890,无人按键)→ RELEASE → CLICK → 状态不变
    reset();
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    reduce_up_press(2890, now + 100);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 200);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 250);
    assert(s.state == APP_ST_TRANSCRIBING);

    // HOME 态迟到文本同样丢弃
    reset();
    s.state = APP_ST_HOME;
    ev.u.transcript.text[0] = 'x'; ev.u.transcript.text[1] = '\0';
    ev.u.transcript.final = false;
    app_state_reduce(&s, &ev, now + 10, out, &on);
    assert(strcmp(s.agent_message, "x") != 0);

    // TRANSCRIBING 态文本照常显示(正常路径不受门禁影响)
    reset();
    s.state = APP_ST_TRANSCRIBING;
    app_state_reduce(&s, &ev, now + 10, out, &on);
    assert(strcmp(s.agent_message, "x") == 0);
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
    s.state = APP_ST_TRANSCRIBING;   // 文本只在转写中显示(READY/HOME 迟到丢弃)
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
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

    // DOWN 长按=全局清空(各态统一,AGENT_RUNNING 也不例外)—— 基线测试过期:
    // 旧断言"任意键全部忽略"未涵盖全局清空语义(先 OK 双击 → UP 双击 → DOWN 长按)
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 30);
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
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
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

// ---- 长按 OK 锁定息屏 ----
// 锁定:亮屏 HOME/READY 下长按 OK(0.5s 阈值)→ locked + 背光灭 + 面板 SLPIN 断电
static void test_ok_long_locks(void) {
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);    // 长按 OK 锁定
    assert(s.locked == true);
    assert(s.screen_on == false);
    assert(s.panel_on == false);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));
    assert(has_action(APP_ACT_UI_PANEL_OFF));
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.screen_on == false);
    assert(snap.panel_on == false);

    // READY 态同样可锁定(单击 OK 先进 READY)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);
    assert(s.locked == true);
    assert(s.screen_on == false);
    assert(s.panel_on == false);
}

// 锁定态按键照常执行但不亮屏(2026-08-28 语义变更:锁定=省电模式,不是输入锁)
// 长按说话/回车/清空全部照常;任何按键不唤醒(口袋盲操作);仅 OK 长按解锁亮屏
static void test_locked_keys_operate_screen_off(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);     // 锁定
    assert(s.locked == true);
    assert(s.screen_on == false);

    // UP 按下即录(2026-08-29):锁定态照常开录,且不亮屏(口袋盲操作)
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 30);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(s.screen_on == false);                          // 保持息屏
    assert(s.panel_on == false);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 100); // 误碰级短按:收束回 READY
    assert(s.state == APP_ST_READY);
    assert(s.screen_on == false);

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 40);   // DOWN 长按:照常清空
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(s.screen_on == false);                          // 执行但不亮屏

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 50);  // DOWN 单击:照常回车
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);
    assert(s.screen_on == false);

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 60);    // UP 单击(READY 无分支)
    assert(on == 0);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 70);    // OK 单击(READY 无分支)
    assert(on == 0);
    assert(s.locked == true);
    assert(s.screen_on == false);

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 80);     // 再按 UP:照常开录
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(s.screen_on == false);                          // 口袋说话,屏幕仍关

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 90);     // 长按 OK:解锁亮屏
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.state == APP_ST_LISTENING);                   // 录音态不受解锁影响
}

// 锁定态收到审批请求:强制解锁亮屏(防口袋盲批;与"APPROVAL 常亮"政策一致)
static void test_locked_approval_forces_wake(void) {
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);     // 锁定
    assert(s.locked == true);
    assert(s.screen_on == false);

    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1",
                                       .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now + 20, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    assert(has_action(APP_ACT_UI_PANEL_ON));

    // 解锁后 OK 单击照常批准(锁定遗留不吞按键)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 30);
    app_action_t *a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_APPROVE);
}

// 锁定态长按 OK 解锁:亮屏 + 全屏重绘,state 保持原值
static void test_ok_long_unlocks(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);     // 锁定
    assert(s.locked == true);
    assert(s.state == APP_ST_READY);                       // 锁定不切走状态

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 30);     // 再长按解锁
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(has_action(APP_ACT_UI_PANEL_ON));
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    assert(has_action(APP_ACT_UI_REFRESH));
    assert(s.state == APP_ST_READY);                       // state 仍为原状态
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.screen_on == true);
    assert(snap.panel_on == true);
}

// 唤醒防误锁:自动息屏 → OK PRESS 唤醒 → 1s 内 OK LONG 不锁定(防"唤醒即锁")
static void test_wake_no_relock(void) {
    reset();
    const uint64_t t0 = now;
    reduce(APP_EV_TICK, t0 + APP_IDLE_BACKLIGHT_OFF_MS + 1);  // 20s 关背光
    assert(s.screen_on == false);
    reduce(APP_EV_TICK, t0 + APP_IDLE_PANEL_OFF_MS + 1);      // 60s 面板断电
    assert(s.panel_on == false);

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, t0 + APP_IDLE_PANEL_OFF_MS + 2); // 长按唤醒的 PRESS
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.wake_ms == now);                                 // 唤醒时刻已记录

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 500);       // 500ms 后 LONG 到达
    assert(s.locked == false);                                // guard 挡住:不锁定
    assert(s.screen_on == true);                              // 保持亮屏
    assert(on == 0);                                          // LONG 零动作
}

// ---- 息屏后按键放行(过修修正 2026-08-28):自动息屏只是省电显示态 ----
// 息屏(超时 tick 驱动)后按键应唤醒并照常执行操作,不再被吞;锁定态的
// "照常执行但不亮屏"由 locked 门禁负责(见 test_locked_keys_operate_screen_off)。
static void test_screen_off_keys_pass_through(void) {
    // a. ▼ DOWN 单击(息屏):唤醒 + APP_KEY_ENTER 上行
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);   // 20s 无键:关背光
    assert(s.screen_on == false);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_DOWN, now + 10);       // 按下:唤醒
    assert(s.screen_on == true);
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 20);       // 单击:照常回车
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);

    // b. ▲ UP 按下(READY,息屏):唤醒 + start_ptt 进 LISTENING(同一个 PRESS)
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);         // → READY
    reduce(APP_EV_TICK, s.last_key_ms + APP_IDLE_BACKLIGHT_OFF_MS + 1); // 息屏
    assert(s.screen_on == false);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 10);         // 按下:唤醒 + 开录
    assert(s.screen_on == true);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));

    // c. ● OK 长按(息屏):PRESS 唤醒后 1s 内 LONG 被 OK_LONG_GUARD 挡,不锁定
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);   // 息屏
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 10);         // 按下:唤醒
    assert(s.screen_on == true);
    assert(s.wake_ms == now);                                   // 唤醒时刻已记录
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 500);         // 500ms 后 LONG
    assert(s.locked == false);                                  // 防"唤醒即锁"
    assert(s.screen_on == true);
    assert(on == 0);                                            // LONG 零动作

    // d. ▼ DOWN 长按(息屏):唤醒 + 清空上行
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);   // 息屏
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_DOWN, now + 10);       // 按下:唤醒
    assert(s.screen_on == true);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 500);       // 0.5s 长按:清空
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
}

// 重复锁定/解锁循环:解锁后(距唤醒 >1s)再长按 OK 正常锁定
static void test_relock_cycle(void) {
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);        // 锁定
    assert(s.locked == true);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);        // 解锁
    assert(s.locked == false);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 30);        // 再锁定
    assert(s.locked == true);
    assert(s.screen_on == false);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 40);        // 再解锁
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.state == APP_ST_HOME);                           // 状态自始至终不变
}

// 回归:LISTENING 中长按 OK 不锁定;APPROVAL 中 OK 单击仍批准、长按不锁定
static void test_lock_guards_listening_approval(void) {
    // LISTENING(录音中):长按 OK 不锁定,录音不受影响
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);       // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);        // 开录
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 30);
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.state == APP_ST_LISTENING);

    // APPROVAL:OK 长按不锁定;OK 单击仍批准
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1",
                                       .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 100);
    assert(s.locked == false);
    assert(s.state == APP_ST_APPROVAL);                       // 不锁定不切换
    assert(s.screen_on == true);                              // 审批常亮保持
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 200);      // 批准
    app_action_t *a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_APPROVE);
    assert(s.state == APP_ST_AGENT_RUNNING);
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

// ---- USB 有线通道:USB_CONNECTED = 链路通(与 BLE 对等;双通道常开无门禁) ----
static void test_usb_link_up(void) {
    reset();
    app_event_t c = { .type = APP_EV_USB_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.link_up == true);
    assert(s.ble_connected == false);
    assert(s.link_channel == APP_CHAN_USB);                 // 首个连接通道 = 会话路由
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "USB") == 0);              // 横幅按通道渲染

    // USB 链路下 PTT 可用(HOME 单击进 READY,再按下开录)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
}

// ---- USB 断开:停流回 READY + 通道名保留(与 BLE 断连同路径) ----
static void test_usb_link_down(void) {
    reset();
    s.link_up = true;
    s.link_channel = APP_CHAN_USB;
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
    s.link_channel = APP_CHAN_USB;
    s.state = APP_ST_APPROVAL;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);

    // 快照横幅名:USB 断线后仍显示 USB
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "USB") == 0);
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


// ---- 双通道常开(2026-08-28):会话粘性 —— 活动会话期间另一通道连/断不夺路 ----
// BLE 会话进行中:USB 连接只刷新(不夺路),USB 断开不掐 BLE 音频流。
static void test_dual_session_survives_other_channel(void) {
    reset();
    s.link_up = true;
    s.link_channel = APP_CHAN_BLE;         // BLE 会话路由中
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;

    // USB 连接:空闲态才夺路;活动会话中只当候选,link_channel 保持 BLE
    app_event_t uc = { .type = APP_EV_USB_CONNECTED };
    app_state_reduce(&s, &uc, now, out, &on);
    assert(s.link_channel == APP_CHAN_BLE);
    assert(s.link_up == true);
    assert(s.state == APP_ST_LISTENING);

    // USB 断开:非会话通道,不影响本会话
    app_event_t ud = { .type = APP_EV_USB_DISCONNECTED };
    app_state_reduce(&s, &ud, now, out, &on);
    assert(s.link_up == true);
    assert(s.state == APP_ST_LISTENING);
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    assert(!has_action(APP_ACT_SEND_VOICE_END));
}

// ---- 双通道常开:会话通道断开 → 收束会话 + 自动切到仍通通道 ----
static void test_dual_active_channel_down_fails_over(void) {
    reset();
    fake_mode_set_channel_up(APP_CHAN_BLE, true);   // BLE 仍连
    s.link_up = true;
    s.link_channel = APP_CHAN_USB;         // USB 会话路由中
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;

    app_event_t ud = { .type = APP_EV_USB_DISCONNECTED };
    app_state_reduce(&s, &ud, now, out, &on);
    assert(s.link_up == true);             // BLE 兜底:链路仍通
    assert(s.link_channel == APP_CHAN_BLE);
    assert(s.state == APP_ST_READY);       // 会话收束(停流回 READY + toast)
    assert(has_action(APP_ACT_STREAM_CANCEL));

    // 双通道均断:link_up=false,通道名保留(横幅显示断掉的通道)
    fake_mode_set_channel_up(APP_CHAN_BLE, false);
    reset();
    s.link_up = true;
    s.link_channel = APP_CHAN_BLE;
    app_event_t bd = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &bd, now, out, &on);
    assert(s.link_up == false);
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "BLE") == 0);
}

// ---- 双通道常开:空闲态"最近连接/使用"胜出 —— 先 BLE 后 USB,PTT 走 USB ----
static void test_dual_idle_last_connect_wins(void) {
    reset();
    app_event_t bc = { .type = APP_EV_BLE_CONNECTED };
    app_state_reduce(&s, &bc, now, out, &on);
    assert(s.link_channel == APP_CHAN_BLE);
    assert(s.link_up == true);

    // 空闲(READY)时 USB 连入:夺路为新会话通道
    app_event_t uc = { .type = APP_EV_USB_CONNECTED };
    app_state_reduce(&s, &uc, now, out, &on);
    assert(s.link_channel == APP_CHAN_USB);

    // PTT 开录:link_channel 保持 USB;期间 BLE 断开不掐流
    // (时间推进 ≥1s:绕过 BLE 连接握手期假长按抑制窗口 BLE_CONNECT_PTT_GUARD)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1100);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 1200);
    assert(s.state == APP_ST_LISTENING);
    assert(s.link_channel == APP_CHAN_USB);
    app_event_t bd = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &bd, now, out, &on);
    assert(s.link_up == true);
    assert(s.state == APP_ST_LISTENING);
}

// ---- 双通道常开:有线主机在位(USB 供电)不自动息屏 ----
static void test_wired_no_screen_off(void) {
    reset();
    fake_mode_set_wired(true);             // USB 主机在位:屏幕常亮
    app_event_t t = { .type = APP_EV_TICK };
    s.last_key_ms = now - 70000;           // 远超 20s 背光 / 60s 面板超时
    app_state_reduce(&s, &t, now, out, &on);
    assert(s.screen_on == true);
    assert(s.panel_on == true);

    fake_mode_set_wired(false);            // 无线:恢复自动息屏
    app_state_reduce(&s, &t, now, out, &on);
    assert(s.screen_on == false);
    assert(s.panel_on == false);
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
    test_up_press_ptt();
    test_ptt_press_start_guards();
    test_timeouts();
    test_agent_running_timeout();
    test_agent_status_flow();
    test_approval();
    test_approval_during_listening();
    test_screen_off_wake();
    test_screen_off_ready();
    test_ok_long_locks();
    test_locked_keys_operate_screen_off();
    test_locked_approval_forces_wake();
    test_ok_long_unlocks();
    test_wake_no_relock();
    test_screen_off_keys_pass_through();
    test_up_taps_never_clear();
    test_ptt_no_confirm_delay();
    test_relock_cycle();
    test_lock_guards_listening_approval();
    test_transcript_display();
    test_transcribing_exit();
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
    test_usb_link_up();
    test_usb_link_down();
    test_time_set();
    test_bounded();
    test_dual_session_survives_other_channel();
    test_dual_active_channel_down_fails_over();
    test_dual_idle_last_connect_wins();
    test_wired_no_screen_off();
    printf("test_app_state: all assertions passed\n");
    return 0;
}
