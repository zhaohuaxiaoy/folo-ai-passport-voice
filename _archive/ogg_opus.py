# companion/ogg_opus.py —— Ogg/Opus 流封装(火山 v3 ASR format=ogg codec=opus 上行)。
#
# 设备 BLE 通道发送 [1B 长度][Opus 帧] TLV 记录;App 攒 5 帧(100ms)封装为
# 一个 Ogg 音频页上送。流起始自动先发 OpusHead + OpusTags 标识页(标准
# Ogg/Opus 解复用器必需,火山服务端按标准 demux)。
#
# granule position 语义(opus): 48kHz 采样单位,音频页 = 页末包结束时的
# 累计采样数。20ms 帧 @16kHz 输入 = 960 单位/帧。
#
# CRC-32 用 zlib.crc32(IEEE 802.3 标准多项式,与 Ogg 规范一致),CRC 字段
# 计算时置零,页组装后回填。
import struct
import zlib

# 单流固定序列号(同一流内必须不变;任意值均可)
SERIAL = 0x50415353          # "PASS"
PRE_SKIP = 312               # OpusHead pre-skip(48kHz 单位,标准值 6.5ms)
GRANULE_PER_FRAME = 960      # 20ms @16kHz → 48kHz 采样数


class OggOpusWriter:
    """Ogg/Opus 页封装器。一个会话一个实例(BLE 压缩路径)。"""

    def __init__(self, channels=1, input_rate=16000):
        self.channels = channels
        self.rate = input_rate
        self.seq = 0          # 页序号(BOS 起 0,音频页递增)
        self.granule = 0      # 已封装帧累计 granule(48kHz 单位)
        self.frames_sent = 0  # 已封装的 opus 帧数(统计口径)
        self._headers_sent = False

    def _page(self, packets, granule, type_flag):
        """组装一页: 27B 头 + 段表(每包一个 lacing,包 < 255B) + 包体。
        packets: 每包一个 Opus 帧(≤255B)。"""
        body = b"".join(packets)
        segs = bytes(len(p) for p in packets)
        prefix = (b"OggS\x00" + bytes([type_flag])
                  + struct.pack("<Q", granule)
                  + struct.pack("<I", SERIAL)
                  + struct.pack("<I", self.seq)
                  + b"\x00\x00\x00\x00"            # CRC 占位
                  + bytes([len(segs)]) + segs)
        crc = zlib.crc32(prefix + body) & 0xFFFFFFFF
        return prefix[:22] + struct.pack("<I", crc) + prefix[26:] + body

    def _header_pages(self):
        """OpusHead(19B) + OpusTags(最小: vendor 空 + 0 注释) 标识页。"""
        head = (b"OpusHead" + bytes([1, self.channels])
                + struct.pack("<H", PRE_SKIP)
                + struct.pack("<I", self.rate)
                + struct.pack("<h", 0)             # 输出增益 0
                + bytes([0]))                       # 声道映射族 0
        tags = b"OpusTags" + struct.pack("<I", 0) + struct.pack("<I", 0)
        bos = self._page([head], 0, 0x02)          # BOS 页
        self.seq += 1
        tags_page = self._page([tags], 0, 0x00)
        self.seq += 1                              # 每页一序(页序号必须严格递增)
        return bos, tags_page

    def add_frames(self, frames):
        """封装一批 Opus 帧(每帧一个包)为一个音频页。

        返回需上送的页列表: 首次调用含标识页(须在音频页之前发送)。
        """
        pages = []
        if not self._headers_sent:
            pages += self._header_pages()
            self._headers_sent = True
        self.granule += GRANULE_PER_FRAME * len(frames)
        self.frames_sent += len(frames)
        pages.append(self._page(list(frames), self.granule, 0x00))
        self.seq += 1
        return pages
