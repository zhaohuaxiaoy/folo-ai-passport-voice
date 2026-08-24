// main/app_state.h —— 纯 C 状态机归约器(无 IDF 依赖,宿主机可测)。
// 模式借鉴 origin/demo/claude-buddy-port 的 buddy_state.c:状态 + 事件 → 动作。
#pragma once

#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    app_stage_t    state;
    app_workflow_t workflow;
    bool           ws_connected;
    bool           net_busy;
    bool           screen_on;
    bool           ble_connected;
    uint64_t       last_key_ms;     // 最近按键时刻(息屏计时)
    uint64_t       state_since_ms;  // 进入当前状态的时刻(超时计时)
    uint64_t       toast_until_ms;
    bool           ptt_pending_end; // LISTENING:已松开、等单击窗口定夺(再按=取消,到期=发送)
    bool           wifi_configured; // NVS 已有凭据(未配网横幅依据)
    bool           provisioning;    // BLE 配网会话进行中
    uint64_t       prov_deadline_ms;// 配网超时时刻(provisioning 时有效)
    char           wifi_ssid[PROV_SSID_MAX + 1];
    char           toast[APP_TOAST_MAX];
    uint8_t        mac_cpu, mac_ram, mac_batt;
    bool           mac_charging;
    char           active_app[APP_METRIC_APP_MAX];
    char           agent_message[APP_AGENT_MSG_MAX];
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
