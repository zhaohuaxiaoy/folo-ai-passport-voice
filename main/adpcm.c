// main/adpcm.c —— IMA ADPCM 4:1 编解码实现(设计与位序见 adpcm.h)。
// 纯函数、无全局状态、无动态分配:可直接在 host 测试中跑往返与独立性用例。
#include "adpcm.h"

// IMA ADPCM 标准表(与 WAV / libsndfile / Mac 端 companion/adpcm.py 必须一致)
static const int16_t k_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t k_index_delta[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

#define INDEX_MAX 88

static inline int clamp_index(int idx) {
    if (idx < 0) return 0;
    if (idx > INDEX_MAX) return INDEX_MAX;
    return idx;
}

// int16 饱和:ADPCM 重建值可能越界,必须夹紧(否则回绕成反相爆音)
static inline int16_t clamp_s16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void adpcm_state_reset(adpcm_state_t *st) {
    if (!st) return;
    st->predictor = 0;
    st->index = 0;
}

// 单样本 → 4bit code,并就地推进 predictor/index(编解码必须走同一重建公式,
// 否则编码端与解码端的 predictor 会分叉 → 失真累积)。
static uint8_t encode_sample(adpcm_state_t *st, int16_t sample) {
    int step = k_step[st->index];
    int32_t diff = (int32_t)sample - (int32_t)st->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;          // 符号位
        diff = -diff;
    }
    int32_t tmp = step;
    if (diff >= tmp) { code |= 4; diff -= tmp; }
    tmp >>= 1;
    if (diff >= tmp) { code |= 2; diff -= tmp; }
    tmp >>= 1;
    if (diff >= tmp) { code |= 1; }

    // 与解码端逐字对应的重建
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    int32_t pred = (int32_t)st->predictor + ((code & 8) ? -diffq : diffq);
    st->predictor = clamp_s16(pred);
    st->index = (uint8_t)clamp_index((int)st->index + k_index_delta[code]);
    return code;
}

static int16_t decode_sample(adpcm_state_t *st, uint8_t code) {
    int step = k_step[st->index];
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    int32_t pred = (int32_t)st->predictor + ((code & 8) ? -diffq : diffq);
    st->predictor = clamp_s16(pred);
    st->index = (uint8_t)clamp_index((int)st->index + k_index_delta[code]);
    return st->predictor;
}

size_t adpcm_encode_block(adpcm_state_t *st, const int16_t *pcm, size_t samples,
                          uint8_t *out, size_t out_cap) {
    if (!st || !pcm || !out || samples == 0) return 0;
    const size_t need = ADPCM_HEADER_BYTES + samples / 2;
    if (out_cap < need) return 0;

    // 块头:首样本原值 + 当前自适应 index。predictor 每块以原值重置 ——
    // ①解码端无需上一块即可自洽 ②跨块误差不累积。
    st->predictor = pcm[0];
    out[0] = (uint8_t)((uint16_t)pcm[0] & 0xFF);
    out[1] = (uint8_t)(((uint16_t)pcm[0] >> 8) & 0xFF);
    out[2] = st->index;
    out[3] = 0;   // 保留:置 0,解码端不解释

    size_t w = ADPCM_HEADER_BYTES;
    // 首样本已在头部,数据区从第 2 个样本起;低半字节先。
    // 奇数个待编码样本时,末字节高半字节留 0(解码端按 samples 计数,不会读它)。
    for (size_t i = 1; i < samples; i++) {
        uint8_t code = encode_sample(st, pcm[i]);
        if ((i & 1) == 1) {
            out[w] = (uint8_t)(code & 0x0F);        // 低半字节:先写,占位
            if (i + 1 == samples) w++;              // 收尾:高半字节留 0
        } else {
            out[w] = (uint8_t)(out[w] | (uint8_t)(code << 4));
            w++;
        }
    }
    return need;
}

size_t adpcm_decode_block(const uint8_t *in, size_t in_len, size_t samples,
                          int16_t *pcm, size_t pcm_cap) {
    if (!in || !pcm || samples == 0) return 0;
    const size_t need = ADPCM_HEADER_BYTES + samples / 2;
    if (in_len < need || pcm_cap < samples) return 0;

    adpcm_state_t st;
    st.predictor = (int16_t)(uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    st.index = (uint8_t)clamp_index(in[2]);   // 越界头部按夹紧处理,不越表读
    pcm[0] = st.predictor;

    size_t r = ADPCM_HEADER_BYTES;
    for (size_t i = 1; i < samples; i++) {
        uint8_t code;
        if ((i & 1) == 1) {
            code = (uint8_t)(in[r] & 0x0F);
        } else {
            code = (uint8_t)((in[r] >> 4) & 0x0F);
            r++;
        }
        pcm[i] = decode_sample(&st, code);
    }
    return samples;
}
