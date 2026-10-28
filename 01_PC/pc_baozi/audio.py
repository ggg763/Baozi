"""音频工具：环形缓冲、PCM 拼接、采样率转换、重采样到 16k。

只用标准库 + numpy。numpy 是硬依赖（一条 pip 命令）。
"""

from __future__ import annotations

import math
import threading
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16000


# ---------------------------------------------------------------------
# PCM 表示约定：numpy int16 一维数组（单声道）
# ---------------------------------------------------------------------
def pcm_from_bytes(data: bytes) -> np.ndarray:
    n = len(data) // 2
    return np.frombuffer(data[: n * 2], dtype="<i2").astype(np.int16, copy=True)


def pcm_to_bytes(pcm: np.ndarray) -> bytes:
    return np.ascontiguousarray(pcm, dtype="<i2").tobytes()


def pcm_duration_ms(pcm: np.ndarray, rate: int = SAMPLE_RATE) -> float:
    return len(pcm) * 1000.0 / rate


def dbfs(pcm: np.ndarray) -> float:
    """信号的 dBFS（满量程 = 0 dB）。空信号返回 -120。"""
    if pcm.size == 0:
        return -120.0
    x = pcm.astype(np.float32) / 32768.0
    rms = float(np.sqrt(np.mean(x * x)))
    return 20.0 * math.log10(rms) if rms > 1e-9 else -120.0


# ---------------------------------------------------------------------
# 重采样（线性插值）。上游源大多是 24k/32k/44.1k，转 16k 足够用。
# ---------------------------------------------------------------------
def resample(pcm: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if src_rate == dst_rate or pcm.size == 0:
        return pcm.astype(np.int16, copy=False)

    n_src = pcm.size
    n_dst = int(round(n_src * dst_rate / float(src_rate)))
    if n_dst <= 0:
        return np.zeros(0, dtype=np.int16)

    # 用 n_dst 个目标采样点对应的源坐标做线性插值
    src_idx = np.linspace(0.0, n_src - 1.0, n_dst, dtype=np.float64)
    i0 = np.floor(src_idx).astype(np.int64)
    i1 = np.minimum(i0 + 1, n_src - 1)
    frac = src_idx - i0

    x = pcm.astype(np.float64)
    out = x[i0] * (1.0 - frac) + x[i1] * frac
    return np.clip(np.rint(out), -32768, 32767).astype(np.int16)


def to_16k(pcm: np.ndarray, src_rate: int) -> np.ndarray:
    return resample(pcm, src_rate, SAMPLE_RATE)


# ---------------------------------------------------------------------
# 解码任意音频文件 -> 16k mono int16 pcm
# 用标准库 wave 处理 WAV；其余格式（edge-tts 的 MP3）交给 miniaudio/ffmpeg。
# ---------------------------------------------------------------------
def decode_audio_file(path: str | Path) -> np.ndarray:
    """读一个音频文件，返回 16k / 单声道 / int16 的 PCM。"""
    p = Path(path)

    if p.suffix.lower() == ".wav":
        return _decode_wav(p)

    # 非 WAV：先用 miniaudio（pip install miniaudio）解码
    try:
        import miniaudio  # type: ignore

        dec = miniaudio.decode_file(
            str(p), output_format=miniaudio.SampleFormat.SIGNED16
        )
        pcm = np.frombuffer(dec.samples, dtype="<i2")
        if dec.nchannels > 1:
            pcm = pcm.reshape(-1, dec.nchannels).mean(axis=1).astype(np.int16)
        return to_16k(pcm, dec.sample_rate)
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            f"解码 {p.name} 需要 miniaudio：pip install miniaudio"
        ) from exc


def _decode_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        nch = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())

    if width == 1:  # 8bit 是无符号的
        pcm = ((np.frombuffer(raw, dtype=np.uint8).astype(np.int16) - 128) << 8)
    elif width == 2:
        pcm = np.frombuffer(raw, dtype="<i2").astype(np.int16)
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
        v = np.where(v & 0x800000, v - 0x1000000, v)
        pcm = (v >> 8).astype(np.int16)
    elif width == 4:
        pcm = (np.frombuffer(raw, dtype="<i4") >> 16).astype(np.int16)
    else:
        raise RuntimeError(f"不支持的 WAV 位深: {width * 8} bit")

    if nch > 1:
        pcm = pcm.reshape(-1, nch).mean(axis=1).astype(np.int16)
    return to_16k(pcm, rate)


def encode_wav(pcm: np.ndarray, path: str | Path, rate: int = SAMPLE_RATE) -> None:
    """把 16k mono int16 PCM 存成 WAV（调试/存档用）。"""
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm_to_bytes(pcm))


# ---------------------------------------------------------------------
# 采集端的 PCM 环形缓冲：网络收包和 VAD 消费解耦
# ---------------------------------------------------------------------
class PcmRing:
    """按样本计的环形缓冲，满了丢最老的（实时场景宁可丢旧的也别阻塞）。

    线程安全：push/pop 都会重建内部数组，所以必须加锁。
    典型用法是一个线程 push、另一个线程 pop，不加锁会丢样本甚至切错数据。
    """

    def __init__(self, capacity_samples: int) -> None:
        self._cap = int(capacity_samples)
        self._buf = np.zeros(0, dtype=np.int16)
        self._lock = threading.Lock()
        self.dropped_samples = 0

    def __len__(self) -> int:
        with self._lock:
            return self._buf.size

    def push(self, pcm: np.ndarray) -> None:
        if pcm.size == 0:
            return
        with self._lock:
            self._buf = np.concatenate((self._buf, pcm.astype(np.int16, copy=False)))
            if self._buf.size > self._cap:
                self.dropped_samples += self._buf.size - self._cap
                self._buf = self._buf[-self._cap :]

    def pop(self, n: int) -> np.ndarray:
        with self._lock:
            n = min(int(n), self._buf.size)
            if n <= 0:
                return np.zeros(0, dtype=np.int16)
            out = self._buf[:n].copy()
            self._buf = self._buf[n:]
            return out

    def peek_tail(self, n: int) -> np.ndarray:
        """取最近 n 个样本（用于 VAD 的 preroll）。"""
        with self._lock:
            n = min(int(n), self._buf.size)
            return self._buf[-n:].copy() if n > 0 else np.zeros(0, dtype=np.int16)

    def clear(self) -> None:
        with self._lock:
            self._buf = np.zeros(0, dtype=np.int16)
