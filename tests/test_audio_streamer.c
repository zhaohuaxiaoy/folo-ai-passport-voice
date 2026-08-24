// tests/test_audio_streamer.c —— 音频流管线宿主测试(fake RTOS 层,真并发线程)。
// 存在的意义(外部 Review 教训):此前 host 测试不编译 audio_streamer.c,导致
// 调用了 ESP-IDF 不存在的 API(xRingbufferReset)未被捕获。本测试把 audio_streamer.c
// 连同 fake ringbuf/semphr/task 一起编译——编译期即可拦截"不存在的 ESP-IDF API"
// 类回归,运行期覆盖 cancel 的取消/幂等/在途/超时语义。
// 编译宏:CANCEL_DRAIN_TIMEOUT_MS=200(由 CMake 注入,加速超时路径用例)。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_types.h"
#include "audio_streamer.h"
#include "esp_err.h"

// fake_rtos.c 提供的可控钩子
extern int g_notify_count;       // 到达发送层的帧数(cancel 丢弃的不计)
extern int g_notify_rc;          // notify 返回码(非 0 → 丢帧计数路径)
extern int g_notify_block_ms;    // notify 阻塞毫秒(模拟在途)
extern int g_fake_audio_fail;    // 非 0 → bsp_audio_read 报错
extern app_event_t g_events[];
extern int g_event_count;
extern void fake_reset(void);

static int s_inited = 0;
static void ensure_init(void) {
    if (!s_inited) {
        assert(audio_streamer_init() == ESP_OK);
        s_inited = 1;
    }
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int event_seen(app_event_type_t t) {
    for (int i = 0; i < g_event_count; i++) {
        if (g_events[i].type == t) return 1;
    }
    return 0;
}

// 1) 取消丢弃残留:第一块阻塞在 notify(在途),cancel 等其完成但不再发送任何帧;
//    取消后新会话从零开始(残留不流入)。
//    确定性:worker 阻塞在 notify 时 s_cancel 置位,它返回后只归还,不会再有 notify 调用。
static void test_cancel_drops_remaining(void) {
    fake_reset();
    ensure_init();
    g_notify_block_ms = 150;              // 第一块进 notify 即阻塞(模拟流控在途,< 200ms 轮询上限)
    audio_streamer_start();
    usleep(50 * 1000);
    assert(g_notify_count == 1);          // 恰一帧在途(其完成不可撤回)
    audio_streamer_cancel();              // 等待在途完成;后续帧全部丢弃
    assert(g_notify_count == 1);          // 无任何新帧被发送
    g_notify_block_ms = 0;
    audio_streamer_start();               // 新会话:旧残留已清
    usleep(50 * 1000);
    assert(g_notify_count >= 2);
    audio_streamer_stop();                // 收尾:停流
    audio_streamer_cancel();              // 收尾:排空在途尾帧(下个用例零残留)
}

// 2) 幂等:未采集时取消无害;STOP 后(残留仍待 drain 发送)取消同样丢弃残留
//    允许 ±1:cancel 置位瞬间"已通过检查正在发送"的一帧会完成(不可撤回,真实语义)
static void test_cancel_idempotent(void) {
    fake_reset();
    ensure_init();
    audio_streamer_cancel();              // 从未 start:无害
    audio_streamer_cancel();              // 重复调用
    audio_streamer_start();
    usleep(40 * 1000);
    audio_streamer_stop();                // 正常停:残留留在环内(等 voice.end 前 drain)
    int before = g_notify_count;
    audio_streamer_cancel();              // 断链兜底:残留丢弃(此前实现直接 return 不清)
    assert(g_notify_count <= before + 1);
    audio_streamer_start();
    usleep(40 * 1000);
    assert(g_notify_count > before);
    audio_streamer_stop();
    audio_streamer_cancel();              // 收尾:排空在途尾帧(下个用例零残留)
}

// 3) 排空超时:cancel 及时返回,但丢帧模式持续——残留仍被丢弃,绝不流入下次会话;
//    随后 start() 先等环空再清丢帧模式,新会话正常(这是 T1 正常路径的反面)
static void test_cancel_timeout_keeps_dropping(void) {
    fake_reset();
    ensure_init();
    g_notify_block_ms = 1000;             // 远超注入的 CANCEL_DRAIN_POLLS×5ms=200ms
    audio_streamer_start();
    usleep(50 * 1000);
    assert(g_notify_count == 1);
    int64_t t0 = now_ms();
    audio_streamer_cancel();              // ~200ms 轮询超时即返回
    assert(now_ms() - t0 <= 800);
    usleep(1300 * 1000);                  // notify 结束 + worker 继续丢弃至环空
    assert(g_notify_count == 1);          // 残留从未被发送
    g_notify_block_ms = 0;
    audio_streamer_start();               // start 收尾:等环空 + 清丢帧模式
    usleep(50 * 1000);
    assert(g_notify_count >= 2);          // 新会话正常
    audio_streamer_stop();
    audio_streamer_cancel();              // 收尾:排空在途尾帧(下个用例零残留)
}

// 4) 残留未排空时 start 拒绝启动:不置 active、丢帧模式保持;
//    排空完成后下次 start 成功(旧帧绝不流入新会话)
static void test_start_rejected_until_drained(void) {
    fake_reset();
    ensure_init();
    g_notify_block_ms = 1000;             // notify 长时间在途 → cancel 排空超时
    audio_streamer_start();
    usleep(50 * 1000);
    assert(g_notify_count == 1);
    audio_streamer_cancel();              // ~200ms 超时,s_cancel 保持
    audio_streamer_start();               // 环未空(残留还在) → 拒绝启动
    assert(!audio_streamer_active());     // 关键:未进入录音,旧帧未放行
    usleep(1300 * 1000);                  // notify 完成 + worker 丢完残留
    g_notify_block_ms = 0;                // 最终会话不留长在途(与 T3 一致;否则 1s 在途
                                          // notify 跨用例存活,污染下个用例的 cancel/start)
    audio_streamer_start();               // 排空完成 → 正常启动
    assert(audio_streamer_active());
    usleep(50 * 1000);
    assert(g_notify_count >= 2);          // 新会话正常(旧帧零泄漏)
    audio_streamer_stop();
    audio_streamer_cancel();              // 收尾:排空在途尾帧(下个用例零残留)
}

// 5) 丢帧对账:发送失败计数;取消丢弃的帧不计数
static void test_drop_accounting(void) {
    fake_reset();
    ensure_init();
    g_notify_rc = 1;                      // 发送失败 → 丢帧计数
    audio_streamer_start();
    usleep(50 * 1000);
    audio_streamer_stop();
    assert(audio_streamer_take_drops() > 0);
    assert(event_seen(APP_EV_AUDIO_DROP_START));
    // stop() 只停采集,不会清理环内残留;先取消收尾,避免下一段 fake_reset
    // 与上一会话的 ble_worker 在途帧并发,污染时序。
    audio_streamer_cancel();

    fake_reset();
    g_notify_rc = 0;
    // 消费慢 → 环满丢帧(计数 >0)。必须 < CANCEL_DRAIN_POLLS×5ms(200ms):
    // 若 ≥200ms,cancel 轮询窗口内排不完 → 残留 s_cancel 传染下一个用例
    // (新 start() 会拒绝启动,见 test_start_rejected_until_drained 语义)。
    g_notify_block_ms = 150;
    audio_streamer_start();
    usleep(80 * 1000);
    audio_streamer_stop();                // 停采集:计数冻结(环内残留仍由 worker 发送)
    assert(audio_streamer_take_drops() > 0);     // 环满丢帧已计数
    audio_streamer_cancel();              // 取消:残留丢弃
    assert(audio_streamer_take_drops() == 0);    // 取消丢弃的帧不进丢帧计数
    g_notify_block_ms = 0;
}

// 6) 采集硬件错误:停流 + AUDIO_ERROR 事件;随后取消幂等
static void test_audio_error_event(void) {
    fake_reset();
    ensure_init();
    g_fake_audio_fail = 1;
    audio_streamer_start();
    usleep(50 * 1000);
    assert(!audio_streamer_active());
    assert(event_seen(APP_EV_AUDIO_ERROR));
    audio_streamer_cancel();              // 已停,幂等无害
    assert(audio_streamer_take_drops() == 0);
    audio_streamer_stop();                // 幂等收尾
}

int main(void) {
    test_cancel_drops_remaining();
    test_cancel_idempotent();
    test_cancel_timeout_keeps_dropping();
    test_start_rejected_until_drained();
    test_drop_accounting();
    test_audio_error_event();
    printf("test_audio_streamer: all assertions passed\n");
    return 0;
}
