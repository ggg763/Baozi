"""VAD（语音活动检测）。

两种后端：
  * ``energy``  —— 纯能量 + 自适应噪声底，零额外依赖，中文连续语音够用；
  * ``webrtc``  —— webrtcvad 库，更抗噪，但要 pip install webrtcvad。

统一接口是 :class:`Vad`：反复喂 20ms 的块，它负责吐出
``SpeechStart`` / ``SpeechEnd``（带 preroll）事件。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Protocol

import numpy as np

from audio import SAMPLE_RATE, dbfs

FRAME_MS = 20
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000  # 320


# ---------------------------------------------------------------------
# 事件
# ---------------------------------------------------------------------
@dataclass
class SpeechStart:
    preroll: np.ndarray  # 说话前保留的音频


@dataclass
class SpeechEnd:
    pcm: np.ndarray  # 这一段完整语音
    reason: str  # "silence" | "maxlen"


# ---------------------------------------------------------------------
# 后端接口
# ---------------------------------------------------------------------
class VadBackend(Protocol):
    def is_speech(self, frame: np.ndarray) -> bool: ...

    def set_threshold_db(self, db: float) -> None: ...


class EnergyVad:
    """能量 VAD：用自适应噪声底 + 门限判断。

    安静时噪声底自己会降下来，突然有空调/风扇噪声它也会跟着抬高，
    所以同一套参数换房间基本不用重调。

    ⚠️ 支持【双门限】（2026-09 新增，解决"吞字/反应慢"）：
        - 判定"开始说话"用 threshold_db（更灵敏，避免把开头几个字漏掉）；
        - 判定"说话中/是否结束"用 hold_db（更保守，避免噪声把一句话拖长）。
      实测：单门限时要么噪音误触发、要么吞字，两者不可兼得。
    """

    # 自适应门限允许比绝对门限高多少 dB（见 is_speech 里的说明）。
    # 调大 -> 更抗环境噪声，但噪声底一高就会盖掉绝对门限、话尾判不出来；
    # 调小 -> 更听话尾，但嘈杂环境下容易在句子中间误判"说完了"。
    _ADAPTIVE_MARGIN_DB = 6.0

    def __init__(self, threshold_db: float = -42.0, floor_db: float = -70.0,
                 hold_db: float | None = None) -> None:
        self.threshold_db = float(threshold_db)
        # 说话中的判定门限；默认与开始门限相同（单门限行为）
        self.hold_db = float(hold_db) if hold_db is not None else float(threshold_db)
        self._floor_db = float(floor_db)  # 噪声底的估计值
        self._floor_init = False
        # 当前生效的是哪个门限（由 Vad 状态机在开始/持续阶段切换）
        self._active_is_start = True

    def set_threshold_db(self, db: float) -> None:
        self.threshold_db = float(db)

    def set_hold_db(self, db: float) -> None:
        self.hold_db = float(db)

    def set_phase_start(self) -> None:
        """切到"开始判定"阶段（用更灵敏的 threshold_db）。"""
        self._active_is_start = True

    def set_phase_hold(self) -> None:
        """切到"持续/结束判定"阶段（用更保守的 hold_db）。"""
        self._active_is_start = False

    @property
    def noise_floor_db(self) -> float:
        return self._floor_db

    def is_speech(self, frame: np.ndarray) -> bool:
        level = dbfs(frame)
        self.update_floor(level)
        return level > self.gate_db(level)

    def gate_db(self, level: float) -> float:
        """当前生效的门限（dBFS）。

        抽成独立方法是为了能被单独测试：is_speech() 有副作用（会更新噪声底），
        想验证"门限不会随噪声底无限上浮"就必须能在不改状态的前提下取值。
        """
        base = self.threshold_db if self._active_is_start else self.hold_db

        # 取【更严格】的那个：绝对门限始终是下限，同时保留"噪声底+8dB"。
        # 但"噪声底+8dB"必须有个上限，否则噪声底被抬太高时会把真人声也挡掉。
        #
        # ⚠️ 这个上限原来写的是 base + 12dB，实测不足（2026-11 故障）：
        #    麦克风增益过大 / 削顶时，噪声底会被抬到 -30dB 以上，
        #    此时 adaptive 能顶到 -10dB，直接【压过】hold_db(-22) ——
        #    于是"说话结束"永远判不出来，每句话都被拖到 vad_max_utterance_ms
        #    才切一刀，表现为"一直在听、一直不结束"。
        #    收紧到 +6dB：自适应只在噪声底真的接近人声时才介入，
        #    且不会把绝对门限完全架空。
        adaptive = min(self._floor_db + 8.0, base + self._ADAPTIVE_MARGIN_DB)
        return max(base, adaptive)

    def update_floor(self, level: float) -> None:
        """按当前帧电平更新噪声底估计（is_speech 内部会调）。"""
        if not self._floor_init:
            # ⚠️ 初始化不能直接等于第一帧电平（2026-10 实测故障）：
            #    如果程序启动时用户正在说话（或环境有噪音），噪声底会等于
            #    那个电平，之后 _floor_db + 8.0 永远高于信号 —— 于是 VAD
            #    一直认为"没人说话"，已经进入的 _in_speech 又攒不满静音帧，
            #    直接卡在 LISTENING 直到 maxlen。
            #    改成给一个保守的初始值（-55dB，典型安静房间），
            #    让噪声底从低处慢慢往上爬。
            self._floor_db = min(level, -55.0)
            self._floor_init = True
            return
        # 噪声底：下降快（跟得上环境变安静）、上升慢（不吞掉长句子尾部）
        if level < self._floor_db:
            self._floor_db += (level - self._floor_db) * 0.30
        else:
            self._floor_db += (level - self._floor_db) * 0.002

    @property
    def noise_floor_db(self) -> float:
        return self._floor_db


class WebrtcVad:
    """webrtcvad 后端：需要 webrtcvad-wheels 或 webrtcvad。"""

    def __init__(self, aggressiveness: int = 2, threshold_db: float = -60.0) -> None:
        try:
            import webrtcvad  # type: ignore
        except ImportError as exc:  # pragma: no cover
            raise RuntimeError(
                "webrtc VAD 需要先安装：pip install webrtcvad-wheels"
            ) from exc

        self._vad = webrtcvad.Vad(aggressiveness)
        self.threshold_db = float(threshold_db)

    def set_threshold_db(self, db: float) -> None:
        raise RuntimeError("webrtc VAD 后端不支持运行时改门限，请改 aggressiveness")

    def is_speech(self, frame: np.ndarray) -> bool:
        # webrtcvad 要求 16bit 单声道，长度必须是 10/20/30ms
        raw = np.ascontiguousarray(frame, dtype="<i2").tobytes()
        if len(raw) not in (320, 640, 960):
            # 补齐/截到 20ms
            target = FRAME_SAMPLES
            f = np.zeros(target, dtype=np.int16)
            n = min(target, frame.size)
            f[:n] = frame[:n]
            raw = f.astype("<i2").tobytes()
        return self._vad.is_speech(raw, SAMPLE_RATE)


# ---------------------------------------------------------------------
# 状态机
# ---------------------------------------------------------------------
class Vad:
    """把连续 PCM 流变成一句一句的话。

    送入的块长度必须尽量是 20ms 的整数倍（ESP32 就是按 20ms 发的）。
    不足一帧的部分会被内部缓存起来。
    """

    def __init__(
        self,
        backend: VadBackend,
        start_ms: int = 224,
        end_ms: int = 700,
        max_utterance_ms: int = 15000,
        preroll_ms: int = 300,
    ) -> None:
        self.backend = backend
        self.start_frames = max(1, start_ms // FRAME_MS)
        self.end_frames = max(1, end_ms // FRAME_MS)
        self.max_frames = max(self.start_frames, max_utterance_ms // FRAME_MS)
        self.preroll_frames = max(0, preroll_ms // FRAME_MS)

        self._leftover = np.zeros(0, dtype=np.int16)
        self._preroll: List[np.ndarray] = []
        self._speech: List[np.ndarray] = []
        self._in_speech = False
        self._trigger = 0  # 连续 speech 帧计数
        self._silence = 0  # 说话中连续静音帧计数
        # 双门限切换（backend 支持时才有意义）
        self._set_phase("start")

    def _set_phase(self, phase: str) -> None:
        """通知 backend 当前处于"开始判定"还是"持续判定"阶段。

        开始阶段用更灵敏的门限（别吞字），持续阶段用更保守的门限
        （别被噪声拖尾）。backend 不支持时静默忽略。
        """
        try:
            if phase == "start":
                self.backend.set_phase_start()
            else:
                self.backend.set_phase_hold()
        except AttributeError:
            pass

    # ---- 状态查询 ----
    @property
    def in_speech(self) -> bool:
        return self._in_speech

    @property
    def triggered(self) -> bool:
        """已经连续检测到若干帧语音，但还没正式进入 in_speech。"""
        return self._trigger > 0

    @property
    def preroll(self) -> List[np.ndarray]:
        return self._preroll

    def last_frame_db(self) -> float:
        if not self._preroll:
            return -120.0
        return dbfs(self._preroll[-1])

    # ---- 主入口 ----
    def feed(self, pcm: np.ndarray):
        """喂入 PCM，产出 SpeechStart / SpeechEnd 事件。"""
        if pcm.size:
            self._leftover = np.concatenate((self._leftover, pcm))

        while self._leftover.size >= FRAME_SAMPLES:
            frame = self._leftover[:FRAME_SAMPLES]
            self._leftover = self._leftover[FRAME_SAMPLES:]

            speech = self.backend.is_speech(frame)

            if not self._in_speech:
                # 维护 preroll（只留最近 N 帧）
                self._preroll.append(frame)
                if len(self._preroll) > self.preroll_frames:
                    self._preroll.pop(0)

                if speech:
                    self._trigger += 1
                    if self._trigger >= self.start_frames:
                        preroll = (
                            np.concatenate(self._preroll)
                            if self._preroll
                            else np.zeros(0, dtype=np.int16)
                        )
                        self._in_speech = True
                        self._silence = 0
                        self._speech = [preroll]
                        self._preroll = []
                        self._trigger = 0
                        self._set_phase("hold")     # 进入说话：换保守门限
                        yield SpeechStart(preroll=preroll)
                else:
                    self._trigger = 0
            else:
                self._speech.append(frame)

                if speech:
                    self._silence = 0
                else:
                    self._silence += 1

                total = sum(f.size for f in self._speech)
                frames = total // FRAME_SAMPLES

                if self._silence >= self.end_frames:
                    yield self._finish("silence")
                elif frames >= self.max_frames:
                    yield self._finish("maxlen")

    def _finish(self, reason: str) -> SpeechEnd:
        pcm = (
            np.concatenate(self._speech) if self._speech else np.zeros(0, dtype=np.int16)
        )
        if reason == "silence":
            # 掐掉尾部的静音（留一点点，避免把字的尾音削掉）
            keep_tail = FRAME_MS * 2 // FRAME_MS
            if len(self._speech) > keep_tail:
                self._speech = self._speech[:-keep_tail]
                pcm = np.concatenate(self._speech)
        self._in_speech = False
        self._trigger = 0
        self._silence = 0
        self._speech = []
        self._preroll = []
        self._set_phase("start")        # 说完一句，回到灵敏的"开始判定"
        return SpeechEnd(pcm=pcm, reason=reason)

    def reset(self) -> None:
        """打断时调用：把正在攒的这段丢掉。"""
        self._in_speech = False
        self._trigger = 0
        self._silence = 0
        self._speech = []
        self._preroll = []
        self._leftover = np.zeros(0, dtype=np.int16)


# ---------------------------------------------------------------------
def make_vad(cfg) -> Vad:
    """按配置造一个 VAD。cfg 里用到的字段：
    vad_backend / vad_threshold_db / vad_start_ms / vad_end_ms /
    vad_max_utterance_ms / vad_preroll_ms
    """
    backend_name = (cfg.vad_backend or "energy").lower()

    if backend_name == "webrtc":
        try:
            backend: VadBackend = WebrtcVad(aggressiveness=2)
        except RuntimeError as exc:
            print(f"[vad] {exc}，已自动回退到 energy 后端")
            backend = EnergyVad(
                threshold_db=getattr(cfg, "vad_start_threshold_db", cfg.vad_threshold_db),
                hold_db=getattr(cfg, "vad_end_threshold_db", None),
            )
    elif backend_name == "energy":
        # 双门限：开始判定用灵敏门限（别吞字），持续判定用保守门限（别拖尾）
        backend = EnergyVad(
            threshold_db=getattr(cfg, "vad_start_threshold_db", cfg.vad_threshold_db),
            hold_db=getattr(cfg, "vad_end_threshold_db", None),
        )
    else:
        raise ValueError(f"未知的 vad_backend: {cfg.vad_backend}")

    return Vad(
        backend,
        start_ms=cfg.vad_start_ms,
        end_ms=cfg.vad_end_ms,
        max_utterance_ms=cfg.vad_max_utterance_ms,
        preroll_ms=cfg.vad_preroll_ms,
    )


__all__ = [
    "Vad",
    "VadBackend",
    "EnergyVad",
    "WebrtcVad",
    "SpeechStart",
    "SpeechEnd",
    "make_vad",
    "FRAME_MS",
    "FRAME_SAMPLES",
]
