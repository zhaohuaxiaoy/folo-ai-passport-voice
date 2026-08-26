// main/app_ui.c —— 产品 UI 实现。见 app_ui.h 布局说明。
#include "app_ui.h"
#include "bsp_battery.h"     // 电量(主循环已把真实值补进快照,此处仅渲染)
#include "bsp_display.h"
#include "time_sync.h"       // 顶栏 HH:MM(校时源仅电脑客户端,未校时 "--:--")
#include "ui_pixel.h"
#include "lvgl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---- 布局常量 ----
#define BAR_H        26   // 顶栏高
#define BANNER_Y     28   // OFFLINE / NET BUSY 横幅
#define BANNER_H     18
#define CONTENT_Y    48   // 内容区起点
#define HINT_Y       286  // 各页底部提示行
#define W            240
#define H            320

// 顶栏左→右:BLE 图标 12px、电量、CPU/RAM(压缩 "C50 R60")、HH:MM 时间、
// 活跃应用(右对齐截断)。240px 满:6+12 / 24+36 / 64+48 / 116+52 / 172+64。
#define BLE_X        6
#define BATT_X       24
#define METRICS_X    64
#define METRICS_W    48
#define TIME_X       116
#define TIME_W       52
#define APP_X        172

// ---- 页内部件索引(与 page 切换共用) ----
typedef struct {
    lv_obj_t *root;                       // 本页容器(显隐切换)
    lv_obj_t *rec_elapsed;                // LISTENING:计时
    lv_obj_t *tr_message;                 // TRANSCRIBING:消息
    lv_obj_t *run_state;                  // AGENT_RUNNING:状态名
    lv_obj_t *run_message;                // AGENT_RUNNING:消息
    lv_obj_t *ap_risk_banner;             // APPROVAL:风险条
    lv_obj_t *ap_risk_label;              // APPROVAL:风险文本
    lv_obj_t *ap_title;                   // APPROVAL:标题
    lv_obj_t *ap_target;                  // APPROVAL:目标
    lv_obj_t *ap_diff;                    // APPROVAL:摘要/详情
} page_t;

static lv_obj_t *s_chrome;                // 顶层容器(lv_layer_top)
static lv_obj_t *s_ble_icon;
static lv_obj_t *s_batt_label;
static lv_obj_t *s_metrics_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_app_label;
static lv_obj_t *s_offline_banner;
static lv_obj_t *s_netbusy_banner;
static lv_obj_t *s_toast;

static page_t s_pages[APP_ST_COUNT];
static app_stage_t s_cur_page = APP_ST_COUNT;
static bool s_last_screen_on = true;
static lv_obj_t *s_bg;   // 基底屏:所有状态页都是它的子对象(单屏方案)

static const char *const RISK_NAMES[APP_RISK_COUNT] = { "LOW RISK", "MEDIUM RISK", "HIGH RISK" };
static const uint32_t RISK_COLORS[APP_RISK_COUNT] = { UI_GRASS, UI_YELLOW, UI_RED };

// ---- 基础块(无 LVGL 样式噪音) ----
static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *hint_label(lv_obj_t *parent, const char *text)
{
    return label(parent, text, &lv_font_montserrat_14, UI_MUTED, 0, HINT_Y, W);
}

// ---- 基底屏:天空 + 云 + 草地(复用 ui_pixel 视觉语言) ----
static void build_background(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);  // LVGL 9.5: 创建顶层 screen(旧 lv_screen_create 已移除)
    s_bg = scr;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_SKY), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 云(简版,取自 ui_pixel_screen_create)
    block(scr, 189, 15, 43, 10, UI_INK);
    block(scr, 193, 12, 35, 10, 0xFFFFFF);
    block(scr, 200, 8, 10, 9, 0xFFFFFF);
    block(scr, 215, 9, 9, 8, 0xFFFFFF);

    // 草地 + 纹理
    block(scr, 0, 286, 240, 34, UI_GRASS);
    block(scr, 0, 286, 240, 4, 0xA7D93E);
    for (int x = 0; x < 240; x += 30) {
        block(scr, x, 312, 18, 8, UI_GRASS_DARK);
        block(scr, x + 18, 316, 12, 4, 0x75452E);
    }
    lv_screen_load(scr);
}

// ---- chrome:常驻顶栏 / 横幅 / Toast(顶层,所有页共用) ----
static void build_chrome(void)
{
    s_chrome = lv_display_get_layer_top(lv_display_get_default());  // LVGL 9.5 改名
    lv_obj_remove_flag(s_chrome, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_chrome, 0, 0);

    // 顶栏
    lv_obj_t *bar = block(s_chrome, 0, 0, W, BAR_H, UI_INK);
    (void)bar;
    s_ble_icon = block(s_chrome, BLE_X, 7, 12, 12, UI_MUTED);   // 断开=灰,连接后改亮色
    s_batt_label = label(s_chrome, "--", &lv_font_montserrat_14, 0xFFFFFF,
                         BATT_X, 5, 36);
    s_metrics_label = label(s_chrome, "", &lv_font_montserrat_14, 0xFFFFFF,
                            METRICS_X, 5, METRICS_W);
    s_time_label = label(s_chrome, "--:--", &lv_font_montserrat_14, 0xFFFFFF,
                         TIME_X, 5, TIME_W);
    s_app_label = label(s_chrome, "", &lv_font_montserrat_14, UI_MUTED,
                        APP_X, 5, W - APP_X);

    // BLE 断线横幅(链路断时整宽显示;link_up = EVENT 特征已订阅)
    s_offline_banner = block(s_chrome, 0, BANNER_Y, W, BANNER_H, UI_RED);
    label(s_offline_banner, "BLE DISCONNECTED - reconnecting...", &lv_font_montserrat_14,
          0xFFFFFF, 0, 0, W);

    // BLE BUSY(音频丢帧中)
    s_netbusy_banner = block(s_chrome, 0, BANNER_Y, W, BANNER_H, UI_ORANGE);
    label(s_netbusy_banner, "BLE BUSY - dropping frames", &lv_font_montserrat_14,
          UI_INK, 0, 0, W);

    // Toast(底部浮层,空文本即隐藏)
    lv_obj_t *tbg = block(s_chrome, 30, 272, 180, 30, UI_INK);
    s_toast = label(tbg, "", &lv_font_montserrat_14, 0xFFFFFF, 0, 5, 180);
}

// ---- 各状态页 ----
static void build_home(void)
{
    page_t *p = &s_pages[APP_ST_HOME];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    lv_obj_t *plate = ui_pixel_panel_create(p->root, 20, CONTENT_Y + 8, 200, 40, UI_PAPER);
    label(plate, "AI PASSPORT", &lv_font_montserrat_20, UI_INK, 0, 8, 200);
    ui_pixel_mascot_create(p->root, 101, 100);
    label(p->root, "hold OK to enter", &lv_font_montserrat_14, UI_MUTED, 0, 200, W);
    hint_label(p->root, "OK: ENTER   DBL-OK: CLEAR   DOWN: ENTER");
}

static void build_ready(void)
{
    page_t *p = &s_pages[APP_ST_READY];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    // 工作流切换已取消(固定 build),READY 为简单就绪页
    label(p->root, "READY", &lv_font_montserrat_20, UI_INK, 0, CONTENT_Y + 24, W);
    hint_label(p->root, "HOLD OK: SPEAK   DOWN: ENTER   DBL-OK: CLEAR");
}

static void build_listening(void)
{
    page_t *p = &s_pages[APP_ST_LISTENING];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    // 麦克风图标(块状像素画, 居中; 替代 REC 红点/文字与音量条 —— 录音中
    // 只显示麦克风, 不做音量可视化)
    block(p->root, 112, 58, 16, 26, UI_RED);    // 拾音头(胶囊)
    block(p->root, 108, 68, 24, 6, UI_RED);     // 拾音头顶部弧
    block(p->root, 108, 78, 24, 6, UI_RED);     // 拾音头底部弧
    block(p->root, 116, 84, 8, 8, UI_RED);      // 拾音头底口
    block(p->root, 116, 92, 8, 12, UI_RED);     // 立杆
    block(p->root, 104, 104, 32, 6, UI_RED);    // 底座上沿
    block(p->root, 112, 110, 16, 6, UI_RED);    // 底座

    p->rec_elapsed = label(p->root, "0s", &lv_font_montserrat_20, UI_INK, 0, 200, W);
    hint_label(p->root, "RELEASE OK: SEND");
}

static void build_transcribing(void)
{
    page_t *p = &s_pages[APP_ST_TRANSCRIBING];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    label(p->root, "Transcribing...", &lv_font_montserrat_20, UI_INK, 0, 96, W);
    p->tr_message = label(p->root, "", &lv_font_montserrat_14, UI_MUTED, 20, 140, 200);
    hint_label(p->root, "PLEASE WAIT");
}

static void build_running(void)
{
    page_t *p = &s_pages[APP_ST_AGENT_RUNNING];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    block(p->root, 108, 76, 24, 24, UI_SKY_DARK);              // 静态"spinner"块
    p->run_state = label(p->root, "running", &lv_font_montserrat_20, UI_INK,
                         0, 112, W);
    p->run_message = label(p->root, "", &lv_font_montserrat_14, UI_MUTED,
                           20, 148, 200);
    hint_label(p->root, "AGENT WORKING...");
}

static void build_approval(void)
{
    page_t *p = &s_pages[APP_ST_APPROVAL];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    p->ap_risk_banner = block(p->root, 20, CONTENT_Y + 8, 200, 28, UI_GRASS);
    p->ap_risk_label = label(p->ap_risk_banner, "", &lv_font_montserrat_14, UI_INK,
                             0, 5, 200);

    p->ap_title = label(p->root, "", &lv_font_montserrat_20, UI_INK, 20, 108, 200);
    lv_obj_set_style_text_align(p->ap_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(p->ap_title, LV_LABEL_LONG_WRAP);

    p->ap_target = label(p->root, "", &lv_font_montserrat_14, UI_SKY_DARK, 20, 150, 200);
    p->ap_diff = label(p->root, "", &lv_font_montserrat_14, UI_MUTED, 20, 176, 200);
    lv_obj_set_style_text_align(p->ap_diff, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(p->ap_diff, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(p->ap_diff, 88);

    hint_label(p->root, "OK: APPROVE   UP: REJECT   DOWN: ENTER");
}

static void build_done(void)
{
    page_t *p = &s_pages[APP_ST_DONE];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    // 对勾块状画
    block(p->root, 96, 96, 48, 48, UI_GRASS);
    block(p->root, 108, 112, 8, 18, 0xFFFFFF);
    block(p->root, 108, 122, 22, 8, 0xFFFFFF);
    block(p->root, 122, 108, 8, 8, 0xFFFFFF);
    block(p->root, 122, 116, 8, 8, 0xFFFFFF);

    label(p->root, "DONE", &lv_font_montserrat_20, UI_INK, 0, 164, W);
    label(p->root, "text injected into your editor", &lv_font_montserrat_14,
          UI_MUTED, 0, 196, W);
    hint_label(p->root, "OK: BACK TO HOME   DOWN: ENTER");
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// U1 dirty-check:文本未变则跳过 set_text。set_text 会重排标签并标记整行
// 无效重绘——LISTENING 10fps 渲染下反复写相同文本(计时秒数、电量、CPU/RAM、
// agent 消息)是无谓开销。lv_label_get_text 返回当前文本,比较后决定是否写。
static void label_set_if_changed(lv_obj_t *l, const char *text)
{
    if (!l || !text) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(l, text);
}

static void label_set_fmt_if_changed(lv_obj_t *l, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    label_set_if_changed(l, buf);
}

// 转写预览态:文本尾部附一个光标感字符 '_'(未定稿视觉);定稿后移除。
// 改动最小方案:不动样式,只改文本。栈缓冲覆盖 满长文本 + 光标。
static void set_agent_message(lv_obj_t *label_obj, const char *msg, bool preview)
{
    char buf[APP_AGENT_MSG_MAX + 2];
    if (preview && msg && msg[0]) {
        snprintf(buf, sizeof(buf), "%s_", msg);
        label_set_if_changed(label_obj, buf);
    } else {
        label_set_if_changed(label_obj, msg ? msg : "");
    }
}

// ---- 页切换 ----
static void show_page(app_stage_t st)
{
    if (st == s_cur_page) return;
    if (s_cur_page < APP_ST_COUNT) lv_obj_add_flag(s_pages[s_cur_page].root, LV_OBJ_FLAG_HIDDEN);
    if (st < APP_ST_COUNT) lv_obj_remove_flag(s_pages[st].root, LV_OBJ_FLAG_HIDDEN);
    s_cur_page = st;
}

esp_err_t app_ui_init(void)
{
    build_background();
    build_chrome();
    build_home();
    build_ready();
    build_listening();
    build_transcribing();
    build_running();
    build_approval();
    build_done();
    for (int i = 0; i < APP_ST_COUNT; i++) {
        lv_obj_add_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
    }
    show_page(APP_ST_HOME);
    return ESP_OK;
}

// ---- 渲染 ----
void app_ui_render(const app_ui_snapshot_t *snap)
{
    // 息屏/唤醒:背光切换(内容照常更新,唤醒后即为最新)
    if (snap->screen_on != s_last_screen_on) {
        bsp_display_backlight(snap->screen_on ? 100 : 0);
        s_last_screen_on = snap->screen_on;
    }
    if (!snap->screen_on) return;

    // ---- chrome ----
    lv_obj_set_style_bg_color(s_ble_icon,
        lv_color_hex(snap->ble_connected ? 0x7CD6FF : UI_MUTED), 0);
    if (snap->battery_available) {
        label_set_fmt_if_changed(s_batt_label, "%d%%", snap->battery_soc);
    } else {
        label_set_if_changed(s_batt_label, "--");
    }
    label_set_fmt_if_changed(s_metrics_label, "C%d R%d",
                             snap->mac_cpu, snap->mac_ram);
    {
        char t[16];
        time_sync_format_local(t, sizeof(t));
        label_set_if_changed(s_time_label, t);
    }
    label_set_if_changed(s_app_label, snap->active_app[0] ? snap->active_app : "");

    // 横幅互斥:OFFLINE(通道断线)> BUSY(同位置 BANNER_Y)
    // 文案按当前链路通道渲染(BLE/WiFi;Windows 移植:WiFi 通道断线显示 WiFi 字样)
    label_set_fmt_if_changed(s_offline_banner, "%s DISCONNECTED - reconnecting...",
                             snap->link_name);
    label_set_fmt_if_changed(s_netbusy_banner, "%s BUSY - dropping frames",
                             snap->link_name);
    set_hidden(s_offline_banner, snap->link_up);
    set_hidden(s_netbusy_banner, !snap->net_busy || !snap->link_up);

    if (snap->toast[0]) {
        label_set_if_changed(s_toast, snap->toast);
        set_hidden(lv_obj_get_parent(s_toast), false);
    } else {
        set_hidden(lv_obj_get_parent(s_toast), true);
    }

    // ---- 页内容 ----
    show_page(snap->state);
    switch (snap->state) {
    case APP_ST_LISTENING:
        // 录音中只显示麦克风图标(静态), 无音量可视化; 仅计时实时刷新
        label_set_fmt_if_changed(s_pages[APP_ST_LISTENING].rec_elapsed, "%ds",
                                 snap->elapsed_ms / 1000);
        break;
    case APP_ST_TRANSCRIBING:
        set_agent_message(s_pages[APP_ST_TRANSCRIBING].tr_message,
                          snap->agent_message, !snap->transcript_final);
        break;
    case APP_ST_AGENT_RUNNING:
        label_set_if_changed(s_pages[APP_ST_AGENT_RUNNING].run_state,
                             snap->agent_state_name);
        set_agent_message(s_pages[APP_ST_AGENT_RUNNING].run_message,
                          snap->agent_message, !snap->transcript_final);
        break;
    case APP_ST_APPROVAL: {
        uint8_t r = snap->approval_risk < APP_RISK_COUNT ? snap->approval_risk : APP_RISK_MEDIUM;
        lv_obj_set_style_bg_color(s_pages[APP_ST_APPROVAL].ap_risk_banner,
                                  lv_color_hex(RISK_COLORS[r]), 0);
        label_set_if_changed(s_pages[APP_ST_APPROVAL].ap_risk_label, RISK_NAMES[r]);
        label_set_if_changed(s_pages[APP_ST_APPROVAL].ap_title, snap->approval_title);
        label_set_fmt_if_changed(s_pages[APP_ST_APPROVAL].ap_target, "target: %s",
                                 snap->approval_target);
        label_set_if_changed(s_pages[APP_ST_APPROVAL].ap_diff, snap->approval_diff);
        break;
    }
    default:
        break;
    }
}
