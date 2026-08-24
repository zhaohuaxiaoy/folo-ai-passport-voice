// main/app_state.c —— 状态机归约器实现。
// 纯 C,不依赖 IDF。按键语义与超时规则见 prd/design 文档。
#include "app_state.h"
#include <string.h>
#include <stdio.h>

const char *const APP_WORKFLOW_NAMES[APP_WF_COUNT] = {
    [APP_WF_BUILD]   = "BUILD",
    [APP_WF_DEBUG]   = "DEBUG",
    [APP_WF_REVIEW]  = "REVIEW",
    [APP_WF_TEST]    = "TEST",
    [APP_WF_CAPTURE] = "CAPTURE",
};

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
    s->workflow = APP_WF_BUILD;
    s->screen_on = true;
    // 开机即按"未连接"渲染(无 Mac 时不会收到断开事件,初始 true 会一直误判为
    // 链路通 → PTT 可用但音频全丢且无离线横幅)。Mac 连入订阅 EVENT 后翻 true。
    s->ble_connected = false;
    s->link_up = false;
    s->link_channel = 0;           // 缺省 BLE
    s->wifi_fail_reason = 0;
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
    snap->workflow         = s->workflow;
    snap->link_up          = s->link_up;
    snap->screen_on        = s->screen_on;
    snap->net_busy         = s->net_busy;
    snap->ble_connected    = s->ble_connected;
    str_cpy(snap->link_name, sizeof(snap->link_name),
            s->link_channel == 1 ? "WiFi" : "BLE");
    snap->elapsed_ms       = (uint32_t)(now_ms - s->state_since_ms);
    snap->battery_available = true; // 由主循环在快照后补真实值,见 app_ui
    snap->mac_cpu          = s->mac_cpu;
    snap->mac_ram          = s->mac_ram;
    snap->mac_batt         = s->mac_batt;
    snap->mac_charging     = s->mac_charging;
    str_cpy(snap->active_app, sizeof(snap->active_app), s->active_app);
    str_cpy(snap->agent_message, sizeof(snap->agent_message), s->agent_message);
    snap->transcript_final = s->transcript_final;
    str_cpy(snap->agent_state_name, sizeof(snap->agent_state_name), s->agent_state_name);
    str_cpy(snap->task_id, sizeof(snap->task_id), s->task_id);
    str_cpy(snap->approval_title, sizeof(snap->approval_title), s->approval_title);
    str_cpy(snap->approval_target, sizeof(snap->approval_target), s->approval_target);
    str_cpy(snap->approval_diff, sizeof(snap->approval_diff), s->approval_diff);
    snap->approval_risk    = s->approval_risk;
    snap->approval_details = s->approval_details;
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

// 工作流轮播(▲ 上一个 / ▼ 下一个)
static void cycle_workflow(app_state_t *s, int dir) {
    s->workflow = (app_workflow_t)((s->workflow + dir + APP_WF_COUNT) % APP_WF_COUNT);
}

static void go_ready(app_state_t *s, uint64_t now_ms, app_action_t *out, uint8_t *n, uint8_t max) {
    s->state = APP_ST_READY;
    s->state_since_ms = now_ms;
    app_action_t a = { .type = APP_ACT_SEND_WORKFLOW_SWITCH };
    a.u.workflow = (uint8_t)s->workflow;
    emit(out, n, max, a);
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
    // 注意:不重发 workflow.switch —— 工作流未变,Mac 侧无需重复通知
}

static void handle_key(app_state_t *s, const app_event_t *ev, uint64_t now_ms,
                       app_action_t *out, uint8_t *n, uint8_t max) {
    const uint8_t b = ev->u.key.btn;
    const bool wake_only = !s->screen_on; // 息屏时任何键只唤醒

    if (wake_only) {
        if (ev->type == APP_EV_KEY_PRESS) {
            s->screen_on = true;
            s->last_key_ms = now_ms;
            app_action_t a = { .type = APP_ACT_UI_SCREEN_ON };
            emit(out, n, max, a);
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        }
        return;
    }

    switch (s->state) {
    case APP_ST_HOME:
        if (ev->type == APP_EV_KEY_CLICK && (b == APP_BTN_UP || b == APP_BTN_DOWN)) {
            cycle_workflow(s, b == APP_BTN_UP ? -1 : +1);
            go_ready(s, now_ms, out, n, max);
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            go_ready(s, now_ms, out, n, max);
        }
        break;

    case APP_ST_READY:
        if (ev->type == APP_EV_KEY_CLICK && (b == APP_BTN_UP || b == APP_BTN_DOWN)) {
            cycle_workflow(s, b == APP_BTN_UP ? -1 : +1);
            app_action_t a = { .type = APP_ACT_SEND_WORKFLOW_SWITCH };
            a.u.workflow = (uint8_t)s->workflow;
            emit(out, n, max, a);
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        } else if (ev->type == APP_EV_KEY_PRESS && b == APP_BTN_OK) {
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
        break;

    case APP_ST_LISTENING:
        // 松开不立即结束,进入"待定结束"——单击窗口内再按 = 双击取消,窗口到期 = 发送。
        // 原因:iot_button 的 SINGLE/DOUBLE_CLICK 都要等双击窗口才上报,若松开即结束,
        // 双击的第二次按键必然落在 TRANSCRIBING(忽略),"双击取消"就成了死代码。
        if (ev->type == APP_EV_KEY_RELEASE && b == APP_BTN_OK) {
            s->ptt_pending_end = true;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        } else if (ev->type == APP_EV_KEY_PRESS && b == APP_BTN_OK && s->ptt_pending_end) {
            // 双击的第二次按下:立即取消(不用等 DOUBLE 事件,响应更快)。
            // CANCEL 清残留(防流入下一次会话) + voice.end 结束 Mac 端会话
            // (否则 relay 的 ASR 会话悬挂到超时)。
            app_action_t st = { .type = APP_ACT_STREAM_CANCEL };
            emit(out, n, max, st);
            app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
            emit(out, n, max, v);
            s->ptt_pending_end = false;
            set_toast(s, now_ms, "Recording cancelled");
            abort_to_ready(s, now_ms, NULL, out, n, max); // toast 已设,不再覆盖
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            // 单击窗口到期:正式结束并发送
            s->ptt_pending_end = false;
            app_action_t st = { .type = APP_ACT_STREAM_STOP };
            emit(out, n, max, st);
            app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
            emit(out, n, max, v);
            app_action_t t = { .type = APP_ACT_PLAY_TONE };
            t.u.tone = APP_TONE_SEND;
            emit(out, n, max, t);
            s->state = APP_ST_TRANSCRIBING;
            s->state_since_ms = now_ms;
            s->agent_state_name[0] = '\0';   // 新会话开始,清除旧 agent 状态
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        } else if (ev->type == APP_EV_KEY_DOUBLE && b == APP_BTN_OK) {
            // 防御分支:某些驱动在双击检测中抑制第二次 PRESS_DOWN,届时由 DOUBLE 取消
            app_action_t st = { .type = APP_ACT_STREAM_CANCEL };
            emit(out, n, max, st);
            app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
            emit(out, n, max, v);
            s->ptt_pending_end = false;
            set_toast(s, now_ms, "Recording cancelled");
            abort_to_ready(s, now_ms, NULL, out, n, max);
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
            s->approval_details = !s->approval_details;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        }
        break;

    case APP_ST_DONE:
        if (ev->type == APP_EV_KEY_CLICK && (b == APP_BTN_OK || b == APP_BTN_DOWN)) {
            s->state = APP_ST_HOME;
            s->state_since_ms = now_ms;
            s->approval_details = false;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
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

    if (!s->screen_on) return;

    // 息屏:HOME/READY/DONE 下 60s 无按键(APPROVAL 保持常亮:安全审批不熄屏)
    if ((s->state == APP_ST_HOME || s->state == APP_ST_READY || s->state == APP_ST_DONE) &&
        (now_ms - s->last_key_ms) >= APP_IDLE_SCREENOFF_MS) {
        s->screen_on = false;
        app_action_t a = { .type = APP_ACT_UI_SCREEN_OFF };
        emit(out, n, max, a);
        return;
    }

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
        s->ptt_pending_end = false;
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
        s->last_key_ms = now_ms;
        handle_key(s, ev, now_ms, out, out_n, max);
        break;

    case APP_EV_MAC_METRICS:
        s->mac_cpu    = ev->u.metrics.cpu;
        s->mac_ram    = ev->u.metrics.ram;
        s->mac_batt   = ev->u.metrics.battery;
        s->mac_charging = ev->u.metrics.charging;
        str_cpy(s->active_app, sizeof(s->active_app), ev->u.metrics.active_app);
        app_action_t m = { .type = APP_ACT_UI_REFRESH };
        emit(out, out_n, max, m);
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
                s->state = APP_ST_DONE;
                s->state_since_ms = now_ms;
                app_action_t t = { .type = APP_ACT_PLAY_TONE };
                t.u.tone = APP_TONE_SUCCESS;
                emit(out, out_n, max, t);
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
        // 审批打断录音:必须先停流,否则管线永远泄漏(APPROVAL 下没有按键能停它)
        if (s->state == APP_ST_LISTENING) {
            app_action_t st = { .type = APP_ACT_STREAM_STOP };
            emit(out, out_n, max, st);
            app_action_t ve = { .type = APP_ACT_SEND_VOICE_END };
            emit(out, out_n, max, ve);
            s->ptt_pending_end = false;
        }
        str_cpy(s->task_id, sizeof(s->task_id), ev->u.approval.task_id);
        str_cpy(s->approval_title, sizeof(s->approval_title), ev->u.approval.title);
        str_cpy(s->approval_target, sizeof(s->approval_target), ev->u.approval.target);
        str_cpy(s->approval_diff, sizeof(s->approval_diff), ev->u.approval.diff_summary);
        s->approval_risk = ev->u.approval.risk < APP_RISK_COUNT ? ev->u.approval.risk : APP_RISK_MEDIUM;
        s->approval_details = false;
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
        // 语义:EVENT 特征已订阅(链路通,PTT 可用)
        s->ble_connected = true;
        s->link_channel = 0;
        s->link_up = true;
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_BLE_DISCONNECTED:
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

    // ---- WiFi/WS 通道(Windows 移植:无蓝牙 PC 走 WiFi,语义与 BLE 对等)----

    case APP_EV_WIFI_CONNECTED:
        // 拿到 IP 只是链路半程:WS 尚未连上,link_up 仍由 WS_CONNECTED 置位。
        // 无动作 —— 仅信息事件(WS 断开后的自动重连链由 wifi_app 驱动)。
        break;

    case APP_EV_WIFI_CONNECT_FAIL:
        // 按 reason 去重:重连风暴中同一原因只 toast 一次;不同原因(或链路已恢复
        // 后的新失败,见 WS_CONNECTED 清位)允许再次提示。
        if (s->wifi_fail_reason != ev->u.wifi_fail.reason) {
            s->wifi_fail_reason = ev->u.wifi_fail.reason;
            set_toast(s, now_ms, "WiFi disconnected");
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;

    case APP_EV_WS_CONNECTED:
        // 与 BLE_CONNECTED 同构:WS 通道通 = 链路通,PTT 可用
        s->link_channel = 1;
        s->link_up = true;
        s->wifi_fail_reason = 0;   // 链路已恢复:后续新失败允许再次 toast
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_WS_DISCONNECTED:
        s->link_up = false;
        s->wifi_fail_reason = 0;   // 新的失败片段:允许再 toast
        // 状态收束与 BLE 断连同路径(停流/回 READY/审批保持);toast 文案区分通道
        handle_link_down(s, now_ms, "Companion offline", out, out_n, max);
        {
            // 触发 mDNS 重查:Companion 重启后自动重连(auto 模式)
            app_action_t rs = { .type = APP_ACT_RESOLVE_SERVICE };
            emit(out, out_n, max, rs);
        }
        break;

    case APP_EV_WS_TARGET_FOUND:
        {
            // 新目标 ≠ 缓存目标(去重在 mdns_resolver 内部),交给执行器 retarget
            app_action_t rt = { .type = APP_ACT_WS_RETARGET };
            str_cpy(rt.u.ws_target.url, sizeof(rt.u.ws_target.url), ev->u.ws_target.url);
            emit(out, out_n, max, rt);
        }
        break;

    case APP_EV_MODE_SWITCH:
        // 不进归约器:main.c 排空循环里直接调 mode_switch()(射频切换与状态机解耦)。
        // 归约器永不收到该事件 —— 此处仅占位以满足 -Wswitch。
        break;
    }
}
