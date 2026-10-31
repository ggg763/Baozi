"""对话流水线：VAD -> ASR -> LLM -> TTS。

设计成**与网络无关**的纯状态机：喂进麦克风 PCM，自己吐出要播的 PCM 和
各种状态事件。这样：
  * server.py 只负责搬运字节；
  * 自测脚本可以完全不连硬件、不连网络地跑完整条链路。
"""

from __future__ import annotations

import queue
import re
import threading
import time
from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path
from typing import Callable, Optional

import numpy as np

from audio import SAMPLE_RATE, PcmRing, encode_wav, pcm_duration_ms
from vad import SpeechEnd, SpeechStart, Vad, make_vad


class State(Enum):
    IDLE = auto()        # 等用户说话
    LISTENING = auto()   # 正在采集一句话
    THINKING = auto()    # ASR + LLM + TTS 中
    WAITING = auto()     # 话已备好，先等一下看用户会不会接着说
    SPEAKING = auto()    # TTS 音频正在推给 ESP32 播放


@dataclass
class Events:
    """流水线向外抛的事件（server 只关心前两个）。"""

    on_play_pcm: Optional[Callable[[np.ndarray], None]] = None   # 要推给 ESP32 播的音频
    on_play_begin: Optional[Callable[[], None]] = None           # 新一轮播放开始
    on_play_end: Optional[Callable[[], None]] = None             # 本轮播放结束
    on_play_abort: Optional[Callable[[], None]] = None           # 要求 ESP32 立刻停播
    on_partial: Optional[Callable[[np.ndarray], None]] = None    # VAD 判定的实时语音片段
    on_transcript: Optional[Callable[[str], None]] = None
    on_reply: Optional[Callable[[str], None]] = None
    on_emotion: Optional[Callable[[str], None]] = None   # LLM 给出的情绪 -> 下发 TD1601
    on_intent: Optional[Callable[[str], None]] = None    # LLM 给出的意图 -> 下发 TD1601
    on_intensity: Optional[Callable[[str], None]] = None  # 情感强度(low/medium/high) -> 下发 TD1601
    on_track: Optional[Callable[[bool], None]] = None     # 红外追踪开关 -> 下发 TD1601（TRACK:ON/OFF）
    on_state: Optional[Callable[[State], None]] = None
    on_log: Optional[Callable[[str], None]] = None

    def update(self, **handlers) -> None:
        """只覆盖显式给出的回调，其余保持不变。

        这样 server 接管流水线时不会把自测脚本挂的钩子覆盖掉。
        """
        for name, fn in handlers.items():
            if fn is None:
                continue
            if not hasattr(self, name):
                raise AttributeError(f"Events 没有字段 {name!r}")
            setattr(self, name, fn)


class Pipeline:
    """麦克风 PCM 进，TTS PCM 出。"""

    def __init__(self, cfg, asr, llm, tts, events: Optional[Events] = None,
                 vision=None) -> None:
        self.cfg = cfg
        self.asr = asr
        self.llm = llm
        self.tts = tts
        self.ev = events or Events()
        # 视觉（按需）：由 main.py 注入 VisionDescriber；为 None 时完全不看
        self._vision = vision

        self.vad: Vad = make_vad(cfg)

        # 麦克风环形缓冲：放 5 秒都绰绰有余
        self._mic = PcmRing(SAMPLE_RATE * 5)

        self._state = State.IDLE
        self._lock = threading.RLock()
        self._save_lock = threading.Lock()

        # 需要走 ASR/LLM/TTS 的句子放在队列里，另一个线程慢慢处理，
        # 这样 VAD 永远不会被模型推理卡住（对实时性很关键）。
        self._jobs: "queue.Queue[np.ndarray]" = queue.Queue()
        self._worker: Optional[threading.Thread] = None
        self._timer: Optional[threading.Thread] = None
        self._stop = threading.Event()

        # "先备好、等一会儿再开口"用：备好的 TTS 音频 + 等待起始时刻
        self._pending_speech: Optional[np.ndarray] = None
        self._wait_start = 0.0
        self._last_mic_time = time.monotonic()   # 最近一次收到麦克风数据的时刻

        # 声学回环屏蔽：播放期间（+结束后一段拖尾）丢弃麦克风数据。
        # 否则包子会听到自己说话 -> 当成用户输入 -> 自己跟自己对话。
        self._echo_mute_until = 0.0
        self._echo_dropped_ms = 0

        self._save_dir: Optional[str] = cfg.save_audio_dir or None
        self._save_idx = 0
        self._turn = 0

    # ------------------------------------------------------------------
    def start(self) -> None:
        if self._worker and self._worker.is_alive():
            return
        self._stop.clear()
        self._worker = threading.Thread(
            target=self._worker_loop, name="pipeline-worker", daemon=True
        )
        self._worker.start()
        # 另一个线程专管 WAITING 超时：等待窗口不能只靠 feed_mic() 推进，
        # 否则麦克风数据一断（比如网络抖动、设备重连）就永远等不到超时，
        # 包子会卡在 WAITING 一直不开口。
        self._timer = threading.Thread(
            target=self._wait_timer_loop, name="pipeline-wait-timer", daemon=True
        )
        self._timer.start()

    def stop(self) -> None:
        self._stop.set()
        self._jobs.put(np.zeros(0, dtype=np.int16))  # 唤醒队列
        if self._worker:
            self._worker.join(timeout=2.0)
            self._worker = None
        if self._timer:
            self._timer.join(timeout=2.0)
            self._timer = None

    def close(self) -> None:
        self.stop()
        for obj in (self.asr, self.llm, self.tts):
            close = getattr(obj, "close", None)
            if callable(close):
                close()

    # ------------------------------------------------------------------
    @property
    def state(self) -> State:
        with self._lock:
            return self._state

    def set_state(self, st: State) -> None:
        with self._lock:
            if self._state is st:
                return
            self._state = st
        self._emit_state(st)

    def _emit_state(self, st: State) -> None:
        if self.ev.on_state:
            self.ev.on_state(st)

    @property
    def busy(self) -> bool:
        """正在思考、正准备开口、或正在说话。"""
        return self.state in (State.THINKING, State.WAITING, State.SPEAKING)

    # ------------------------------------------------------------------
    # 主入口：喂麦克风音频（任意长度，但最好是 20ms 的整数倍）
    # ------------------------------------------------------------------
    def feed_mic(self, pcm: np.ndarray) -> None:
        if pcm.size == 0:
            return

        # ---- 声学回环屏蔽 ----
        # 包子说话期间（以及结束后 echo_guard_ms 的拖尾），喇叭的声音会被
        # 自己麦克风收进去。如果照常喂给 VAD，就会"听到自己说话" ->
        # 当成用户输入 -> 自己跟自己对话。
        # 这里直接丢弃，并且清掉 VAD 里可能残留的半句（否则恢复收音后
        # 会把屏蔽前那半句和之后的连起来）。
        if self._echo_muted():
            self._echo_dropped_ms += pcm.size * 1000 // SAMPLE_RATE
            if self.vad.in_speech or self.vad.triggered or self._mic_len() > 0:
                self.vad.reset()
                self._mic.clear()
            return

        self._last_mic_time = time.monotonic()
        self._mic.push(pcm)
        # 一次只消化缓冲里的数据；太大就分块，保证 VAD 的粒度稳定
        chunk_samples = max(320, 320 * 50)  # 最多 1 秒一批
        while len(self._mic) > 0:
            block = self._mic.pop(min(chunk_samples, len(self._mic)))
            self._process_block(block)

        # WAITING 状态的超时检查：等够了就把备好的话说出来
        self._check_wait_timeout()

    def _mic_len(self) -> int:
        return len(self._mic)

    def _echo_muted(self) -> bool:
        """现在是否处于"丢弃麦克风"的窗口内。

        ★ 2026-11 加上 THINKING：处理中（尤其看图那 10+ 秒的视觉+LLM）麦克风
        必须屏蔽，否则这段时间的环境噪声会被 VAD 录进去、模拟成"用户在说话"，
        反复触发 `语音结束(10000ms, maxlen)` 和 `识别 '.'`，把已经生成好的回复
        冲掉（用户实测："一问看到了什么，回复就被打断"）。
        SPEAKING 和播放拖尾原本就在屏蔽，这里补上 THINKING。
        """
        if self.state in (State.THINKING, State.SPEAKING):
            return True
        return time.monotonic() < self._echo_mute_until

    def _process_block(self, block: np.ndarray) -> None:
        for evt in self.vad.feed(block):
            if isinstance(evt, SpeechStart):
                self._on_speech_start(evt)
            elif isinstance(evt, SpeechEnd):
                self._on_speech_end(evt)

    # ------------------------------------------------------------------
    def _audio_frames_recent(self, ms: int = 200) -> bool:
        """最近 ms 毫秒内有没有收到过麦克风数据。"""
        return (time.monotonic() - self._last_mic_time) < (ms / 1000.0)

    def _wait_timer_loop(self) -> None:
        """独立推进 WAITING 超时（不依赖 feed_mic）。"""
        while not self._stop.is_set():
            self._check_wait_timeout()
            time.sleep(0.02)

    def _check_wait_timeout(self) -> None:
        """WAITING：备好话之后等的那个窗口，到点就开口。"""
        if self.state is not State.WAITING:
            return        # 保险：如果麦克风数据已经断了，再等下去也没意义（没人会打断我们），
        # 直接开口，别卡在这儿。
        stalled = not self._audio_frames_recent(500)
        if not stalled and (time.monotonic() - self._wait_start) < (self.cfg.pre_speak_wait_ms / 1000.0):
            return

        pending = self._pending_speech
        self._pending_speech = None
        if pending is None or pending.size == 0:
            self.set_state(State.IDLE)
            return

        if stalled:
            self._log("麦克风数据已断，不再等待，直接播放")
        else:
            self._log(f"等待 {self.cfg.pre_speak_wait_ms} ms 内用户未说话，开始播放")
        try:
            self._start_playback(pending)
            self._log("[diag] _start_playback 返回")
        except Exception as exc:  # noqa: BLE001
            self._log(f"!!! _start_playback 抛异常: {exc!r}")
            self.set_state(State.IDLE)

    # ------------------------------------------------------------------
    def _on_speech_start(self, evt: SpeechStart) -> None:
        # 正在播放时什么也不做：不打断、也不改状态。
        # 旧版本会在这里打断，把包子的话切成一两个字的碎片（VAD 门限太低，
        # 喇叭自身的声音都会被当成"用户说话"）。
        if self.state is State.SPEAKING:
            return

        # ⚠️ 注意：这里【不再】立刻取消已备好的回复。
        # 旧做法是"VAD 一触发就 _pending_speech = None"，结果环境里一声
        # 咳嗽/碰桌/键盘声就能把包子的话顶掉，回复永远说不出来。
        # 现在改成：WAITING 期间出现的较长声音只排队，由 worker 在 ASR 之后
        # 按【有没有实际内容】决定是否取消回复（见 _on_speech_end /
        # _handle_utterance），噪声再长也顶不掉回复。
        if self.state is not State.WAITING:
            self.set_state(State.LISTENING)

        if self.ev.on_partial and evt.preroll.size:
            self.ev.on_partial(evt.preroll)
        self._log("检测到语音开始")

    def _on_speech_end(self, evt: SpeechEnd) -> None:
        pcm = evt.pcm
        ms = pcm_duration_ms(pcm)
        self._log(f"语音结束（{ms:.0f} ms, 原因={evt.reason}）")

        # ---- WAITING 窗口里的判断：这到底算不算"用户还在说"？----
        # ⚠️ 这里【不能】按"时长够长"就直接取消已备好的回复（2026-10 实测故障）：
        #    环境噪声也能拖出 10 秒的 maxlen 片段，时长根本区分不了人和噪声；
        #    而 ASR 要 ~0.27s，250ms 的等待窗口里也来不及先识别再决定。
        #    所以改成：够长就只【排队】，取消与否交给 worker 在 ASR 之后判——
        #    有实际内容 -> 按"用户插话"取消回复去处理这句（见 _handle_utterance）；
        #    无内容（"。"之类）-> 丢弃并还原 WAITING，回复照常播出。
        if self.state is State.WAITING:
            if ms >= self.cfg.wait_cancel_min_ms:
                self._log(
                    f"等待期间出现一段 {ms:.0f} ms 的声音，排队待识别"
                    f"（有内容才算插话，噪声不会顶掉回复）"
                )
                self._save_audio(pcm, "mic")
                self._jobs.put(pcm)
            else:
                self._log(
                    f"等待期间只检测到 {ms:.0f} ms 的短促声音，"
                    f"不取消回复（阈值 {self.cfg.wait_cancel_min_ms} ms）"
                )
            return

        if ms < self.cfg.asr_min_ms:
            self._log(f"片段太短（{ms:.0f} ms < {self.cfg.asr_min_ms} ms），忽略")
            if not self.busy:
                self.set_state(State.IDLE)
            return

        self._save_audio(pcm, "mic")
        self._jobs.put(pcm)

    # ------------------------------------------------------------------
    def abort(self, why: str = "外部请求") -> None:
        """外部（比如 ESP32 断线、或你想手动停下）强制停止：清空待处理并静音。

        注意：这【不是】用来做"用户说话打断"的。自动打断已移除，
        因为它会把包子的话切成一两个字的碎片。
        """
        self._log(f"中止：{why}")
        self._pending_speech = None
        # 丢掉还没处理的句子，并让 ESP32 立刻闭嘴
        try:
            while True:
                self._jobs.get_nowait()
        except queue.Empty:
            pass
        self.vad.reset()
        self._mic.clear()
        if self.ev.on_play_abort:
            self.ev.on_play_abort()
        # 中止后也可能有喇叭余音，同样屏蔽一段
        self._echo_mute_until = time.monotonic() + (self.cfg.echo_guard_ms / 1000.0)
        self.set_state(State.IDLE)

    # ------------------------------------------------------------------
    def _worker_loop(self) -> None:
        while not self._stop.is_set():
            try:
                pcm = self._jobs.get(timeout=0.2)
            except queue.Empty:
                continue

            if pcm.size == 0:
                continue

            try:
                self._handle_utterance(pcm)
            except Exception as exc:  # 单句失败不能把整条链路弄死
                self._log(f"处理这句话时出错: {exc!r}")

    def _handle_utterance(self, pcm: np.ndarray) -> None:
        self._turn += 1
        t0 = time.perf_counter()

        # ---- ASR ----
        self.set_state(State.THINKING)
        text = self.asr.transcribe(pcm).strip()
        t_asr = time.perf_counter() - t0
        if not text:
            self._log("识别结果为空，忽略")
            self._settle_after_rejected_utterance()
            return

        self._log(f"识别({t_asr:.2f}s): {text!r}")
        if not self._has_content(text):
            # ⚠️ 不只是"空串"：噪音被 VAD 当成一句话时，SenseVoice 常常吐出
            #    一个孤立的标点（最常见"。"，也有"、"、"…"）。
            #    以前只挡空串，这些标点就穿过去喂给 LLM —— 包子对着一个句号
            #    瞎回一句（还配了情绪），白录播一轮。
            #    统一当作"没听清"忽略。
            self._log(f"识别结果无有效内容（{text!r}），忽略")
            self._settle_after_rejected_utterance()
            return

        # 这句话有实际内容。若上一轮回复已经备好但还没播出，按"用户插话"
        # 处理，取消那条回复 —— 和 _on_speech_end 里 WAITING 分支的语义一致。
        if self._pending_speech is not None:
            self._pending_speech = None
            self._log("用户插话（有实际内容），取消已备好的回复，转去处理这句")
        if self.ev.on_transcript:
            self.ev.on_transcript(text)

        # ---- 功能命令拦截：红外追踪开关 -------------------------------
        # 这类是"指令"而不是"聊天"，刻意【不走 LLM】：
        #   ① 省掉 1~2 秒推理，响应更跟手；
        #   ② 不写进对话历史 —— 否则包子会记着"用户让我开追踪"这种
        #      无意义上下文，把后面几轮真正的对话挤掉。
        # 命中就返回，后续的视觉/计算器/LLM 全部跳过。
        if self._handle_track_command(text):
            return
        # 拍照同样是指令：存图 + 回一句，不进 LLM
        if self._handle_photo_command(text):
            return

        # ---- 同音字纠正（热词）：把听到的名字改成已确认过的那个人 ----
        # 用户诉求：已确认"陈若语"后，再听到"陈若雨/陈若羽"应优先识别成陈若语。
        text = self._fix_known_names(text)

        # ---- 视觉（按需）：只有这句在"看图"时才抓图 + 描述 ----------
        # 平时摄像头只初始化不拍照，这里也不会产生任何开销；
        # 用户问"你看到了什么/这是什么"这类问题时才真正调用。
        vision_context = ""
        if self._vision is not None and hasattr(self.llm, "need_vision"):
            t_v = time.perf_counter()
            try:
                if self.llm.need_vision(text):
                    self._log("判断：这句话需要看图 -> 抓图 + 视觉描述")
                    v_text = self._vision.look()
                    if v_text:
                        vision_context = v_text
                        self._log(f"视觉({time.perf_counter() - t_v:.2f}s): {v_text}")
                    else:
                        self._log("视觉没拿到结果，按普通对话继续")
                else:
                    self._log("判断：不需要看图")
            except Exception as exc:  # noqa: BLE001
                self._log(f"视觉流程出错（忽略，继续对话）：{exc!r}")

        # ---- 计算器（按需）：话里有算式就直接用代码算好 ------------------
        # 7B 模型做多位数乘法不可靠（三位数×三位数经常错），但加减/简单题能对。
        # 这里把精确结果作为上下文注入，模型只负责"把答案说出来"，不负责算。
        calc_context = ""
        try:
            from calculator import calc_hint
            calc_context = calc_hint(text)
            if calc_context:
                self._log(f"计算器: {calc_context}")
        except Exception as exc:  # noqa: BLE001
            self._log(f"计算器出错（忽略）：{exc!r}")

        # ---- 联网检索（按需）：问天气就用代码去查实时数据 ----------------
        # 包子（本地模型）没有联网能力，问它天气只会编。这里查到真数据再注入。
        web_context = ""
        t_w = time.perf_counter()
        try:
            if getattr(self.cfg, "weather_enabled", True):
                from weather import weather_hint
                web_context = weather_hint(
                    text,
                    default_city=getattr(self.cfg, "weather_default_city", ""),
                    timeout_s=getattr(self.cfg, "weather_timeout_s", 6.0),
                )
            if web_context:
                self._log(f"天气检索({time.perf_counter() - t_w:.2f}s): "
                          f"{web_context.splitlines()[0]}")
        except Exception as exc:  # noqa: BLE001
            self._log(f"联网检索出错（忽略）：{exc!r}")

        # ---- 报时 / 报日期（按需）：钟点是确定性事实，让代码给，不让模型猜 ----
        # 提示词状态行里也写了时间，但 7B 常常视而不见（问"几点了"回"大概下午"）；
        # "还有多久到六点"这种要算的，模型更是不知道现在几点。这里纯本地，零耗时。
        time_context = ""
        try:
            from clock import time_hint
            time_context = time_hint(text)
            if time_context:
                self._log(f"报时: {time_context}")
        except Exception as exc:  # noqa: BLE001
            self._log(f"报时出错（忽略）：{exc!r}")

        # ---- LLM（文本 + 情绪 + 意图；有画面就把画面一起给它）----------
        t1 = time.perf_counter()
        try:
            reply, emotion, intent, intensity = self.llm.chat_ex(
                text, vision_context, calc_context, web_context, time_context)
            reply = reply.strip()
        except Exception as exc:
            # 大模型出错必须让人看见，否则表现就是"说了话但喇叭一直不出声"
            self._log(f"!!! 大模型调用失败: {exc}")
            self.set_state(State.IDLE)
            return
        t_llm = time.perf_counter() - t1
        if not reply:
            self._log("大模型返回为空，忽略")
            self.set_state(State.IDLE)
            return

        self._log(f"回复({t_llm:.2f}s): {reply}  [情绪={emotion} 意图={intent} 强度={intensity}]")
        # 提示词里写了「reply 不超过 50 字」，但 7B 模型不一定听话。
        # 超了就记一笔 —— 这是判断"要不要在代码里硬截断"的唯一依据，
        # 顺便也能看出合成预算是不是被拉长了。
        if len(reply) > 60:
            self._log(f"⚠️ 回复 {len(reply)} 字，超过 50 字上限"
                      f"（本句合成预算 {4 + 0.02 * len(reply):.1f}s）")
        if self.ev.on_reply:
            self.ev.on_reply(reply)
        # 情绪/意图/强度：交给上层下发 TD1601（行为层）+ 写回 PC 情绪内存
        if self.ev.on_emotion:
            self.ev.on_emotion(emotion)
        if self.ev.on_intent:
            self.ev.on_intent(intent)
        if self.ev.on_intensity:
            self.ev.on_intensity(intensity)

        # ---- TTS ----
        t2 = time.perf_counter()
        self._log(f"[diag] TTS 开始合成（{len(reply)} 字）")
        try:
            speech = self.tts.synthesize(reply)
        except Exception as exc:
            self._log(f"!!! 语音合成失败: {exc}")
            fb = self._fallback_speech()
            if fb is not None:
                self._log("合成失败，改播本机预生成的兜底话术")
                speech = fb
            else:
                self.set_state(State.IDLE)
                return
        t_tts = time.perf_counter() - t2
        self._log(f"[diag] TTS 返回：{speech.size} 采样点，耗时 {t_tts:.2f}s")

        if speech.size == 0:
            fb = self._fallback_speech()
            if fb is not None:
                self._log("TTS 没有产出音频，改播本机预生成的兜底话术")
                speech = fb
            else:
                self._log("TTS 没有产出音频，且无兜底话术")
                self.set_state(State.IDLE)
                return

        self._save_audio(speech, "tts")
        self._log(
            f"合成({t_tts:.2f}s): {pcm_duration_ms(speech):.0f} ms 音频, "
            f"总耗时 {time.perf_counter() - t0:.2f}s"
        )

        # ---- 备好话，先等一会儿再开口 ----
        # 不直接播放，而是进入 WAITING：给用户 pre_speak_wait_ms 的时间插话。
        # 如果这段时间他开口了，就把这段话丢掉重新听（见 _on_speech_start）；
        # 他一直没说话，超时后由 _check_wait_timeout() 真正开始播放。
        self._pending_speech = speech
        self._wait_start = time.monotonic()
        self.set_state(State.WAITING)
        self._log(f"回复已备好，等待 {self.cfg.pre_speak_wait_ms} ms 看用户是否还要说")

    # ------------------------------------------------------------------
    #: 被视为"没有有效内容"的整串：ASR 对噪音最常见的输出。
    #: 注意只匹配【整串就是这些】，不做包含匹配 —— 用户真说过"嗯"时
    #: 也应该正常回话，不能把有意义的应答吃掉。
    _EMPTY_LIKE = {"", "。", "，", ",", ".", "、", "…", "···", "...", "——",
                   "？", "?", "！", "!", "；", ";", "：", ":", "～", "~",
                   "的", "呃", "啊", "哦", "噢", "嗯", "唔", "呀", "唉"}

    @classmethod
    def _has_content(cls, text: str) -> bool:
        """这段识别结果有没有"实际内容"？

        目的是挡掉噪音被 VAD 误判成一句话后 ASR 的产物 —— 通常是空串或
        一个孤立标点（"。"）。判据保守，宁可漏挡也不误杀：

          · 一个汉字/字母/数字都没有 -> 无内容（"。" "，" "..." 都中）
          · 整串正是上面那张表的语气词 -> 无内容（"嗯。" "呃" 这种）
          · 其余一律放行（"好" "不" "嗯嗯" 这类短应答是有效对话）
        """
        if not text:
            return False
        t = text.strip()
        if not t:
            return False
        # 连一个实义字符都没有 -> 纯标点/空白
        if not re.search(r"[\w\u4e00-\u9fff]", t):
            return False
        # 整串是纯语气词（去掉标点后再比）
        bare = re.sub(r"[\W_]+", "", t, flags=re.UNICODE)
        if bare in cls._EMPTY_LIKE:
            return False
        return True

    def _settle_after_rejected_utterance(self) -> None:
        """一条话被判定为噪声/无内容后，把状态放到"该去的地方"。

        ⚠️ 关键修复（2026-10 实测故障："看图后回复被噪声冲掉，包子不说话"）：
        worker 在处理上一轮（视觉+LLM+TTS 要十几秒）时麦克风不屏蔽，
        环境噪声会被 VAD 排进 _jobs。等回复备好、状态设成 WAITING 后，
        worker 会立刻取出这条噪声任务，第一行 set_state(THINKING) 就把
        WAITING 踩掉了；随后 ASR 认出"。"无内容，旧代码 set_state(IDLE)
        收尾 —— _check_wait_timeout() 只在 WAITING 才肯播放，于是备好的
        话永远说不出来。这里必须把状态【还原回 WAITING】，让等待定时器
        把话说完；噪声任务本身则安静地丢掉。

        另外两种情况不去踩：SPEAKING（回复已经开始播了，别把状态拉回
        WAITING 造成二次播放）；LISTENING（用户真在说话，别打断采集）。
        """
        st = self.state
        if st is State.SPEAKING or st is State.LISTENING:
            return
        # ★ 2026-10 之后改为【直接丢弃】已备好的回复：
        #    识别无内容 = 这条"话"是噪声，不是用户说话。既然用户没真的说话，
        #    就没理由把上一轮备好的回复播出来 —— 那会变成"包子自说自话"，
        #    而且上一轮回复往往还带着 ASR 听错的人名，纯属乱说话（用户实测抱怨）。
        self._pending_speech = None
        self.set_state(State.IDLE)

    # ------------------------------------------------------------------
    # 红外追踪开关：语音指令 -> TRACK:ON / TRACK:OFF
    # ------------------------------------------------------------------
    # 为什么用关键词而不是让 LLM 判断：
    #   · 这是一条"控制命令"，语义固定，关键词既准又快（省 1~2 秒推理）；
    #   · 走 LLM 会被它的人格干扰 —— 7B 模型经常把"开启追踪"当成一句闲聊
    #     来回应，而不是稳定地输出一个动作标记，实测不稳定得多。
    # 代价：说法要落在这张表里。换说法时往表里加一行即可。

    # 显式短语（命中即判）。长短语在前、短语在后，靠 in 判断子串。
    _TRACK_OFF_PHRASES = (
        "停止红外追踪", "关闭红外追踪", "关掉红外追踪", "结束红外追踪",
        "停止追踪", "关闭追踪", "关掉追踪", "结束追踪",
        "停止跟随", "关闭跟随", "停止跟踪", "关闭跟踪",
        "别盯着我", "不要盯着我", "不用盯着我",
        "别跟着我", "不要跟着我", "不用跟着我",
        "别追了", "不要追了", "不用追了",
        "别看了", "不要看了",
    )
    _TRACK_ON_PHRASES = (
        "开启红外追踪", "打开红外追踪", "开始红外追踪", "启动红外追踪",
        "红外追踪开启", "红外追踪打开",
        "开启追踪", "打开追踪", "开始追踪", "启动追踪",
        "开启跟随", "开始跟随", "开启跟踪", "开始跟踪",
        "盯着我", "看着我", "跟着我", "追着我", "追我",
    )
    # 否定/停止词 + 对象词 -> 一律判"关闭"。
    # 这一条是为了兜住表里没列全的说法（如"不要再开启追踪了"）。
    # ⚠️ 刻意不放"别"：中文里"别人"含"别"，会和"追踪"凑成误判。
    _TRACK_NEG = ("不要", "不用", "停止", "关闭", "关掉", "结束")
    _TRACK_OBJ = ("追踪", "跟随", "跟踪", "盯着")

    @classmethod
    def _match_track_command(cls, text: str) -> Optional[bool]:
        """是不是追踪开关命令。True=开启 / False=关闭 / None=不是命令。"""
        # 归一化：ASR 可能给出"开启 红外追踪。"这种带空格/标点/大小写的文本
        t = re.sub(r"[\s\W_]+", "", text, flags=re.UNICODE).lower()
        if not t:
            return None

        # ① 先判"关闭"：否定/停止词 + 对象词。
        #    放在开启前面，是为了让"不要开启追踪"这类句子正确判为关闭
        #    （否则会被 ② 里的"开启追踪"抢先命中）。
        for neg in cls._TRACK_NEG:
            if neg not in t:
                continue
            for obj in cls._TRACK_OBJ:
                if obj in t:
                    return False

        # ② 显式短语
        for p in cls._TRACK_OFF_PHRASES:
            if p in t:
                return False
        for p in cls._TRACK_ON_PHRASES:
            if p in t:
                return True
        return None

    def _handle_track_command(self, text: str) -> bool:
        """命中追踪开关命令 -> 下发 + 说一句固定回应 -> 返回 True。"""
        want = self._match_track_command(text)
        if want is None:
            return False

        self._log(f"追踪命令：{'开启' if want else '停止'}")
        if self.ev.on_track:
            try:
                self.ev.on_track(want)
            except Exception as exc:  # noqa: BLE001
                self._log(f"追踪命令下发失败（忽略）：{exc!r}")

        # 固定话术：指令类交互不交给 LLM，避免它把"开追踪"当闲聊回应。
        reply = ("好呀，我开始盯着你了。" if want
                 else "好，那我不看你了。")
        self._speak_reply(reply,
                          "happy" if want else "neutral",
                          "command",
                          "medium" if want else "low")
        return True

    # ------------------------------------------------------------------
    # 语音拍照：把当前画面存一张到 pc_baozi/photos/
    # ------------------------------------------------------------------
    # 和追踪开关一样属于"指令"，同样不走 LLM。
    # 用的是视觉模块里缓存的实时流帧 —— 不碰 ESP32 的播放链路。
    _PHOTO_PHRASES = (
        "拍照", "拍张照", "拍一张", "拍个照", "拍下来",
        "照一张", "照张相", "来张照片", "帮我拍", "给我拍",
    )

    @classmethod
    def _match_photo_command(cls, text: str) -> bool:
        """是不是「拍照」指令。"""
        t = re.sub(r"[\s\W_]+", "", text, flags=re.UNICODE).lower()
        if not t:
            return False
        return any(p in t for p in cls._PHOTO_PHRASES)

    def _handle_photo_command(self, text: str) -> bool:
        """命中拍照指令 -> 存图 + 回一句 -> 返回 True。"""
        if not self._match_photo_command(text):
            return False

        if self._vision is None:
            self._log("拍照命令：视觉未启用（--no-vision）")
            self._speak_reply("我现在还没装上眼睛呢。",
                              "sad", "command", "low")
            return True

        self._log("拍照命令：抓取当前画面存盘")
        path = None
        try:
            path = self._vision.take_photo()
        except Exception as exc:  # noqa: BLE001
            self._log(f"拍照失败（忽略）：{exc!r}")

        if not path:
            self._speak_reply("这会儿没拍到画面。",
                              "sad", "command", "low")
            return True

        n = 0
        try:
            n = self._vision.photo_count()
        except Exception:  # noqa: BLE001
            pass
        reply = f"好，拍好了，这是第 {n} 张。" if n else "好，拍好了。"
        self._speak_reply(reply, "happy", "command", "medium")
        return True

    # ------------------------------------------------------------------
    def _fix_known_names(self, text: str) -> str:
        """把句子里**同音的已知人名**纠正成已确认的写法。

        例：已知"陈若语"，说"陈若雨今天来吗" -> "陈若语今天来吗"。

        只在有已确认的人物档案时才动手；出错不动原文（不能因为纠正功能
        让整句话废掉）。
        """
        mem = getattr(self.llm, "memory", None)
        if mem is None or not text:
            return text
        try:
            known = mem.list_people()          # 已确认的"其他人"名字
            if not known:
                return text
            from name_fix import rewrite_names
            fixed = rewrite_names(text, known)
            if fixed != text:
                self._log(f"同音字纠正: {text}  ->  {fixed}")
            return fixed
        except Exception as exc:  # noqa: BLE001
            self._log(f"同音字纠正出错（忽略）：{exc!r}")
            return text

    # ------------------------------------------------------------------
    def _fallback_speech(self) -> "np.ndarray | None":
        """合成失败时，从 TTS 后端取一段本机预生成的兜底话术（assets/fallback/*.wav）。

        返回 None 表示没有兜底可用（TTS 不是 CachedFallbackTts，或它也没备成）。
        """
        getter = getattr(self.tts, "fallback_pcm", None)
        if not callable(getter):
            return None
        try:
            pcm = getter()
        except Exception as exc:  # noqa: BLE001
            self._log(f"取兜底话术失败：{exc!r}")
            return None
        if pcm is None or pcm.size == 0:
            return None
        return pcm

    def _speak_reply(self, reply: str, emotion: str, intent: str,
                     intensity: str) -> None:
        """把一句话走完"上报 + 下发情绪 + TTS + 等插话"的收尾流程。

        正常对话和"名字确认"这类短路回复都用它，避免两处逻辑漂移。
        """
        if self.ev.on_reply:
            self.ev.on_reply(reply)
        if self.ev.on_emotion:
            self.ev.on_emotion(emotion)
        if self.ev.on_intent:
            self.ev.on_intent(intent)
        if self.ev.on_intensity:
            self.ev.on_intensity(intensity)

        t2 = time.perf_counter()
        try:
            speech = self.tts.synthesize(reply)
        except Exception as exc:
            self._log(f"!!! 语音合成失败: {exc}")
            fb = self._fallback_speech()
            if fb is not None:
                self._log("合成失败，改播本机预生成的兜底话术")
                speech = fb
            else:
                self.set_state(State.IDLE)
                return
        if speech.size == 0:
            fb = self._fallback_speech()
            if fb is not None:
                self._log("TTS 没有产出音频，改播本机预生成的兜底话术")
                speech = fb
            else:
                self._log("TTS 没有产出音频，且无兜底话术")
                self.set_state(State.IDLE)
                return

        self._save_audio(speech, "tts")
        self._log(f"合成({time.perf_counter() - t2:.2f}s): "
                  f"{pcm_duration_ms(speech):.0f} ms 音频")
        self._pending_speech = speech
        self._wait_start = time.monotonic()
        self.set_state(State.WAITING)

    # ------------------------------------------------------------------
    def _start_playback(self, speech: np.ndarray) -> None:
        """真正把备好的 TTS 音频推给 ESP32。"""
        # 只有确认有上层负责推流时才进 SPEAKING：否则一旦没人调用
        # notify_play_done()，状态会永远卡在 SPEAKING，之后就不再响应说话了。
        if self.ev.on_play_pcm is None:
            self._log("没有 on_play_pcm 回调，跳过播放")
            self.set_state(State.IDLE)
            return

        # 从这一刻起丢弃麦克风（真正开始屏蔽在进入 SPEAKING 时生效，
        # 这里同时清一下 VAD，避免把"等待期间"拾到的半句带进去）
        self.vad.reset()
        self._mic.clear()
        self._echo_dropped_ms = 0

        self.set_state(State.SPEAKING)
        try:
            if self.ev.on_play_begin:
                self.ev.on_play_begin()
            self.ev.on_play_pcm(speech)
        except Exception as exc:
            # 推流起不来时必须把状态放回 IDLE，否则整条链路就死了
            self._log(f"启动播放失败: {exc!r}")
            self.set_state(State.IDLE)
            return

    def say(self, text: str) -> bool:
        """播报一段与对话无关的文本（提醒 / 通知），复用同一条 TTS→播放链路。

        返回 True = 已经交给播放链路（on_play_begin + on_play_pcm，
        VoiceServer 会按 ESP32 的缓冲水位限速推流）；False = 没播。

        什么时候返回 False：
          · 没有 TTS / 空文本；
          · 正在对话（THINKING / WAITING / SPEAKING）—— 提醒不插话，
            否则会把用户这一轮的回答搅乱；
          · 没人负责推流（on_play_pcm 为空，比如没接 ESP32）。
        调用方（app/link.py 的 speak）拿到 False 就只弹卡不出声。
        """
        if not text or self.tts is None:
            return False
        if self.state in (State.THINKING, State.WAITING, State.SPEAKING):
            self._log(f"提醒播报跳过（当前状态 {self.state.name}）")
            return False
        if self.ev.on_play_pcm is None:
            return False
        try:
            speech = self.tts.synthesize(text)
        except Exception as exc:  # noqa: BLE001 —— 合成失败不该影响主链路
            self._log(f"提醒合成失败: {exc!r}")
            fb = self._fallback_speech()
            if fb is None:
                return False
            self._log("提醒合成失败，改播本机预生成的兜底话术")
            speech = fb
        if speech is None or not len(speech):
            fb = self._fallback_speech()
            if fb is None:
                return False
            self._log("提醒 TTS 没有产出音频，改播本机预生成的兜底话术")
            speech = fb
        self._start_playback(speech)
        return True

    def say_pcm(self, pcm: np.ndarray, label: str = "固定音频") -> bool:
        """直接播一段**已经准备好**的 PCM，不经过 TTS。

        用途：开机自我介绍（assets/boot_hello.wav）。它必须和 TTS / 网络
        完全解耦 —— 开机时网络可能还没起来，但"我上线了"这句话无论如何
        都得说出来，顺便当整条音频链路（PC -> ESP32 -> 喇叭）的自检。

        返回 False 的两种情况与 say() 一致：正在对话中，或没人负责推流。
        """
        if pcm is None or not len(pcm):
            return False
        if self.state in (State.THINKING, State.WAITING, State.SPEAKING):
            self._log(f"{label}播报跳过（当前状态 {self.state.name}）")
            return False
        if self.ev.on_play_pcm is None:
            self._log(f"{label}没有播放通路，跳过")
            return False
        self._save_audio(pcm, "boot")
        self._start_playback(pcm)
        return True

    def notify_play_done(self) -> None:
        """由上层（server）在“ESP32 那边播完了”时调用。

        只负责把状态从 SPEAKING 切回 IDLE。**不重置 VAD**：
        播放期间用户可能已经开始说下一句了，重置会把那句话的头切掉。
        """
        if self.state is State.SPEAKING:
            if self.ev.on_play_end:
                self.ev.on_play_end()

            # 开一段"回环拖尾屏蔽"：ESP32 那边可能还在放缓冲里的余音，
            # 而且声音传到麦克风也有延迟。这段时间继续丢弃麦克风，
            # 否则喇叭尾音会被当成用户说话，包子又自己接自己的话。
            self._echo_mute_until = time.monotonic() + (self.cfg.echo_guard_ms / 1000.0)
            self.vad.reset()
            self._mic.clear()

            self.set_state(State.IDLE)
            self._log(
                f"播放结束；再屏蔽麦克风 {self.cfg.echo_guard_ms} ms 防回环"
                f"（本轮共丢弃 {self._echo_dropped_ms} ms 音频）"
            )

    # ------------------------------------------------------------------
    def _save_audio(self, pcm: np.ndarray, tag: str) -> None:
        if not self._save_dir or pcm.size == 0:
            return
        # 这个方法会被 pump 线程和 worker 线程同时调用，_save_idx 的自增
        # 不是原子的，不锁会产生重名文件互相覆盖。
        with self._save_lock:
            try:
                d = Path(self._save_dir)
                d.mkdir(parents=True, exist_ok=True)
                self._save_idx += 1
                name = f"{self._turn:04d}_{tag}_{self._save_idx:03d}.wav"
                encode_wav(pcm, d / name)
            except Exception as exc:  # 存档失败不影响主流程
                self._log(f"存档失败: {exc!r}")

    def _log(self, msg: str) -> None:
        if self.ev.on_log:
            self.ev.on_log(msg)
