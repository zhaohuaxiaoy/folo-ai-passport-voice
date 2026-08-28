# companion/adpcm.py —— IMA ADPCM 4:1 编解码(Mac 端,与固件 main/adpcm.c 逐字节对齐)。
#
# 为什么是 ADPCM 而不是 Opus:BLE 上不去的真瓶颈是 mbuf 池 + "首片失败即整帧
# 作废",不是空口带宽(任务 design.md §3);8KB/s 只需 ~0.7 包/连接事件,Opus
# 多出的压缩率无处可花,却要 38.5KB 静态状态 + 16KB 栈。ADPCM 4:1、零状态。
#
# block 独立可解:每块头部携带 predictor(该块首样本原值)与自适应 index,
# 解码只依赖头部 —— 丢一块只损失该 100ms。位序低半字节先(与 WAV IMA ADPCM
# 一致)。表/重建公式与 C 端逐项相同,共享向量见 tests/test_adpcm.py 与
# tests/test_adpcm.c(任一端改动,共享向量用例即断)。

ADPCM_HEADER_BYTES = 4      # [int16 LE predictor][uint8 index][uint8 rsv=0]
ADPCM_BLOCK_SAMPLES = 1600  # 100ms @ 16kHz
ADPCM_BLOCK_BYTES = ADPCM_HEADER_BYTES + ADPCM_BLOCK_SAMPLES // 2  # 804

# IMA ADPCM 标准表(与 WAV / libsndfile / 固件 main/adpcm.c 必须一致)
K_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
K_INDEX_DELTA = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
]
INDEX_MAX = 88
S16_MAX = 32767
S16_MIN = -32768


def _clamp_index(idx):
    return 0 if idx < 0 else (INDEX_MAX if idx > INDEX_MAX else idx)


def _clamp_s16(v):
    if v > S16_MAX:
        return S16_MAX
    if v < S16_MIN:
        return S16_MIN
    return v


class AdpcmState:
    """编码器自适应状态(4 字节)。解码不需要跨块状态(头自带 predictor/index)。"""

    __slots__ = ("predictor", "index")

    def __init__(self):
        self.reset()

    def reset(self):
        self.predictor = 0
        self.index = 0


def _encode_sample(st, sample):
    """单样本 → 4bit code,并就地推进 predictor/index。

    编解码必须走同一重建公式,否则两端 predictor 分叉 → 失真累积。
    """
    step = K_STEP[st.index]
    diff = sample - st.predictor
    code = 0
    if diff < 0:
        code = 8          # 符号位
        diff = -diff
    tmp = step
    if diff >= tmp:
        code |= 4
        diff -= tmp
    tmp >>= 1
    if diff >= tmp:
        code |= 2
        diff -= tmp
    tmp >>= 1
    if diff >= tmp:
        code |= 1
    # 与解码端逐字对应的重建
    diffq = step >> 3
    if code & 4:
        diffq += step
    if code & 2:
        diffq += step >> 1
    if code & 1:
        diffq += step >> 2
    pred = st.predictor + (-diffq if (code & 8) else diffq)
    st.predictor = _clamp_s16(pred)
    st.index = _clamp_index(st.index + K_INDEX_DELTA[code])
    return code


def _decode_sample(st, code):
    step = K_STEP[st.index]
    diffq = step >> 3
    if code & 4:
        diffq += step
    if code & 2:
        diffq += step >> 1
    if code & 1:
        diffq += step >> 2
    pred = st.predictor + (-diffq if (code & 8) else diffq)
    st.predictor = _clamp_s16(pred)
    st.index = _clamp_index(st.index + K_INDEX_DELTA[code])
    return st.predictor


def encode_block(st, pcm, samples=None):
    """编码一块 PCM 为 bytes。pcm:样本序列(iterable);samples 缺省取全长。

    块头 = [int16 LE predictor = 首样本原值][uint8 index][0];数据区从第 2 个
    样本起,低半字节先;奇数个样本时末字节高半字节留 0(与 C 端一致)。
    """
    if samples is None:
        samples = len(pcm)
    if samples == 0:
        return b""
    pcm = list(pcm)
    first = pcm[0]
    st.predictor = _clamp_s16(first)
    out = bytearray(ADPCM_HEADER_BYTES + samples // 2)
    out[0] = first & 0xFF
    out[1] = (first >> 8) & 0xFF
    out[2] = st.index
    out[3] = 0
    w = ADPCM_HEADER_BYTES
    for i in range(1, samples):
        code = _encode_sample(st, pcm[i])
        if (i & 1) == 1:
            out[w] = code & 0x0F            # 低半字节先,占位
            if i + 1 == samples:
                w += 1                      # 收尾:高半字节留 0
        else:
            out[w] |= (code << 4) & 0xF0
            w += 1
    return bytes(out)


def decode_block_samples(data, samples):
    """解码一个 block 的前 samples 个样本。samples ≤ 1600,无需外部状态。

    越界头部 index 按夹紧处理(不越表读)。返回样本列表。
    """
    need = ADPCM_HEADER_BYTES + samples // 2
    if len(data) < need or samples == 0:
        return []
    predictor = data[0] | (data[1] << 8)
    if predictor > S16_MAX:
        predictor -= 0x10000               # int16 LE
    st = AdpcmState()
    st.predictor = predictor
    st.index = _clamp_index(data[2])
    out = [st.predictor]
    r = ADPCM_HEADER_BYTES
    for i in range(1, samples):
        if (i & 1) == 1:
            code = data[r] & 0x0F
        else:
            code = (data[r] >> 4) & 0x0F
            r += 1
        out.append(_decode_sample(st, code))
    return out


def decode_block(data):
    """解码完整 block(1600 样本)。"""
    return decode_block_samples(data, ADPCM_BLOCK_SAMPLES)
