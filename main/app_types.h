// main/app_types.h —— 纯 C 共享类型:状态机/事件/动作/UI 快照。
// 不 include 任何 ESP-IDF 头,保证可在宿主机上直接编译测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- 工作流 ----------------
typedef enum {
    APP_WF_BUILD = 0,
    APP_WF_DEBUG,
    APP_WF_REVIEW,
    APP_WF_TEST,
    APP_WF_CAPTURE,
    APP_WF_COUNT,
} app_workflow_t;

extern const char *const APP_WORKFLOW_NAMES[APP_WF_COUNT];

// ---------------- 状态 ----------------
// 注意:命名为 app_stage_t 而非 app_state_t —— 后者是 app_state.h 里的完整状态结构体。
typedef enum {
    APP_ST_HOME = 0,        // 待机主页
    APP_ST_READY,           // 工作流就绪(可 PTT)
    APP_ST_LISTENING,       // 录音中(PTT 按住)
    APP_ST_TRANSCRIBING,    // 等待 Mac 转写
    APP_ST_AGENT_RUNNING,   // Agent 执行中
    APP_ST_APPROVAL,        // 待物理审批
    APP_ST_DONE,            // 任务完成
    APP_ST_COUNT,
} app_stage_t;

// ---------------- 按键 ----------------
typedef enum {
    APP_BTN_UP = 0,
    APP_BTN_DOWN,
    APP_BTN_OK,
} app_btn_t;

// ---------------- 提示音 ----------------
typedef enum {
    APP_TONE_START = 0,     // 440Hz/80ms  录音就绪
    APP_TONE_SEND,          // 880Hz/60ms  语音已发送
    APP_TONE_APPROVAL,      // 587+880/150ms 审批提醒
    APP_TONE_SUCCESS,       // 660→880/200ms 完成
    APP_TONE_REJECT,        // 220Hz/120ms 拒绝
    APP_TONE_ERROR,         // 180Hz/100ms 离线/非法操作
    APP_TONE_COUNT,
} app_tone_t;

// ---------------- 事件 ----------------
typedef enum {
    APP_EV_KEY_PRESS = 0,
    APP_EV_KEY_RELEASE,
    APP_EV_KEY_CLICK,
    APP_EV_KEY_DOUBLE,
    APP_EV_KEY_LONG,
    APP_EV_MAC_METRICS,
    APP_EV_AGENT_STATUS,
    APP_EV_APPROVAL_REQUEST,
    APP_EV_TRANSCRIPT,
    APP_EV_AUDIO_DROP_START,
    APP_EV_AUDIO_DROP_END,
    APP_EV_BLE_CONNECTED,   // 链路通:EVENT 特征已被订阅(PTT 可用的充分条件)
    APP_EV_BLE_DISCONNECTED,// 链路断(连接断开或 EVENT 订阅取消)
    APP_EV_BLE_DROP,        // BLE 事件行因断连/未订阅被丢弃(UI toast 反馈)
    APP_EV_AUDIO_ERROR,     // 采集硬件错误
    APP_EV_TICK,            // 100ms 心跳,驱动超时/息屏
} app_event_type_t;

// Agent 状态(与协议字符串互转在 app_protocol.c)
typedef enum {
    APP_AGENT_READY = 0,
    APP_AGENT_THINKING,
    APP_AGENT_RUNNING,
    APP_AGENT_ERROR,
    APP_AGENT_DONE,
    APP_AGENT_COUNT,
} app_agent_state_t;

// 注入方式(协议兼容保留:Mac 端注入,设备不再消费;UI 显示转写)
typedef enum {
    APP_INJECT_TYPE = 0,    // 旧:HID 逐字键入(退役,仅字段保留)
    APP_INJECT_PASTE,       // 旧:粘贴 Cmd+V(退役,仅字段保留)
} app_inject_mode_t;

// 审批风险等级
typedef enum {
    APP_RISK_LOW = 0,
    APP_RISK_MEDIUM,
    APP_RISK_HIGH,
    APP_RISK_COUNT,
} app_risk_t;

// 审批动作
typedef enum {
    APP_ACTION_APPROVE = 0,
    APP_ACTION_REJECT,
    APP_ACTION_DETAILS,
} app_approval_decision_t;

// 文本长度上限(Mac 端按条下发,设备仅显示)
#define APP_TRANSCRIPT_MAX    128
#define APP_TASK_ID_MAX       32
#define APP_TITLE_MAX         64
#define APP_TARGET_MAX        64
#define APP_DIFF_MAX          64
#define APP_METRIC_APP_MAX    24
// 显示通道:须 ≥ APP_TRANSCRIPT_MAX,保证 relay 按 128B 切分的转写行完整落屏
#define APP_AGENT_MSG_MAX     APP_TRANSCRIPT_MAX
#define APP_TOAST_MAX         64

// 事件(定长结构,入队拷贝,union 保小)
typedef struct {
    app_event_type_t type;
    union {
        struct { uint8_t btn; } key;                    // KEY_*
        struct {
            uint8_t cpu; uint8_t ram; uint8_t battery;  // 0..100
            bool charging;
            char active_app[APP_METRIC_APP_MAX];
        } metrics;                                       // MAC_METRICS
        struct {
            uint8_t state;                              // app_agent_state_t
            char message[APP_AGENT_MSG_MAX];
        } agent_status;                                 // AGENT_STATUS
        struct {
            char task_id[APP_TASK_ID_MAX];
            char title[APP_TITLE_MAX];
            char target[APP_TARGET_MAX];
            char diff_summary[APP_DIFF_MAX];
            uint8_t risk;                               // app_risk_t
        } approval;                                     // APPROVAL_REQUEST
        struct {
            char text[APP_TRANSCRIPT_MAX];
            uint8_t inject_mode;                        // app_inject_mode_t(协议兼容保留)
            bool final;                                 // false=预览态(未定稿);true=定稿落定
        } transcript;                                   // TRANSCRIPT
    } u;
} app_event_t;

// ---------------- 动作 ----------------
typedef enum {
    APP_ACT_NONE = 0,
    APP_ACT_UI_REFRESH,
    APP_ACT_UI_SCREEN_OFF,
    APP_ACT_UI_SCREEN_ON,
    APP_ACT_SEND_VOICE_START,
    APP_ACT_SEND_VOICE_END,
    APP_ACT_SEND_WORKFLOW_SWITCH,
    APP_ACT_SEND_AGENT_ACTION,
    APP_ACT_STREAM_START,
    APP_ACT_STREAM_STOP,
    APP_ACT_PLAY_TONE,
} app_action_type_t;

#define APP_ACT_MAX 4   // 单事件最多产出的动作数

typedef struct {
    app_action_type_t type;
    union {
        uint8_t tone;                                   // PLAY_TONE
        uint8_t workflow;                               // SEND_WORKFLOW_SWITCH(app_workflow_t)
        struct {
            char task_id[APP_TASK_ID_MAX];
            uint8_t decision;                           // app_approval_decision_t
        } agent_action;                                 // SEND_AGENT_ACTION
    } u;
} app_action_t;

// ---------------- UI 快照 ----------------
typedef struct {
    app_stage_t    state;
    app_workflow_t workflow;
    bool           link_up;       // false → OFFLINE(BLE DISCONNECTED)横幅 + 禁 PTT;true = EVENT 已订阅
    bool           screen_on;
    bool           net_busy;      // 音频丢帧中 → BLE BUSY
    bool           ble_connected; // BLE 连接是否建立(未订阅时图标灰色)
    bool           battery_available;
    uint8_t        battery_soc;   // 0..100
    uint8_t        mac_cpu;       // 0..100
    uint8_t        mac_ram;       // 0..100
    uint8_t        mac_batt;      // 0..100
    bool           mac_charging;
    char           active_app[APP_METRIC_APP_MAX];
    char           agent_message[APP_AGENT_MSG_MAX];
    bool           transcript_final; // false → 当前 agent_message 是转写预览(未定稿,UI 加光标感)
    char           agent_state_name[16];
    char           task_id[APP_TASK_ID_MAX];
    char           approval_title[APP_TITLE_MAX];
    char           approval_target[APP_TARGET_MAX];
    char           approval_diff[APP_DIFF_MAX];
    uint8_t        approval_risk;   // app_risk_t
    bool           approval_details; // ▼ 展开的 Diff 详情视图
    uint32_t       elapsed_ms;      // 当前状态已持续时长(主循环在快照时补)
    char           toast[APP_TOAST_MAX];
} app_ui_snapshot_t;

// ---------------- 超时常量 ----------------
#define APP_IDLE_SCREENOFF_MS   60000u   // 无按键息屏
#define APP_TRANSCRIBE_TIMEOUT  30000u   // 转写等待超时
#define APP_AGENT_RUN_TIMEOUT   90000u   // Agent 执行超时
#define APP_TICK_MS             100u     // 应用任务心跳

#ifdef __cplusplus
}
#endif
