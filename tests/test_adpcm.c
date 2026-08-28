// tests/test_adpcm.c —— IMA ADPCM 编解码宿主测试。
// 覆盖 PRD 验收项:①往返 SNR 下限 ②块独立可解(丢任意块,其后各块逐字节一致)
// ③容量/非法参数不越界写读 ④与 companion/adpcm.py 的共享向量(位序对齐)。
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adpcm.h"

#define SAMPLES  ADPCM_BLOCK_SAMPLES
#define BLOCKS   5

// 语音状 16kHz 测试信号:基频 + 二次谐波 + 缓变包络(纯正弦对 ADPCM 过于友好)
static void make_signal(int16_t *pcm, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++) {
        double t = (double)(i + seed * n) / 16000.0;
        double env = 0.55 + 0.45 * sin(2.0 * M_PI * 3.0 * t);
        double v = 0.62 * sin(2.0 * M_PI * 220.0 * t)
                 + 0.28 * sin(2.0 * M_PI * 440.0 * t)
                 + 0.10 * sin(2.0 * M_PI * 1750.0 * t);
        pcm[i] = (int16_t)(env * v * 11000.0);
    }
}

static double snr_db(const int16_t *ref, const int16_t *got, size_t n) {
    double sig = 0.0, err = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s = (double)ref[i];
        double e = (double)ref[i] - (double)got[i];
        sig += s * s;
        err += e * e;
    }
    if (err == 0.0) return 999.0;
    return 10.0 * log10(sig / err);
}

// 1) 往返 SNR:4:1 压缩率下的失真下限。IMA ADPCM 对语音状信号典型 >20dB;
//    门槛设 18dB(留表实现差异余量),低于此说明位序/重建公式出错而非"音质略差"。
static void test_roundtrip_snr(void) {
    static int16_t pcm[SAMPLES], out[SAMPLES];
    static uint8_t blk[ADPCM_BLOCK_BYTES];
    adpcm_state_t st;
    adpcm_state_reset(&st);
    make_signal(pcm, SAMPLES, 0);

    assert(adpcm_encode_block(&st, pcm, SAMPLES, blk, sizeof(blk)) == ADPCM_BLOCK_BYTES);
    assert(adpcm_decode_block(blk, sizeof(blk), SAMPLES, out, SAMPLES) == SAMPLES);
    assert(out[0] == pcm[0]);   // 首样本走块头,必须精确

    double snr = snr_db(pcm, out, SAMPLES);
    printf("  ADPCM 往返 SNR = %.1f dB (压缩 %zu → %d 字节)\n",
           snr, sizeof(pcm), ADPCM_BLOCK_BYTES);
    assert(snr >= 18.0);
    assert(4 * ADPCM_BLOCK_BYTES <= (int)sizeof(pcm) + 4 * ADPCM_HEADER_BYTES);  // ≈4:1
}

// 2) 块独立:编码端 index 跨块延续(音质),但每块头部带快照 → 丢块不传染。
//    判据(PRD):丢弃任意一块后,其后各块解码结果与不丢块时逐字节一致。
static void test_block_independence(void) {
    static int16_t pcm[BLOCKS][SAMPLES];
    static uint8_t enc[BLOCKS][ADPCM_BLOCK_BYTES];
    static int16_t ref[BLOCKS][SAMPLES], got[SAMPLES];
    adpcm_state_t st;
    adpcm_state_reset(&st);
    for (int b = 0; b < BLOCKS; b++) {
        make_signal(pcm[b], SAMPLES, (unsigned)b);
        assert(adpcm_encode_block(&st, pcm[b], SAMPLES, enc[b], ADPCM_BLOCK_BYTES)
               == ADPCM_BLOCK_BYTES);
        assert(adpcm_decode_block(enc[b], ADPCM_BLOCK_BYTES, SAMPLES, ref[b], SAMPLES)
               == SAMPLES);
    }
    // 逐一丢弃某块:其余块解码结果必须与全收时逐字节相同
    for (int drop = 0; drop < BLOCKS; drop++) {
        for (int b = 0; b < BLOCKS; b++) {
            if (b == drop) continue;
            memset(got, 0, sizeof(got));
            assert(adpcm_decode_block(enc[b], ADPCM_BLOCK_BYTES, SAMPLES, got, SAMPLES)
                   == SAMPLES);
            assert(memcmp(got, ref[b], sizeof(got)) == 0);
        }
    }
    // 每块 SNR 都达标(不因 index 延续或块边界重置而塌陷)
    for (int b = 0; b < BLOCKS; b++) {
        double snr = snr_db(pcm[b], ref[b], SAMPLES);
        assert(snr >= 18.0);
    }
}

// 3) 边界与非法输入:不足容量返回 0(不写越界);头部 index 越界被夹紧不越表读。
static void test_bounds(void) {
    static int16_t pcm[SAMPLES], out[SAMPLES];
    static uint8_t blk[ADPCM_BLOCK_BYTES];
    adpcm_state_t st;
    adpcm_state_reset(&st);
    make_signal(pcm, SAMPLES, 3);

    assert(adpcm_encode_block(&st, pcm, SAMPLES, blk, ADPCM_BLOCK_BYTES - 1) == 0);
    assert(adpcm_encode_block(NULL, pcm, SAMPLES, blk, sizeof(blk)) == 0);
    assert(adpcm_encode_block(&st, NULL, SAMPLES, blk, sizeof(blk)) == 0);
    assert(adpcm_encode_block(&st, pcm, 0, blk, sizeof(blk)) == 0);

    adpcm_state_reset(&st);
    assert(adpcm_encode_block(&st, pcm, SAMPLES, blk, sizeof(blk)) == ADPCM_BLOCK_BYTES);
    assert(adpcm_decode_block(blk, ADPCM_BLOCK_BYTES - 1, SAMPLES, out, SAMPLES) == 0);
    assert(adpcm_decode_block(blk, ADPCM_BLOCK_BYTES, SAMPLES, out, SAMPLES - 1) == 0);
    assert(adpcm_decode_block(NULL, ADPCM_BLOCK_BYTES, SAMPLES, out, SAMPLES) == 0);

    blk[2] = 200;   // 非法 index(表长 89):必须夹紧,不越表读
    assert(adpcm_decode_block(blk, ADPCM_BLOCK_BYTES, SAMPLES, out, SAMPLES) == SAMPLES);

    // 奇数样本数:末字节高半字节留 0,长度按 samples/2 折算
    static int16_t odd_in[9];
    static int16_t odd_out[9];
    static uint8_t odd_blk[ADPCM_HEADER_BYTES + 4];
    for (int i = 0; i < 9; i++) odd_in[i] = (int16_t)(i * 300 - 1200);
    adpcm_state_reset(&st);
    assert(adpcm_encode_block(&st, odd_in, 9, odd_blk, sizeof(odd_blk))
           == ADPCM_HEADER_BYTES + 4);
    assert(adpcm_decode_block(odd_blk, sizeof(odd_blk), 9, odd_out, 9) == 9);
    assert(odd_out[0] == odd_in[0]);
}

// 4) 共享向量:固定输入的编码字节必须与 companion/adpcm.py 的同名向量一致
//    (位序/表/重建公式跨语言对齐;任何一端改动都会在这里断)。
static void test_shared_vector(void) {
    // 阶跃 + 斜坡 + 静音:覆盖 index 上爬、下降与符号翻转
    int16_t in[16] = { 0, 1000, 2000, 4000, 8000, 4000, 0, -4000,
                       -8000, -4000, 0, 100, 50, 0, -50, -100 };
    uint8_t blk[ADPCM_HEADER_BYTES + 8];
    int16_t out[16];
    adpcm_state_t st;
    adpcm_state_reset(&st);
    size_t n = adpcm_encode_block(&st, in, 16, blk, sizeof(blk));
    assert(n == ADPCM_HEADER_BYTES + 8);
    printf("  共享向量(与 companion/adpcm.py 比对):");
    for (size_t i = 0; i < n; i++) printf("%02x", blk[i]);
    printf("\n");
    // 跨语言契约字节。正确性由 SNR/独立性用例保证,此处只锁定位序与表,
    // 使 companion/adpcm.py 必须逐字节复现同一结果(任一端改动即在此断)。
    static const uint8_t k_expect[ADPCM_HEADER_BYTES + 8] = {
        0x00, 0x00, 0x00, 0x00, 0x77, 0x77, 0xe7, 0xff, 0x68, 0x08, 0x08, 0x08,
    };
    assert(memcmp(blk, k_expect, n) == 0);

    assert(adpcm_decode_block(blk, n, 16, out, 16) == 16);
    assert(out[0] == in[0]);
    printf("  解码样本(16,供 companion/test_adpcm.py 固化):");
    for (size_t i = 0; i < 16; i++) printf("%d,", out[i]);
    printf("\n");
    // 注意:step_index 从 0 起(step=7),对 0→8000 的阶跃只能逐步爬升,
    // 因此不能断言"4 个样本内追上"。只校验跟踪方向正确(上升段升、下降段降)。
    assert(out[4] > out[1] && out[4] > 0);
    assert(out[8] < out[4]);
}

int main(void) {
    test_roundtrip_snr();
    test_block_independence();
    test_bounds();
    test_shared_vector();
    printf("test_adpcm: OK\n");
    return 0;
}
