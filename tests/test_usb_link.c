// tests/test_usb_link.c —— USB 帧协议主机测试(assert 风格,纯 C 无 IDF 依赖)。
// 覆盖:组帧↔解析回环(各类型/各长度)、垃圾前缀、半帧、跨 feed、超长 len、
// 非法 type、坏 checksum、0xA5 0xA5 0x5A 假锚点、日志噪声混流恢复、连续多帧。
// 与 companion/tests/test_serial_frame.py 同构(两端状态机逐字节一致,共享测试意图)。
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "usb_link_framing.h"

static uint8_t s_payload[USB_FRAME_PAYLOAD_MAX + 1];   /* 解码目标缓冲 */
static uint8_t s_out[USB_FRAME_PAYLOAD_MAX + USB_FRAME_HEADER];

static void feed_all(usb_frame_ctx_t *ctx, const uint8_t *data, size_t len)
{
    /* 逐字节喂(等价设备读线程 read_bytes → 逐字节 feed 的真实路径) */
    for (size_t i = 0; i < len; i++) {
        usb_frame_feed(ctx, data[i], NULL, s_payload, NULL);
    }
}

/* 组帧 + 逐字节喂 → 断言 DONE 且载荷一致(基本回环) */
static void assert_roundtrip(uint8_t type, const uint8_t *payload, size_t len)
{
    size_t frame_len = usb_frame_build(type, payload, len, s_out, sizeof(s_out));
    assert(frame_len == USB_FRAME_HEADER + len);

    usb_frame_ctx_t ctx = { 0 };
    uint8_t got_type = 0;
    size_t got_len = 0;
    for (size_t i = 0; i < frame_len; i++) {
        usb_frame_feed_rc_t rc = usb_frame_feed(&ctx, s_out[i],
                                                &got_type, s_payload, &got_len);
        if (i < frame_len - 1) assert(rc == USB_FRAME_NONE);
        else assert(rc == USB_FRAME_DONE);
    }
    assert(got_type == type);
    assert(got_len == len);
    if (len) assert(memcmp(s_payload, payload, len) == 0);
}

static void test_build_roundtrip(void)
{
    static const uint8_t types[] = { USB_FRAME_EVENT, USB_FRAME_AUDIO,
                                     USB_FRAME_CTRL, USB_FRAME_SYS, USB_FRAME_SYS_RESP };
    uint8_t data[USB_FRAME_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7 + 1);

    for (size_t t = 0; t < sizeof(types); t++) {
        assert_roundtrip(types[t], data, 0);                    /* 空载荷 */
        assert_roundtrip(types[t], data, 1);
        assert_roundtrip(types[t], data, 128);                  /* SYS 上限附近 */
        assert_roundtrip(types[t], data, 2048);                 /* CTRL 上限附近 */
        assert_roundtrip(types[t], data, 3200);                 /* 音频帧 */
        assert_roundtrip(types[t], data, USB_FRAME_PAYLOAD_MAX); /* 载荷上限 */
    }
}

static void test_build_rejects(void)
{
    uint8_t b[128];   /* ≥ 最后一个断言的成功分支所需的 100B 载荷 */
    /* len 超上限 */
    assert(usb_frame_build(USB_FRAME_EVENT, b, USB_FRAME_PAYLOAD_MAX + 1,
                           s_out, sizeof(s_out)) == 0);
    /* cap 不足(只给帧头) */
    assert(usb_frame_build(USB_FRAME_EVENT, b, 100, s_out, USB_FRAME_HEADER) == 0);
    assert(usb_frame_build(USB_FRAME_EVENT, b, 100, s_out, USB_FRAME_HEADER + 99) == 0);
    /* 恰足:6 + 100 */
    assert(usb_frame_build(USB_FRAME_EVENT, b, 100, s_out, USB_FRAME_HEADER + 100) == 106);
}

static void test_garbage_prefix(void)
{
    /* 日志/启动噪声在前:重扫吞掉,帧仍恢复 */
    const char *noise = "I (1234) main: AI Passport 固件启动\r\n\0\x01\xff";
    usb_frame_ctx_t ctx = { 0 };
    feed_all(&ctx, (const uint8_t *)noise, strlen(noise) + 3);

    uint8_t payload[] = { 0x01, 0x02, 0x03 };
    size_t frame_len = usb_frame_build(USB_FRAME_EVENT, payload, sizeof(payload),
                                       s_out, sizeof(s_out));
    uint8_t got_type = 0;
    size_t got_len = 0;
    usb_frame_feed_rc_t rc = USB_FRAME_NONE;
    for (size_t i = 0; i < frame_len; i++)
        rc = usb_frame_feed(&ctx, s_out[i], &got_type, s_payload, &got_len);
    assert(rc == USB_FRAME_DONE);
    assert(got_type == USB_FRAME_EVENT);
    assert(got_len == sizeof(payload));
    assert(memcmp(s_payload, payload, sizeof(payload)) == 0);
}

static void test_half_frame_resume(void)
{
    /* 半帧挂起(模拟读线程分片到达),补喂后完成 */
    uint8_t payload[100];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    size_t frame_len = usb_frame_build(USB_FRAME_CTRL, payload, sizeof(payload),
                                       s_out, sizeof(s_out));

    usb_frame_ctx_t ctx = { 0 };
    uint8_t got_type = 0;
    size_t got_len = 0;
    for (size_t i = 0; i < 10; i++)   /* 只有帧头前 10 字节 */
        assert(usb_frame_feed(&ctx, s_out[i], &got_type, s_payload, &got_len)
               == USB_FRAME_NONE);
    for (size_t i = 10; i < frame_len; i++) {
        usb_frame_feed_rc_t rc = usb_frame_feed(&ctx, s_out[i],
                                                &got_type, s_payload, &got_len);
        if (i < frame_len - 1) assert(rc == USB_FRAME_NONE);
        else assert(rc == USB_FRAME_DONE);
    }
    assert(got_type == USB_FRAME_CTRL);
    assert(got_len == sizeof(payload));
    assert(memcmp(s_payload, payload, sizeof(payload)) == 0);
}

static void test_false_anchor(void)
{
    /* 0xA5 0xA5 0x5A:第二、三字节构成合法帧头,不得丢帧 */
    uint8_t payload[] = { 0xAA, 0xBB };
    size_t frame_len = usb_frame_build(USB_FRAME_EVENT, payload, sizeof(payload),
                                       s_out, sizeof(s_out));
    uint8_t stream[8 + sizeof(payload)];
    stream[0] = 0xA5;                    /* 假 magic0 */
    memcpy(stream + 1, s_out, frame_len); /* 真帧紧随 */

    usb_frame_ctx_t ctx = { 0 };
    uint8_t got_type = 0;
    size_t got_len = 0;
    usb_frame_feed_rc_t rc = USB_FRAME_NONE;
    for (size_t i = 0; i < 1 + frame_len; i++)
        rc = usb_frame_feed(&ctx, stream[i], &got_type, s_payload, &got_len);
    assert(rc == USB_FRAME_DONE);
    assert(got_type == USB_FRAME_EVENT);
    assert(memcmp(s_payload, payload, sizeof(payload)) == 0);
}

static void test_oversize_len(void)
{
    /* payload_len 超上限:立即 ERR_OVERSIZE 重扫,且不缓冲巨型长度 */
    usb_frame_ctx_t ctx = { 0 };
    uint8_t head[] = { 0xA5, 0x5A, USB_FRAME_EVENT, 0x00, 0x11 };  /* len = 0x1100 > 4096 */
    usb_frame_feed_rc_t rc = USB_FRAME_NONE;
    for (size_t i = 0; i < sizeof(head); i++)
        rc = usb_frame_feed(&ctx, head[i], NULL, s_payload, NULL);
    assert(rc == USB_FRAME_ERR_OVERSIZE);

    /* 重扫后正常帧仍可解析 */
    uint8_t payload[] = { 0x01 };
    size_t frame_len = usb_frame_build(USB_FRAME_SYS, payload, sizeof(payload),
                                       s_out, sizeof(s_out));
    uint8_t got_type = 0;
    size_t got_len = 0;
    for (size_t i = 0; i < frame_len; i++)
        rc = usb_frame_feed(&ctx, s_out[i], &got_type, s_payload, &got_len);
    assert(rc == USB_FRAME_DONE);
    assert(got_type == USB_FRAME_SYS);
}

static void test_bad_type(void)
{
    uint8_t bad[] = { 0xA5, 0x5A, 0x00, 0x01, 0x00, 0x00, 0x00 };   /* type 0 */
    uint8_t bad6[] = { 0xA5, 0x5A, 0x06, 0x01, 0x00, 0x00, 0x00 };  /* type 6 */
    uint8_t badff[] = { 0xA5, 0x5A, 0xFF, 0x01, 0x00, 0x00, 0x00 }; /* type 0xFF */
    usb_frame_ctx_t ctx = { 0 };
    /* 错误发生在第 3 字节,后续字节喂入会落回 NONE —— 用捕获标志断言 */
    usb_frame_feed_rc_t rc;
    bool saw = false;
    for (size_t i = 0; i < sizeof(bad); i++)
        if ((rc = usb_frame_feed(&ctx, bad[i], NULL, s_payload, NULL))
            == USB_FRAME_ERR_BAD) saw = true;
    assert(saw);
    saw = false;
    for (size_t i = 0; i < sizeof(bad6); i++)
        if ((rc = usb_frame_feed(&ctx, bad6[i], NULL, s_payload, NULL))
            == USB_FRAME_ERR_BAD) saw = true;
    assert(saw);
    saw = false;
    for (size_t i = 0; i < sizeof(badff); i++)
        if ((rc = usb_frame_feed(&ctx, badff[i], NULL, s_payload, NULL))
            == USB_FRAME_ERR_BAD) saw = true;
    assert(saw);
}

static void test_bad_checksum(void)
{
    uint8_t payload[] = { 0x10, 0x20, 0x30 };
    size_t frame_len = usb_frame_build(USB_FRAME_AUDIO, payload, sizeof(payload),
                                       s_out, sizeof(s_out));
    s_out[6] ^= 0xFF;   /* 篡改一个载荷字节 → checksum 不符 */

    usb_frame_ctx_t ctx = { 0 };
    usb_frame_feed_rc_t rc = USB_FRAME_NONE;
    for (size_t i = 0; i < frame_len; i++)
        rc = usb_frame_feed(&ctx, s_out[i], NULL, s_payload, NULL);
    assert(rc == USB_FRAME_ERR_SUM);

    /* 重扫后正常帧仍可解析 */
    size_t f2 = usb_frame_build(USB_FRAME_EVENT, payload, sizeof(payload),
                                s_out, sizeof(s_out));
    uint8_t got_type = 0;
    size_t got_len = 0;
    for (size_t i = 0; i < f2; i++)
        rc = usb_frame_feed(&ctx, s_out[i], &got_type, s_payload, &got_len);
    assert(rc == USB_FRAME_DONE);
    assert(got_type == USB_FRAME_EVENT);
}

static void test_concat_frames(void)
{
    /* 两帧紧连:两次 DONE,载荷互不串扰 */
    uint8_t p1[] = { 1, 2, 3 };
    uint8_t p2[] = { 4, 5, 6, 7, 8 };
    size_t f1 = usb_frame_build(USB_FRAME_EVENT, p1, sizeof(p1), s_out, sizeof(s_out));
    size_t f2 = usb_frame_build(USB_FRAME_AUDIO, p2, sizeof(p2), s_out + f1, sizeof(s_out) - f1);

    usb_frame_ctx_t ctx = { 0 };
    uint8_t got_type = 0;
    size_t got_len = 0;
    size_t done = 0;
    for (size_t i = 0; i < f1 + f2; i++) {
        if (usb_frame_feed(&ctx, s_out[i], &got_type, s_payload, &got_len)
            == USB_FRAME_DONE) {
            if (done == 0) {
                assert(got_type == USB_FRAME_EVENT);
                assert(got_len == sizeof(p1));
                assert(memcmp(s_payload, p1, sizeof(p1)) == 0);
            } else {
                assert(got_type == USB_FRAME_AUDIO);
                assert(got_len == sizeof(p2));
                assert(memcmp(s_payload, p2, sizeof(p2)) == 0);
            }
            done++;
        }
    }
    assert(done == 2);
}

static void test_log_noise_between_bytes(void)
{
    /* 日志行从帧中间插入(模拟写分片间隙被日志抢占):payload 累积被噪声
     * 填满 → checksum 失败 → 立即重扫;噪声持续期间不产生 DONE;
     * 噪声结束后后续完整帧恢复。注意:帧边界保护是协议正常行为 ——
     * 噪声未填满 payload 前会被吸收为载荷(本帧作废),故噪声必须 ≥
     * 剩余 payload 长度才能让本帧快速失败 */
    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    usb_frame_build(USB_FRAME_AUDIO, payload, sizeof(payload), s_out, sizeof(s_out));
    const char noise[] = "E (500) mode: 模式切换失败\r\n"
                         "E (501) mode: 重试(日志填满剩余载荷)\r\n";
    /* 帧头 10 字节后插入:剩余 payload = 64-10+1(checksum) = 55 字节 */
    assert(sizeof(noise) - 1 >= 55);

    usb_frame_ctx_t ctx = { 0 };
    usb_frame_feed_rc_t rc = USB_FRAME_NONE;
    for (size_t i = 0; i < 10; i++)
        rc = usb_frame_feed(&ctx, s_out[i], NULL, s_payload, NULL);
    for (size_t i = 0; noise[i]; i++)
        rc = usb_frame_feed(&ctx, (uint8_t)noise[i], NULL, s_payload, NULL);
    /* 噪声期间:残帧 ERR 或落回 NONE,不得 DONE */
    assert(rc != USB_FRAME_DONE);

    /* 后续完整帧恢复 */
    uint8_t p2[] = { 0x42 };
    size_t f2 = usb_frame_build(USB_FRAME_SYS_RESP, p2, sizeof(p2), s_out, sizeof(s_out));
    uint8_t got_type = 0;
    size_t got_len = 0;
    for (size_t i = 0; i < f2; i++)
        rc = usb_frame_feed(&ctx, s_out[i], &got_type, s_payload, &got_len);
    assert(rc == USB_FRAME_DONE);
    assert(got_type == USB_FRAME_SYS_RESP);
    assert(got_len == sizeof(p2));
    assert(s_payload[0] == 0x42);
}

int main(void)
{
    test_build_roundtrip();
    test_build_rejects();
    test_garbage_prefix();
    test_half_frame_resume();
    test_false_anchor();
    test_oversize_len();
    test_bad_type();
    test_bad_checksum();
    test_concat_frames();
    test_log_noise_between_bytes();
    printf("test_usb_link: 全部通过\n");
    return 0;
}
