// main/adpcm.h —— IMA ADPCM 4:1 编解码(BLE 音频通道压缩)。
//
// 为什么是 ADPCM 而不是 Opus:BLE 上不去的真瓶颈是 mbuf 池(4×256B)+ "首片
// 失败即整帧作废",不是空口带宽(见任务 design.md §3)。修掉瓶颈后 8KB/s 只需
// 约 0.7 包/连接事件,Opus 多出的压缩率没有用处可花,却要 38.5KB 静态状态 +
// 16KB 栈 —— 恰好挤掉修瓶颈所需的堆。ADPCM 状态 4 字节、无堆、无 alloca。
//
// block 独立可解:每块头部携带 predictor(该块首样本原值)与自适应 index,
// 解码器只依赖头部,不依赖上一块 —— 丢一块只损失该块 100ms,不会污染后续。
// 编码器的 index 跨块延续(仅为音质:避免每块从 step_table[0] 重新爬升),
// 但既然快照进了头部,独立性不受影响;predictor 每块以首样本原值重置,
// 顺带消除跨块误差累积。
//
// 位序:低半字节先(与 WAV IMA ADPCM 一致),便于 Mac 端 Python 解码对齐。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADPCM_HEADER_BYTES   4      // [int16 LE predictor][uint8 index][uint8 rsv=0]
#define ADPCM_BLOCK_SAMPLES  1600   // 100ms @ 16kHz(= AUDIO_FRAME_BYTES / 2)
// 首样本走头部原值,其余 samples-1 个各占 4bit(奇数个时末字节高半字节填 0)
#define ADPCM_BLOCK_BYTES    (ADPCM_HEADER_BYTES + (ADPCM_BLOCK_SAMPLES / 2))   // 804

// 编码器自适应状态(4 字节,栈上即可;解码不需要跨块状态)
typedef struct {
    int16_t predictor;   // 上一次重建样本值
    uint8_t index;       // step_table 下标(0..88)
} adpcm_state_t;

// 复位到初始自适应状态。会话开始时调用(不跨会话携带上一次的静音自适应)。
void adpcm_state_reset(adpcm_state_t *st);

// 编码一个 block。samples 必须 ≥1;out_cap 不足或参数非法返回 0(不写越界)。
// 返回写入 out 的字节数 = ADPCM_HEADER_BYTES + samples/2(samples 为偶数时精确)。
// st 在返回时携带本块结束后的自适应 index,供下一块延续。
size_t adpcm_encode_block(adpcm_state_t *st, const int16_t *pcm, size_t samples,
                          uint8_t *out, size_t out_cap);

// 解码一个 block。samples = 该块原始样本数(协议固定:ADPCM_BLOCK_SAMPLES)。
// in_len / pcm_cap 不足或参数非法返回 0。返回写入 pcm 的样本数。
// 不需要外部状态:predictor/index 全部来自块头 —— 单块自洽。
size_t adpcm_decode_block(const uint8_t *in, size_t in_len, size_t samples,
                          int16_t *pcm, size_t pcm_cap);

#ifdef __cplusplus
}
#endif
