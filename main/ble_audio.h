// main/ble_audio.h —— BLE 直连音频/事件通道。
// 上半部分:分片打包纯函数(纯 C:零全局状态、零分配,无 ESP-IDF 依赖,可主机测试)。
// 下半部分:NimBLE 外设接口(同文件实现于 ble_audio.c;头文件保持无 IDF 依赖,
// 用 int/bool 等基本类型返回码,宿主机测试可照常编译)。
//
// 承载契约(GATT 由 ble_audio.c 注册):
//   Service 0xA2B0 (0000A2B0-0000-1000-8000-00805F9B34FB)
//     ├─ 0xA2B1 CTRL  WRITE|WRITE_ENC    Mac→设备:JSON 行 ≤2048B(APP_PROTO_RX_CAP;长写由 NimBLE
//     │                                   重组,设备端写回调只做 app_protocol_parse→投事件,零阻塞)
//     ├─ 0xA2B2 EVENT NOTIFY             设备→Mac:JSON 行 ≤512B(app_protocol_* 序列化;
//     │                                   单行跨包 → 应用层分片,末片带行分隔 '\n')
//     └─ 0xA2B3 AUDIO NOTIFY             设备→Mac:一帧 = 100ms 音频。BLE 上是 804B
//                                        IMA ADPCM block(4B 首部+800B),USB 是 3200B PCM。
//                                        每片带 2B 帧头 [块序号][片序号|0x80 末片]:片级
//                                        丢失只损坏该块,后续块不失步(见 design.md §4 D4/D11)
//   ATT 载荷上限 = MTU-3(1 字节 opcode + 2 字节 handle),下行 WRITE 同此开销。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 常量 ----
#define ATT_MTU_MIN        23     // BLE 规范最小 ATT MTU(低于此值非法)
#define PAYLOAD_OVERHEAD   3      // ATT 载荷开销(opcode 1 + handle 2);载荷上限 = MTU-3
// AUDIO 分片帧头:[uint8 块序号(每帧 ++)][uint8 片序号 0..127 | 0x80 末片标志]。
// 为什么需要:重组按字节对齐,中间少一片会让此后所有块错位(静默毁掉整个会话)。
// 有帧头后 Mac 端能识别缺片、只丢该块并在下一块重新对齐,才敢"只丢片不丢帧"。
#define AUDIO_CHUNK_HDR    2
#define AUDIO_CHUNK_LAST   0x80   // 片序号字节的末片标志位
#define AUDIO_CHUNK_IDX_MASK 0x7F // 片序号取模 128(重组端同样按 128 取模比对)
#define AUDIO_FRAME_BYTES  3200   // 100ms @ 16kHz/16bit/mono(与 audio_streamer CHUNK_BYTES 对齐)
#define EVENT_LINE_MAX     512    // 事件行上限(与 APP_PROTO_TX_CAP 对齐)
#define AUDIO_SVC_UUID     0xA2B0 // GATT 服务 128-bit 基 UUID 的 16 位前缀
#define CTRL_CHR_UUID      0xA2B1
#define EVENT_CHR_UUID     0xA2B2
#define AUDIO_CHR_UUID     0xA2B3
#define CTRL_PAYLOAD_MAX   2048   // CTRL 写载荷上限(= APP_PROTO_RX_CAP;超长拒写)

// ---- 错误码 ----
typedef enum {
    BLE_AUDIO_OK = 0,
    BLE_AUDIO_DONE,           // 迭代已取完(仅 pack_next 返回)
    BLE_AUDIO_ERR_NULL,       // 空指针
    BLE_AUDIO_ERR_MTU,        // mtu < ATT_MTU_MIN
    BLE_AUDIO_ERR_EMPTY,      // 空帧/空行
    BLE_AUDIO_ERR_TOO_LONG,   // 事件行 > EVENT_LINE_MAX
    BLE_AUDIO_ERR_SMALL_CAP,  // chunk 描述数组容量不足
} ble_audio_err_t;

// ---- 纯函数 ----
// 帧长 + MTU → 分片总数;参数非法(空帧 / mtu < ATT_MTU_MIN)返回 0。
size_t ble_audio_chunk_count(size_t frame_len, uint16_t mtu);
// 第 idx 片(0 起)的字节数(末片为余量,恒 ≤ mtu-3);越界或参数非法返回 0。
size_t ble_audio_chunk_len(size_t frame_len, uint16_t mtu, size_t idx);

// ---- 一次性打包:写满 chunk 描述数组(指针零拷贝指向帧内部) ----
typedef struct {
    const uint8_t *data;
    size_t len;
} ble_audio_chunk_t;

// 帧 → 分片。chunks_cap 需 ≥ ble_audio_chunk_count(frame_len, mtu),
// 否则返回 BLE_AUDIO_ERR_SMALL_CAP。成功时 *chunk_count = 分片数;失败不写输出。
ble_audio_err_t ble_audio_pack_audio(ble_audio_chunk_t *chunks, size_t chunks_cap,
                                     const uint8_t *frame, size_t frame_len, uint16_t mtu,
                                     size_t *chunk_count);

// ---- 迭代打包(等 NOTIFY_TX 流控的 P2 发送循环用;迭代期间 frame 须保持有效) ----
typedef struct {
    const uint8_t *frame;   // 帧基址(零拷贝)
    size_t frame_len;       // 帧字节数
    size_t offset;          // 已取走的字节数
    uint16_t mtu;           // 已校验
} ble_audio_packer_t;

ble_audio_err_t ble_audio_pack_init(ble_audio_packer_t *p, const uint8_t *frame,
                                    size_t frame_len, uint16_t mtu);
// 取下一片:BLE_AUDIO_OK 时 *chunk/*chunk_len 指向当前片;取完返回 BLE_AUDIO_DONE(重复调用仍 DONE)。
ble_audio_err_t ble_audio_pack_next(ble_audio_packer_t *p, const uint8_t **chunk,
                                    size_t *chunk_len);

// ---- 事件行打包(app_protocol_* 序列化出的行,含结尾 '\n';同样按 MTU-3 分片) ----
// line_len 为行字节数(含 '\n');> EVENT_LINE_MAX 返回 BLE_AUDIO_ERR_TOO_LONG。
// 错误码/容量语义同 ble_audio_pack_audio。
ble_audio_err_t ble_audio_event_chunks(ble_audio_chunk_t *chunks, size_t chunks_cap,
                                       const char *line, size_t line_len, uint16_t mtu,
                                       size_t *chunk_count);

// ---- NimBLE 外设接口(实现于 ble_audio.c;无 IDF 依赖的返回码契约) ----
// 返回码约定:0 = 成功;非 0 = 失败(订阅缺失/参数非法/发送中断),调用方自行计数丢帧。

// 启动 NimBLE 主机、注册 GATT 服务(0xA2B0:CTRL/EVENT/AUDIO)并开始广播。
// 仿旧 ble_hid_keyboard_init 时序;广播名 "AI Passport"。失败返回非 0。
int ble_audio_init(void);

// 上行音频帧(通常 AUDIO_FRAME_BYTES):按协商 MTU 分片,等上一片 NOTIFY_TX 完成再发下一片。
// 无连接/未订阅 AUDIO 时返回非 0(上层计入掉帧,UI 走 BLE BUSY)。
int ble_audio_notify_audio(const uint8_t *frame, size_t len);

// 上行事件行(≤ EVENT_LINE_MAX,含结尾 '\n'):非阻塞入队,event_worker 任务
// 串行发送(≤MTU-3 单包直发,否则同一分片流控)。调用方缓冲可立即复用。
// 无连接/未订阅/队列满时返回非 0(内部累加 event 掉帧计数,console st 可查)。
int ble_audio_notify_event(const char *line, size_t len);
// 阻塞入队版本:队列满等 ≤timeout_ms 而非丢弃(voice.start/end 会话边界帧不丢,
// 审查 P2:事件队列 4 深在按键风暴期可丢 voice.end → Mac 端会话状态悬挂)。
// 队列不满时与普通版同开销(零等待);超时仍计数丢弃(队列持续拥塞 = 发送慢,
// 会话可对账,voice.end 有 status 帧兜底)。
int ble_audio_notify_event_blocking(const char *line, size_t len, uint32_t timeout_ms);

// ---- 状态查询(console `st` / 主循环) ----
bool ble_audio_connected(void);          // BLE 连接已建立
bool ble_audio_event_subscribed(void);   // EVENT 特征 CCCD 已订阅(链路通,link_up 依据)
uint16_t ble_audio_mtu(void);            // 当前连接协商 ATT MTU(无连接返回 0)
uint32_t ble_audio_audio_drops(void);    // 音频帧丢弃累计(无订阅/发送失败)
uint32_t ble_audio_event_drops(void);    // 事件行丢弃累计(无订阅/发送失败)

#ifdef __cplusplus
}
#endif
