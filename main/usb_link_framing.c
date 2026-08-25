// main/usb_link_framing.c —— 帧协议状态机实现(纯 C,零 IDF 依赖)。
// 状态机与 companion/serial_frame.py 的 FrameDecoder 逐字节同构,改动必须两端同步
// (两侧各有测试锁住:test_usb_link.c / test_serial_frame.py)。
#include "usb_link_framing.h"

static void rescan(usb_frame_ctx_t *ctx, uint8_t b)
{
    /* 滑动重扫:当前字节重新当 magic0 判,不整流丢弃 */
    if (b == USB_FRAME_MAGIC0) {
        ctx->state = USB_FS_MAGIC1;
        ctx->sum = USB_FRAME_MAGIC0;
    } else {
        ctx->state = USB_FS_MAGIC0;
        ctx->sum = 0;
    }
}

usb_frame_feed_rc_t usb_frame_feed(usb_frame_ctx_t *ctx, uint8_t b,
                                   uint8_t *type, uint8_t *payload,
                                   size_t *payload_len)
{
    switch (ctx->state) {
    case USB_FS_MAGIC0:
        if (b == USB_FRAME_MAGIC0) {
            ctx->state = USB_FS_MAGIC1;
            ctx->sum = USB_FRAME_MAGIC0;
        }
        return USB_FRAME_NONE;

    case USB_FS_MAGIC1:
        if (b == USB_FRAME_MAGIC1) {
            ctx->state = USB_FS_TYPE;
            ctx->sum += USB_FRAME_MAGIC1;
        } else {
            rescan(ctx, b);   /* 0xA5 0xA5 0x5A:第二个 0xA5 重当 magic0 */
        }
        return USB_FRAME_NONE;

    case USB_FS_TYPE:
        if (b >= USB_FRAME_TYPE_MIN && b <= USB_FRAME_TYPE_MAX) {
            ctx->state = USB_FS_LEN_LO;
            ctx->sum += b;
            ctx->type = b;
        } else {
            rescan(ctx, b);   /* 非法 type:重扫 */
            return USB_FRAME_ERR_BAD;
        }
        return USB_FRAME_NONE;

    case USB_FS_LEN_LO:
        ctx->len = b;
        ctx->sum += b;
        ctx->state = USB_FS_LEN_HI;
        return USB_FRAME_NONE;

    case USB_FS_LEN_HI:
        ctx->len |= (uint16_t)b << 8;
        ctx->sum += b;
        if (ctx->len > USB_FRAME_PAYLOAD_MAX) {
            rescan(ctx, b);
            return USB_FRAME_ERR_OVERSIZE;   /* 不缓冲巨型长度,立即重扫 */
        }
        ctx->pos = 0;
        ctx->state = (ctx->len == 0) ? USB_FS_CHECKSUM : USB_FS_PAYLOAD;
        return USB_FRAME_NONE;

    case USB_FS_PAYLOAD:
        payload[ctx->pos++] = b;
        ctx->sum += b;
        if (ctx->pos == ctx->len) ctx->state = USB_FS_CHECKSUM;
        return USB_FRAME_NONE;

    case USB_FS_CHECKSUM:
        if ((uint8_t)(ctx->sum + b) == 0) {
            *type = ctx->type;
            *payload_len = ctx->len;
            rescan(ctx, 0);   /* 恢复扫描起点,等下一帧 */
            return USB_FRAME_DONE;
        }
        rescan(ctx, b);
        return USB_FRAME_ERR_SUM;
    }
    /* 不可达(状态枚举封闭) */
    rescan(ctx, 0);
    return USB_FRAME_ERR_BAD;
}

size_t usb_frame_build(uint8_t type, const uint8_t *payload, size_t len,
                       uint8_t *out, size_t cap)
{
    if (len > USB_FRAME_PAYLOAD_MAX) return 0;
    if (cap < USB_FRAME_HEADER + len) return 0;

    uint8_t sum = (uint8_t)(USB_FRAME_MAGIC0 + USB_FRAME_MAGIC1
                            + type + (len & 0xFF) + ((len >> 8) & 0xFF));
    out[0] = USB_FRAME_MAGIC0;
    out[1] = USB_FRAME_MAGIC1;
    out[2] = type;
    out[3] = (uint8_t)(len & 0xFF);
    out[4] = (uint8_t)(len >> 8);
    for (size_t i = 0; i < len; i++) {
        out[USB_FRAME_HEADER - 1 + i] = payload[i];
        sum += payload[i];
    }
    out[USB_FRAME_HEADER - 1 + len] = (uint8_t)(-sum);   /* 0 - sum mod 256 */
    return USB_FRAME_HEADER + len;
}
