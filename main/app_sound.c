// main/app_sound.c —— 提示音实现。
// 每个音由一个或多个方波段组成(频率×时长),段间无缝衔接。
// 全部走 16kHz/16bit/单声道 —— 与录音流同格式,bsp_audio_set_format 只调一次。
#include "app_sound.h"
#include "app_events.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_sound";

#define SAMPLE_RATE 16000
#define CHUNK_SAMPLES 256   // 每块 ~16ms,控制栈上临时缓冲
#define AMPLITUDE 5000      // 方波幅度(±),低于 demo 的 6000 稍柔和

// ---- 音色表:各音 = 若干 {频率Hz, 时长ms} 段 ----
typedef struct { uint16_t hz; uint16_t ms; } tone_seg_t;

static const tone_seg_t TONE_TABLE[APP_TONE_COUNT][3] = {
    [APP_TONE_START]    = { { 440, 80 },  { 0, 0 }, { 0, 0 } },  // 录音就绪
    [APP_TONE_SEND]     = { { 880, 60 },  { 0, 0 }, { 0, 0 } },  // 语音已发送
    [APP_TONE_APPROVAL] = { { 587, 75 },  { 880, 75 }, { 0, 0 } }, // 审批提醒(双音)
    [APP_TONE_SUCCESS]  = { { 660, 100 }, { 880, 100 }, { 0, 0 } }, // 完成(上行音)
    [APP_TONE_REJECT]   = { { 220, 120 }, { 0, 0 }, { 0, 0 } },  // 拒绝
    [APP_TONE_ERROR]    = { { 180, 100 }, { 0, 0 }, { 0, 0 } },  // 离线/非法
};

// 播放一个段:方波整块生成 + 阻塞写 I2S。
static void play_segment(uint16_t hz, uint16_t ms) {
    int16_t buf[CHUNK_SAMPLES];
    const int period = SAMPLE_RATE / hz;         // 每周期采样数
    const int half = period / 2;
    int total = (int)SAMPLE_RATE * ms / 1000;
    int phase = 0;
    while (total > 0) {
        int n = total < CHUNK_SAMPLES ? total : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            buf[i] = (phase < half) ? AMPLITUDE : -AMPLITUDE;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "bsp_audio_write 失败");
            return;
        }
        total -= n;
    }
}

static void play_tone_impl(app_tone_t tone) {
    if (tone >= APP_TONE_COUNT) return;
    const tone_seg_t (*segs)[3] = &TONE_TABLE[tone];
    for (int i = 0; i < 3; i++) {
        if ((*segs)[i].hz == 0) break;
        play_segment((*segs)[i].hz, (*segs)[i].ms);
    }
}

// ---- 异步播放:静态队列 + worker ----
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[APP_TONE_COUNT * sizeof(uint8_t)];
static QueueHandle_t s_queue;

static void sound_worker(void *arg) {
    (void)arg;
    uint8_t tone;
    for (;;) {
        if (xQueueReceive(s_queue, &tone, portMAX_DELAY) == pdTRUE) {
            play_tone_impl((app_tone_t)tone);
            // START 音播完 → 通知 app_task 开流(分时语义:滴声先于采集)。
            // 其余音不需要,不产生事件。
            if (tone == APP_TONE_START) {
                app_event_t ev = { .type = APP_EV_TONE_DONE };
                app_event_post(&ev);
            }
        }
    }
}

esp_err_t app_sound_init(void) {
    esp_err_t e = bsp_audio_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init 失败: %s", esp_err_to_name(e));
        return e;
    }
    e = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_set_format 失败: %s", esp_err_to_name(e));
        return e;
    }
    bsp_audio_set_volume(80);

    s_queue = xQueueCreateStatic(APP_TONE_COUNT, sizeof(uint8_t),
                                 s_queue_storage, &s_queue_struct);
    if (!s_queue) return ESP_FAIL;
    // 静态栈(.bss):1536B 够提示音(单段 256 采样 buf);heap 极限下每字节
    // 都决定 host 任务(5120B 动态创建)能否成功。
    static StackType_t s_snd_stack[1536 / sizeof(StackType_t)];
    static StaticTask_t s_snd_tcb;
    if (!xTaskCreateStatic(sound_worker, "sound_worker", 1536, NULL, 3,
                           s_snd_stack, &s_snd_tcb)) {
        ESP_LOGE(TAG, "sound worker 创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "提示音就绪");
    return ESP_OK;
}

bool app_sound_play(app_tone_t tone) {
    if (xQueueSend(s_queue, &tone, 0) != pdTRUE) {
        ESP_LOGW(TAG, "提示音队列满,丢弃 %d", (int)tone);
        return false;
    }
    return true;
}

void app_sound_play_sync(app_tone_t tone) {
    play_tone_impl(tone);
}
