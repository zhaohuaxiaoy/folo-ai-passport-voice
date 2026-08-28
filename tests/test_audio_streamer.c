// tests/test_audio_streamer.c —— 音频流管线宿主测试(fake RTOS 层,真并发线程)。
// 存在的意义(外部 Review 教训):此前 host 测试不编译 audio_streamer.c,导致
// 调用了 ESP-IDF 不存在的 API(xRingbufferReset)未被捕获。本测试把 audio_streamer.c
// 连同 fake ringbuf/semphr/task 一起编译——编译期即可拦截"不存在的 ESP-IDF API"
// 类回归,运行期覆盖会话 token 的残留失效/直通语义与 cancel 的幂等/快速返回。
// 编译宏:WORKER_EXIT_TIMEOUT_MS=200(由 CMake 注入,保持显式;等待为信号量
// 事件驱动,fake 层是真实 pthread 信号量)。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "adpcm.h"
#include "app_types.h"
#include "audio_streamer.h"
#include "esp_err.h"

// fake_rtos.c 提供的可控钩子
extern int g_notify_count;       // 到达发送层的帧数(cancel/token 丢弃的不计)
extern int g_notify_rc;          // 发送桩返回码(非 0 → 丢帧计数路径)
extern int g_notify_block_ms;    // 发送桩阻塞毫秒(模拟在途)
extern int g_notify_min_len;     // 发送桩帧长范围(min/max 相等 ⇔ 无残片泄漏)
extern int g_notify_max_len;
extern uint8_t g_notify_last[];  // 最后一帧副本(压缩路径解码校验)
extern int g_notify_last_len;
extern int g_fake_audio_fail;    // 非 0 → bsp_audio_read 报错
extern int g_fake_ring_fail;     // 非 0 → xRingbufferCreateStatic 失败(init 失败注入)
extern int g_fake_ring_residue;  // 非 0 → 环创建时预置短残片(长度异常闸门)
extern uint32_t g_fake_ring_residue_token;  // 非 0 → 环创建时预置旧 token item(token 闸门)
extern int g_fake_sem_fail;      // 非 0 → xSemaphoreCreateBinary 失败(init 失败注入)
extern int g_fake_task_fail_at;  // N>0 → 第 N 次 xTaskCreate 失败(1=audio, 2=ble 回滚)
extern int g_sem_live;           // 存活信号量计数(断言 init 失败路径零泄漏)
extern app_event_t g_events[];
extern int g_event_count;
extern void fake_reset(void);
extern int link_send_audio(const uint8_t *frame, size_t len);   // fake 发送桩

// 一块 = 100ms @16kHz/16bit 单声道(audio_streamer.c 的 CHUNK_BYTES,不外露)
#define CHUNK_BYTES_EXPECT 3200

static int s_inited = 0;
static void ensure_init(void) {
    if (!s_inited) {
        assert(audio_streamer_init() == ESP_OK);
        audio_streamer_set_sender(link_send_audio);   // 注册发送桩(默认 NULL 会全丢)
        s_inited = 1;
    }
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 等到发送层至少收到 n 帧(上限 ms 毫秒)。固定 usleep 后直接断言会在宿主
// 调度抖动下偶发失败(实测 10 次有 1~2 次):worker 线程刚建好、还没跑到第一次
// notify,50ms 窗口就到点了。语义不变(仍是"应当很快发出"),只是不再赌调度。
static int wait_notify(int n, int ms) {
    for (int waited = 0; waited < ms; waited += 2) {
        if (g_notify_count >= n) return 1;
        usleep(2 * 1000);
    }
    return g_notify_count >= n;
}

// 有界等待丢帧计数出现(take_drops 会清零,故累加)。
static uint32_t wait_drops(int ms) {
    uint32_t got = 0;
    for (int waited = 0; waited < ms; waited += 5) {
        got += audio_streamer_take_drops();
        if (got > 0) break;
        usleep(5 * 1000);
    }
    return got;
}

static int event_seen(app_event_type_t t) {
    for (int i = 0; i < g_event_count; i++) {
        if (g_events[i].type == t) return 1;
    }
    return 0;
}

// 0) init 失败后公开 API 空转保护(审查 P2):环创建失败 / 信号量创建失败 →
//    init 返回失败,主流程继续;此后 start/cancel/drain 必须空转不崩溃
//    (不访问 NULL ring/sem),且不投递任何事件。复核 R1 后:失败路径释放
//    已创建信号量(g_sem_live 归零,零泄漏)。
static void test_init_failure_api_noop(void) {
    fake_reset();
    g_fake_ring_fail = 1;                     // 第一个失败点:环创建
    assert(audio_streamer_init() != ESP_OK);  // 主流程"继续运行"场景
    g_fake_ring_fail = 0;
    assert(audio_streamer_start() != ESP_OK); // 空转:返回 INVALID_STATE 而非崩溃
    audio_streamer_cancel();                  // 空转:不 xRingbufferGetCurFreeSize(NULL)
    audio_streamer_drain(50);                 // 空转:同上
    assert(!audio_streamer_active());
    assert(g_event_count == 0);
    assert(g_sem_live == 0);                  // R1:无信号量泄漏

    fake_reset();
    g_fake_sem_fail = 1;                      // 环已建,信号量失败
    assert(audio_streamer_init() != ESP_OK);
    g_fake_sem_fail = 0;
    audio_streamer_start();
    audio_streamer_cancel();
    assert(!audio_streamer_active());
    assert(g_event_count == 0);
    assert(g_sem_live == 0);                  // R1:无信号量泄漏
}

// 1) worker 创建失败(复核 R1):audio_worker 失败 / ble_worker 失败(回滚路径)
//    → init 返回失败,已建信号量全部释放(g_sem_live 归零),API 空转。
//    泄漏的 audio_worker 线程(回滚 vTaskDelete 宿主桩为空实现)永久阻塞在
//    旧信号量 cond wait,无唤醒者,不碰全局状态 —— 宿主进程级回收,可接受。
//    失败后的"重试成功"由 test_token_drops_stale 覆盖(本用例之后无成功
//    init,它做首次成功 init —— 语义:失败序列后管线从干净状态重建)。
static void test_worker_fail_cleanup(void) {
    fake_reset();
    g_fake_task_fail_at = 1;                  // 第 1 次创建(audio_worker)失败
    assert(audio_streamer_init() != ESP_OK);
    g_fake_task_fail_at = 0;
    audio_streamer_start();
    audio_streamer_cancel();
    assert(!audio_streamer_active());
    assert(g_event_count == 0);
    assert(g_sem_live == 0);                  // R1:信号量零泄漏

    fake_reset();
    g_fake_task_fail_at = 2;                  // 第 2 次创建(ble_worker)失败 → 回滚 audio
    assert(audio_streamer_init() != ESP_OK);
    g_fake_task_fail_at = 0;
    audio_streamer_start();
    audio_streamer_cancel();
    audio_streamer_drain(50);
    assert(!audio_streamer_active());
    assert(g_event_count == 0);
    assert(g_sem_live == 0);                  // R1:回滚路径信号量零泄漏
}

// 2) 残留注入(双闸门,首测必跑在首次成功 init 处:成功 init 后幂等早返回,
//    注入不再生效):g_fake_ring_residue=64 长度异常残片 + g_fake_ring_residue_token
//    完整旧 token item。token 方案核心 —— start 永不因残留拒绝,注入残留后
//    一次即成功(旧实现返回 ESP_ERR_TIMEOUT 拒绝启动)。
static void test_token_drops_stale(void) {
    fake_reset();
    g_fake_ring_residue = 64;                 // 长度异常残片(长度闸门,丢弃留证)
    g_fake_ring_residue_token = 1;            // 旧 token item(长度合法,token 闸门静默丢弃)
    assert(audio_streamer_init() == ESP_OK);  // 首次成功 init(注入生效)
    audio_streamer_set_sender(link_send_audio);
    // 核心断言:残留不构成拒绝条件 —— 一次即成功,无重试
    assert(audio_streamer_start() == ESP_OK);
    assert(audio_streamer_active());
    // 残片在 init 后即被 ble_worker 处理(worker 常驻,不 gate by active);
    // start 后若旧 item 泄漏(无 token 闸门),首个 receive 即会发送 → count≥1。
    // token 闸门正确时 count 保持 0(新会话首帧 ~100ms 后才到)。
    assert(g_notify_count == 0);
    assert(wait_notify(1, 500));
    // R1.6:64B 残片绝不进发送层(长度闸门;若被当定长块解读会以垃圾帧
    // 发出 → min<max;载荷恒 3200B 完整块)
    assert(g_notify_min_len == g_notify_max_len);
    assert(g_notify_min_len == CHUNK_BYTES_EXPECT);
    // 长度异常留证(DROP_START 事件由 64B 残片触发);token 丢弃静默
    // (不计 drop 不投事件 —— 与 cancel 丢弃的语义一致,见 test_drop_accounting)
    assert(event_seen(APP_EV_AUDIO_DROP_START));
    (void)audio_streamer_take_drops();        // 清计数,不外溢到下一用例
    g_fake_ring_residue = 0;
    g_fake_ring_residue_token = 0;
    audio_streamer_stop();
}

// 3) 取消丢弃残留:第一块阻塞在 notify(在途),cancel 停采集 + token 作废;
//    在途帧完成不可撤回(±1),环内残留与迟到块靠 token 失效静默丢弃;
//    取消后立即重开(无排空门禁),新会话从零开始。
static void test_cancel_drops_remaining(void) {
    fake_reset();
    ensure_init();
    g_notify_block_ms = 150;              // 第一块进 send 桩即阻塞(模拟流控在途)
    assert(audio_streamer_start() == ESP_OK);
    usleep(50 * 1000);
    assert(g_notify_count == 1);          // 恰一帧在途(2 槽环:1 在途 + 1 环内,后续环满丢帧)
    audio_streamer_cancel();              // 停采集 + token 作废
    usleep(200 * 1000);                   // 在途 notify 完成窗口(150ms)
    // 计数在调用时即计入(含在途),完成不重复计数 —— cancel 后恒为 1:
    // 在途 1 帧(不可撤回)+ 零新增;环内残留/迟到块从未被发送
    assert(g_notify_count == 1);
    g_notify_block_ms = 0;
    assert(audio_streamer_start() == ESP_OK);   // 立即重开:无排空门禁
    assert(wait_notify(2, 500));          // 新会话帧正常(旧残留被 token 丢弃)
    audio_streamer_stop();
}

// 4) 幂等:未采集时取消无害;STOP 后(残留仍待发送)取消同样作废残留。
//    允许 ±1:cancel 置位瞬间"已通过检查正在发送"的一帧会完成(不可撤回,真实语义)。
static void test_cancel_idempotent(void) {
    fake_reset();
    ensure_init();
    audio_streamer_cancel();              // 从未 start:无害
    audio_streamer_cancel();              // 重复调用
    assert(audio_streamer_start() == ESP_OK);
    usleep(40 * 1000);
    audio_streamer_stop();                // 正常停:残留留在环内(等 voice.end 前 drain)
    int before = g_notify_count;
    audio_streamer_cancel();              // 断链兜底:残留 token 作废
    assert(g_notify_count <= before + 1); // 无新帧(在途帧最多 +1)
    assert(audio_streamer_start() == ESP_OK);   // 立即重开(旧实现可能被拒绝/需重试)
    assert(wait_notify(before + 1, 500));       // 新会话帧正常
    audio_streamer_stop();
}

// 5) cancel 快速返回:发送桩 1000ms 长在途时,cancel 只等 worker 退出
//    (≤ WORKER_EXIT_TIMEOUT_MS + 余量),不等环空、不等在途 notify
//    (旧实现等环空 → 200ms 超时后丢帧模式残留,start 被拒绝)。
//    残留与在途帧靠 token 失效:在途 1 帧完成即止,后续零发送。
static void test_cancel_returns_quickly(void) {
    fake_reset();
    ensure_init();
    g_notify_block_ms = 1000;             // 远超 WORKER_EXIT_TIMEOUT_MS
    assert(audio_streamer_start() == ESP_OK);
    usleep(50 * 1000);
    assert(g_notify_count == 1);          // 第一块阻塞在途
    int64_t t0 = now_ms();
    audio_streamer_cancel();
    assert(now_ms() - t0 <= 500);         // 快速返回:不等 1s 在途 notify
    usleep(1200 * 1000);                  // 在途 notify 完成 + 环内残留被 token 丢弃
    // 计数调用时即计入:在途 1 帧(cancel 前已计数)+ 零新增 = 恒 1
    assert(g_notify_count == 1);          // 环内残留从未被发送
    g_notify_block_ms = 0;
    assert(audio_streamer_start() == ESP_OK);   // 立即重开:无排空门禁
    assert(wait_notify(2, 500));          // 新会话正常
    audio_streamer_stop();
}

// 6) BLE 压缩路径:一块 3200B PCM → 一个 804B ADPCM block 一次发送。
//     只断言长度不够(804B 垃圾也是 804B),因此把最后一帧解回 PCM 与采集桩
//     写入的常量比对——证明块头 predictor 与码流真的对得上。
static void test_compressed_path_emits_adpcm_blocks(void) {
    fake_reset();
    ensure_init();
    // 上一用例 stop 残留(token 未变、匹配会话)可能仍被 ble_worker 处理:
    // 若在切换压缩开关前发出,会以 3200B PCM 帧污染帧长统计。确定性自净:
    // ①drain 等残留全部发完(此时 s_compressed 仍为 false,PCM 帧无所谓)
    // ②fake_reset 重置帧长统计 ③环已空,开压缩会话 —— 统计窗口内无竞态。
    audio_streamer_drain(500);            // 等上一用例残留发完(环空)
    fake_reset();                         // 重置统计:drain 期间帧不计入本用例
    audio_streamer_set_compressed(true);
    assert(audio_streamer_start() == ESP_OK);
    assert(wait_notify(2, 500));      // 至少两帧:证明连续编码,不只首帧
    audio_streamer_stop();
    assert(g_notify_min_len == ADPCM_BLOCK_BYTES);   // 每帧都是完整 block
    assert(g_notify_max_len == ADPCM_BLOCK_BYTES);
    // 不断言零丢帧:fake 采集桩不限速,环满丢帧是宿主固有现象。
    // 关键是"到达发送层的每一帧都是完整 block",丢帧只清计数不外溢到下一用例。
    (void)audio_streamer_take_drops();

    // 解码最后一帧:桩用 0x40+seq 的常量填充,PCM 样本为同一个定值
    static int16_t pcm[ADPCM_BLOCK_SAMPLES];
    assert(g_notify_last_len == ADPCM_BLOCK_BYTES);
    assert(adpcm_decode_block(g_notify_last, (size_t)g_notify_last_len,
                              ADPCM_BLOCK_SAMPLES, pcm, ADPCM_BLOCK_SAMPLES)
           == ADPCM_BLOCK_SAMPLES);
    for (int i = 1; i < ADPCM_BLOCK_SAMPLES; i++) {
        // 直流信号:predictor 精确起步,后续样本不应偏离(允许 1 LSB 量化抖动)
        int d = pcm[i] - pcm[0];
        assert(d <= 1 && d >= -1);
    }
    audio_streamer_set_compressed(false);            // 复位:后续用例走 PCM
}

// 7) 丢帧对账:发送失败计数;取消/token 丢弃的帧不计数(静默丢弃语义)
static void test_drop_accounting(void) {
    fake_reset();
    ensure_init();
    g_notify_rc = 1;                      // 发送失败 → 丢帧计数
    assert(audio_streamer_start() == ESP_OK);
    assert(wait_drops(500) > 0);          // 发送失败必然计数(有界等待,不赌调度)
    audio_streamer_stop();
    assert(event_seen(APP_EV_AUDIO_DROP_START));
    // stop 只停采集,环内残留留待 cancel;下一段 start 时残留已 token 失效,
    // 无需排空收尾(用例间零传染)。

    fake_reset();
    g_notify_rc = 0;
    g_notify_block_ms = 150;              // 消费慢 → 环满丢帧(计数 >0)
    assert(audio_streamer_start() == ESP_OK);
    assert(wait_drops(800) > 0);          // 环满丢帧已计数(有界等待)
    audio_streamer_stop();                // 停采集:不再有新写入 → 环满丢帧到此为止
    (void)audio_streamer_take_drops();    // 清掉 stop 前累积的环满丢帧,只看下一段
    audio_streamer_cancel();              // 取消:残留 token 作废(丢弃不计)
    assert(audio_streamer_take_drops() == 0);    // token 丢弃的帧不进丢帧计数
    g_notify_block_ms = 0;
}

// 8) drain 与 token 并存:voice.end 帧序靠 drain 等环空。stop 后残留是"应
//    发送完"的帧(drain 等其送达);cancel 后残留走 token 丢弃路径,该路径
//    必须归还计数(ring_item_returned),否则 drain 的环空判定会误等超时
//    —— 回归锁。
static void test_drain_with_token(void) {
    fake_reset();
    ensure_init();
    assert(audio_streamer_start() == ESP_OK);
    assert(wait_notify(1, 500));
    audio_streamer_stop();
    audio_streamer_drain(500);            // voice.end 帧序:等环内残留发送完
    audio_streamer_cancel();              // 残留(若有)token 作废
    audio_streamer_drain(500);            // token 丢弃路径归还计数正确 → 环空判定成立
    assert(audio_streamer_start() == ESP_OK);
    assert(wait_notify(2, 500));
    audio_streamer_stop();
}

// 9) 重复 init 幂等(复核 R1):成功态重入直接返回 ESP_OK,不重建 —— 句柄
//    不换、worker 不换(旧 worker 仍读同一全局句柄),管线行为与初始会话一致。
static void test_reinit_idempotent(void) {
    fake_reset();
    assert(audio_streamer_init() == ESP_OK);  // 幂等早返回(首次 init 已在前方用例)
    assert(audio_streamer_init() == ESP_OK);  // 重入:幂等(不覆盖句柄)
    audio_streamer_set_sender(link_send_audio);
    assert(audio_streamer_start() == ESP_OK);
    assert(wait_notify(1, 500));              // 行为一致:worker 仍工作
    audio_streamer_stop();
}

// 10) 采集硬件错误:停流 + AUDIO_ERROR 事件;随后取消幂等
static void test_audio_error_event(void) {
    fake_reset();
    ensure_init();
    g_fake_audio_fail = 1;
    assert(audio_streamer_start() == ESP_OK);
    usleep(50 * 1000);
    assert(!audio_streamer_active());
    assert(event_seen(APP_EV_AUDIO_ERROR));
    audio_streamer_cancel();              // 已停,幂等无害
    assert(audio_streamer_take_drops() == 0);
    audio_streamer_stop();                // 幂等收尾
}

// 11) token 直通:连续会话帧全部通过(token 递增匹配),stop→start 快速
//     重开无丢失;全链路帧长恒为完整块。
static void test_token_pass_through(void) {
    fake_reset();
    ensure_init();
    assert(audio_streamer_start() == ESP_OK);   // 会话 A
    assert(wait_notify(2, 500));
    audio_streamer_stop();                // 残留留环(旧 token)
    int before = g_notify_count;
    assert(audio_streamer_start() == ESP_OK);   // 会话 B:立即重开(残留 token 失效)
    assert(wait_notify(before + 1, 500));       // B 首帧通过
    assert(g_notify_min_len == g_notify_max_len);
    assert(g_notify_min_len == CHUNK_BYTES_EXPECT);
    audio_streamer_stop();
}

int main(void) {
    // 失败注入用例放最前:s_ready 初始 false 时注入才真正触发 init 的创建路径
    // (成功 init 后幂等早返回,注入不再生效)。test_token_drops_stale 是首次
    // 成功 init(唯一可注入环残留的位置),此后用例复用同一环 —— 用例间残留
    // 靠 token 失效自净,无需排空收尾(token 方案的测试红利)。
    test_init_failure_api_noop();
    test_worker_fail_cleanup();
    test_token_drops_stale();
    test_reinit_idempotent();
    test_cancel_drops_remaining();
    test_cancel_idempotent();
    test_cancel_returns_quickly();
    test_compressed_path_emits_adpcm_blocks();
    test_drop_accounting();
    test_drain_with_token();
    test_audio_error_event();
    test_token_pass_through();
    printf("test_audio_streamer: all assertions passed\n");
    return 0;
}
