#!/usr/bin/env python3
"""火山引擎豆包流式语音识别(v3 双向流式)客户端。

协议出处: https://docs.volcengine.com/docs/6561/1354869(流式语音识别-大模型版)
- 端点: wss://openspeech.bytedance.com/api/v3/sauc/bigmodel
- 鉴权(新版控制台,单 API Key): 握手头 X-Api-Key / X-Api-Resource-Id /
  X-Api-Request-Id(UUID) / X-Api-Sequence(固定 -1)
- 二进制帧: 4B header + 4B 大端 payload size + payload
  byte0: protocol version(4b)=0001 | header size(4b)=0001
  byte1: message type(4b) | flags(4b)
  byte2: serialization(4b) | compression(4b)
  byte3: 保留 0x00
- 首帧 full client request(type=0001, JSON+Gzip); 音频帧 audio only
  request(type=0010, 无序列化+Gzip), 最后一包 flags=0b0010
- 服务端响应 type=1001(JSON+Gzip, 结果在 result.text), 末包结果 flags 带 0b0011;
  错误帧 type=1111: 4B 错误码 + 4B 错误信息长度 + UTF8 信息

用法:
  python3 asr_client.py <pcm|wav 文件> [--resource volc.bigasr.sauc.duration] [--chunk-ms 100]
密钥来源: 同目录 config.local.json(git 忽略)的 volcano_api_key, 或环境变量 VOLCANO_API_KEY。
"""
import argparse
import asyncio
import gzip
import json
import os
import struct
import sys
import uuid

CHUNK_BYTES_100MS = 3200  # 100ms @ 16kHz/16bit/单声道(与固件音频块一致)


# ---- 二进制帧 ----

def frame(msg_type, payload, flags=0, compression=1, serialization=1):
    """拼一帧: 4B header + 4B 大端 payload 长度 + payload。"""
    header = bytes([0x11, (msg_type << 4) | flags,
                    (serialization << 4) | compression, 0x00])
    return header + struct.pack(">I", len(payload)) + payload


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
        payload = raw[12:12 + size]
        if not payload:  # 空应答(如配置帧的 ack, 尚无结果)
            return ("result", flags, {})
        data = gzip.decompress(payload) if comp == 0x1 else payload
        try:
            return ("result", flags, json.loads(data.decode("utf-8", "replace")))
        except (json.JSONDecodeError, gzip.BadGzipFile, EOFError):
            return ("result", flags, {})
    if mtype == 0xF:  # error
        code = struct.unpack(">I", raw[4:8])[0]
        msize = struct.unpack(">I", raw[8:12])[0]
        msg = raw[12:12 + msize].decode("utf-8", "replace")
        return ("error", flags, {"code": code, "message": msg})
    return ("unknown", flags, {"mtype": mtype})


def build_full_request():
    return {
        "user": {"uid": "ai-passport"},
        "audio": {"format": "pcm", "rate": 16000, "bits": 16, "channel": 1},
        "request": {"model_name": "bigmodel", "enable_punc": True, "enable_itn": True},
    }


# ---- 配置 ----

def load_config():
    cfg = {}
    here = os.path.dirname(os.path.abspath(__file__))
    p = os.path.join(here, "config.local.json")
    if os.path.exists(p):
        with open(p, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    cfg.setdefault("volcano_api_key", os.environ.get("VOLCANO_API_KEY", ""))
    cfg.setdefault("volcano_ws_url",
                   "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel")
    cfg.setdefault("volcano_resource_id", "volc.bigasr.sauc.duration")
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


# ---- 识别主流程 ----

async def transcribe(pcm, cfg, chunk_bytes=CHUNK_BYTES_100MS, on_partial=None):
    """流式上传 pcm(每包 发一收一), 返回最终识别文本。"""
    import websockets
    if not cfg["volcano_api_key"]:
        sys.exit("缺少火山 API Key: 配置 companion/config.local.json 的 "
                 "volcano_api_key, 或设置环境变量 VOLCANO_API_KEY")
    headers = {
        "X-Api-Key": cfg["volcano_api_key"],
        "X-Api-Resource-Id": cfg["volcano_resource_id"],
        "X-Api-Request-Id": str(uuid.uuid4()),
        "X-Api-Sequence": "-1",
    }
    try:
        connect = websockets.connect(cfg["volcano_ws_url"],
                                     extra_headers=headers, max_size=None)
    except TypeError:  # websockets <= 12 用 additional_headers
        connect = websockets.connect(cfg["volcano_ws_url"],
                                     additional_headers=headers, max_size=None)

    # 首帧: full client request; 之后每个音频包 3200B(100ms), 最后一包带结束标志
    frames = [frame(0x1, gzip.compress(json.dumps(
        build_full_request()).encode("utf-8")))]
    n = len(pcm)
    for i in range(0, n, chunk_bytes):
        is_last = (i + chunk_bytes >= n)
        frames.append(frame(0x2, gzip.compress(pcm[i:i + chunk_bytes]),
                            flags=0x2 if is_last else 0x0, serialization=0x0))

    final_text = ""
    async with connect as ws:
        for fr in frames:
            await ws.send(fr)
            raw = await asyncio.wait_for(ws.recv(), timeout=20)
            kind, flags, data = parse_server(raw)
            if kind == "error":
                raise RuntimeError(f"ASR 错误码 {data['code']}: {data['message']}")
            if kind == "result":
                text = ((data.get("result") or {}).get("text")) or ""
                if text:
                    final_text = text
                    if on_partial:
                        on_partial(text)
                if flags & 0x2:  # 末包结果
                    return final_text
        # 兜底: 服务端未回末包标志时, 再等一小段时间收尾
        try:
            while True:
                raw = await asyncio.wait_for(ws.recv(), timeout=5)
                kind, flags, data = parse_server(raw)
                if kind == "result":
                    text = ((data.get("result") or {}).get("text")) or ""
                    if text:
                        final_text = text
                    if flags & 0x2:
                        return final_text
        except asyncio.TimeoutError:
            return final_text


# ---- CLI ----

def main():
    ap = argparse.ArgumentParser(description="火山豆包流式 ASR 本地测试")
    ap.add_argument("audio", help="PCM 或 WAV 文件(16kHz/16bit/单声道)")
    ap.add_argument("--resource", default=None,
                    help="覆盖资源 ID(默认 config 里的; ASR 2.0 用 "
                         "volc.seedasr.sauc.duration)")
    ap.add_argument("--chunk-ms", type=int, default=100,
                    help="分包大小(ms), 默认 100")
    args = ap.parse_args()

    cfg = load_config()
    if args.resource:
        cfg["volcano_resource_id"] = args.resource
    pcm, desc = load_pcm(args.audio)
    dur = len(pcm) / 3200 * 0.1
    print(f"音频: {desc} {len(pcm)} B (约 {dur:.1f}s @16kHz)")
    print(f"资源: {cfg['volcano_resource_id']}")

    text = asyncio.run(transcribe(
        pcm, cfg,
        chunk_bytes=args.chunk_ms * 16000 * 2 // 1000,
        on_partial=lambda t: print(f"[partial] {t}")))
    print(f"[final] {text}")


if __name__ == "__main__":
    main()
