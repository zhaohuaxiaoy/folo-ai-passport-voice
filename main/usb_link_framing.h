// main/usb_link_framing.h —— USB 有线通道字节流分帧协议(纯 C,零 IDF 依赖)。
// 设计要点:
//   - USB 是字节流(无帧类型、无 BLE 的 GATT 特征天然分帧),统一二进制帧:
//       [magic 2B: 0xA5 0x5A][type 1B][payload_len 2B LE][payload][checksum 1B]
//     总帧长 = 6 + len;checksum = 帧头+payload 全部字节和 mod 256(与硬件 CRC 互补)。
//   - 失同步恢复:两端同构滑动重扫状态机,任何一步非法立即重扫且当前字节重新
//     当 magic0 判(日志噪声/启动残帧只损失个别字节;0xA5 0xA5 0x5A 假锚点不丢帧)。
//   - 零全局状态、零分配:payload 缓冲由调用方提供(≥ USB_FRAME_PAYLOAD_MAX)。
//   - 宿主可测:本模块与 companion/serial_frame.py 逐字节同构,共享同一测试向量。
#ifndef USB_LINK_FRAMING_H
#define USB_LINK_FRAMING_H

#include <stddef.h>
#include <stdint.h>

#define USB_FRAME_MAGIC0      0xA5
#define USB_FRAME_MAGIC1      0x5A
#define USB_FRAME_HEADER      6    /* magic2 + type1 + len2 + checksum1 */
/* 方向感知契约(审查 P1):feed 按本端接收方向过滤合法类型 —— 固件 RX 只收
 * CTRL/SYS、companion RX 只收 EVENT/AUDIO/SYS_RESP;方向不符在 TYPE 状态即
 * ERR_BAD(写 payload 前拒绝)。类型固有上限在 LEN_HI 校验(超 → ERR_OVERSIZE)。
 * 缓冲按方向实际最大分配:下行 RX 2048(CTRL 上限)、上行 TX 3200(AUDIO 帧),
 * 省 ~2.9KB 静态 RAM —— 方向过滤 + 类型上限保证 2048 缓冲永不被写越界。 */
#define USB_FRAME_RX_PAYLOAD_MAX 2048 /* 下行缓冲分配(CTRL 上限) */
#define USB_FRAME_TX_PAYLOAD_MAX 3200 /* 上行缓冲分配(AUDIO 帧) */
#define USB_FRAME_CTRL_MAX       2048 /* PC→设备 协议 JSON 下行 */
#define USB_FRAME_SYS_MAX        128  /* PC→设备 命令文本 */
#define USB_FRAME_EVENT_MAX      512  /* 设备→PC 事件行(JSON) */
#define USB_FRAME_AUDIO_MAX      3200 /* 设备→PC 音频帧 */
#define USB_FRAME_SYS_RESP_MAX   2048 /* 设备→PC 命令输出 */
#define USB_FRAME_PAYLOAD_MAX    3200 /* 任一类型最大载荷(总闸,文档值) */

/* 本端接收方向:DOWIN = PC→设备(固件 RX);UP = 设备→PC(companion RX)。
 * 方向决定合法类型集合(见 usb_frame_dir_ok),防止上行类型帧经下行链路
 * 写爆 RX 缓冲(审查 P1)。 */
typedef enum { USB_FRAME_DIR_DOWN = 0, USB_FRAME_DIR_UP } usb_frame_dir_t;

/* 帧类型(设备↔PC 契约,见 design.md 帧协议节) */
#define USB_FRAME_EVENT     0x01 /* 设备→PC: EVENT JSON 行(含 '\n') */
#define USB_FRAME_AUDIO     0x02 /* 设备→PC: 3200B 裸 PCM 帧 */
#define USB_FRAME_CTRL      0x03 /* PC→设备: 协议 JSON 下行(不带 '\n',≤2048B) */
#define USB_FRAME_SYS       0x04 /* PC→设备: 控制台命令文本(≤128B) */
#define USB_FRAME_SYS_RESP  0x05 /* 设备→PC: 命令输出(≤2048B 超限截断) */

#define USB_FRAME_TYPE_MIN  0x01
#define USB_FRAME_TYPE_MAX  0x05

/* feed 返回码 */
typedef enum {
    USB_FRAME_NONE = 0,      /* 字节被消费,尚未成帧 */
    USB_FRAME_DONE,          /* 完整帧:type/payload/payload_len 有效 */
    USB_FRAME_ERR_BAD,       /* 非法 type / 帧头残缺:已重扫 */
    USB_FRAME_ERR_OVERSIZE,  /* payload_len > 上限:已重扫(不缓冲巨型长度) */
    USB_FRAME_ERR_SUM,       /* checksum 不符:该帧丢弃,已重扫 */
} usb_frame_feed_rc_t;

typedef enum {
    USB_FS_MAGIC0 = 0,
    USB_FS_MAGIC1,
    USB_FS_TYPE,
    USB_FS_LEN_LO,
    USB_FS_LEN_HI,
    USB_FS_PAYLOAD,
    USB_FS_CHECKSUM,
} usb_frame_state_t;

typedef struct {
    uint8_t  state;   /* usb_frame_state_t */
    uint8_t  dir;     /* usb_frame_dir_t:本端接收方向(初始化设置,feed 只读) */
    uint16_t len;     /* payload_len(帧头解析出) */
    size_t   pos;     /* payload 已收字节 */
    uint8_t  sum;     /* 累计校验(magic0 起) */
    uint8_t  type;
} usb_frame_ctx_t;

/* 逐字节喂入(调用方循环 read_bytes 后逐字节调用)。
 * ctx.dir 在初始化时设置(USB_FRAME_DIR_DOWN = 固件 RX / USB_FRAME_DIR_UP =
 * companion RX);payload 为调用方缓冲(容量 ≥ 该方向的 RX_PAYLOAD_MAX),
 * DONE 时帧载荷已写入。方向过滤与类型上限校验均发生在写 payload 之前
 * (审查 P1)。任何返回码之后 ctx 均已回扫描起点,可继续喂下一字节。 */
usb_frame_feed_rc_t usb_frame_feed(usb_frame_ctx_t *ctx, uint8_t b,
                                   uint8_t *type, uint8_t *payload,
                                   size_t *payload_len);

/* 组帧:out 容量 cap,成功返回总帧长(6+len),cap 不足或 len 超限返回 0。 */
size_t usb_frame_build(uint8_t type, const uint8_t *payload, size_t len,
                       uint8_t *out, size_t cap);

#endif /* USB_LINK_FRAMING_H */
