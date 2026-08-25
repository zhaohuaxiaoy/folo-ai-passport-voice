// main/usb_link_framing.c —— 帧协议状态机实现(纯 C,零 IDF 依赖)。
// 状态机与 companion/serial_frame.py 的 FrameDecoder 逐字节同构,改动必须两端同步
// (两侧各有测试锁住:test_usb_link.c / test_serial_frame.py)。
#include <stdbool.h>
#include "usb_link_framing.h"

/* 类型固有最大载荷(方向无关):LEN_HI 用类型上限校验,杜绝"大载荷经小缓冲
 * 方向"越界写(审查 P1)。 */
static size_t type_max_payload(uint8_t type)
{
    switch (type) {
    case USB_FRAME_CTRL:     return USB_FRAME_CTRL_MAX;
    case USB_FRAME_SYS:      return USB_FRAME_SYS_MAX;
    case USB_FRAME_EVENT:    return USB_FRAME_EVENT_MAX;
    case USB_FRAME_AUDIO:    return USB_FRAME_AUDIO_MAX;
    case USB_FRAME_SYS_RESP: return USB_FRAME_SYS_RESP_MAX;
    default:                 return 0;   /* 非法 type 不合法(调用方不应传入) */
    }
}

/* 方向合法类型:DOWN(固件 RX)只收 PC 下行 CTRL/SYS;UP(companion RX)只收
 * 设备上行 EVENT/AUDIO/SYS_RESP。方向不符在 TYPE 状态 ERR_BAD —— 上行帧
 * 类型经下行链路在写 payload 前被拒,RX 缓冲按下行上限分配即安全。 */
static bool type_dir_ok(usb_frame_dir_t dir, uint8_t type)
{
    if (dir == USB_FRAME_DIR_DOWN)
        return type == USB_FRAME_CTRL || type == USB_FRAME_SYS;
    return type == USB_FRAME_EVENT || type == USB_FRAME_AUDIO
        || type == USB_FRAME_SYS_RESP;
}

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
        /* 方向过滤先于长度校验:上行类型帧经下行链路(或反之)在写 payload
         * 前被拒 —— RX 缓冲按下行上限分配即安全(审查 P1) */
        if (b >= USB_FRAME_TYPE_MIN && b <= USB_FRAME_TYPE_MAX
            && type_dir_ok((usb_frame_dir_t)ctx->dir, b)) {
            ctx->state = USB_FS_LEN_LO;
            ctx->sum += b;
            ctx->type = b;
        } else {
            rescan(ctx, b);   /* 非法 type / 方向违约:重扫 */
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
        if (ctx->len > type_max_payload(ctx->type)) {
            rescan(ctx, b);
            return USB_FRAME_ERR_OVERSIZE;   /* 类型上限(≤方向缓冲):不缓冲巨型长度 */
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
    if (len > type_max_payload(type)) return 0;   /* 类型固有上限 */
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
