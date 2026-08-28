#!/usr/bin/env python3
"""SerialTransport 测试:os.openpty() 假设备侧(主端模拟固件串口)。

覆盖:ping/pong 握手、握手超时、订阅前缓冲(hello 在 start_notify 前到达)、
EVENT/AUDIO 上行分发、CTRL 下行到达、SYS 命令往返、master 关闭 → on_disconnect。
设备侧帧由 serial_frame 编解码,与固件逐字节同构。
"""
import asyncio
import os
import select
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from serial_frame import (FRAME_AUDIO, FRAME_CTRL, FRAME_DONE, FRAME_EVENT,
                          FRAME_SYS, FRAME_SYS_RESP, FrameDecoder, encode_frame)
from serial_transport import (AUDIO_UUID, CTRL_UUID, EVENT_UUID,
                              SerialError, SerialTransport)

PING = (FRAME_SYS, b"ping")


def read_frame(fd, timeout=1.0):
    """非阻塞主端:读到一帧完整帧返回 (type, payload);EOF/超时返回 None。

    dec 跨 chunk 保留半帧状态;select 可读但读空 = 主端 EOF。
    """
    dec = FrameDecoder()
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            return None
        rc, frames = dec.feed(chunk)
        if frames:
            return frames[-1]
    return None


def read_all_frames(fd, timeout=1.0):
    """读直到空闲(select 无新数据)或超时, 返回全部帧列表。

    dec 跨 chunk 保留半帧状态; 一次 os.read 可能含多帧(两帧紧连下行),
    必须全部取出——read_frame(单帧版)会丢 frames[-1] 之前的帧。
    """
    dec = FrameDecoder()
    frames = []
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            if frames:
                break            # 空闲: 已取完当前批
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            break
        rc, fs = dec.feed(chunk)
        frames.extend(fs)
    return frames


def make_pair():
    """返回 (master_fd, slave_fd, slave_port);主端非阻塞。"""
    master, slave = os.openpty()
    os.set_blocking(master, False)
    return master, slave, os.ttyname(slave)


def close_pair(master, slave):
    try:
        os.close(master)
    except OSError:
        pass
    try:
        os.close(slave)
    except OSError:
        pass


def write_all(fd, data):
    """非阻塞主端补写:pty 输入队列有限(实测短写 1022B),分片写完。

    真实固件同样分片写(USB 物理层逐块传输)——分片到达正是 transport 的
    解码器必须跨 read 保留状态的原因,测试故意保真。
    """
    while data:
        try:
            n = os.write(fd, data)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        assert n > 0
        data = data[n:]


async def connect_handshake(master, port, handshake_timeout=1.0):
    """connect → 主端收 ping → 回 pong → 返回已连接的 transport。"""
    t = SerialTransport(handshake_timeout=handshake_timeout)
    task = asyncio.create_task(t.connect(port))
    frame = await asyncio.to_thread(read_frame, master)
    assert frame == PING, f"握手应收到 SYS ping,实际 {frame}"
    os.write(master, encode_frame(FRAME_SYS_RESP, b"pong"))
    await asyncio.wait_for(task, 2.0)
    assert t._ser is not None and t._ser.is_open
    return t


async def wait_until(pred, timeout=1.0):
    """轮询等待谓词为真(handler 是异步投递的)。"""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return
        await asyncio.sleep(0.01)
    assert pred(), "等待超时"


def test_handshake():
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def test_connect_timeout():
    master, slave, port = make_pair()

    async def scenario():
        t = SerialTransport(handshake_timeout=0.3)
        try:
            await t.connect(port)
            assert False, "无 pong 应抛 SerialError"
        except SerialError:
            pass
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def test_pending_buffer():
    # hello 在 start_notify 前到达 → 缓冲 → 两个 handler 注册后冲刷
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        hello = b'{"type":"device.hello","proto":2}\n'
        write_all(master, encode_frame(FRAME_EVENT, hello))

        got = []
        await t.start_notify(EVENT_UUID, lambda p: got.append(p))
        await t.start_notify(AUDIO_UUID, lambda p: got.append(p))
        await wait_until(lambda: len(got) == 1)
        assert got == [hello]
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def test_dispatch_event_audio():
    # 注册后:EVENT 行 + AUDIO 帧上行分发
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        evts, auds = [], []
        await t.start_notify(EVENT_UUID, lambda p: evts.append(p))
        await t.start_notify(AUDIO_UUID, lambda p: auds.append(p))

        line = b'{"type":"voice.end","ok":true}\n'
        audio = bytes(range(256)) * 12 + bytes(range(128))  # 3200B
        assert len(audio) == 3200
        write_all(master, encode_frame(FRAME_EVENT, line))
        write_all(master, encode_frame(FRAME_AUDIO, audio))
        await wait_until(lambda: len(evts) == 1 and len(auds) == 1)
        assert evts == [line]
        assert auds == [audio]
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def test_ctrl_arrival():
    # PC→设备:CTRL 整帧写,主端收到帧级字节(协议组帧由 serial_frame 负责)
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        await t.start_notify(EVENT_UUID, lambda p: None)
        await t.start_notify(AUDIO_UUID, lambda p: None)

        ctrl = b'{"type":"voice.start","file":"a.wav"}'
        await t.write_gatt_char(CTRL_UUID, ctrl)
        frame = await asyncio.to_thread(read_frame, master)
        assert frame == (FRAME_CTRL, ctrl)
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def test_syscmd_roundtrip():
    # SYS 命令下行 → SYS_RESP 响应上行 → send_syscmd 返回文本
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        task = asyncio.create_task(t.send_syscmd("mode usb"))
        frame = await asyncio.to_thread(read_frame, master)
        assert frame == (FRAME_SYS, b"mode usb")
        os.write(master, encode_frame(FRAME_SYS_RESP,
                                      b"switching to USB: reboot"))
        resp = await asyncio.wait_for(task, 2.0)
        assert resp == "switching to USB: reboot"
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def test_master_close_triggers_disconnect():
    # 主端关闭(模拟拔线/设备复位):EOF → on_disconnect 回调
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        disc = asyncio.Event()
        t._on_disconnect = lambda: disc.set()
        os.close(master)
        await asyncio.wait_for(disc.wait(), 1.0)
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        os.close(slave)


def test_fixed_port_scan():
    # config usb_port 直连:scan_for_device 直接返回该路径,不枚举
    async def scenario():
        t = SerialTransport(port="/dev/cu.usbmodem1")
        addr = await t.scan_for_device("AI Passport", 1.0)
        assert addr == "/dev/cu.usbmodem1"

    asyncio.run(scenario())


def test_concurrent_write_serialized():
    # 审查 P1:并发 CTRL/SYS 写严格串行且不挂起。
    # 时序:首个写协程拿到 asyncio.Lock 并进入 to_thread;第二个协程 await 锁挂起
    # (不阻塞事件循环),第一个完成后释放,第二个继续。两帧完整到达、无字节交错。
    # threading.Lock 版本会确定性死锁:第二个协程同步 acquire 阻塞事件循环,
    # 持锁方 to_thread 完成回调无法调度 → gather 永不完成 → wait_for 超时炸出。
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        ctrl = b'{"type":"transcript","text":"hi","final":false}'
        sysc = b"mode usb"
        await asyncio.wait_for(asyncio.gather(
            t._write_frame(FRAME_CTRL, ctrl),
            t._write_frame(FRAME_SYS, sysc)), 2.0)
        frames = read_all_frames(master)
        assert sorted(frames) == sorted([(FRAME_CTRL, ctrl), (FRAME_SYS, sysc)]), \
            f"两帧应完整到达且不交错, 实际 {frames}"
        await t.disconnect()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def test_disconnect_does_not_block_loop():
    # 复核 R2:disconnect() 不得阻塞事件循环。同步 join(timeout=2) 在事件
    # 循环线程会卡住 ticker —— 异步轮询下 ticker 在 disconnect 挂起期间
    # 及时执行(≈0.15s),disconnect 总耗时远小于 join 上限 2s。
    master, slave, port = make_pair()

    async def scenario():
        t = await connect_handshake(master, port)
        ticked = []

        async def ticker():
            await asyncio.sleep(0.15)
            ticked.append(time.monotonic())

        t0 = time.monotonic()
        d = asyncio.ensure_future(t.disconnect())
        await ticker()                     # disconnect 挂起期间必须及时执行
        await d
        dt = time.monotonic() - t0
        assert ticked, "ticker 未执行(事件循环被阻塞)"
        assert dt < 1.0, f"disconnect 阻塞事件循环 {dt:.2f}s"
        assert t._reader is None or not t._reader.is_alive()

    try:
        asyncio.run(scenario())
    finally:
        close_pair(master, slave)


def main():
    tests = [test_handshake, test_connect_timeout, test_pending_buffer,
             test_dispatch_event_audio, test_ctrl_arrival,
             test_syscmd_roundtrip, test_master_close_triggers_disconnect,
             test_fixed_port_scan, test_concurrent_write_serialized,
             test_disconnect_does_not_block_loop]
    for t in tests:
        t()
        print(f"  {t.__name__} ok")
    print("test_serial_transport: 全部通过")


if __name__ == "__main__":
    main()
