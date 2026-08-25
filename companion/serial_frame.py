#!/usr/bin/env python3
"""USB 有线通道帧编解码(与固件 main/usb_link_framing.c 逐字节同构)。

帧格式:
  [magic 2B: 0xA5 0x5A][type 1B][payload_len 2B LE][payload][checksum 1B]
  checksum = 帧头+payload 全部字节和 mod 256;总帧长 = 6 + len, len ≤ 3200
  (任一方向最大合法载荷 = 音频帧;下行 CTRL 2048/SYS 128、上行 AUDIO 3200/
  EVENT 512/SYS_RESP 2048 全部 ≤3200;与固件 USB_FRAME_PAYLOAD_MAX 契约一致)。

失同步恢复:滑动重扫状态机(任何一步非法立即重扫且当前字节重新当 magic0 判;
0xA5 0xA5 0x5A 假锚点不丢帧;日志噪声/启动残帧只损失个别字节)。
改本模块必须同步改固件 usb_link_framing.c(两侧测试锁住:test_serial_frame.py
与 tests/test_usb_link.c 共享测试意图)。
"""

MAGIC0 = 0xA5
MAGIC1 = 0x5A
HEADER = 6

# 帧类型(与固件 USB_FRAME_* 契约一致)
FRAME_EVENT = 0x01     # 设备→PC: EVENT JSON 行(含 '\n')
FRAME_AUDIO = 0x02     # 设备→PC: 3200B 裸 PCM 帧
FRAME_CTRL = 0x03      # PC→设备: 协议 JSON 下行(不带 '\n',≤2048B)
FRAME_SYS = 0x04       # PC→设备: 控制台命令文本(≤128B)
FRAME_SYS_RESP = 0x05  # 设备→PC: 命令输出(≤2048B 超限截断)

TYPE_MIN, TYPE_MAX = FRAME_EVENT, FRAME_SYS_RESP

# 类型固有最大载荷(方向无关;与固件 USB_FRAME_*_MAX 契约一致,改一侧必须
# 同步另一侧 —— 两侧测试共享边界意图)。帧协议方向感知(审查 P1):解码器
# 按本端接收方向过滤合法类型 + 类型上限校验,均在写 payload 前拒绝。
TYPE_MAX_PAYLOAD = {
    FRAME_CTRL: 2048,
    FRAME_SYS: 128,
    FRAME_EVENT: 512,
    FRAME_AUDIO: 3200,
    FRAME_SYS_RESP: 2048,
}
PAYLOAD_MAX = max(TYPE_MAX_PAYLOAD.values())   # 总闸(任一类型上限)

# 本端接收方向(DIR_DOWN = 固件 RX 只收 CTRL/SYS;DIR_UP = companion RX 只收
# EVENT/AUDIO/SYS_RESP)。方向不符 → FRAME_ERR_BAD(写 payload 前拒绝)。
DIR_DOWN = "down"
DIR_UP = "up"
_DIR_TYPES = {DIR_DOWN: (FRAME_CTRL, FRAME_SYS),
              DIR_UP: (FRAME_EVENT, FRAME_AUDIO, FRAME_SYS_RESP)}

# 状态机状态(与固件 usb_frame_state_t 对应)
_S_MAGIC0, _S_MAGIC1, _S_TYPE, _S_LEN_LO, _S_LEN_HI, _S_PAYLOAD, _S_CHECKSUM = range(7)

# 返回码(与固件 usb_frame_feed_rc_t 对应)
FRAME_NONE = 0
FRAME_DONE = 1
FRAME_ERR_BAD = 2
FRAME_ERR_OVERSIZE = 3
FRAME_ERR_SUM = 4


class FrameError(Exception):
    """帧协议错误(组帧参数非法/超限)。"""


def encode_frame(frametype, payload):
    """组帧: bytes。len 超该类型上限抛 FrameError。"""
    payload = bytes(payload)
    limit = TYPE_MAX_PAYLOAD.get(frametype, 0)
    if len(payload) > limit:
        raise FrameError(f"payload {len(payload)}B 超 {frametype} 上限 {limit}B")
    head = bytes((MAGIC0, MAGIC1, frametype,
                  len(payload) & 0xFF, (len(payload) >> 8) & 0xFF))
    csum = (-(sum(head) + sum(payload))) & 0xFF
    return head + payload + bytes((csum,))


class FrameDecoder:
    """逐字节喂入的分帧解码器(与固件 usb_frame_feed 逐字节同构)。

    用法:
        dec = FrameDecoder(dir=DIR_UP)   # companion 永远收设备上行
        rc = dec.feed_byte(b)
        if rc == FRAME_DONE:
            use(dec.type, dec.payload)
    也提供 feed(bytes) 一次喂多字节,返回 (rcs 列表或最后一个 rc, 帧列表)。
    dir 默认 DIR_DOWN(=固件 RX 方向语义):显式传参,方向决定合法类型集合
    (方向不符 → FRAME_ERR_BAD,写 payload 前拒绝,审查 P1)。
    """

    def __init__(self, dir=DIR_DOWN):
        self._dir = dir
        self.reset()

    def reset(self):
        self._state = _S_MAGIC0
        self._sum = 0
        self._len = 0
        self._pos = 0
        self.type = 0
        self.payload = bytearray()

    def feed_byte(self, b):
        """单字节喂入,返回 FRAME_* 码;DONE 时 self.type/self.payload 有效。"""
        st = self._state
        if st == _S_MAGIC0:
            if b == MAGIC0:
                self._state = _S_MAGIC1
                self._sum = MAGIC0
            return FRAME_NONE
        if st == _S_MAGIC1:
            if b == MAGIC1:
                self._state = _S_TYPE
                self._sum = (self._sum + MAGIC1) & 0xFF
            else:
                self._rescan(b)
            return FRAME_NONE
        if st == _S_TYPE:
            if TYPE_MIN <= b <= TYPE_MAX and b in _DIR_TYPES[self._dir]:
                self._state = _S_LEN_LO
                self._sum = (self._sum + b) & 0xFF
                self.type = b
            else:
                self._rescan(b)
                return FRAME_ERR_BAD
            return FRAME_NONE
        if st == _S_LEN_LO:
            self._len = b
            self._sum = (self._sum + b) & 0xFF
            self._state = _S_LEN_HI
            return FRAME_NONE
        if st == _S_LEN_HI:
            self._len |= b << 8
            self._sum = (self._sum + b) & 0xFF
            if self._len > TYPE_MAX_PAYLOAD[self.type]:
                self._rescan(b)
                return FRAME_ERR_OVERSIZE
            self._pos = 0
            self.payload = bytearray()
            self._state = _S_CHECKSUM if self._len == 0 else _S_PAYLOAD
            return FRAME_NONE
        if st == _S_PAYLOAD:
            self.payload.append(b)
            self._sum = (self._sum + b) & 0xFF
            self._pos += 1
            if self._pos == self._len:
                self._state = _S_CHECKSUM
            return FRAME_NONE
        # _S_CHECKSUM
        if (self._sum + b) & 0xFF == 0:
            # 先保存结果再复位(DONE 后 self.type/payload 仍指向本帧,
            # 对齐 C 端输出参数语义:reset 后由调用者持有的拷贝仍有效)
            done_type, done_payload = self.type, bytes(self.payload)
            self.reset()
            self.type, self.payload = done_type, done_payload
            return FRAME_DONE
        self._rescan(b)
        return FRAME_ERR_SUM

    def feed(self, data):
        """一次喂多字节:返回 (最后一个返回码, DONE 帧列表)。"""
        frames = []
        rc = FRAME_NONE
        for b in data:
            rc = self.feed_byte(b)
            if rc == FRAME_DONE:
                frames.append((self.type, bytes(self.payload)))
        return rc, frames

    def _rescan(self, b):
        """滑动重扫:当前字节重新当 magic0 判,不整流丢弃。"""
        if b == MAGIC0:
            self._state = _S_MAGIC1
            self._sum = MAGIC0
        else:
            self._state = _S_MAGIC0
            self._sum = 0
