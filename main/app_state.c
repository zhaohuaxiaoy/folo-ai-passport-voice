// main/app_state.c —— 状态机归约器实现。
// 纯 C,不依赖 IDF。按键语义与超时规则见 prd/design 文档。
#include "app_state.h"
#include "mode.h"   // 链路事件通道门禁(审查 P1):按当前模式隔离通道事件
#include <string.h>
#include <stdio.h>

// 长按松开后忽略双击的窗口:机械回弹/ADC 阈值穿越会把"长按+回弹"误判成
// 双击 → clear 上行在注入完成后才到达,删掉刚注入的文本。真实双击清空
// 发生在注入后(用户看到文本才决定清空),远晚于此窗口。
// 窗口必须 ≥ 回弹按压延迟(~300ms)+ iot_button 双击判定延迟(第二按松开后
// 再等 short_press_ticks=300ms 才上报 DOUBLE)——实测误判上报在松开后
// 400-600ms,300ms 窗口覆盖不住,故取 700ms。
#define PTT_REBOUND_GUARD_MS 700

// 连接握手期(~0.4-1s:参数协商/DLE/PHY 更新)密集射频会把按键 ADC 整段
// 腐蚀成 0mV → 假 UP 长按 → 自动开录音(真机实测: 连接后 130s 内自动触发
// 19 次 PTT 录音)。真实用户连接后 1s 内按住说话的概率极低,此窗口只吞
// 假事件;超窗后正常。
#define BLE_CONNECT_PTT_GUARD_MS 1000

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
    s->link_channel = 0;           // 缺省 BLE
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
            s->link_channel == 2 ? "USB"
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
    // 息屏 = 自动息屏的省电显示态(非锁定):按键先恢复显示,事件照常放行执行。
    // 锁定态见下方 locked 门禁:操作照常放行但不亮屏(与自动息屏的唤醒是两回事)。
    const bool screen_was_off = !s->screen_on;
    // 诊断(2026-08-28):按键误触取证 —— "按 OK 后疯狂录音"定位;定位后删除。
    printf("[KEYDBG] key btn=%u type=%u state=%u locked=%d screen=%d\n",
           b, ev->type, (unsigned)s->state, s->locked, s->screen_on);

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
            // 音量加长按 ≥0.3s = 说话(滴声只在长按判定时响,双击不会走到这里)。
            // OK 键已退出 PTT:按住/松开不再开录音。
            // 连接握手期假长按抑制:密集射频腐蚀 → 假 UP 长按,窗口内吞掉。
            if (s->ble_connected && now_ms - s->ble_connect_ms < BLE_CONNECT_PTT_GUARD_MS) {
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

    default:
        // TRANSCRIBING / AGENT_RUNNING:按键全部忽略(仅唤醒已在上面处理)
        break;
    }
}

static void handle_tick(app_state_t *s, uint64_t now_ms, app_action_t *out, uint8_t *n, uint8_t max) {
    // toast 过期
    if (s->toast[0] && now_ms >= s->toast_until_ms) {
        s->toast[0] = '\0';
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
    }

    // 分级息屏:HOME/READY 下无按键(APPROVAL 保持常亮:安全审批不熄屏)
    // 20s → 关背光(渲染跳过,面板冻结最后一帧);60s → 面板 SLPIN 断电(μA 级)。
    // 背光关后 tick 仍需走到面板判定,故不提前 return。
    // USB 模式(有线)不熄屏:屏幕常亮,省电只针对无线场景(用户需求)。
    // 锁定息屏为显式操作(locked 时 screen_on/panel_on 已 false,超时分支
    // 本不会触发),此处防御性排除使意图明确。
    const bool idle_state = (mode_get() != APP_MODE_USB) &&
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
        // 注入已迁 Mac 端,设备只做转写文本显示(任意状态可显示)。
        // final:false = 预览态(未定稿,UI 附光标感);final:true = 定稿落定。
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
        // 通道门禁(审查 P1):非 BLE 模式下手机连入只更新图标,不写 link_up ——
        // USB 音频会话不被 BLE 事件翻转;mode_switching 窗口放行切换期
        // 投递的收束事件(见 mode.h)。
        s->ble_connect_ms = now_ms;   // 握手期假长按抑制计时起点(见 handle_key)
        if (mode_get() != APP_MODE_BLE && !mode_switching()) {
            s->ble_connected = true;
            {
                app_action_t b = { .type = APP_ACT_UI_REFRESH };
                emit(out, out_n, max, b);
            }
            break;
        }
        s->ble_connected = true;
        s->link_channel = 0;
        s->link_up = true;
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_BLE_DISCONNECTED:
        if (mode_get() != APP_MODE_BLE && !mode_switching()) {
            s->ble_connected = false;   // 图标如实反映手机连入状态,链路不受影响
            {
                app_action_t b = { .type = APP_ACT_UI_REFRESH };
                emit(out, out_n, max, b);
            }
            break;
        }
        s->ble_connected = false;
        s->link_channel = 0;
        s->link_up = false;
        handle_link_down(s, now_ms, "Mac disconnected", out, out_n, max);
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

    // ---- USB 有线通道(语义与 BLE 对等)----

    case APP_EV_USB_CONNECTED:
        // 通道门禁:非 USB 模式忽略(仅 USB 模式有 usb_link 任务,防御性);
        // mode_switching 窗口放行切换期收束事件。
        if (mode_get() != APP_MODE_USB && !mode_switching()) break;
        // 与 BLE_CONNECTED 同构:USB 会话通 = 链路通,PTT 可用
        s->link_channel = 2;
        s->link_up = true;
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_USB_DISCONNECTED:
        // 通道门禁:非 USB 模式忽略;mode_switching 窗口放行切换期收束事件。
        if (mode_get() != APP_MODE_USB && !mode_switching()) break;
        s->link_channel = 2;   // 断线横幅仍显示通道名(USB)
        s->link_up = false;
        // 状态收束与 BLE 断连同路径;toast 文案区分通道
        handle_link_down(s, now_ms, "USB disconnected", out, out_n, max);
        break;

    case APP_EV_MODE_SWITCH:
        // 不进归约器:main.c 排空循环里直接调 mode_switch()(射频切换与状态机解耦)。
        // 归约器永不收到该事件 —— 此处仅占位以满足 -Wswitch。
        break;
    }
}
