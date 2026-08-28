// components/bsp/src/bsp_button.c
// 移植自 trae_card/components/platform/platform_esp32/src/btn_iot_button.c
#include "bsp_button.h"
#include "bsp_pins.h"
#include "iot_button.h"
#include "button_adc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"

// button_adc.c 补丁(managed_component,升级会覆盖):面板上电 SPI 窗抑制。
extern void button_adc_set_ignore_until(int64_t until_us);

// 面板上电瞬态抑制窗口:SLPOUT+120ms+DISPON+LVGL 全屏刷新 ~300ms,取 600ms。
#define BSP_BTN_PANEL_GLITCH_US (600 * 1000)

static const char *TAG = "bsp_btn";

static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

static button_handle_t s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t    s_cb;
static void           *s_user;

// ADC1 是 unit 级独占资源:iot_button 与 bsp_button_read_mv() 必须共用同一个 oneshot
// 句柄。谁第二个调 adc_oneshot_new_unit() 谁就拿到 "adc1 is already in use"。
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

// 电压读取的衰减档必须与 button 组件内部的 ADC_BUTTON_ATTEN 一致 —— 通道只被配置一次
// (由组件在 iot_button_new_adc_device() 里下发),两边对不上会让读数与按键阈值错位。
// managed_components/espressif__button/button_adc.c:26 在 C3 上取 ADC_ATTEN_DB_6+1。
#define BSP_BTN_ATTEN  ADC_ATTEN_DB_12       // 量程约 0~3100mV,覆盖松开态

// 每个按键把"哪个键"随回调带回来。button 组件的回调签名固定,故用 usr_data 传索引。
static void on_event(void *arg, void *usr_data, bsp_btn_ev_t ev) {
    (void)arg;
    if (!s_cb) return;
    s_cb((bsp_btn_t)(intptr_t)usr_data, ev, s_user);
}
static void cb_press  (void *a, void *u) { on_event(a, u, BSP_BTN_PRESS);   }
static void cb_release(void *a, void *u) { on_event(a, u, BSP_BTN_RELEASE); }
static void cb_click  (void *a, void *u) { on_event(a, u, BSP_BTN_CLICK);   }
static void cb_double (void *a, void *u) { on_event(a, u, BSP_BTN_DOUBLE);  }
static void cb_long   (void *a, void *u) { on_event(a, u, BSP_BTN_LONG);    }
static void cb_long_up(void *a, void *u) { on_event(a, u, BSP_BTN_LONG_UP); }

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    s_cb = cb; s_user = user;

    // 先由 BSP 建 unit,再把句柄交给 button 组件(button_adc.h:adc_handle 非 NULL 即复用),
    // 这样本文件的 bsp_button_read_mv() 也能读同一路 ADC。
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t ae = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit 创建失败 (%s)", esp_err_to_name(ae));
        s_adc = NULL;
        return ae;
    }

    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        const button_adc_config_t ac = {
            .adc_handle   = &s_adc,          // 复用上面这一个,别让组件自建
            .unit_id      = BSP_BTN_ADC_UNIT,
            .adc_channel  = BSP_BTN_ADC_CHANNEL,
            .button_index = i,
            .min          = BTN_MV[i][0],
            .max          = BTN_MV[i][1],
        };
        // OK 长按 = 手动锁定息屏(0.5s 判定;与 UP 的 0.4s 区分,避免误触):
        // 锁定态再长按 OK 解锁。启用后进入长按态松开的 OK 只报
        // BUTTON_LONG_PRESS_UP 而不报 SINGLE_CLICK —— 单击语义(HOME 进
        // READY / APPROVAL 批准)只覆盖 <0.5s 的短按,长按不再误触发。
        // UP 键(音量加)长按 = 说话(PTT 已从 OK 移师 UP 键):0.4s 判定,
        // 长按态松开上报 BUTTON_LONG_PRESS_UP → BSP_BTN_LONG_UP 带出"说话结束"。
        button_config_t bc = { 0 };
        if (i == BSP_BTN_OK) bc.long_press_time = 500;      // 0.5s 判定锁定(与 UP 区分)
        // 0.4s 判定长按说话(2026-08-28 用户指定,由 300 上调)。取值就是双击
        // 与说话在同一颗键上的分界线:用户自己的轻点实测 285~300ms,300ms 阈值
        // 只剩 5~15ms 余量,一旦被判成长按 iot_button 就再也不报 CLICK/DOUBLE,
        // 清空直接消失、那一下还变成录音。400ms 给到 100~115ms 余量。
        if (i == BSP_BTN_UP) bc.long_press_time = 400;
        esp_err_t e = iot_button_new_adc_device(&bc, &ac, &s_btn[i]);
        if (e != ESP_OK || !s_btn[i]) {
            ESP_LOGE(TAG, "按键 %d 创建失败 (%s) —— 检查 GPIO%d 的 ADC 配置与分压电阻",
                     i, esp_err_to_name(e), BSP_BTN_ADC_CHANNEL);
            return e == ESP_OK ? ESP_FAIL : e;
        }
        void *idx = (void *)(intptr_t)i;
        iot_button_register_cb(s_btn[i], BUTTON_PRESS_DOWN,      NULL, cb_press,   idx);
        iot_button_register_cb(s_btn[i], BUTTON_PRESS_UP,        NULL, cb_release, idx);
        iot_button_register_cb(s_btn[i], BUTTON_SINGLE_CLICK,    NULL, cb_click,   idx);
        iot_button_register_cb(s_btn[i], BUTTON_DOUBLE_CLICK,    NULL, cb_double,  idx);
        iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_START,NULL, cb_long,    idx);
        iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_UP,  NULL, cb_long_up,  idx);
    }

    // 通道已由组件配置好,这里只补一份校准句柄给 bsp_button_read_mv() 用。
    // 失败不致命:按键照常工作,只是读不出电压(标定分压电阻时才需要)。
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id  = BSP_BTN_ADC_UNIT,
        .chan     = BSP_BTN_ADC_CHANNEL,
        .atten    = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC 校准创建失败,Button 页将无法显示电压");
        s_cali = NULL;
    }

    ESP_LOGI(TAG, "按键就绪:ADC1_CH%d 三键分压", BSP_BTN_ADC_CHANNEL);
    return ESP_OK;
}

void bsp_button_suppress_panel_glitch(void) {
    button_adc_set_ignore_until(esp_timer_get_time() + BSP_BTN_PANEL_GLITCH_US);
}

int bsp_button_read_mv(void) {
    // 读的是 bsp_button_init() 建好、并与 iot_button 共用的那一路 ADC。
    // 单次采样与组件的按键轮询互不干扰(oneshot 内部自带锁)。
    if (!s_adc || !s_cali) return -1;

    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}
