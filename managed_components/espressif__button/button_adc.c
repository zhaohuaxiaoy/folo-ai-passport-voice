/* SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "soc/soc_caps.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "button_adc.h"
#include "button_interface.h"

static const char *TAG = "adc_button";

#define DEFAULT_VREF    1100
#define NO_OF_SAMPLES   CONFIG_ADC_BUTTON_SAMPLE_TIMES     //Multisampling

/*!< Using atten bigger than 6db by default, it will be 11db or 12db in different target */
#if SOC_ADC_ATTEN_NUM >= 4
#define DEFAULT_ADC_ATTEN         (ADC_ATTEN_DB_6 + 1)
#else
#define DEFAULT_ADC_ATTEN         ADC_ATTEN_DB_0
#endif

#ifdef SOC_ADC_RTC_MAX_BITWIDTH
#define ADC_BUTTON_WIDTH          SOC_ADC_RTC_MAX_BITWIDTH
#else
#define ADC_BUTTON_WIDTH          SOC_ADC_DIGI_MAX_BITWIDTH
#endif

#define ADC_BUTTON_CHANNEL_MAX(adc_unit)    SOC_ADC_CHANNEL_NUM(adc_unit)
#define ADC_BUTTON_ATTEN          DEFAULT_ADC_ATTEN

#define ADC_BUTTON_MAX_CHANNEL  CONFIG_ADC_BUTTON_MAX_CHANNEL
#define ADC_BUTTON_MAX_BUTTON   CONFIG_ADC_BUTTON_MAX_BUTTON_PER_CHANNEL

// ESP32C3 ADC2 it has been deprecated.
#if (SOC_ADC_PERIPH_NUM >= 2) && !CONFIG_IDF_TARGET_ESP32C3
#define ADC_UNIT_NUM 2
#else
#define ADC_UNIT_NUM 1
#endif

typedef struct {
    uint16_t min;
    uint16_t max;
} button_data_t;

typedef struct {
    uint8_t channel;
    uint8_t is_init;
    button_data_t btns[ADC_BUTTON_MAX_BUTTON];  /* all button on the channel */
    uint64_t last_time;  /* the last time of adc sample */
} btn_adc_channel_t;

typedef enum {
    ADC_NONE_INIT = 0,
    ADC_INIT_BY_ADC_BUTTON,
    ADC_INIT_BY_USER,
} adc_init_info_t;

typedef struct {
    adc_init_info_t is_configured;
    adc_cali_handle_t adc_cali_handle;
    adc_oneshot_unit_handle_t adc_handle;
    btn_adc_channel_t ch[ADC_BUTTON_MAX_CHANNEL];
    uint8_t ch_num;
} btn_adc_unit_t;

typedef struct {
    btn_adc_unit_t unit[ADC_UNIT_NUM];
} button_adc_t;
typedef struct {
    button_driver_t base;
    adc_unit_t unit_id;
    uint32_t ch;
    uint32_t index;
} button_adc_obj;

static button_adc_t g_button = {0};

static int find_unused_channel(adc_unit_t unit_id)
{
    for (size_t i = 0; i < ADC_BUTTON_MAX_CHANNEL; i++) {
        if (0 == g_button.unit[unit_id].ch[i].is_init) {
            return i;
        }
    }
    return -1;
}

static int find_channel(adc_unit_t unit_id, uint8_t channel)
{
    for (size_t i = 0; i < ADC_BUTTON_MAX_CHANNEL; i++) {
        if (channel == g_button.unit[unit_id].ch[i].channel) {
            return i;
        }
    }
    return -1;
}
static bool adc_calibration_init(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BUTTON_WIDTH,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BUTTON_WIDTH,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "ADC calibration is not supported, skip software calibration");
    } else {
        ESP_LOGE(TAG, "ADC calibration init failed: %s", esp_err_to_name(ret));
    }

    return calibrated;
}

static bool adc_calibration_deinit(adc_cali_handle_t handle)
{
    if (handle == NULL) {
        return true;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (adc_cali_delete_scheme_curve_fitting(handle) == ESP_OK) {
        return true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (adc_cali_delete_scheme_line_fitting(handle) == ESP_OK) {
        return true;
    }
#endif

    return false;
}

esp_err_t button_adc_del(button_driver_t *button_driver)
{
    button_adc_obj *adc_btn = __containerof(button_driver, button_adc_obj, base);
    ESP_RETURN_ON_FALSE(adc_btn->ch < ADC_BUTTON_CHANNEL_MAX(adc_btn->unit_id), ESP_ERR_INVALID_ARG, TAG, "channel out of range");
    ESP_RETURN_ON_FALSE(adc_btn->index < ADC_BUTTON_MAX_BUTTON, ESP_ERR_INVALID_ARG, TAG, "button_index out of range");

    int ch_index = find_channel(adc_btn->unit_id, adc_btn->ch);
    ESP_RETURN_ON_FALSE(ch_index >= 0, ESP_ERR_INVALID_ARG, TAG, "can't find the channel");

    g_button.unit[adc_btn->unit_id].ch[ch_index].btns[adc_btn->index].max = 0;
    g_button.unit[adc_btn->unit_id].ch[ch_index].btns[adc_btn->index].min = 0;

    /** check button usage on the channel*/
    uint8_t unused_button = 0;
    for (size_t i = 0; i < ADC_BUTTON_MAX_BUTTON; i++) {
        if (0 == g_button.unit[adc_btn->unit_id].ch[ch_index].btns[i].max) {
            unused_button++;
        }
    }
    if (unused_button == ADC_BUTTON_MAX_BUTTON && g_button.unit[adc_btn->unit_id].ch[ch_index].is_init) {  /**< if all button is unused, deinit the channel */
        g_button.unit[adc_btn->unit_id].ch[ch_index].is_init = 0;
        g_button.unit[adc_btn->unit_id].ch[ch_index].channel = ADC_BUTTON_CHANNEL_MAX(adc_btn->unit_id);
        ESP_LOGD(TAG, "all button is unused on channel%d, deinit the channel", g_button.unit[adc_btn->unit_id].ch[ch_index].channel);
    }

    /** check channel usage on the adc*/
    uint8_t unused_ch = 0;
    for (size_t i = 0; i < ADC_BUTTON_MAX_CHANNEL; i++) {
        if (0 == g_button.unit[adc_btn->unit_id].ch[i].is_init) {
            unused_ch++;
        }
    }
    if (unused_ch == ADC_BUTTON_MAX_CHANNEL && g_button.unit[adc_btn->unit_id].is_configured) { /**< if all channel is unused, deinit the adc */
        if (g_button.unit[adc_btn->unit_id].is_configured == ADC_INIT_BY_ADC_BUTTON) {
            esp_err_t ret = adc_oneshot_del_unit(g_button.unit[adc_btn->unit_id].adc_handle);
            ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "adc oneshot del unit fail");
            adc_calibration_deinit(g_button.unit[adc_btn->unit_id].adc_cali_handle);
        }

        g_button.unit[adc_btn->unit_id].is_configured = ADC_NONE_INIT;
        memset(&g_button.unit[adc_btn->unit_id], 0, sizeof(btn_adc_unit_t));
        ESP_LOGD(TAG, "all channel is unused, , deinit adc");
    }
    free(adc_btn);

    return ESP_OK;
}

// 手改补丁(AI Passport 2026-08-27): 读取失败必须返回上次有效电压,不能返回 0;
// 松开态→低压突变时重读一次,防 BLE 射频窗把按键腐蚀成 0mV 假按下。
// 背景: ESP32-C3 的 SAR ADC 在 RTC 域,与 BLE 射频活动(连接事件 ~1.6ms 窗/
// 15ms 周期)冲突时转换出错:adc_oneshot_read 成功但 raw≈0 → 电压 0mV。若某
// 按键电压窗口从 0 开始(本项目 UP 键 = {0,150}mV),0mV 被误判"按下",射频
// 活跃期产生假按键/假长按(真机实测: 连接后 130s 内自动触发 19 次 PTT 录音)。
// 修复一: 单次读失败跳过该样本(只平均成功样本);全部失败则沿用上次有效电压,
// 宁可漏检一次真按键,也不能凭空按下。
// 修复二(条件重读): 腐蚀读数(0-5mV)与真实按压(UP≈3-5mV)同处低压,单次
// 读数不可区分;但腐蚀是射频窗内的瞬态,重读即恢复 2890mV。
// 只在"上次读数高(松开态) → 本次突变低压"时重读:真实按压的后续 tick 上次
// 读数已低,不触发重读,长按判定节奏不变;重读用 vTaskDelay 让出 CPU(实测
// busy-wait 版把单核占满,系统卡顿)。
// 升级(2026-08-28 真机取证): 单次重读(2ms)不够 —— BLE PHY 温度补偿校准
// 每 ~4s 无条件执行,射频窗持续 ~35-50ms,恰好盖过 debounce 的 7×5ms 连续
// 低压判定:校准期 ADC 全读成 ~0mV → UP 键(窗口 {0,150})假按下 → 300ms 后
// 假 LONG → PTT 开录 → 校准结束恢复 2890 → 假 LONG_UP 停录,循环往复。
// KEYDBG 取证: 假按判定瞬间 bsp_button_read_mv()=3mV,其余时刻稳定 2890;
// 假按全部落在 btn=0(UP),间隔 ~4.1-4.6s,无连接也触发(校准与连接无关)。
// 因此改为"持续重读":低压后每 2ms 重读,直到恢复松开态或超 RF_GUARD_MAX_RETRY_MS。
// 校准窗结束即恢复 → 该 tick 判高 → debounce 清零,假按下无法累积;真实按压
// 超上限仍低 → 按按压判定(首 tick 延迟最多 100ms,长按 300ms 阈值从按下起算,
// 物理按住 ~400ms 报 LONG,无感)。重读期间占住 button 定时器任务 ≤100ms,
// 三键共享,首键重读时其余键判定顺延,可接受。
// 修复三(2026-08-28 真机"一次长按响两次"取证): 上述防护只覆盖松开→低压
// (防假按下)。按住→高压突变(假释放)不受防护: 长按期间 ADC 闪断为松开
// 电平 → iot_button 判假松开(LONG_UP 停录)→ 恢复低压 → 重新按下 →
// 300ms 后第二次 LONG(第二声滴)。用户实测: 一次长按(手未松)连续两声滴
// <1 秒,与 35ms 去抖 + 300ms 重按合成间隔吻合;环取证 LONG_UP 时刻 mv
// =2890(未松手却读成松开)。对称防护: 按住→高压同样持续重读,恢复低压
// 即按按住处理;真实松开首样本即确认,延迟 ~2ms,节奏不变。
// 抗腐蚀另一主力: sdkconfig CONFIG_BUTTON_DEBOUNCE_TICKS=7(35ms 连续低才
// 判定按下)。SAMPLE_TIMES 保持 1(多采平均会把单样本腐蚀折算成 1445mV,
// 恰好落进 OK 键窗口,已实测废弃)。
// 组件升版会覆盖此补丁,须同步移植。
#define RF_GUARD_LOW_MV   150    // 低压怀疑线上沿(UP 窗 {0,150};腐蚀/按压都低于此)
#define RF_GUARD_HIGH_MV  2000   // 松开态下限(实测 2890;高于此必为真实释放)
#define RF_GUARD_RETRY_MS 2      // 重读间隔 > 最长射频窗(~1.6ms@1M / ~1ms@2M PHY)
#define RF_GUARD_MAX_RETRY_MS 100 // 重读上限:覆盖 phy 温度校准窗(~35-50ms);超上限仍低=真实按压
static uint32_t s_last_good_voltage;   // 上次有效读数(RAW);全失败时沿用
static uint32_t s_last_mv = 3000;      // 上次返回电压(松开态初始,首读低压即重读)

// 补丁 2026-08-28(面板上电瞬态): 解锁/唤醒触发 PANEL_ON 后,面板 SLPOUT+
// DISPON+LVGL 刷新期 SPI 活动窗与 ADC1_CH0 硬件耦合,ADC 持续读 ~0mV →
// UP 假按 → 300ms 假 LONG → PTT 开录(真机 KEYDBG 取证: 解锁后 25963ms
// UP PRESS_DOWN mv=598,面板稳定后 27098ms 恢复,PTT 自动停)。
// bsp_display_power(true) 入口调用 button_adc_set_ignore_until() 设置窗口;
// 窗口内 get_key_level 直接返回 INACTIVE —— 状态机当"松开"处理,不吞回调、
// 不污染 LONG 态(组件升版会覆盖此补丁,须同步移植)。
static int64_t s_ignore_until_us;
void button_adc_set_ignore_until(int64_t until_us) { s_ignore_until_us = until_us; }

static uint32_t get_adc_voltage(adc_unit_t unit_id, uint8_t channel)
{
    uint32_t adc_reading = 0;
    int adc_raw = 0;
    int ok = 0;
    for (int i = 0; i < NO_OF_SAMPLES; i++) {
        if (adc_oneshot_read(g_button.unit[unit_id].adc_handle, channel, &adc_raw) == ESP_OK) {
            adc_reading += adc_raw;
            ok++;
        }
    }
    if (ok > 0) {
        adc_reading /= ok;
        s_last_good_voltage = adc_reading;
    } else {
        adc_reading = s_last_good_voltage;   // 全失败: 沿用上次有效值
    }
    //Convert adc_reading to voltage in mV
    int voltage = 0;
    if (g_button.unit[unit_id].adc_cali_handle != NULL) {
        esp_err_t ret = adc_cali_raw_to_voltage(g_button.unit[unit_id].adc_cali_handle, adc_reading, &voltage);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC calibration conversion failed: %s", esp_err_to_name(ret));
            return s_last_good_voltage;   // 同补丁: 失败沿用上次有效值,不回 0
        }
    } else {
#if CONFIG_IDF_TARGET_ESP32S31
        // Keep the offset correction in signed arithmetic to avoid unsigned underflow near zero.
        voltage = (int32_t)adc_reading * 4 * 1000 / 4393 - 2;
        if (voltage < 0) {
            voltage = 0;
        }
#else
        // No calibration handle means the chip does not support software calibration; return 0 to indicate that the voltage value is not available.
        voltage = 0;
#endif
    }

    // 修复二: 松开态→低压突变,持续重读直到恢复或超上限(见文件头注释)。
    // 修复三(2026-08-28 真机"一次长按响两次"取证): 按住态→高压突变同样
    // 重读 —— 否则 ADC 闪断为松开电平 → iot_button 判假松开(LONG_UP 停录)
    // → 恢复低压 → 重新按下 → 300ms 后第二次 LONG(第二声滴)。两次 LONG
    // 最小间隔 ≈ debounce 35ms + 重按 300ms ≈ 0.34s,与用户描述"连续滴-滴
    // (<1 秒)"吻合;现场 LONG_UP 时刻 mv=2890(用户未松手却读成松开)。
    // 真实松开/按压: 重读首样本即确认,仅延迟 ~2ms,节奏不变。
    const bool dip    = (uint32_t)voltage < RF_GUARD_LOW_MV
                        && s_last_mv >= RF_GUARD_HIGH_MV;   // 松开→低:防假按下
    const bool spike  = (uint32_t)voltage >= RF_GUARD_HIGH_MV
                        && s_last_mv < RF_GUARD_LOW_MV;     // 按住→高:防假释放
    if (dip || spike) {
        const uint64_t retry_t0 = esp_timer_get_time();
        int low_confirm = 0;   // spike 方向连续低(恢复按住)确认计数
        while (esp_timer_get_time() - retry_t0 < (uint64_t)RF_GUARD_MAX_RETRY_MS * 1000) {
            vTaskDelay(pdMS_TO_TICKS(RF_GUARD_RETRY_MS));   // 让出 CPU,等射频窗过去
            uint32_t re_reading = 0;
            int re_raw = 0;
            ok = 0;
            for (int i = 0; i < NO_OF_SAMPLES; i++) {
                if (adc_oneshot_read(g_button.unit[unit_id].adc_handle, channel, &re_raw) == ESP_OK) {
                    re_reading += re_raw;
                    ok++;
                }
            }
            if (ok > 0) {
                re_reading /= ok;
                s_last_good_voltage = re_reading;
                if (g_button.unit[unit_id].adc_cali_handle != NULL) {
                    int re_voltage = 0;
                    if (adc_cali_raw_to_voltage(g_button.unit[unit_id].adc_cali_handle,
                                                re_reading, &re_voltage) == ESP_OK) {
                        voltage = re_voltage;
                        if (dip && (uint32_t)voltage >= RF_GUARD_HIGH_MV) break;  // 恢复松开
                        if (spike) {
                            // 修复四(2026-08-28 松开假按取证):spike 方向"恢复
                            // 按住"必须连续 2 次低才确认 —— 单次 <150 会被射频
                            // 腐蚀窗(采样读 0mV)误触发:松开态 s_last_mv 残留 0
                            // (上次 dip 超时判按压退出)时,每 tick 采样 2890 →
                            // spike → 重读撞腐蚀窗读 0 → 单次 break 判"恢复按住"
                            // → iot_button 判 ACTIVE → 假 PRESS/LONG 风暴(环取证
                            // 每会话后 0.14-1.6s 假按,回调 mv 全 2890)。真实按住
                            // 是持续低压(可跨多个 2ms 重读),腐蚀是瞬态(单次)。
                            // 一次高(2890)即重置计数 —— 真实松开不会误判按住。
                            if ((uint32_t)voltage < RF_GUARD_LOW_MV) {
                                if (++low_confirm >= 2) break;   // 恢复按住
                            } else {
                                low_confirm = 0;
                            }
                        }
                    }
                }
            }
        }
        // 超上限退出: dip   → 持续低压 = 真实按压(保持低压,判定按下);
        //             spike → 持续高压 = 真实松开(保持高压,判定松开)。
    }
    s_last_mv = (uint32_t)voltage;

    ESP_LOGD(TAG, "Raw: %"PRIu32"\tVoltage: %dmV", adc_reading, voltage);
    return (uint32_t)voltage;
}

uint8_t button_adc_get_key_level(button_driver_t *button_driver)
{
    button_adc_obj *adc_btn = __containerof(button_driver, button_adc_obj, base);
    static uint16_t vol = 0;
    uint32_t ch = adc_btn->ch;
    uint32_t index = adc_btn->index;
    ESP_RETURN_ON_FALSE(ch < ADC_BUTTON_CHANNEL_MAX(adc_btn->unit_id), 0, TAG, "channel out of range");
    ESP_RETURN_ON_FALSE(index < ADC_BUTTON_MAX_BUTTON, 0, TAG, "button_index out of range");

    int ch_index = find_channel(adc_btn->unit_id, ch);
    ESP_RETURN_ON_FALSE(ch_index >= 0, 0, TAG, "The button_index is not init");

    // 面板上电 SPI 窗(见文件头 button_adc_set_ignore_until 注释):判定直接
    // 返回 INACTIVE,ADC 读数在窗内不可信。
    if (esp_timer_get_time() < s_ignore_until_us) {
        return BUTTON_INACTIVE;
    }

    /** It starts only when the elapsed time is more than 1ms */
    if ((esp_timer_get_time() - g_button.unit[adc_btn->unit_id].ch[ch_index].last_time) > 1000) {
        vol = get_adc_voltage(adc_btn->unit_id, ch);
        g_button.unit[adc_btn->unit_id].ch[ch_index].last_time = esp_timer_get_time();
    }

    if (vol <= g_button.unit[adc_btn->unit_id].ch[ch_index].btns[index].max &&
            vol >= g_button.unit[adc_btn->unit_id].ch[ch_index].btns[index].min) {
        return BUTTON_ACTIVE;
    }
    return BUTTON_INACTIVE;
}

esp_err_t iot_button_new_adc_device(const button_config_t *button_config, const button_adc_config_t *adc_config, button_handle_t *ret_button)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(button_config && adc_config && ret_button, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");
    ESP_RETURN_ON_FALSE(adc_config->unit_id < ADC_UNIT_NUM, ESP_ERR_INVALID_ARG, TAG, "adc_handle out of range");
    ESP_RETURN_ON_FALSE(adc_config->adc_channel < ADC_BUTTON_CHANNEL_MAX(adc_config->unit_id), ESP_ERR_INVALID_ARG, TAG, "channel out of range");
    ESP_RETURN_ON_FALSE(adc_config->button_index < ADC_BUTTON_MAX_BUTTON, ESP_ERR_INVALID_ARG, TAG, "button_index out of range");
    ESP_RETURN_ON_FALSE(adc_config->max > 0, ESP_ERR_INVALID_ARG, TAG, "key max voltage invalid");
    button_adc_obj *adc_btn = calloc(1, sizeof(button_adc_obj));
    ESP_RETURN_ON_FALSE(adc_btn, ESP_ERR_NO_MEM, TAG, "calloc fail");
    adc_btn->unit_id = adc_config->unit_id;

    int ch_index = find_channel(adc_btn->unit_id, adc_config->adc_channel);
    if (ch_index >= 0) { /**< the channel has been initialized */
        ESP_GOTO_ON_FALSE(g_button.unit[adc_btn->unit_id].ch[ch_index].btns[adc_config->button_index].max == 0, ESP_ERR_INVALID_STATE, err, TAG, "The button_index has been used");
    } else { /**< this is a new channel */
        int unused_ch_index = find_unused_channel(adc_config->unit_id);
        ESP_GOTO_ON_FALSE(unused_ch_index >= 0, ESP_ERR_INVALID_STATE, err, TAG, "exceed max channel number, can't create a new channel");
        ch_index = unused_ch_index;
    }

    /** initialize adc */
    if (0 == g_button.unit[adc_btn->unit_id].is_configured) {
        esp_err_t ret;
        if (NULL == adc_config->adc_handle) {
            //ADC1 Init
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = adc_btn->unit_id,
            };
            ret = adc_oneshot_new_unit(&init_config, &g_button.unit[adc_btn->unit_id].adc_handle);
            ESP_GOTO_ON_FALSE(ret == ESP_OK, ESP_FAIL, err, TAG, "adc oneshot new unit fail!");
            g_button.unit[adc_btn->unit_id].is_configured = ADC_INIT_BY_ADC_BUTTON;
        } else {
            g_button.unit[adc_btn->unit_id].adc_handle = *adc_config->adc_handle;
            ESP_LOGI(TAG, "ADC1 has been initialized");
            g_button.unit[adc_btn->unit_id].is_configured = ADC_INIT_BY_USER;
        }

    }

    /** initialize adc channel */
    if (0 == g_button.unit[adc_btn->unit_id].ch[ch_index].is_init) {
        //ADC1 Config
        adc_oneshot_chan_cfg_t oneshot_config = {
            .bitwidth = ADC_BUTTON_WIDTH,
            .atten = ADC_BUTTON_ATTEN,
        };
        esp_err_t ret = adc_oneshot_config_channel(g_button.unit[adc_btn->unit_id].adc_handle, adc_config->adc_channel, &oneshot_config);
        ESP_GOTO_ON_FALSE(ret == ESP_OK, ESP_FAIL, err, TAG, "adc oneshot config channel fail!");
        //-------------ADC1 Calibration Init---------------//
        adc_calibration_init(adc_btn->unit_id, ADC_BUTTON_ATTEN, &g_button.unit[adc_btn->unit_id].adc_cali_handle);
        g_button.unit[adc_btn->unit_id].ch[ch_index].channel = adc_config->adc_channel;
        g_button.unit[adc_btn->unit_id].ch[ch_index].is_init = 1;
        g_button.unit[adc_btn->unit_id].ch[ch_index].last_time = 0;
    }
    g_button.unit[adc_btn->unit_id].ch[ch_index].btns[adc_config->button_index].max = adc_config->max;
    g_button.unit[adc_btn->unit_id].ch[ch_index].btns[adc_config->button_index].min = adc_config->min;
    g_button.unit[adc_btn->unit_id].ch_num++;

    adc_btn->ch = adc_config->adc_channel;
    adc_btn->index = adc_config->button_index;
    adc_btn->base.get_key_level = button_adc_get_key_level;
    adc_btn->base.del = button_adc_del;
    ret = iot_button_create(button_config, &adc_btn->base, ret_button);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ESP_FAIL, err, TAG, "Create button failed");

    return ESP_OK;
err:
    if (adc_btn) {
        free(adc_btn);
    }
    return ret;
}
