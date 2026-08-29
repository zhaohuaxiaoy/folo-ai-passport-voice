// main/app_types.h —— 纯 C 共享类型:状态机/事件/动作/UI 快照。
// 不 include 任何 ESP-IDF 头,保证可在宿主机上直接编译测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- 按键动作(下行注入,由 PC client 执行) ----------------
typedef enum {
    APP_KEY_ENTER = 0,   // 输入框回车(提交)
    APP_KEY_CLEAR,       // 清空输入框全部文字
} app_key_action_t;

// ---------------- 状态 ----------------
// 注意:命名为 app_stage_t 而非 app_state_t —— 后者是 app_state.h 里的完整状态结构体。
typedef enum {
    APP_ST_HOME = 0,        // 待机主页
    APP_ST_READY,           // 工作流就绪(可 PTT)
    APP_ST_LISTENING,       // 录音中(PTT 按住)
    APP_ST_TRANSCRIBING,    // 等待 Mac 转写
    APP_ST_AGENT_RUNNING,   // Agent 执行中
    APP_ST_APPROVAL,        // 待物理审批
    APP_ST_COUNT,
} app_stage_t;

// ---------------- 按键 ----------------
// 物理键语义(2026-08-29):UP(音量加)= 按下即录、松开发送(按住说话);
// DOWN(音量减)= 单击回车 / 长按 0.5s 清空输入框;OK = 单击导航·批准 /
// 长按 0.5s 锁屏解锁。同一颗键上只放"单击 + 长按",不放双击(见 app_state.c)。
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
    APP_EV_KEY_LONG,        // 长按达阈值瞬间(UP 键 0.5s → 说话开始)
    APP_EV_KEY_LONG_UP,     // 长按超时后松开(长按态不再报 RELEASE;UP 键 → 说话结束)
    APP_EV_AGENT_STATUS,
    APP_EV_APPROVAL_REQUEST,
    APP_EV_TRANSCRIPT,
    APP_EV_AUDIO_DROP_START,
    APP_EV_AUDIO_DROP_END,
    APP_EV_BLE_CONNECTED,   // 链路通:EVENT 特征已被订阅(PTT 可用的充分条件)
    APP_EV_BLE_DISCONNECTED,// 链路断(连接断开或 EVENT 订阅取消)
    APP_EV_BLE_DROP,        // BLE 事件行因断连/未订阅被丢弃(UI toast 反馈)
    APP_EV_AUDIO_ERROR,     // 采集硬件错误
    APP_EV_TONE_DONE,       // START 提示音播放完成(异步开流的信号,见 design)
    APP_EV_TICK,            // 100ms 心跳,驱动超时/息屏
    // ---- USB 有线通道(USB-Serial-JTAG,见 design.md)----
    APP_EV_USB_CONNECTED,   // USB 会话通(收到 PC 握手 ping;link_up 的 USB 侧充分条件)
    APP_EV_USB_DISCONNECTED, // USB 会话断(拔线:is_connected 翻转)
    APP_EV_TIME_SET,         // 校时下行(epoch 秒 UTC;双通道共用:CTRL time.set 行 + SYS time set)
} app_event_type_t;

// ---------------- 链路通道(双通道常开架构,2026-08-28) ----------------
// 旧互斥模式(BLE=0/USB=2, NVS 持久化 + 重启切换)已整体退役:BLE 广播常驻,
// USB 数据通道插线即用(usb_link 待命,主机出现自动接入)。会话路由按
// link_channel 取值:状态机里最近连接/使用的通道,音频与 voice.* 上行跟随它。
typedef enum {
    APP_CHAN_BLE = 0,       // BLE(无线):EVENT 特征订阅即通
    APP_CHAN_USB = 2,       // USB(有线):PC ping 握手即通
    APP_CHAN_NONE = 0xFF,   // 双通道均断(link_up=false)
} app_chan_t;

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
// 显示通道:须 ≥ APP_TRANSCRIPT_MAX,保证 relay 按 128B 切分的转写行完整落屏
#define APP_AGENT_MSG_MAX     APP_TRANSCRIPT_MAX
#define APP_TOAST_MAX         64

// 事件(定长结构,入队拷贝,union 保小)
typedef struct {
    app_event_type_t type;
    union {
        // mv = 回调时刻 bsp_button_read_mv()(mV):真实按压 3-5,假按(射频
        // 腐蚀)2890=松开电平 —— app_state 据此区分真假 LONG(见 READY 态)
        struct { uint8_t btn; uint16_t mv; } key;       // KEY_*
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
        struct { int64_t epoch; } time_set;             // TIME_SET(UTC 秒,int64 对齐 8,union 仍 ≤228B)
    } u;
} app_event_t;

// ---------------- 动作 ----------------
typedef enum {
    APP_ACT_NONE = 0,
    APP_ACT_UI_REFRESH,
    APP_ACT_UI_SCREEN_OFF,
    APP_ACT_UI_SCREEN_ON,
    APP_ACT_UI_PANEL_OFF,   // 面板 SLPIN 断电(60s 无操作级)
    APP_ACT_UI_PANEL_ON,    // 面板 SLPOUT 上电(唤醒)
    APP_ACT_SEND_VOICE_START,
    APP_ACT_SEND_VOICE_END,
    APP_ACT_SEND_KEY_ACTION,   // 上行按键动作(enter/clear,PC client 执行注入)
    APP_ACT_SEND_AGENT_ACTION,
    APP_ACT_STREAM_START,
    APP_ACT_STREAM_STOP,
    APP_ACT_STREAM_CANCEL,   // 取消/断链:停采集 + 清空 ring + 丢弃在途帧(与 STOP 区别:不排空发送)
    APP_ACT_PLAY_TONE,
    APP_ACT_TIME_SET,        // time_sync_set_epoch(校时落地)
} app_action_type_t;

// 单事件最多产出的动作数。emit() 满了就静默丢弃,所以这个值必须 ≥ 最长的
// 那一条归约路径,否则丢的是尾部动作 —— 4 → 6(2026-08-29):息屏/断电态按
// 音量加开录音是最长路径,唤醒三连(UI_PANEL_ON + UI_SCREEN_ON + UI_REFRESH)
// 之后还有 PLAY_TONE + SEND_VOICE_START = 5 条,第 5 条正好被丢 —— 结果是
// 滴声照响、采集照开,而 Mac 侧从没收到 voice.start(整段话进不了 ASR)。
// 同长的还有"息屏 + 音量减长按清空"(唤醒三连 + SEND_KEY_ACTION + PLAY_TONE)
// 与离线 PTT(唤醒三连 + PLAY_TONE(ERROR) + UI_REFRESH)。取 6 留一条余量。
#define APP_ACT_MAX 6

typedef struct {
    app_action_type_t type;
    union {
        uint8_t tone;                                   // PLAY_TONE
        struct { uint8_t action; } key_action;          // SEND_KEY_ACTION(app_key_action_t)
        struct {
            char task_id[APP_TASK_ID_MAX];
            uint8_t decision;                           // app_approval_decision_t
        } agent_action;                                 // SEND_AGENT_ACTION
        struct { int64_t epoch; } time_set;             // TIME_SET
    } u;
} app_action_t;

// ---------------- UI 快照 ----------------
typedef struct {
    app_stage_t    state;
    bool           link_up;       // false → OFFLINE(link_name DISCONNECTED)横幅 + 禁 PTT;true = 通道已通
    bool           screen_on;
    bool           panel_on;      // 面板供电(SLPIN 断电后 false)
    bool           net_busy;      // 音频丢帧中 → BUSY 横幅
    bool           ble_connected; // BLE 连接是否建立(未订阅时图标灰色)
    char           link_name[8];  // 当前链路通道名:"BLE"/"USB"(断线横幅按此渲染)
    bool           battery_available;
    uint8_t        battery_soc;   // 0..100
    char           agent_message[APP_AGENT_MSG_MAX];
    bool           transcript_final; // false → 当前 agent_message 是转写预览(未定稿,UI 加光标感)
    char           agent_state_name[16];
    char           task_id[APP_TASK_ID_MAX];
    char           approval_title[APP_TITLE_MAX];
    char           approval_target[APP_TARGET_MAX];
    char           approval_diff[APP_DIFF_MAX];
    uint8_t        approval_risk;   // app_risk_t
    uint32_t       elapsed_ms;      // 当前状态已持续时长(主循环在快照时补)
    char           toast[APP_TOAST_MAX];
} app_ui_snapshot_t;

// ---------------- 超时常量 ----------------
#define APP_IDLE_BACKLIGHT_OFF_MS   20000u   // 无按键 → 关背光(息屏,渲染跳过)
#define APP_IDLE_PANEL_OFF_MS       60000u   // 无按键 → 面板 SLPIN 断电(μA 级)
#define APP_TRANSCRIBE_TIMEOUT  30000u   // 转写等待超时
#define APP_AGENT_RUN_TIMEOUT   90000u   // Agent 执行超时
#define APP_TICK_MS             100u     // 应用任务心跳
// S3:START 音后未收 TONE_DONE 的最大等待(兜底开流)。500 → 200(2026-08-29):
// 滴声本身只有 80ms,500ms 的余量是给"声音任务被挤住"留的,可它同时也是 PTT 最
// 坏延迟 —— 真机抓到过一次按下到开录 622ms(= 本值 + APP_TICK_MS 粒度),用户已
// 在说话而设备还没开录。降到 200 仍有 2.5 倍余量;代价是滴声真被拖慢时会与采集
// 重叠(前几帧录进 440Hz),只发生在退化路径,可接受。兜底触发在串口留痕
// (main.c 心跳里辨认 STREAM_START),下次再慢可直接看日志而不是猜。
#define APP_TONE_PENDING_TIMEOUT_MS 200u
// PTT 最长单次说话(2026-08-29 新增):LISTENING 原本是唯一没有退出超时的状态
// —— 松开事件一旦丢了(on_key 走 post_important(100ms),队列满仍可能丢),设备就
// 永久 LISTENING:一直采集、一直占着会话 PM 锁(进不了低功耗)、Mac 侧挂着永不
// 收束的 voice 流,只能靠断电恢复。到点按"正常松手"收束(STREAM_STOP +
// SEND_VOICE_END,已录的照常转写),不当错误处理 —— 真人一口气说不到 60s,正常
// 使用永远碰不到这条路径。
#define APP_PTT_MAX_TALK_MS     60000u

#ifdef __cplusplus
}
#endif
