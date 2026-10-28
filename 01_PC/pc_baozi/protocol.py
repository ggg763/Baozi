"""ESP32 <-> PC 的帧协议（Python 侧实现）。

与 esp32_baozi/protocol.h 必须保持一致。

★ 2026-11 起传输介质改成 USB 串口（USB CDC）：
   ESP32 通过 USB 线插在跑本程序的电脑上，PC 与 ESP32 之间不再走 WiFi/TCP，
   PC 的网络只用于访问外部服务。帧格式本身没变，所以本文件只多了几个类型。

帧结构（全部小端）::

    偏移  长度  字段
    0     1     0xA5            magic0
    1     1     0x5A            magic1
    2     1      type           见 PKT_*
    3     1      flags           保留
    4     2     seq             uint16
    6     2     payload_len     uint16
    8     N     payload

串口是字节流，一次 read 可能拿到半帧或好几帧，所以用 FrameDecoder
累积 + 逐字节重新同步；payload_len 超限即视为流错位。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

MAGIC0 = 0xA5
MAGIC1 = 0x5A
HEADER_LEN = 8
MAX_PAYLOAD = 2048

# 8 字节头：<BBBBHH -> magic0, magic1, type, flags, seq, payload_len
_HEADER = struct.Struct("<BBBBHH")


class Pkt(IntEnum):
    MIC_PCM = 0x01  # ESP32 -> PC  麦克风 PCM（16k/16bit/mono）
    TTS_PCM = 0x02  # PC -> ESP32  待播放 PCM
    CTRL = 0x03  # 双向          控制/事件
    TEXT = 0x04  # PC -> ESP32  调试/识别文本
    LOG = 0x05  # ESP32 -> PC  日志文本（PC 端按行打 [esp] 前缀）
    CAM_JPEG = 0x06  # ESP32 -> PC  摄像头 JPEG 分片


class Ctrl(IntEnum):
    PLAY_BEGIN = 0x01
    PLAY_END = 0x02
    PLAY_ABORT = 0x03
    PING = 0x04
    PONG = 0x05
    HELLO = 0x06
    HELLO_ACK = 0x07
    AUDIO_STATE = 0x08
    EMOTION = 0x09  # PC -> ESP32  整行 "EMO:HAPPY"
    INTENT = 0x0A  # PC -> ESP32  整行 "INTENT:CHAT"
    TD_TEXT = 0x0B  # PC -> ESP32  任意一行，转发给 TD1601
    INTENSITY = 0x0C  # PC -> ESP32  整行 "INTENSITY:HIGH"
    STATE = 0x0D  # PC -> ESP32  (情绪,意图,强度) 打包成 1 字节
    CAM_REQ = 0x0E  # PC -> ESP32  请求抓一帧
    CAM_DONE = 0x0F  # ESP32 -> PC  一帧传完（见 cam 分片说明）


@dataclass(frozen=True)
class Frame:
    type: int
    seq: int
    payload: bytes


def encode(type_: int, payload: bytes = b"", seq: int = 0) -> bytes:
    """把一帧编码成字节串。"""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload 太大: {len(payload)} > {MAX_PAYLOAD}")
    return _HEADER.pack(MAGIC0, MAGIC1, int(type_), 0, seq & 0xFFFF, len(payload)) + payload


def encode_ctrl(cmd: int, extra: bytes = b"", seq: int = 0) -> bytes:
    return encode(Pkt.CTRL, bytes([int(cmd)]) + extra, seq=seq)


def encode_cam_done(ok: bool, total: int, req_id: int = 0, seq: int = 0) -> bytes:
    """ESP32 侧用：一帧 JPEG 发完后收尾。Python 端解码见 decode_cam_done()。"""
    extra = bytes([0 if ok else 1]) + int(total & 0xFFFFFFFF).to_bytes(4, "little")
    extra += bytes([req_id & 0xFF])
    return encode_ctrl(Ctrl.CAM_DONE, extra, seq=seq)


def decode_cam_done(payload: bytes) -> tuple[bool, int, int]:
    """解 CTRL_CAM_DONE：返回 (ok, 总字节数, 请求序号)。字段缺失时给保守默认。"""
    if len(payload) < 2:
        return False, 0, 0
    ok = payload[1] == 0
    total = int.from_bytes(payload[2:6], "little") if len(payload) >= 6 else 0
    req_id = payload[6] if len(payload) >= 7 else 0
    return ok, total, req_id


class FrameDecoder:
    """把字节流切成帧。

    用法::

        dec = FrameDecoder()
        for frame in dec.feed(chunk):
            ...
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self.bad_bytes = 0  # 统计丢弃的乱码字节，便于定位协议不一致

    def feed(self, data: bytes):
        """喂入新收到的字节，产出解析出来的 Frame。"""
        if data:
            self._buf.extend(data)

        while True:
            if len(self._buf) < HEADER_LEN:
                return

            if self._buf[0] != MAGIC0 or self._buf[1] != MAGIC1:
                # 逐字节重新同步
                self._buf.pop(0)
                self.bad_bytes += 1
                continue

            _, _, type_, _flags, seq, plen = _HEADER.unpack_from(self._buf, 0)

            if plen > MAX_PAYLOAD:
                self._buf.pop(0)
                self.bad_bytes += 1
                continue

            total = HEADER_LEN + plen
            if len(self._buf) < total:
                return

            payload = bytes(self._buf[HEADER_LEN:total])
            del self._buf[:total]
            yield Frame(type=int(type_), seq=seq, payload=payload)


def pcm_to_bytes(samples) -> bytes:
    """int16 numpy 数组 / bytes -> 小端字节串。"""
    if isinstance(samples, (bytes, bytearray, memoryview)):
        return bytes(samples)
    import numpy as np

    arr = np.asarray(samples)
    if arr.dtype != np.int16:
        arr = arr.astype(np.int16)
    return arr.tobytes()


def bytes_to_pcm(data: bytes):
    """小端字节串 -> int16 numpy 数组。"""
    import numpy as np

    n = len(data) // 2
    return np.frombuffer(data[: n * 2], dtype="<i2")
