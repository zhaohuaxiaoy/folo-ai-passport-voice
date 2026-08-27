// main/app_state.h —— 纯 C 状态机归约器(无 IDF 依赖,宿主机可测)。
// 模式借鉴 origin/demo/claude-buddy-port 的 buddy_state.c:状态 + 事件 → 动作。
#pragma once

#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    app_stage_t    state;
    bool           link_up;       // 当前通道已通(BLE:EVENT 已订阅 / WiFi:WS 已连);false → 禁 PTT + OFFLINE
    bool           net_busy;
    bool           screen_on;
    bool           panel_on;      // 面板供电(20s 背光灭后 60s 级 SLPIN 断电)
    bool           ble_connected; // BLE 连接已建立(与 link_up 区别:连接但未订阅)
    uint8_t        link_channel;  // 0=BLE / 1=WiFi(link_up 时是当前通道;断后保留供横幅显示)
    uint16_t       wifi_fail_reason; // 已 toast 的 WiFi 失败 reason(0=无;同因去重)
    uint64_t       last_key_ms;     // 最近按键时刻(息屏计时)
    uint64_t       state_since_ms;  // 进入当前状态的时刻(超时计时)
    uint64_t       toast_until_ms;
    bool           stream_started;  // 本会话采集是否已开(START 音播完后 TONE_DONE 驱动)
    char           toast[APP_TOAST_MAX];
    char           agent_message[APP_AGENT_MSG_MAX];
    bool           transcript_final;   // false → agent_message 是转写预览(未定稿);true = 定稿/非转写
    char           agent_state_name[16];
    char           task_id[APP_TASK_ID_MAX];
    char           approval_title[APP_TITLE_MAX];
    char           approval_target[APP_TARGET_MAX];
    char           approval_diff[APP_DIFF_MAX];
    uint8_t        approval_risk;
    bool           approval_details;   // ▼ 详情视图开关
} app_state_t;

void app_state_init(app_state_t *s);
// 处理一个事件,产出 0..out_max 个动作。now_ms 为毫秒单调时钟。
void app_state_reduce(app_state_t *s, const app_event_t *ev,
                      uint64_t now_ms, app_action_t *out, uint8_t *out_n);

void app_state_snapshot(const app_state_t *s, uint64_t now_ms, app_ui_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
