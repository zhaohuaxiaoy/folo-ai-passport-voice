#!/usr/bin/env python3
"""火山引擎豆包流式语音识别(v3 双向流式)客户端。

协议出处: https://docs.volcengine.com/docs/6561/1354869(流式语音识别-大模型版)
- 端点: wss://openspeech.bytedance.com/api/v3/sauc/bigmodel
  (优化版二遍识别: .../api/v3/sauc/bigmodel_async + request.enable_nonstream=true,
  definite 定稿; 两种端点共用本客户端的帧格式与鉴权)
- 鉴权(新版控制台,单 API Key): 握手头 X-Api-Key / X-Api-Resource-Id /
  X-Api-Request-Id(UUID) / X-Api-Sequence(固定 -1)
- 二进制帧: 4B header + 4B 大端 payload size + payload
  byte0: protocol version(4b)=0001 | header size(4b)=0001
  byte1: message type(4b) | flags(4b)
  byte2: serialization(4b) | compression(4b)
  byte3: 保留 0x00
- 首帧 full client request(type=0001, JSON+Gzip); 音频帧 audio only
  request(type=0010, 无序列化+Gzip), 最后一包 flags=0b0010
- 服务端响应 type=1001(JSON+Gzip, 结果在 result.text, 每包音频回一包结果,
  末包结果 flags 带 0b0011), result.text 为全量累计文本
- 错误帧 type=1111: 4B 错误码 + 4B 错误信息长度 + UTF8 信息

两种用法:
  文件模式: python3 asr_client.py <pcm|wav 文件> [--ws-url ...] [--model ...]
  流式模式(relay 用): StreamingASR.connect() / send_frame() / send_end() /
  results() 产出 (text, is_final)
密钥来源: 同目录 config.local.json(git 忽略)的 volcano_api_key, 或环境变量
VOLCANO_API_KEY。勿打印/勿提交密钥。
"""
import argparse
import asyncio
import gzip
import io
import json
import os
import struct
import sys
import uuid

CHUNK_BYTES_100MS = 3200  # 100ms @ 16kHz/16bit/单声道(与固件音频块一致)

# 端点/模型默认值(与 config.example.json 对齐; config.local.json 可覆盖):
# 2026-08-24 本机 wav 实测 bigmodel_async + enable_nonstream 可用(见任务
# 报告), 故默认用优化版二遍识别; 若服务不可用回退改这里为 bigmodel。
DEFAULT_WS_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
DEFAULT_MODEL = "bigmodel_async"
DEFAULT_NONSTREAM = True
# 服务端帧上限(PERF P2-5): 语音 JSON 通常 <10KB; 无上限时畸形 gzip 可 OOM。
WS_MAX_SIZE = 256 * 1024
GZIP_MAX = 64 * 1024


# ---- 二进制帧 ----

def frame(msg_type, payload, flags=0, compression=1, serialization=1):
    """拼一帧: 4B header + 4B 大端 payload 长度 + payload。"""
    header = bytes([0x11, (msg_type << 4) | flags,
                    (serialization << 4) | compression, 0x00])
    return header + struct.pack(">I", len(payload)) + payload


def _safe_gunzip(payload, max_length=GZIP_MAX):
    """gzip 解压, 输出超过 max_length 则失败(防 zip bomb)。"""
    with gzip.GzipFile(fileobj=io.BytesIO(payload)) as f:
        chunks = []
        n = 0
        while True:
            b = f.read(8192)
            if not b:
                break
            n += len(b)
            if n > max_length:
                raise ValueError("gunzip exceeds max_length")
            chunks.append(b)
        return b"".join(chunks)


def parse_server(raw):
    """解析服务端帧 -> (kind, flags, data)。kind: result / error / unknown。

    服务端响应比客户端帧多一个 4B 大端 sequence 字段(官方文档 full server
    response 布局: Header + Sequence + Payload size + Payload):
      result: hdr + seq(4) + size(4) + payload(JSON, 与请求同压缩, 可 gzip)
      error : hdr + code(4) + size(4) + message(UTF8)
    """
    if len(raw) < 12:
        return ("unknown", 0, {"raw_len": len(raw)})
    hdr = raw[:4]
    mtype, flags = hdr[1] >> 4, hdr[1] & 0x0F
    comp = hdr[2] & 0x0F
    if mtype == 0x9:  # full server response
        size = struct.unpack(">I", raw[8:12])[0]
        if size > WS_MAX_SIZE:
            return ("unknown", flags, {"too_large": size})
        payload = raw[12:12 + size]
        if not payload:  # 空应答(如配置帧的 ack, 尚无结果)
            return ("result", flags, {})
        try:
            data = _safe_gunzip(payload) if comp == 0x1 else payload
            if not isinstance(data, (bytes, bytearray)):
                return ("result", flags, {})
            if len(data) > GZIP_MAX:
                return ("unknown", flags, {"too_large": len(data)})
            return ("result", flags, json.loads(data.decode("utf-8", "replace")))
        except json.JSONDecodeError:
            return ("result", flags, {})
        except ValueError:
            return ("unknown", flags, {"too_large": True})
        except (gzip.BadGzipFile, EOFError, OSError):
            return ("result", flags, {})
    if mtype == 0xF:  # error
        code = struct.unpack(">I", raw[4:8])[0]
        msize = struct.unpack(">I", raw[8:12])[0]
        if msize > GZIP_MAX:
            return ("error", flags, {"code": code, "message": "too_large"})
        msg = raw[12:12 + msize].decode("utf-8", "replace")
        return ("error", flags, {"code": code, "message": msg})
    return ("unknown", flags, {"mtype": mtype})


def build_full_request(cfg=None):
    """full client request。cfg 可选(volcano_model / volcano_enable_nonstream)。

    音频格式恒 pcm(ADPCM 压缩块在 relay 重组层已解回 16 kHz PCM;
    cfg["audio_format"] 由 relay 恒设为 "pcm")。
    """
    cfg = cfg or {}
    req = {
        "model_name": cfg.get("volcano_model", "bigmodel"),
        "enable_punc": True,
        "enable_itn": True,
    }
    if cfg.get("volcano_enable_nonstream"):
        req["enable_nonstream"] = True
    audio = {"format": "pcm", "rate": 16000, "bits": 16, "channel": 1}
    return {
        "user": {"uid": "ai-passport"},
        "audio": audio,
        "request": req,
    }


# ---- 配置 ----

def load_config():
    # 审查 P1-6: 与 fre_state 同源(frozen 打包时 config 落在用户数据目录,
    # 与模块同目录的旧逻辑在 _MEIPASS 临时目录写入必丢)。
    from fre_state import config_path
    cfg = {}
    p = config_path()
    if os.path.exists(p):
        with open(p, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    cfg.setdefault("volcano_api_key", os.environ.get("VOLCANO_API_KEY", ""))
    cfg.setdefault("volcano_ws_url", DEFAULT_WS_URL)
    cfg.setdefault("volcano_resource_id", "volc.bigasr.sauc.duration")
    cfg.setdefault("volcano_model", DEFAULT_MODEL)
    cfg.setdefault("volcano_enable_nonstream", DEFAULT_NONSTREAM)
    # Windows 移植(P4+):通道/注入参数, 缺省保持 macOS 现有行为
    cfg.setdefault("channel", "ble")            # "ble" | "usb"
    cfg.setdefault("inject_focus_delay", 2.0)   # Windows 注入:等用户切到目标窗口的秒数
    # FRE 向导 + 注入双策略(P7): unicode(键盘事件, 不碰剪贴板) / clipboard / auto
    cfg.setdefault("inject_mode", "auto")
    return cfg


# ---- 音频输入 ----

def load_pcm(path):
    """返回 (pcm_bytes, desc)。自动剥离 WAV 头, 否则按裸 PCM 处理。"""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] == b"RIFF" and raw[8:12] == b"WAVE":
        pos, data = 12, None
        while pos + 8 <= len(raw):
            cid = raw[pos:pos + 4]
            size = struct.unpack("<I", raw[pos + 4:pos + 8])[0]
            if cid == b"data":
                data = raw[pos + 8:pos + 8 + size]
                break
            pos += 8 + size + (size & 1)
        if data is None:
            raise ValueError("WAV 中没有 data 块")
        return data, "wav"
    return raw, "raw pcm"


# ---- 流式会话(relay 用) ----

RESULTS_Q_MAX = 32  # 结果队列有界: 下行变慢时丢中间结果, 定稿/错误保底不丢

class StreamingASR:
    """双向流式 ASR 会话: 边收音频边发, 边收结果边取。

    协议细节: 音频帧每包 3200B(100ms); send_frame 内部保持一帧延迟,
    保证真正的最后一块音频在 send_end() 时带结束标志(0b0010)发出。
    服务端每包音频回一包结果, result.text 为全量累计文本, 末包结果 flags
    带 0b0011(是末包)。每个实例只服务一次 voice 会话。

    用法:
      asr = StreamingASR(cfg)
      await asr.connect()
      consume = asyncio.create_task(asr.consume(loop))   # 见 relay.py
      await asr.send_frame(pcm) ...                      # 每帧 100ms
      await asr.send_end()
      await asr.close()
    """

    def __init__(self, cfg=None, chunk_bytes=CHUNK_BYTES_100MS):
        self.cfg = load_config() if cfg is None else cfg
        self.chunk_bytes = chunk_bytes
        self._ws = None
        self._recv_task = None
        self._results_q = asyncio.Queue(maxsize=RESULTS_Q_MAX)
        self._dropped_results = 0  # 队列满丢弃的中间结果数(有界:满则丢,见 _put_result)
        self._pending = None   # 末帧保底: 延迟一帧, 由 send_end 带结束标志发出
        self._closed = False

    # -- 连接 --

    def _open_ws(self):
        import websockets
        headers = {
            "X-Api-Key": self.cfg["volcano_api_key"],
            "X-Api-Resource-Id": self.cfg["volcano_resource_id"],
            "X-Api-Request-Id": str(uuid.uuid4()),
            "X-Api-Sequence": "-1",
        }
        # websockets 15+ 参数名是 additional_headers(旧版 extra_headers 会把
        # 未知 kwarg 透传到 create_connection 才报错, 本仓库锁 17.0.1)。
        return websockets.connect(self.cfg["volcano_ws_url"],
                                  additional_headers=headers,
                                  max_size=WS_MAX_SIZE)

    async def connect(self):
        if not self.cfg.get("volcano_api_key"):
            raise RuntimeError(
                "缺少火山 API Key: 配置 companion/config.local.json 的 "
                "volcano_api_key, 或设置环境变量 VOLCANO_API_KEY")
        self._ws = await self._open_ws()
        req = frame(0x1, gzip.compress(
            json.dumps(build_full_request(self.cfg)).encode("utf-8")))
        await self._ws.send(req)
        self._recv_task = asyncio.create_task(self._recv_loop())

    async def _put_result(self, item):
        """结果入队(有界): 队列满时丢弃中间结果(partial, 预览滞后无碍),
        定稿/错误帧保底不丢(await 入队——阻塞的只是本接收循环, 唯一消费者)。"""
        is_final = item[0] == "result" and item[2]
        try:
            self._results_q.put_nowait(item)
        except asyncio.QueueFull:
            if is_final or item[0] == "error":
                await self._results_q.put(item)
            else:
                self._dropped_results += 1

    async def _recv_loop(self):
        try:
            while True:
                raw = await self._ws.recv()
                kind, flags, data = parse_server(raw)
                if kind == "error":
                    await self._put_result(("error", data))
                elif kind == "result":
                    text = ((data.get("result") or {}).get("text")) or ""
                    await self._put_result(("result", text, bool(flags & 0x2)))
        except asyncio.CancelledError:
            raise
        except Exception as e:
            if not self._closed:
                await self._put_result(
                    ("error", {"code": "conn", "message": str(e)}))

    # -- 音频上行 --

    async def send_frame(self, pcm_bytes):
        """发送一个音频帧(不带结束标志)。内部保持末帧延迟。"""
        if self._pending is not None:
            await self._ws.send(frame(0x2, gzip.compress(self._pending),
                                      serialization=0x0))
        self._pending = bytes(pcm_bytes)

    async def send_end(self):
        """把最后一块音频带结束标志(0b0010)发出, 结束本次音频流。"""
        if self._pending is not None:
            await self._ws.send(frame(0x2, gzip.compress(self._pending),
                                      flags=0x2, serialization=0x0))
            self._pending = None
        else:
            # 零音频会话: 发一个空末帧收尾
            await self._ws.send(frame(0x2, gzip.compress(b""),
                                      flags=0x2, serialization=0x0))

    # -- 结果下行 --

    async def results(self):
        """产出 (text, is_final)。text 为全量累计文本; is_final=True 后流结束。

        服务端错误帧/连接中断抛 RuntimeError。调用方应在 is_final 后停止
        迭代(之后不会再出结果)。
        """
        while True:
            item = await self._results_q.get()
            if item[0] == "error":
                raise RuntimeError(
                    f"ASR 错误({item[1].get('code', '?')}): "
                    f"{item[1].get('message', '?')}")
            yield item[1], item[2]

    async def close(self):
        self._closed = True
        if self._recv_task is not None:
            self._recv_task.cancel()
            try:
                await self._recv_task
            except (asyncio.CancelledError, Exception):
                pass
        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:
                pass


async def asr_test_connection(cfg=None, timeout=8.0):
    """零音频握手验证 Key/端点(向导 ASR 配置页"测试连接"用)。

    流程: 开 WS(与 StreamingASR 同鉴权头) → 发 full client request →
    等首帧: result(含空 ack)→ 成功返回; error 帧 → RuntimeError(带 code);
    超时/连接被关 → RuntimeError。任何路径都不回显密钥。
    """
    # 与 load_config 合并: 保证 resource_id/ws_url/model 缺省(向导传入的
    # config.local.json 可能只含部分字段)
    merged = {**load_config(), **(cfg or {})}
    asr = StreamingASR(merged)
    ws = None
    try:
        ws = await asr._open_ws()
    except Exception as e:
        raise RuntimeError(f"WebSocket 连接失败: {e}") from e
    try:
        req = frame(0x1, gzip.compress(
            json.dumps(build_full_request(merged)).encode("utf-8")))
        await ws.send(req)
        raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
        kind, flags, data = parse_server(raw)
        if kind == "error":
            raise RuntimeError(
                f"ASR 鉴权失败({data.get('code', '?')}): "
                f"{data.get('message', '?')}")
        return data  # 空 ack: {}; 罕见直接回文本: {"result": {...}}
    except asyncio.TimeoutError as e:
        raise RuntimeError(f"等待 ASR 响应超时({timeout:.0f}s)") from e
    except RuntimeError:
        raise
    except Exception as e:
        raise RuntimeError(f"ASR 连接异常: {e}") from e
    finally:
        if ws is not None:
            try:
                await ws.close()
            except Exception:
                pass


# ---- 文件模式(回归保留) ----

async def transcribe(pcm, cfg, chunk_bytes=CHUNK_BYTES_100MS, on_partial=None):
    """文件模式: 流式上传 pcm(每包一帧), 返回最终识别文本。

    基于 StreamingASR 实现(同一帧格式/鉴权/结果路径), 供本机验证与测试。
    """
    asr = StreamingASR(cfg, chunk_bytes=chunk_bytes)
    await asr.connect()
    n = len(pcm)
    for i in range(0, n, chunk_bytes):
        await asr.send_frame(pcm[i:i + chunk_bytes])
    await asr.send_end()

    final_text = ""
    results = asr.results()
    try:
        while True:
            try:
                text, is_final = await asyncio.wait_for(
                    results.__anext__(), timeout=20)
            except StopAsyncIteration:
                break
            if text:
                final_text = text
                if on_partial:
                    on_partial(text)
            if is_final:
                break
    except asyncio.TimeoutError:
        pass  # 服务端未回末包标志: 以最后收到的文本收尾
    await asr.close()
    return final_text


# ---- CLI ----

def main():
    ap = argparse.ArgumentParser(description="火山豆包流式 ASR 本地测试(文件模式)")
    ap.add_argument("audio", help="PCM 或 WAV 文件(16kHz/16bit/单声道)")
    ap.add_argument("--resource", default=None,
                    help="覆盖资源 ID(默认 config 里的; ASR 2.0 用 "
                         "volc.seedasr.sauc.duration)")
    ap.add_argument("--chunk-ms", type=int, default=100,
                    help="分包大小(ms), 默认 100")
    ap.add_argument("--ws-url", default=None,
                    help="覆盖 WebSocket 端点(默认 config 里的)")
    ap.add_argument("--model", default=None,
                    help="覆盖模型名(bigmodel / bigmodel_async)")
    ap.add_argument("--nonstream", action="store_true",
                    help="开启 enable_nonstream(二遍识别, 配 bigmodel_async)")
    args = ap.parse_args()

    cfg = load_config()
    if args.resource:
        cfg["volcano_resource_id"] = args.resource
    if args.ws_url:
        cfg["volcano_ws_url"] = args.ws_url
    if args.model:
        cfg["volcano_model"] = args.model
    if args.nonstream:
        cfg["volcano_enable_nonstream"] = True

    pcm, desc = load_pcm(args.audio)
    dur = len(pcm) / 3200 * 0.1
    print(f"音频: {desc} {len(pcm)} B (约 {dur:.1f}s @16kHz)")
    print(f"端点: {cfg['volcano_ws_url']}  model={cfg.get('volcano_model')}  "
          f"nonstream={bool(cfg.get('volcano_enable_nonstream'))}")

    text = asyncio.run(transcribe(
        pcm, cfg,
        chunk_bytes=args.chunk_ms * 16000 * 2 // 1000,
        on_partial=lambda t: print(f"[partial] {t}")))
    print(f"[final] {text}")


if __name__ == "__main__":
    main()
