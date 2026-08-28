// main/app_state.c —— 状态机归约器实现。
// 纯 C,不依赖 IDF。按键语义与超时规则见 prd/design 文档。
// 链路语义(双通道常开,2026-08-28):BLE/USB 同时可用,link_up = 任一通道通;
// link_channel = 会话路由通道(最近连接/使用,活动会话期间另一通道连接不夺路)。
#include "app_state.h"
#include "mode.h"
#include <string.h>
#include <stdio.h>

// 长按松开后忽略双击的窗口:机械回弹/ADC 阈值穿越会把"长按+回弹"误判成
// 双击 → clear 上行在注入完成后才到达,删掉刚注入的文本。真实双击清空
// 发生在注入后(用户看到文本才决定清空),远晚于此窗口。
// 窗口必须 ≥ 回弹按压延迟(~300ms)+ iot_button 双击判定延迟(第二按松开后
// 再等 short_press_ticks=300ms 才上报 DOUBLE)——实测误判上报在松开后
// 400-600ms,300ms 窗口覆盖不住,故取 700ms。
#define PTT_REBOUND_GUARD_MS 700
// 假 LONG 判定阈值:on_key 回调时刻 ADC 读数 ≥2000mV = 松开电平 → 假按。
// 真实按压回调读 3-5mV(事件处理时刻手指仍在键上)。三档键:UP 按下 0-150,
// 松开 2890,2000 在两者之间留足余量(RF_GUARD_HIGH_MV 同值)。
#define PTT_FAKE_LONG_MV 2000

// 连接握手期(~0.4-1s:参数协商/DLE/PHY 更新)密集射频会把按键 ADC 整段
// 腐蚀成 0mV → 假 UP 长按 → 自动开录音(真机实测: 连接后 130s 内自动触发
// 19 次 PTT 录音)。真实用户连接后 1s 内按住说话的概率极低,此窗口只吞
// 假事件;超窗后正常。
#define BLE_CONNECT_PTT_GUARD_MS 1000

// ---- 双击清空 vs 长按说话的消歧(2026-08-28 用户反馈)----
// 同一颗 UP 键既是"长按说话"又是"双击清空",而 iot_button 在按住到阈值的当下
// 就报 LONG_PRESS_START —— 双击的第二按只要多按几十毫秒就先变成录音,随后
// DOUBLE 又被 PTT_REBOUND_GUARD_MS 吞掉 → 用户实测"双击经常不好用,而且经常
// 误触录音"。对"疑似双击第二按"延后确认:若这次按下发生在上一次短按松开后
// UP_TAP_CHAIN_MS 内,LONG 不立即开录音而挂起,再按住 PTT_CONFIRM_MS 仍未松手
// 才真开;中途松手则什么都没发生 —— DOUBLE 照常上报清空,且 ptt_end_ms 未被
// 刷新,不会被回弹窗口吞掉。单独的长按说话(前面没有短按)不受影响。
// 更关键的一半:阈值意味着**任何按住 ≥阈值的轻点都被 iot_button 判成长按**,
// 于是它根本不再上报 CLICK/DOUBLE —— 阈值 300ms 时用户自己的轻点实测 285~300ms,
// 只剩 5~15ms 余量,一下手慢 20ms"清空"就彻底消失(真机取证:52.4s/54.7s/59.8s
// 三次 PRESS 后直接 LONG+LONG_UP,无 DOUBLE)。阈值已上调到 400ms(见
// bsp_button.c)把余量做到 100ms 以上,但状态机不能只依赖驱动上报,自己数轻点:
// PRESS→(RELEASE|LONG_UP) 且按住 < UP_TAP_MAX_MS 记一次轻点,两次轻点间隔
// < UP_TAP_CHAIN_MS 即判双击 → CLEAR。驱动随后若补报 DOUBLE,按 clear_ms 去重。
// 链判据统一用"上一次轻点松手 → 这一次按下"的间隔(不是松手到松手):按住
// 时长本身可长可短(轻点最长 500ms),用松手到松手会把两个 300ms 的轻点误判成
// 出链。同一次松手驱动连报 LONG_UP+RELEASE 时按下时刻没变,天然不成链。
// 窗口取值(2026-08-28 三次放宽,取证驱动):1200ms 仍然不够。用户按"我已经
// 摁快了"的那一对,ring 实测是 300ms 轻点 → 空档 1279ms → 290ms 轻点,两下
// 都正确判成 CLICK,只因空档超窗 79ms 而没成链 —— 用户感知就是"按快也没用",
// 因为他快的是按住时长,卡住的是空档。人手在这颗键上的双击空档实测落在
// 1.1s~1.3s(1094ms 成链那次 / 1279ms 差 79ms 出链那次),1800ms 才有余量。
// 放宽的代价只有一项:轻点后 1.8s 内的"按住说话"要多等 PTT_CONFIRM_MS 才开
// 录音(仍然会开),独立长按不受影响;UP 单击在 READY/HOME 无其它语义,误触
// 链的风险只在 APPROVAL/TRANSCRIBING 里"点一下 + 再点一下",概率低于双击本身
// 失灵的代价。
#define UP_TAP_CHAIN_MS 1800  // 上一次松手到这一次按下的间隔上限
#define UP_TAP_MAX_MS   500   // 按住多久以内算"轻点"(超过 = 真的想按住说话)
#define UP_TAP_DEDUP_MS 100   // 同一次松手驱动会连报 LONG_UP + RELEASE,去重窗口
#define PTT_CONFIRM_MS  250   // 挂起后再按住这么久才真开录音(TICK 100ms 粒度)
// 不足这么久的"录音"不可能有语音(阈值误触/双击第一下):丢音频 + 收束会话,
// 不进转写、不刷新 ptt_end_ms(否则紧随其后的 CLEAR 会被回弹窗口吞掉)。
#define PTT_MIN_TALK_MS 400

// 唤醒防误锁窗口:息屏后长按 OK 唤醒时,PRESS 唤醒瞬间起 1s 内的 OK LONG
// 不触发锁定 —— 否则"唤醒"会立刻变成"再锁"(PRESS 后 ~500ms 即到 LONG)。
// LONG 长按期间只报一次,guard 只挡第一次;松手后再次长按 OK 正常锁定。
#define OK_LONG_GUARD_MS 1000

static const char *const AGENT_STATE_NAMES[APP_AGENT_COUNT] = {
    [APP_AGENT_READY]    = "ready",
    [APP_AGENT_THINKING] = "thinking",
    [APP_AGENT_RUNNING]  = "running",
    [APP_AGENT_ERROR]    = "error",
    [APP_AGENT_DONE]     = "done",
};

void app_state_init(app_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = APP_ST_HOME;
    s->screen_on = true;
    s->panel_on = true;
    // 开机即按"未连接"渲染(无 Mac 时不会收到断开事件,初始 true 会一直误判为
    // 链路通 → PTT 可用但音频全丢且无离线横幅)。Mac 连入订阅 EVENT 后翻 true。
    s->ble_connected = false;
    s->link_up = false;
    s->link_channel = APP_CHAN_BLE;   // 缺省 BLE(首通道连接前 PTT 门禁靠 link_up)
    s->locked = false;             // 开机未锁定
    s->wake_ms = 0;                // 无唤醒史 → 首个 OK LONG 不受 guard 限制
}

static void emit(app_action_t *out, uint8_t *n, uint8_t max, app_action_t a) {
    if (*n < max) out[(*n)++] = a;
}

// 字符串安全拷贝:一律截断并保证 NUL。
static void str_cpy(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static void set_toast(app_state_t *s, uint64_t now_ms, const char *msg) {
    str_cpy(s->toast, sizeof(s->toast), msg);
    s->toast_until_ms = now_ms + 4000;
}

void app_state_snapshot(const app_state_t *s, uint64_t now_ms, app_ui_snapshot_t *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->state            = s->state;
    snap->link_up          = s->link_up;
    snap->screen_on        = s->screen_on;
    snap->panel_on         = s->panel_on;
    snap->net_busy         = s->net_busy;
    snap->ble_connected    = s->ble_connected;
    str_cpy(snap->link_name, sizeof(snap->link_name),
            s->link_channel == APP_CHAN_USB ? "USB"
            : "BLE");
    snap->elapsed_ms       = (uint32_t)(now_ms - s->state_since_ms);
    snap->battery_available = true; // 由主循环在快照后补真实值,见 app_ui
    str_cpy(snap->agent_message, sizeof(snap->agent_message), s->agent_message);
    snap->transcript_final = s->transcript_final;
    str_cpy(snap->agent_state_name, sizeof(snap->agent_state_name), s->agent_state_name);
    str_cpy(snap->task_id, sizeof(snap->task_id), s->task_id);
    str_cpy(snap->approval_title, sizeof(snap->approval_title), s->approval_title);
    str_cpy(snap->approval_target, sizeof(snap->approval_target), s->approval_target);
    str_cpy(snap->approval_diff, sizeof(snap->approval_diff), s->approval_diff);
    snap->approval_risk    = s->approval_risk;
    if (s->toast[0] && s->toast_until_ms > 0) {
        str_cpy(snap->toast, sizeof(snap->toast), s->toast);
    }
    // 默认展示名仅在没有真实 agent.status 时兜底,不覆盖已存的 thinking/error 等
    if ((s->state == APP_ST_AGENT_RUNNING || s->state == APP_ST_TRANSCRIBING) &&
        snap->agent_state_name[0] == '\0') {
        str_cpy(snap->agent_state_name, sizeof(snap->agent_state_name),
                AGENT_STATE_NAMES[APP_AGENT_RUNNING]);
    }
}

// 按键动作(enter/clear):上行给 PC client 执行注入。
static void send_key_action(app_state_t *s, app_key_action_t action,
                            app_action_t *out, uint8_t *n, uint8_t max) {
    (void)s;
    app_action_t a = { .type = APP_ACT_SEND_KEY_ACTION };
    a.u.key_action.action = (uint8_t)action;
    emit(out, n, max, a);
}

// PTT 开始(音量加长按 0.5s 判定):离线 toast+error 音;在线滴声+开流。
// UP 键一次"轻点"(短按松手)登记:第二次落在链内即判双击 → 清空输入框。
// 回弹防御沿用 ptt_end_ms 窗口(真实录音松手后的机械回弹不算轻点)。
static void note_up_tap(app_state_t *s, uint64_t now_ms,
                        app_action_t *out, uint8_t *n, uint8_t max) {
    if (now_ms - s->ptt_end_ms < PTT_REBOUND_GUARD_MS) return;   // 录音松手回弹
    if (s->up_tap_ms && now_ms - s->up_tap_ms < UP_TAP_DEDUP_MS) return;  // 同一次松手重复上报
    if (s->up_tap_ms && s->up_press_ms > s->up_tap_ms &&
        s->up_press_ms - s->up_tap_ms < UP_TAP_CHAIN_MS) {
        s->up_tap_ms = 0;
        s->clear_ms = now_ms;   // 驱动稍后若补报 DOUBLE,按此去重
        send_key_action(s, APP_KEY_CLEAR, out, n, max);
        return;
    }
    s->up_tap_ms = now_ms;
}

static void start_ptt(app_state_t *s, uint64_t now_ms,
                      app_action_t *out, uint8_t *n, uint8_t max) {
    if (!s->link_up) {
        set_toast(s, now_ms, "OFFLINE - PTT blocked");
        app_action_t t = { .type = APP_ACT_PLAY_TONE };
        t.u.tone = APP_TONE_ERROR;
        emit(out, n, max, t);
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
    } else {
        // 分帧时序:滴声 → voice.start → 开流由 TONE_DONE 驱动(播放完成才采,
        // 避免 codec 分时冲突)。开流动作移到 TONE_DONE 分支,app_task 不再
        // 同步阻塞 80ms 播滴声。
        app_action_t t = { .type = APP_ACT_PLAY_TONE };
        t.u.tone = APP_TONE_START;
        emit(out, n, max, t);
        app_action_t v = { .type = APP_ACT_SEND_VOICE_START };
        emit(out, n, max, v);
        s->stream_started = false;
        s->state = APP_ST_LISTENING;
        s->state_since_ms = now_ms;
    }
}

// PTT 结束(音量加长按松开):停流 → voice.end → 转写。
// 不播发送音:用户要求转写期静音(2026-08-28)。
static void end_ptt(app_state_t *s, uint64_t now_ms,
                    app_action_t *out, uint8_t *n, uint8_t max) {
    app_action_t st = { .type = APP_ACT_STREAM_STOP };
    emit(out, n, max, st);
    app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
    emit(out, n, max, v);
    s->state = APP_ST_TRANSCRIBING;
    s->state_since_ms = now_ms;
    s->agent_state_name[0] = '\0';   // 新会话开始,清除旧 agent 状态
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
    s->ptt_end_ms = now_ms;   // 供双击防御:松开后回弹窗口内的假双击忽略
}

static void go_ready(app_state_t *s, uint64_t now_ms, app_action_t *out, uint8_t *n, uint8_t max) {
    s->state = APP_ST_READY;
    s->state_since_ms = now_ms;
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

static void abort_to_ready(app_state_t *s, uint64_t now_ms, const char *toast,
                           app_action_t *out, uint8_t *n, uint8_t max) {
    if (toast) set_toast(s, now_ms, toast);
    s->state = APP_ST_READY;
    s->state_since_ms = now_ms;
    s->agent_state_name[0] = '\0';   // 清除旧的 agent 状态,防止 UI 残留
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

static void handle_key(app_state_t *s, const app_event_t *ev, uint64_t now_ms,
                       app_action_t *out, uint8_t *n, uint8_t max) {
    const uint8_t b = ev->u.key.btn;
    // 记录 UP 键最近一次 PRESS 回调读数(2026-08-28):真实按压 3-5mV(手指在
    // 键上),假按(射频腐蚀)2890mV(无人按键)。CLICK 回调时刻用户已松手,mv
    // 恒 2890 —— 真假单击只能从 PRESS 读数区分(TRANSCRIBING 退出判定用)。
    if (ev->type == APP_EV_KEY_PRESS && b == APP_BTN_UP) {
        s->last_up_press_mv = ev->u.key.mv;
    }
    if (ev->type == APP_EV_KEY_PRESS && b == APP_BTN_UP) {
        s->up_press_ms = now_ms;
    }
    // 松手登记轻点:RELEASE(纯短按)与 LONG_UP(被驱动判成长按的轻点)都算,
    // 同一次松手驱动会两条都报 → note_up_tap 内按 UP_TAP_DEDUP_MS 去重。
    // 假按不记(射频噪声不该给真实长按凭空加 250ms 确认延迟)。
    if (b == APP_BTN_UP && s->last_up_press_mv < PTT_FAKE_LONG_MV &&
        (ev->type == APP_EV_KEY_RELEASE || ev->type == APP_EV_KEY_LONG_UP) &&
        now_ms - s->up_press_ms < UP_TAP_MAX_MS) {
        note_up_tap(s, now_ms, out, n, max);
    }
    // 松手/单击/双击/长按松开 → 挂起的 PTT 作废(说明这不是"按住说话")
    if (b == APP_BTN_UP && (ev->type == APP_EV_KEY_RELEASE ||
                            ev->type == APP_EV_KEY_LONG_UP ||
                            ev->type == APP_EV_KEY_CLICK ||
                            ev->type == APP_EV_KEY_DOUBLE)) {
        s->ptt_pending_ms = 0;
    }
    // 息屏 = 自动息屏的省电显示态(非锁定):按键先恢复显示,事件照常放行执行。
    // 锁定态见下方 locked 门禁:操作照常放行但不亮屏(与自动息屏的唤醒是两回事)。
    const bool screen_was_off = !s->screen_on;

    // 锁定态(2026-08-28 语义变更):长按 OK 解锁亮屏;其余按键照常执行但不
    // 亮屏 —— 锁定 = 屏幕保持关闭的省电模式,不是输入锁(用户要求:息屏后仍
    // 能长按说话/回车/清空,口袋盲操作)。渲染由 snapshot 门禁按 screen_on
    // 跳过,操作产生的动作(语音/按键上行)照常出队。
    if (s->locked) {
        if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK) {
            s->locked = false;
            s->panel_on = true;
            s->screen_on = true;
            s->last_key_ms = now_ms;   // 解锁后息屏计时重新开始
            app_action_t p = { .type = APP_ACT_UI_PANEL_ON };
            emit(out, n, max, p);
            app_action_t a = { .type = APP_ACT_UI_SCREEN_ON };
            emit(out, n, max, a);
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
            return;
        }
        // 其余事件放行:跳过下方唤醒与锁定入口,直接落到双击清空/状态机。
        // 不亮屏不唤醒;锁定仅发生在 HOME/READY,盲操作开录进 LISTENING 后
        // 再长按 OK 仍是解锁(上方分支先行),录音不受影响。
    } else {
        if (screen_was_off) {
            if (ev->type == APP_EV_KEY_PRESS) {
                if (!s->panel_on) {
                    s->panel_on = true;
                    app_action_t p = { .type = APP_ACT_UI_PANEL_ON };
                    emit(out, n, max, p);
                }
                s->screen_on = true;
                s->last_key_ms = now_ms;
                s->wake_ms = now_ms;   // 唤醒时刻(OK_LONG_GUARD:防"唤醒即锁")
                app_action_t a = { .type = APP_ACT_UI_SCREEN_ON };
                emit(out, n, max, a);
                app_action_t r = { .type = APP_ACT_UI_REFRESH };
                emit(out, n, max, r);
            }
            // 不 return:事件继续放行到下方正常逻辑(锁定入口 → 双击清空 → 状态机)。
            // 自动息屏只省显示,不吞按键事件(过修修正:息屏后 UP/DOWN 应唤醒并照常
            // 执行)。放行安全性:
            //  - PRESS 在状态机各态均无处理分支 → 无副作用;
            //  - 息屏只可能发生在 HOME/READY(idle_state 限定;APPROVAL 常亮、
            //    LISTENING 不超时)→ 放行事件只落到 HOME/READY 分支;
            //  - 唤醒后 1s 内 OK LONG 被 OK_LONG_GUARD 挡(防"唤醒即锁");
            //  - 无 PRESS 直接 CLICK(真实事件流不可达,iot_button 任何上报前必有
            //    PRESS)不唤醒但照常执行动作 —— 仅测试可达,非真实交互路径。
        }

        // 锁定入口:亮屏的 HOME/READY 下长按 OK(0.5s 阈值)立即锁定息屏
        // (背光灭 + 面板 SLPIN 断电,不等 60s 超时)。录音/转写/审批/Agent
        // 运行中不可锁定;USB 模式手动锁定允许(显式操作,省电优先)。
        if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK &&
            (s->state == APP_ST_HOME || s->state == APP_ST_READY) &&
            s->screen_on && s->panel_on &&
            now_ms - s->wake_ms > OK_LONG_GUARD_MS) {
            s->locked = true;
            s->screen_on = false;
            s->panel_on = false;
            app_action_t a = { .type = APP_ACT_UI_SCREEN_OFF };
            emit(out, n, max, a);
            app_action_t p = { .type = APP_ACT_UI_PANEL_OFF };
            emit(out, n, max, p);
            return;
        }
    }

    // UP(音量加)双击 = 清空输入框(全局语义,各态统一)。
    // 双击无提示音:两按均 <0.5s,不触发长按判定,自然不响滴声。
    // 防"注入后删除":长按松开瞬间机械回弹/ADC 阈值穿越会把"长按+回弹"误判
    // 成双击 → clear 上行在识别注入完成后才被执行,删掉刚注入的文本(PTT
    // 移师 UP 键后暴露;以前 PTT 在 OK 键,双击无语义)。真实双击清空发生在
    // 注入后(用户看到文本才决定清空),故录音中(LISTENING,手指在按住、
    // 不可能主动双击)与松开后 PTT_REBOUND_GUARD_MS 内的双击一律忽略。
    if (ev->type == APP_EV_KEY_DOUBLE && b == APP_BTN_UP) {
        if (s->state == APP_ST_LISTENING || now_ms - s->ptt_end_ms < PTT_REBOUND_GUARD_MS) {
            return;
        }
        // 自建轻点链刚判过双击并已清空 → 驱动补报的这条丢弃(否则清两次)
        if (s->clear_ms && now_ms - s->clear_ms < PTT_REBOUND_GUARD_MS) {
            return;
        }
        s->up_tap_ms = 0;   // 驱动上报即收束本链
        send_key_action(s, APP_KEY_CLEAR, out, n, max);
        return;
    }

    switch (s->state) {
    case APP_ST_HOME:
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            go_ready(s, now_ms, out, n, max);
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_DOWN) {
            send_key_action(s, APP_KEY_ENTER, out, n, max);
        }
        break;

    case APP_ST_READY:
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_DOWN) {
            send_key_action(s, APP_KEY_ENTER, out, n, max);
        } else if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_UP) {
            // 音量加长按 ≥0.4s = 说话(滴声只在长按判定时响,双击不会走到这里)。
            // OK 键已退出 PTT:按住/松开不再开录音。
            // 连接握手期假长按抑制:密集射频腐蚀 → 假 UP 长按,窗口内吞掉。
            if (s->ble_connected && now_ms - s->ble_connect_ms < BLE_CONNECT_PTT_GUARD_MS) {
                break;
            }
            // 假长按抑制(2026-08-28 真机取证,替代固定 3s 窗口):PTT 松开
            // 后 BLE 链路异常期 SAR ADC 被腐蚀 → 假 PRESS→LONG 序列,落在
            // READY 态即触发 PTT(第二声滴 + 假录音)。iot_button 判定按住
            // 是腐蚀中读低压,但 on_key 回调时刻腐蚀已过 → mv 读回松开电平
            // (2890;真实按压 3-5)。环取证 7 例假 LONG 回调 mv 全 2890、真
            // 实 LONG 全 <150 —— 100% 区分。不能用固定窗口:用户连续长按
            // 间隔实测 2-3s,3s 窗口把 43.17s/50.12s 真实长按吞掉("长按没
            // 反应"根因),故以回调 mv 判假,任何间隔的真实长按都不误伤。
            if (ev->u.key.mv >= PTT_FAKE_LONG_MV) {
                break;
            }
            // 疑似双击第二按:挂起,等 TICK 确认仍在按住才开录音(见 UP_TAP_CHAIN_MS)
            if (s->up_tap_ms && s->up_press_ms > s->up_tap_ms &&
                s->up_press_ms - s->up_tap_ms < UP_TAP_CHAIN_MS) {
                s->ptt_pending_ms = now_ms;
                break;
            }
            start_ptt(s, now_ms, out, n, max);
        }
        break;

    case APP_ST_LISTENING:
        // 音量加长按录音的松开是 LONG_UP(长按态松开不再报 RELEASE),松开立即
        // 结束并发送(无取消窗口)。双击的第二按落在 TRANSCRIBING(录音已发送),
        // 其 DOUBLE 事件由上方全局分支处理为"清空"。
        if (ev->type == APP_EV_KEY_LONG_UP && b == APP_BTN_UP) {
            // 误触收口:不足 PTT_MIN_TALK_MS 就松手 = 阈值边缘的轻点(常见于
            // 双击的第一下),里面不可能有语音。丢掉音频、把会话收束回 READY,
            // 不进转写、不刷 ptt_end_ms —— 紧随其后的第二下轻点照常判双击清空。
            if (now_ms - s->state_since_ms < PTT_MIN_TALK_MS) {
                app_action_t c = { .type = APP_ACT_STREAM_CANCEL };
                emit(out, n, max, c);   // 幂等:滴声未播完时甚至还没开流
                app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
                emit(out, n, max, v);   // 会话必须收束,否则 Mac 侧挂着未结束的 voice
                go_ready(s, now_ms, out, n, max);
                break;
            }
            end_ptt(s, now_ms, out, n, max);
        }
        break;

    case APP_ST_APPROVAL:
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            app_action_t a = { .type = APP_ACT_SEND_AGENT_ACTION };
            str_cpy(a.u.agent_action.task_id, sizeof(a.u.agent_action.task_id), s->task_id);
            a.u.agent_action.decision = APP_ACTION_APPROVE;
            emit(out, n, max, a);
            s->state = APP_ST_AGENT_RUNNING;
            s->state_since_ms = now_ms;
            str_cpy(s->agent_message, sizeof(s->agent_message), "Approved, agent continues...");
            s->transcript_final = true;   // 非转写文本,无预览光标
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_UP) {
            app_action_t a = { .type = APP_ACT_SEND_AGENT_ACTION };
            str_cpy(a.u.agent_action.task_id, sizeof(a.u.agent_action.task_id), s->task_id);
            a.u.agent_action.decision = APP_ACTION_REJECT;
            emit(out, n, max, a);
            app_action_t t = { .type = APP_ACT_PLAY_TONE };
            t.u.tone = APP_TONE_REJECT;
            emit(out, n, max, t);
            s->state = APP_ST_AGENT_RUNNING;
            s->state_since_ms = now_ms;
            str_cpy(s->agent_message, sizeof(s->agent_message), "Rejected by user");
            s->transcript_final = true;   // 非转写文本,无预览光标
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_DOWN) {
            send_key_action(s, APP_KEY_ENTER, out, n, max);
        }
        break;

    case APP_ST_TRANSCRIBING:
        // 音量+单击:退出转写场景(2026-08-28 用户需求)——不等识别结果,直接
        // 回 READY。迟到的识别文本由 APP_EV_TRANSCRIPT 的 READY/HOME 门禁丢弃。
        // 真实单击的 PRESS 回调 mv=3-5(手指在键上);假按风暴的 PRESS mv=2890
        // (腐蚀,无人按键)→ 用 PRESS 读数区分真假单击(CLICK 回调时刻用户已
        // 松手,mv 恒 2890,不可用)。长按(开始新录音)保持忽略:转写中须先
        // 退出或等结果,防误触新会话。
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_UP &&
            s->last_up_press_mv < PTT_FAKE_LONG_MV) {
            abort_to_ready(s, now_ms, NULL, out, n, max);
        }
        break;

    default:
        // AGENT_RUNNING:按键全部忽略(仅唤醒已在上面处理)
        break;
    }
}

static void handle_tick(app_state_t *s, uint64_t now_ms, app_action_t *out, uint8_t *n, uint8_t max) {
    // 挂起的 PTT 确认:仍在 READY 且手指没松(松手会清零)→ 真的是按住说话
    if (s->ptt_pending_ms) {
        if (s->state != APP_ST_READY) {
            s->ptt_pending_ms = 0;
        } else if (now_ms - s->ptt_pending_ms >= PTT_CONFIRM_MS) {
            s->ptt_pending_ms = 0;
            start_ptt(s, now_ms, out, n, max);
        }
    }
    // toast 过期
    if (s->toast[0] && now_ms >= s->toast_until_ms) {
        s->toast[0] = '\0';
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
    }

    // 分级息屏:HOME/READY 下无按键(APPROVAL 保持常亮:安全审批不熄屏)
    // 20s → 关背光(渲染跳过,面板冻结最后一帧);60s → 面板 SLPIN 断电(μA 级)。
    // 背光关后 tick 仍需走到面板判定,故不提前 return。
    // USB 主机在位(有线供电)不熄屏:屏幕常亮,省电只针对无线场景(用户需求)。
    // 锁定息屏为显式操作(locked 时 screen_on/panel_on 已 false,超时分支
    // 本不会触发),此处防御性排除使意图明确。
    const bool idle_state = !mode_wired() &&
                            !s->locked &&
                            (s->state == APP_ST_HOME ||
                             s->state == APP_ST_READY);
    if (idle_state) {
        if (s->screen_on && (now_ms - s->last_key_ms) >= APP_IDLE_BACKLIGHT_OFF_MS) {
            s->screen_on = false;
            app_action_t a = { .type = APP_ACT_UI_SCREEN_OFF };
            emit(out, n, max, a);
        }
        if (s->panel_on && (now_ms - s->last_key_ms) >= APP_IDLE_PANEL_OFF_MS) {
            s->panel_on = false;
            app_action_t a = { .type = APP_ACT_UI_PANEL_OFF };
            emit(out, n, max, a);
        }
    }

    if (!s->screen_on) return;

    // 状态超时
    const uint64_t elapsed = now_ms - s->state_since_ms;
    switch (s->state) {
    case APP_ST_LISTENING:
        // S3 兜底:滴声队列满/声音任务异常 → TONE_DONE 永不来的场景,超时强制开流
        // (宁可无滴声也不让 PTT 卡死无声)。
        if (!s->stream_started && elapsed >= APP_TONE_PENDING_TIMEOUT_MS) {
            app_action_t st = { .type = APP_ACT_STREAM_START };
            emit(out, n, max, st);
            s->stream_started = true;
        }
        break;
    case APP_ST_TRANSCRIBING:
        if (elapsed >= APP_TRANSCRIBE_TIMEOUT) {
            abort_to_ready(s, now_ms, "STT timeout", out, n, max);
        }
        break;
    case APP_ST_AGENT_RUNNING:
        if (elapsed >= APP_AGENT_RUN_TIMEOUT) {
            abort_to_ready(s, now_ms, "Agent timeout", out, n, max);
        }
        break;
    default:
        break; // APPROVAL 无超时(安全要求)
    }
}

// 链路断开(BLE 断连或 EVENT 订阅取消):兜底停流 + 状态回退。
// 审批保持:等待 Mac 重连后重发请求。
static void handle_link_down(app_state_t *s, uint64_t now_ms, const char *toast,
                             app_action_t *out, uint8_t *n, uint8_t max) {
    // CANCEL:断链时无 Mac 可发,清残留防断链前的帧流入下一次会话
    app_action_t st = { .type = APP_ACT_STREAM_CANCEL };
    emit(out, n, max, st);   // 幂等,未开流时执行器无副作用
    set_toast(s, now_ms, toast);   // 任何状态都提示断开(审批保持等场景也可见)
    switch (s->state) {
    case APP_ST_LISTENING:
        abort_to_ready(s, now_ms, NULL, out, n, max);   // toast 已设,不再覆盖
        break;
    case APP_ST_TRANSCRIBING:
    case APP_ST_AGENT_RUNNING:
        abort_to_ready(s, now_ms, NULL, out, n, max);
        break;
    default: {
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
        break;
    }
    }
}

void app_state_reduce(app_state_t *s, const app_event_t *ev, uint64_t now_ms,
                      app_action_t *out, uint8_t *out_n) {
    *out_n = 0;
    const uint8_t max = APP_ACT_MAX;

    switch (ev->type) {
    case APP_EV_KEY_PRESS:
    case APP_EV_KEY_RELEASE:
    case APP_EV_KEY_CLICK:
    case APP_EV_KEY_DOUBLE:
    case APP_EV_KEY_LONG:
    case APP_EV_KEY_LONG_UP:
        s->last_key_ms = now_ms;
        handle_key(s, ev, now_ms, out, out_n, max);
        break;

    case APP_EV_AGENT_STATUS: {
        const uint8_t st = ev->u.agent_status.state;
        str_cpy(s->agent_state_name, sizeof(s->agent_state_name),
                AGENT_STATE_NAMES[st < APP_AGENT_COUNT ? st : APP_AGENT_READY]);
        str_cpy(s->agent_message, sizeof(s->agent_message), ev->u.agent_status.message);
        s->transcript_final = true;   // 消息已被 agent.status 替换,不再是转写预览

        if (s->state == APP_ST_TRANSCRIBING || s->state == APP_ST_AGENT_RUNNING) {
            if (st == APP_AGENT_RUNNING || st == APP_AGENT_THINKING) {
                s->state = APP_ST_AGENT_RUNNING;
                s->state_since_ms = now_ms;
            } else if (st == APP_AGENT_DONE) {
                // 无 DONE 页:识别完成直接回 READY 待命(用户:不要 done 提示),
                // 成功音一并取消(用户要求转写→ready 静音,2026-08-28)
                s->state = APP_ST_READY;
                s->state_since_ms = now_ms;
            } else if (st == APP_AGENT_ERROR) {
                abort_to_ready(s, now_ms, "Agent error", out, out_n, max);
            }
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        } else {
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;
    }

    case APP_EV_APPROVAL_REQUEST:
        // 锁定态收到审批:强制解锁亮屏 —— 审批必须被看见,防口袋盲批
        // (与"APPROVAL 常亮不熄屏"政策一致)。
        if (s->locked) {
            s->locked = false;
            s->panel_on = true;
            s->screen_on = true;
            app_action_t p = { .type = APP_ACT_UI_PANEL_ON };
            emit(out, out_n, max, p);
            app_action_t sc = { .type = APP_ACT_UI_SCREEN_ON };
            emit(out, out_n, max, sc);
        }
        // 审批打断录音:必须先停流,否则管线永远泄漏(APPROVAL 下没有按键能停它)
        if (s->state == APP_ST_LISTENING) {
            app_action_t st = { .type = APP_ACT_STREAM_STOP };
            emit(out, out_n, max, st);
            app_action_t ve = { .type = APP_ACT_SEND_VOICE_END };
            emit(out, out_n, max, ve);
        }
        str_cpy(s->task_id, sizeof(s->task_id), ev->u.approval.task_id);
        str_cpy(s->approval_title, sizeof(s->approval_title), ev->u.approval.title);
        str_cpy(s->approval_target, sizeof(s->approval_target), ev->u.approval.target);
        str_cpy(s->approval_diff, sizeof(s->approval_diff), ev->u.approval.diff_summary);
        s->approval_risk = ev->u.approval.risk < APP_RISK_COUNT ? ev->u.approval.risk : APP_RISK_MEDIUM;
        s->state = APP_ST_APPROVAL;
        s->state_since_ms = now_ms;
        app_action_t t = { .type = APP_ACT_PLAY_TONE };
        t.u.tone = APP_TONE_APPROVAL;
        emit(out, out_n, max, t);
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, out_n, max, r);
        break;

    case APP_EV_TRANSCRIPT:
        // 注入已迁 Mac 端,设备只做转写文本显示。退出转写(音量+单击回 READY)
        // 后迟到的识别文本丢弃:用户已明确退出该场景,预览/定稿不再上屏。
        // 正常路径文本到达时 state=TRANSCRIBING;agent 运行期文本照常显示。
        // final:false = 预览态(未定稿,UI 附光标感);final:true = 定稿落定。
        if (s->state == APP_ST_READY || s->state == APP_ST_HOME) {
            break;
        }
        str_cpy(s->agent_message, sizeof(s->agent_message), ev->u.transcript.text);
        s->transcript_final = ev->u.transcript.final;
        {
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;

    case APP_EV_AUDIO_DROP_START:
        if (!s->net_busy) {
            s->net_busy = true;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;

    case APP_EV_AUDIO_DROP_END:
        if (s->net_busy) {
            s->net_busy = false;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;

    case APP_EV_BLE_CONNECTED:
        // 语义:EVENT 特征已订阅(链路通,PTT 可用)。
        // 双通道常开(2026-08-28):BLE 连接总是链路候选 —— link_up 已通时
        // 只更新图标;空闲态(HOME/READY)记为新会话通道(最近使用),活动
        // 会话中不夺路(音频/voice.end 必须回到发起会话的通道)。
        s->ble_connect_ms = now_ms;   // 握手期假长按抑制计时起点(见 handle_key)
        s->ble_connected = true;
        if (!s->link_up) {
            s->link_channel = APP_CHAN_BLE;
            s->link_up = true;
        } else if (s->state == APP_ST_HOME || s->state == APP_ST_READY) {
            s->link_channel = APP_CHAN_BLE;
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_BLE_DISCONNECTED:
        s->ble_connected = false;
        if (s->link_channel == APP_CHAN_BLE) {
            // 会话通道断开:收束会话(停流回 READY + toast),路由切到仍通
            // 的另一通道(USB)。双通道均断时 link_channel 保留断掉的通道名
            // (横幅显示),link_up=false。另一通道断开不影响本会话(上方
            // 分支不成立时仅刷新图标)。
            s->link_up = false;
            handle_link_down(s, now_ms, "Mac disconnected", out, out_n, max);
            if (mode_channel_up(APP_CHAN_USB)) {
                s->link_channel = APP_CHAN_USB;
                s->link_up = true;
            }
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_BLE_DROP:
        set_toast(s, now_ms, "BLE event dropped");
        {
            app_action_t br = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, br);
        }
        break;

    case APP_EV_AUDIO_ERROR:
        set_toast(s, now_ms, "Audio error");
        abort_to_ready(s, now_ms, NULL, out, out_n, max);
        break;

    case APP_EV_TONE_DONE:
        // START 提示音播放完成:开流(幂等)。非 LISTENING(双击取消/单击发送已离开)
        // 或流已开 → 忽略,保证"滴声先于采集"且取消期间不误开流。
        if (s->state == APP_ST_LISTENING && !s->stream_started) {
            app_action_t st = { .type = APP_ACT_STREAM_START };
            emit(out, out_n, max, st);
            s->stream_started = true;
        }
        break;

    case APP_EV_TICK:
        handle_tick(s, now_ms, out, out_n, max);
        break;

    case APP_EV_TIME_SET:
        // 校时落地:透传 epoch 到动作(app_task 执行 settimeofday)。
        // 与链路事件正交:任何模式下收到即生效,无状态依赖。
        {
            app_action_t t = { .type = APP_ACT_TIME_SET,
                               .u.time_set.epoch = ev->u.time_set.epoch };
            emit(out, out_n, max, t);
        }
        break;

    // ---- USB 有线通道(语义与 BLE 对等;双通道常开下同规则)----

    case APP_EV_USB_CONNECTED:
        // USB 会话通(PC ping 握手)= 链路候选:空闲态记为新会话通道(插线
        // 即用);活动会话中不夺路。与 BLE_CONNECTED 同构。
        if (!s->link_up) {
            s->link_channel = APP_CHAN_USB;
            s->link_up = true;
        } else if (s->state == APP_ST_HOME || s->state == APP_ST_READY) {
            s->link_channel = APP_CHAN_USB;
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_USB_DISCONNECTED:
        if (s->link_channel == APP_CHAN_USB) {
            s->link_up = false;
            // 状态收束与 BLE 断连同路径;toast 文案区分通道
            handle_link_down(s, now_ms, "USB disconnected", out, out_n, max);
            if (mode_channel_up(APP_CHAN_BLE)) {
                s->link_channel = APP_CHAN_BLE;
                s->link_up = true;
            }
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;
    }
}
