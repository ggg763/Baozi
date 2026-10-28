"""离线自测：不需要 ESP32、不需要网络、不需要任何模型。

覆盖三块：
  1) 帧协议的编解码 / 分片 / 乱码重新同步（和固件的 C++ 实现是同一套规则）；
  2) 音频工具（重采样、环形缓冲、dBFS、WAV 读写）；
  3) VAD + 整条流水线（用合成语音模拟“说一句话 -> 停顿”）。

运行::

    python selftest.py
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

from audio import (  # noqa: E402
    PcmRing,
    SAMPLE_RATE,
    dbfs,
    encode_wav,
    pcm_duration_ms,
    pcm_from_bytes,
    pcm_to_bytes,
    resample,
)
from config import Config  # noqa: E402
from pipeline import Events, Pipeline, State  # noqa: E402
from protocol import (  # noqa: E402
    Ctrl,
    FrameDecoder,
    Pkt,
    encode,
    encode_ctrl,
)

PASS = 0
FAIL = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [OK]   {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name} {detail}")


# =====================================================================
# 1. 协议
# =====================================================================
def test_protocol() -> None:
    print("\n== 1. 帧协议 ==")

    payload = bytes(range(256)) * 3
    raw = encode(Pkt.MIC_PCM, payload, seq=7)
    dec = FrameDecoder()
    frames = list(dec.feed(raw))
    check("单帧往返", len(frames) == 1 and frames[0].payload == payload)
    if frames:
        check("type/seq 正确", frames[0].type == Pkt.MIC_PCM and frames[0].seq == 7)

    # 逐字节喂：模拟 TCP 把一帧拆成几十次 recv
    dec = FrameDecoder()
    got = []
    for i in range(len(raw)):
        got.extend(dec.feed(raw[i : i + 1]))
    check("逐字节分片", len(got) == 1 and got[0].payload == payload)

    # 一次喂多帧：模拟粘包
    dec = FrameDecoder()
    stream = encode(Pkt.MIC_PCM, b"a" * 100) + encode_ctrl(Ctrl.PLAY_BEGIN) + encode(
        Pkt.TTS_PCM, b"b" * 50
    )
    frames = list(dec.feed(stream))
    check("粘包拆帧", len(frames) == 3, f"得到 {len(frames)} 帧")
    if len(frames) == 3:
        check(
            "拆帧内容正确",
            frames[0].payload == b"a" * 100
            and frames[1].payload == bytes([Ctrl.PLAY_BEGIN])
            and frames[2].payload == b"b" * 50,
        )

    # 乱码 + 合法帧混在一起：应该丢掉乱码后恢复同步
    dec = FrameDecoder()
    frames = list(dec.feed(b"\x00\x11\x22\x33" + raw))
    check("乱码后重新同步", len(frames) == 1 and frames[0].payload == payload)
    check("乱码被计数", dec.bad_bytes == 4, f"bad_bytes={dec.bad_bytes}")

    # 边界长度
    try:
        encode(Pkt.TTS_PCM, b"x" * 2049)
        check("超长 payload 被拒绝", False)
    except ValueError:
        check("超长 payload 被拒绝", True)

    check("最大长度可用", len(encode(Pkt.TTS_PCM, b"x" * 2048)) == 8 + 2048)


# =====================================================================
# 2. 音频工具
# =====================================================================
def test_audio() -> None:
    print("\n== 2. 音频工具 ==")

    pcm = pcm_from_bytes(pcm_to_bytes(np.arange(-100, 100, dtype=np.int16)))
    check("PCM 字节往返", pcm[0] == -100 and pcm[-1] == 99)

    # 重采样：1 秒 8k 正弦 -> 16k，时长应该翻倍
    t = np.arange(8000) / 8000.0
    src = (np.sin(2 * np.pi * 440 * t) * 10000).astype(np.int16)
    dst = resample(src, 8000, 16000)
    check(
        "重采样长度正确",
        abs(len(dst) - 16000) <= 1,
        f"得到 {len(dst)}",
    )
    check("重采样后仍是 int16", dst.dtype == np.int16)

    # 音量守恒（粗略）：RMS 不应变化超过 20%
    a = np.sqrt(np.mean((src.astype(np.float64)) ** 2))
    b = np.sqrt(np.mean((dst.astype(np.float64)) ** 2))
    check("重采样幅度守恒", abs(a - b) / a < 0.2, f"{a:.0f} -> {b:.0f}")

    # 短音频不越界
    tiny = np.array([1, 2, 3], dtype=np.int16)
    check("短音频重采样不崩", len(resample(tiny, 44100, 16000)) == 1)

    # dBFS
    silence = np.zeros(1600, dtype=np.int16)
    loud = np.full(1600, 32767, dtype=np.int16)
    check("静音 dBFS 很低", dbfs(silence) < -100)
    check("满幅 dBFS ≈ 0", abs(dbfs(loud)) < 0.1, f"{dbfs(loud):.3f}")

    # 环形缓冲：超容量丢最老的
    ring = PcmRing(100)
    ring.push(np.arange(60, dtype=np.int16))
    ring.push(np.arange(60, 120, dtype=np.int16))
    check("环形缓冲容量", len(ring) == 100, f"len={len(ring)}")
    check("环形缓冲丢最老的", ring.pop(1)[0] == 20, f"首样本={ring.pop(0)}")
    ring.clear()
    check("环形缓冲清空", len(ring) == 0)

    # preroll
    ring.push(np.arange(10, 30, dtype=np.int16))
    check("preroll 取尾部", list(ring.peek_tail(3)) == [27, 28, 29])

    # WAV 读写
    tmp = Path("_selftest_tmp.wav")
    data = (np.sin(2 * np.pi * 440 * np.arange(16000) / 16000) * 8000).astype(np.int16)
    encode_wav(data, tmp)
    from audio import decode_audio_file

    back = decode_audio_file(tmp)
    check("WAV 往返长度", len(back) == len(data), f"{len(back)} vs {len(data)}")
    check("WAV 往返内容", np.max(np.abs(back.astype(int) - data.astype(int))) <= 1)
    tmp.unlink(missing_ok=True)


# =====================================================================
# 3. VAD + 流水线
# =====================================================================
def make_speech(ms: int, level_db: float = -20.0, seed: int = 0) -> np.ndarray:
    """造一段“像人说话”的音：带包络的噪声，能量稳定在 level_db 附近。"""
    rng = np.random.default_rng(seed)
    n = SAMPLE_RATE * ms // 1000
    # 用几个正弦叠加 + 噪声，避免纯噪声在自适应噪声底里被当成静音
    t = np.arange(n) / SAMPLE_RATE
    sig = (
        0.6 * np.sin(2 * np.pi * 180 * t)
        + 0.3 * np.sin(2 * np.pi * 320 * t)
        + 0.2 * rng.standard_normal(n)
    )
    # 归一化到目标电平
    amp = 10 ** (level_db / 20.0) * 32768.0
    sig = sig / (np.sqrt(np.mean(sig**2)) + 1e-9) * amp
    # 100Hz 的振幅调制，更像语音的起伏
    sig *= 0.6 + 0.4 * np.sin(2 * np.pi * 4 * t)
    return np.clip(sig, -32768, 32767).astype(np.int16)


def make_silence(ms: int, level_db: float = -75.0, seed: int = 1) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = SAMPLE_RATE * ms // 1000
    amp = 10 ** (level_db / 20.0) * 32768.0
    return np.clip(rng.standard_normal(n) * amp, -32768, 32767).astype(np.int16)


def test_vad_threshold_not_swallowed() -> None:
    """VAD 的自适应门限不能被噪声底"架空"（2026-11 故障回归）。

    故障现场：麦克风增益过大 / 削顶 → 噪声底被抬到 -30dB 以上 →
    自适应门限顶到 -10dB，压过了 hold_db(-22) →
    "说话结束"永远判不出来，每句话都被拖到 maxlen，
    表现就是"一直在听、一直不结束"。

    这里直接查门限：把噪声底人为喂高，生效门限最多只能比设定阈值高
    _ADAPTIVE_MARGIN_DB，绝不能无限上浮。
    """
    print("\n== 2b. VAD 门限不被噪声底吞掉 ==")

    from vad import EnergyVad

    start_db, hold_db = -38.0, -30.0
    vad = EnergyVad(threshold_db=start_db, hold_db=hold_db)
    margin = EnergyVad._ADAPTIVE_MARGIN_DB

    # 直接把噪声底抬到很高的位置，模拟"增益过大 + 削顶"的环境
    vad._floor_init = True
    vad._floor_db = -10.0

    vad.set_phase_hold()
    # 用一帧已知电平的信号来反推生效门限
    def gate_at(level_db: float) -> bool:
        n = SAMPLE_RATE * 20 // 1000
        amp = 10 ** (level_db / 20.0) * 32768.0
        frame = np.clip(np.ones(n) * amp, -32768, 32767).astype(np.int16)
        return vad.is_speech(frame)

    # 比 hold_db 低 1dB 的静音必须是"不说话"
    check("高噪声底时 -31dB 仍判为静音", gate_at(-31.0) is False,
          f"hold_db={hold_db} margin={margin}")
    # 比 hold_db 高很多的人声必须是"说话"
    check("高噪声底时 -20dB 仍判为说话", gate_at(-20.0) is True)

    # 门限上限：effective <= hold_db + margin，且与噪声底无关地保持这个上限
    # ⚠️ 直接用 gate_db() 读门限，别用 is_speech() —— 后者有副作用（会更新
    #    噪声底），拿它反推门限会互相干扰，测出来的东西不可靠。
    check("噪声底 -30dB 时门限 = hold_db + margin",
          abs(vad.gate_db(hold_db) - (hold_db + margin)) < 0.01,
          f"hold_db={hold_db} margin={margin} gate={vad.gate_db(hold_db):.2f}")

    # 关键回归：噪声底再高，门限也不能继续上浮（这正是"一直被听"的成因）
    # ⚠️ worst 必须用 -inf 初始化：门限是负值，用 0 初始化会永远取不到最大值。
    worst = float("-inf")
    for floor_db in (-70, -60, -50, -44, -40, -36, -34, -30, -26, -20, -10, 0):
        vad._floor_init = True
        vad._floor_db = float(floor_db)
        vad.set_phase_hold()
        worst = max(worst, vad.gate_db(hold_db))
    check("噪声底再高门限也被封顶在 hold_db+margin",
          worst <= hold_db + margin + 0.01,
          f"最高门限={worst:.2f}，上限={hold_db + margin:.2f}")


def test_vad_and_pipeline() -> None:
    print("\n== 3. VAD + 流水线 ==")

    cfg = Config()
    cfg.vad_backend = "energy"
    cfg.vad_threshold_db = -42.0
    cfg.vad_start_ms = 200
    cfg.vad_end_ms = 500
    cfg.vad_preroll_ms = 200
    cfg.asr_min_ms = 200
    # pre_speak_wait_ms 在 Pipeline 构造时读入。这里用 800ms：
    # 太短的话测试还没来得及观察到 WAITING 就已经自动播放了。
    cfg.pre_speak_wait_ms = 800

    from asr import MockAsr
    from llm import EchoLlm
    from tts import ToneTts

    spoken: list[np.ndarray] = []
    transcripts: list[str] = []
    replies: list[str] = []
    states: list[State] = []

    pipe = Pipeline(
        cfg,
        asr=MockAsr(),
        llm=EchoLlm(),
        tts=ToneTts(),
        events=Events(
            on_play_pcm=lambda p: spoken.append(p),
            on_transcript=lambda t: transcripts.append(t),
            on_reply=lambda r: replies.append(r),
            on_state=lambda s: states.append(s),
        ),
    )
    pipe.start()

    # —— 场景：静音 -> 说话 800ms -> 静音 800ms -> 静音 -> 说话 400ms -> 静音 ——
    stream = np.concatenate(
        [
            make_silence(600),
            make_speech(800, -20.0, seed=2),
            make_silence(800),
            make_silence(400),
            make_speech(400, -20.0, seed=3),
            make_silence(800),
        ]
    )

    # 按 20ms 一块喂进去，模拟 ESP32 的推送节奏
    step = SAMPLE_RATE * 20 // 1000
    for i in range(0, len(stream), step):
        pipe.feed_mic(stream[i : i + step])

    # 等 ASR/LLM/TTS 的 worker 线程处理完（TTS 跑完 = 整条链路跑完了）
    deadline = time.time() + 10.0
    while not spoken and time.time() < deadline:
        time.sleep(0.05)
    time.sleep(0.1)  # 让 on_state 回调都落地

    check("检测到至少一句话", len(transcripts) >= 1, f"检出 {len(transcripts)} 句")
    check("识别文本非空", bool(transcripts) and len(transcripts[0]) > 0)
    check("产生了回复", len(replies) >= 1)
    check("产生了 TTS 音频", len(spoken) >= 1 and spoken[0].size > 0)
    if spoken:
        check(
            "TTS 音频时长 > 0",
            pcm_duration_ms(spoken[0]) > 0,
            f"{pcm_duration_ms(spoken[0]):.0f} ms",
        )

    # 状态机应该走过 LISTENING / THINKING / WAITING
    check("状态机进入过 LISTENING", State.LISTENING in states)
    check("状态机进入过 THINKING", State.THINKING in states)
    check("状态机进入过 WAITING（备好后先等）", State.WAITING in states)

    # 关键：WAITING 必须出现在 SPEAKING 之前（先备好、等一下、再开口）。
    # 不能直接断言"此刻处于 WAITING"——等待窗口可能已经过去，那属于正常行为。
    i_wait = states.index(State.WAITING) if State.WAITING in states else -1
    i_speak = states.index(State.SPEAKING) if State.SPEAKING in states else -1
    check(
        "WAITING 出现在 SPEAKING 之前",
        i_wait >= 0 and i_speak >= 0 and i_wait < i_speak,
        f"WAITING@{i_wait}, SPEAKING@{i_speak}",
    )

    # 等待窗口内没有说话 -> 到点自动开始播放
    played_before = len(spoken)
    deadline = time.time() + (cfg.pre_speak_wait_ms / 1000.0) + 3.0
    while pipe.state is State.WAITING and time.time() < deadline:
        time.sleep(0.02)
    check("等待结束进入 SPEAKING", pipe.state is State.SPEAKING, f"当前 {pipe.state}")
    check("等待结束后确实播放了", len(spoken) >= played_before and len(spoken) >= 1)

    pipe.notify_play_done()
    check("播放结束后回到 IDLE", pipe.state is State.IDLE, f"当前 {pipe.state}")

    # —— 纯静音不该触发任何识别（注意还要等 worker 线程，别只看立即结果）——
    before = len(transcripts)
    for _ in range(SAMPLE_RATE * 2 // step):
        pipe.feed_mic(make_silence(20, -75.0, seed=7))
    time.sleep(0.5)
    check("静音不误触发", len(transcripts) == before, f"{before} -> {len(transcripts)}")

    # —— 新版行为：SPEAKING 期间说话【不再】打断 ——
    # 旧版本会在这里打断，把包子的话切成一两个字；现在必须保持播放不动。
    aborted: list[bool] = []
    pipe.ev.update(on_play_abort=lambda: aborted.append(True))
    pipe.set_state(State.SPEAKING)
    for _ in range(15):
        pipe.feed_mic(make_silence(20, -75.0, seed=13))
    for _ in range(20):  # 400ms 语音
        pipe.feed_mic(make_speech(20, -15.0, seed=9))
    time.sleep(0.2)
    check("播放期间说话不再打断", len(aborted) == 0, f"abort 次数={len(aborted)}")
    check("播放期间仍在 SPEAKING", pipe.state is State.SPEAKING, f"当前 {pipe.state}")

    pipe.abort("测试收尾")
    check("abort() 能复位状态", pipe.state is State.IDLE, f"当前 {pipe.state}")

    pipe.stop()
    pipe.close()


# =====================================================================
# 3a2. 新机制：备好后等待期间用户又说话 -> 取消这次回复
# =====================================================================
def test_wait_window_cancelled_by_speech() -> None:
    print("\n== 3a2. 等待期间用户又说话则取消回复 ==")

    cfg = Config()
    cfg.vad_backend = "energy"
    cfg.vad_threshold_db = -42.0
    cfg.vad_start_ms = 200
    cfg.vad_end_ms = 500
    cfg.asr_min_ms = 200
    cfg.pre_speak_wait_ms = 1000

    from asr import MockAsr
    from llm import EchoLlm
    from tts import ToneTts

    spoken: list[np.ndarray] = []
    logs: list[str] = []
    states: list[State] = []

    pipe = Pipeline(
        cfg,
        MockAsr(),
        EchoLlm(),
        ToneTts(),
        events=Events(
            on_play_pcm=lambda p: spoken.append(p),
            on_log=lambda m: logs.append(m),
            on_state=lambda s: states.append(s),
        ),
    )
    pipe.start()

    # 说一句，触发 ASR/LLM/TTS
    stream = np.concatenate([make_silence(400), make_speech(600, -20.0, seed=41), make_silence(900)])
    step = SAMPLE_RATE * 20 // 1000
    for i in range(0, len(stream), step):
        pipe.feed_mic(stream[i : i + step])

    # 等它进入 WAITING（话已备好，正在等窗口）
    deadline = time.time() + 8.0
    while pipe.state is not State.WAITING and time.time() < deadline:
        time.sleep(0.05)

    check("进入 WAITING 等待窗口", pipe.state is State.WAITING, f"当前 {pipe.state}")
    played_before = len(spoken)

    # 在窗口内"用户又说话了" —— 应该取消这次回复，不播放
    # ⚠️ 这里要喂满 wait_cancel_min_ms（默认 600ms）之后再多给一点：
    #    判定"用户真的又在说话"靠的是 _on_speech_end 拿到的那段语音时长，
    #    只喂 400ms 是故意不取消的（那是咳嗽/碰桌的容差）。
    for _ in range(35):   # 700ms 语音
        pipe.feed_mic(make_speech(20, -20.0, seed=42))
    # 再补一段静音触发 SpeechEnd（VAD 靠尾部静音判断"说完了"）
    for _ in range(30):   # 600ms 静音
        pipe.feed_mic(make_silence(20, -75.0, seed=43))
        time.sleep(0.005)
    time.sleep(0.3)

    check("被取消后没有播放", len(spoken) == played_before, f"播放次数 {len(spoken)}")
    # 2026-10 起：取消决策挪到了 worker 的 ASR 之后（有内容才算插话），
    # 日志措辞也从"取消本次回复"改成了"取消已备好的回复"。
    check("记录到取消日志", any("取消已备好的回复" in m for m in logs), f"logs tail={logs[-3:]}")
    # 取消之后紧接着会继续处理刚说的那句（开新一轮 WAITING），所以这里不能
    # 直接断言"此刻一定是 LISTENING"——看状态轨迹里它【确实离开过】WAITING。
    i_wait = states.index(State.WAITING) if State.WAITING in states else -1
    left_waiting = any(s is not State.WAITING for s in states[i_wait + 1:]) \
        if i_wait >= 0 else False
    check("取消后离开 WAITING（继续听/接着处理下一句）", left_waiting,
          f"轨迹={[s.name for s in states]}")

    pipe.abort("测试收尾")
    pipe.stop()
    pipe.close()


# =====================================================================
# 3a2b. 回归（2026-10 实机故障）：噪声不能把已备好的回复冲掉
#   场景：上一轮在走视觉/LLM/TTS（十几秒），期间麦克风不屏蔽，环境噪声
#   被 VAD 判成一句话排进队列；回复备好进入 WAITING 后，worker 取出这条
#   噪声任务，旧代码 set_state(THINKING) 踩掉 WAITING，ASR 认出'。'后又
#   set_state(IDLE) —— 备好的话永远播不出来（包子"不说话"）。
# =====================================================================
def test_noise_cannot_kill_pending_reply() -> None:
    print("\n== 3a2b. 噪声（ASR 出'。'）不能顶掉已备好的回复 ==")

    cfg = Config()
    cfg.vad_backend = "energy"
    cfg.vad_threshold_db = -42.0
    cfg.vad_start_ms = 200
    cfg.vad_end_ms = 500
    cfg.asr_min_ms = 200
    cfg.pre_speak_wait_ms = 1500   # 留够时间让噪声片段排进队列

    from llm import EchoLlm
    from tts import ToneTts

    class NoiseThenRealAsr:
        """第一句返回真话，之后全部返回'。'（模拟噪声被 VAD 误判成话）。"""

        name = "noise-mock"

        def __init__(self) -> None:
            self.n = 0

        def transcribe(self, pcm: np.ndarray) -> str:
            self.n += 1
            if self.n == 1:
                return "你好包子"
            return "。"

        def close(self) -> None:
            pass

    spoken: list[np.ndarray] = []
    logs: list[str] = []
    states: list[State] = []

    pipe = Pipeline(
        cfg,
        NoiseThenRealAsr(),
        EchoLlm(),
        ToneTts(),
        events=Events(
            on_play_pcm=lambda p: spoken.append(p),
            on_log=lambda m: logs.append(m),
            on_state=lambda s: states.append(s),
        ),
    )
    pipe.start()

    step = SAMPLE_RATE * 20 // 1000

    # 1) 正常说一句 -> 走完 ASR/LLM/TTS -> WAITING（话已备好）
    stream = np.concatenate(
        [make_silence(400), make_speech(600, -20.0, seed=51), make_silence(900)]
    )
    for i in range(0, len(stream), step):
        pipe.feed_mic(stream[i : i + step])

    deadline = time.time() + 8.0
    while pipe.state is not State.WAITING and time.time() < deadline:
        time.sleep(0.02)
    check("进入 WAITING 等待窗口", pipe.state is State.WAITING, f"当前 {pipe.state}")
    played_before = len(spoken)

    # 2) 窗口内混入一段 700ms 的"噪声"——比 wait_cancel_min_ms(600) 长，
    #    旧逻辑在这里就按"用户插话"把回复取消了；新逻辑只排队，等 ASR 判内容。
    for _ in range(35):
        pipe.feed_mic(make_speech(20, -20.0, seed=52))
    for _ in range(30):
        pipe.feed_mic(make_silence(20, -75.0, seed=53))
        time.sleep(0.005)

    # 3) 噪声被 ASR 认成'。'丢弃后，回复必须照常播出来
    deadline = time.time() + 8.0
    while len(spoken) == played_before and time.time() < deadline:
        time.sleep(0.02)
    check("噪声没有顶掉回复（照常播放）", len(spoken) > played_before,
          f"播放次数 {len(spoken)}（期望 > {played_before}）")
    check("记录到还原日志", any("还原 WAITING" in m for m in logs),
          f"logs tail={logs[-3:]}")

    pipe.abort("测试收尾")
    pipe.stop()
    pipe.close()


# =====================================================================
# 3a3. 声学回环：包子说话时不能把自己说的话听进去
# =====================================================================
def test_echo_guard_ignores_mic_during_playback() -> None:
    print("\n== 3a3. 播放期间屏蔽麦克风（防自问自答）==")

    cfg = Config()
    cfg.vad_backend = "energy"
    cfg.vad_threshold_db = -42.0
    cfg.vad_start_ms = 200
    cfg.vad_end_ms = 500
    cfg.asr_min_ms = 200
    cfg.pre_speak_wait_ms = 100
    cfg.echo_guard_ms = 500

    from asr import MockAsr
    from llm import EchoLlm
    from tts import ToneTts

    transcripts: list[str] = []
    logs: list[str] = []
    states: list[State] = []
    # 必须提供 on_play_pcm，否则流水线会认为"无法播放"直接回 IDLE，
    # 压根进不了 SPEAKING，也就测不到屏蔽逻辑。
    played: list[np.ndarray] = []

    pipe = Pipeline(
        cfg,
        MockAsr(),
        EchoLlm(),
        ToneTts(),
        events=Events(
            on_transcript=lambda t: transcripts.append(t),
            on_log=lambda m: logs.append(m),
            on_state=lambda s: states.append(s),
            on_play_pcm=lambda p: played.append(p),
        ),
    )
    pipe.start()

    step = SAMPLE_RATE * 20 // 1000

    # 1) 先正常说一句，让它走到播放
    stream = np.concatenate([make_silence(400), make_speech(600, -20.0, seed=61), make_silence(900)])
    for i in range(0, len(stream), step):
        pipe.feed_mic(stream[i : i + step])

    deadline = time.time() + 8.0
    while pipe.state is not State.SPEAKING and time.time() < deadline:
        time.sleep(0.03)
    check("进入 SPEAKING", pipe.state is State.SPEAKING, f"当前 {pipe.state}")

    n_before = len(transcripts)

    # 2) 模拟"喇叭的声音被自己麦克风收进去"：持续喂很响的语音
    #    如果没有屏蔽，这些会被 VAD 判为语音 -> 触发新一轮识别
    loud = make_speech(3000, -12.0, seed=62)   # 3 秒很响
    for i in range(0, len(loud), step):
        pipe.feed_mic(loud[i : i + step])
        time.sleep(0.002)
    time.sleep(0.3)

    check(
        "播放期间没有触发新识别（挡住回环）",
        len(transcripts) == n_before,
        f"{n_before} -> {len(transcripts)}",
    )
    check("仍处于 SPEAKING（没被自己的声音打断）",
          pipe.state is State.SPEAKING, f"当前 {pipe.state}")

    # 3) 拖尾屏蔽：播放结束后 echo_guard_ms 内，仍然要丢弃
    pipe.notify_play_done()
    check("播放结束回到 IDLE", pipe.state is State.IDLE, f"当前 {pipe.state}")

    n_after_done = len(transcripts)
    tail = make_speech(300, -12.0, seed=63)   # 拖尾期内喂很响的声音
    for i in range(0, len(tail), step):
        pipe.feed_mic(tail[i : i + step])
    time.sleep(0.1)
    check(
        "拖尾屏蔽期内不误触发",
        len(transcripts) == n_after_done,
        f"{n_after_done} -> {len(transcripts)}",
    )

    # 4) 屏蔽结束后应该恢复正常：这时说话必须能被识别
    time.sleep(cfg.echo_guard_ms / 1000.0 + 0.2)
    n_after_guard = len(transcripts)
    again = np.concatenate([make_speech(700, -20.0, seed=64), make_silence(900)])
    for i in range(0, len(again), step):
        pipe.feed_mic(again[i : i + step])
        time.sleep(0.002)

    deadline = time.time() + 8.0
    while len(transcripts) == n_after_guard and time.time() < deadline:
        pipe.feed_mic(make_silence(20, -75.0, seed=65))
        time.sleep(0.02)

    check(
        "屏蔽结束后能正常识别（没有永久静音）",
        len(transcripts) > n_after_guard,
        f"{n_after_guard} -> {len(transcripts)}",
    )

    pipe.abort("测试收尾")
    pipe.stop()
    pipe.close()


# =====================================================================
# 3b. 没有推流回调时，流水线不能卡死在 SPEAKING
# =====================================================================
def test_no_play_callback_does_not_wedge() -> None:
    print("\n== 3b. 缺少 on_play_pcm 时不会卡死 ==")

    cfg = Config()
    cfg.vad_backend = "energy"
    cfg.vad_threshold_db = -42.0
    cfg.vad_start_ms = 200
    cfg.vad_end_ms = 500
    cfg.asr_min_ms = 200
    cfg.echo_guard_ms = 0      # 关掉屏蔽，专测状态复位

    from asr import MockAsr
    from llm import EchoLlm
    from tts import ToneTts

    replies: list[str] = []
    # 故意不提供 on_play_pcm
    pipe = Pipeline(
        cfg,
        MockAsr(),
        EchoLlm(),
        ToneTts(),
        events=Events(on_reply=lambda r: replies.append(r)),
    )
    pipe.start()

    stream = np.concatenate([make_silence(400), make_speech(600, -20.0, seed=21), make_silence(900)])
    step = SAMPLE_RATE * 20 // 1000
    for i in range(0, len(stream), step):
        pipe.feed_mic(stream[i : i + step])

    deadline = time.time() + 5.0
    while not replies and time.time() < deadline:
        time.sleep(0.05)
    time.sleep(0.2)

    check("仍能走完 ASR/LLM/TTS", len(replies) >= 1, f"回复数={len(replies)}")
    check("没有卡在 SPEAKING", pipe.state is not State.SPEAKING, f"当前 {pipe.state}")
    # 新版：没有 on_play_pcm 时，备好的话会在 WAITING 结束后被判为"无法播放"，
    # 最终回到 IDLE。等一小会儿让它走完这个流程。
    deadline = time.time() + (cfg.pre_speak_wait_ms / 1000.0) + 3.0
    while pipe.state is State.WAITING and time.time() < deadline:
        time.sleep(0.05)
    check("最终回到 IDLE 可以继续听", pipe.state is State.IDLE, f"当前 {pipe.state}")

    pipe.stop()
    pipe.close()


# =====================================================================
# 3c. PcmRing 在多线程下不丢样本
# =====================================================================
def test_pcm_ring_thread_safety() -> None:
    print("\n== 3c. PcmRing 线程安全 ==")

    import threading

    ring = PcmRing(SAMPLE_RATE * 10)
    total = 200_000
    chunk = 320
    pushed = 0

    # 样本值用 0..1999 循环，避免超出 int16 范围（int16 只能到 32767）
    def pattern(start: int, n: int) -> np.ndarray:
        return ((np.arange(start, start + n) % 2000).astype(np.int16))

    # 生产者：连续推递增模式的样本
    def producer():
        nonlocal pushed
        try:
            for i in range(0, total, chunk):
                n = min(chunk, total - i)
                ring.push(pattern(i, n))
                pushed += n
        except Exception as exc:  # 生产者一挂，消费者会永远等下去
            print(f"      producer 异常: {exc!r}")

    out = bytearray()
    consumed = 0

    def consumer():
        nonlocal consumed
        deadline = time.time() + 20.0
        while consumed < total and time.time() < deadline:
            got = ring.pop(min(chunk, total - consumed))
            if got.size:
                out.extend(got.tobytes())
                consumed += got.size
            else:
                time.sleep(0.0005)

    tp = threading.Thread(target=producer)
    tc = threading.Thread(target=consumer)
    tp.start()
    tc.start()
    tp.join(timeout=30)
    tc.join(timeout=30)

    got = np.frombuffer(bytes(out), dtype="<i2")
    check("生产者推满", pushed == total, f"{pushed}/{total}")
    check(
        "消费者拿到完整数据",
        got.size == total,
        f"{got.size}/{total} (丢 {ring.dropped_samples})",
    )
    # 内容必须与推入顺序完全一致：任何交错写入都会在这里露出来
    if got.size == total:
        expect = pattern(0, total)
        check("数据顺序与内容完全正确", bool(np.array_equal(got, expect)))


# =====================================================================
# 3d. 大模型报错时不能卡死，并且错误要能被看见
# =====================================================================
def test_llm_failure_is_recovered() -> None:
    print("\n== 3d. 大模型报错后能恢复 ==")

    cfg = Config()
    cfg.vad_backend = "energy"
    cfg.vad_threshold_db = -42.0
    cfg.vad_start_ms = 200
    cfg.vad_end_ms = 500
    cfg.asr_min_ms = 200
    cfg.echo_guard_ms = 0

    from asr import MockAsr
    from llm import BrokenLlm
    from tts import ToneTts

    logs: list[str] = []
    spoken: list[np.ndarray] = []

    pipe = Pipeline(
        cfg,
        MockAsr(),
        BrokenLlm("模拟故障"),
        ToneTts(),
        events=Events(
            on_log=lambda m: logs.append(m),
            on_play_pcm=lambda p: spoken.append(p),
        ),
    )
    pipe.start()

    stream = np.concatenate([make_silence(400), make_speech(600, -20.0, seed=31), make_silence(900)])
    step = SAMPLE_RATE * 20 // 1000
    for i in range(0, len(stream), step):
        pipe.feed_mic(stream[i : i + step])

    deadline = time.time() + 5.0
    while not any("大模型调用失败" in m for m in logs) and time.time() < deadline:
        time.sleep(0.05)
    time.sleep(0.1)

    check("大模型错误被记录", any("大模型调用失败" in m for m in logs), f"logs={logs[-3:]}")
    check("没有产出音频", len(spoken) == 0)
    check("状态回到 IDLE", pipe.state is State.IDLE, f"当前 {pipe.state}")

    # 恢复后还能继续处理下一句
    pipe.ev.update(on_play_pcm=lambda p: spoken.append(p))
    pipe.stop()
    pipe.close()


# =====================================================================
# 4. 端到端：真的走一遍帧协议（本机 socket，模拟 WiFi 模式）
# =====================================================================
def test_loopback_over_tcp() -> None:
    print("\n== 4. TCP 端到端（本机回环，模拟 ESP32）==")

    import socket
    import threading

    cfg = Config()
    cfg.transport = "tcp"          # 这个用例专门测 WiFi 模式那条路径
    cfg.host = "127.0.0.1"
    cfg.port = 18765
    cfg.vad_backend = "energy"
    cfg.echo_guard_ms = 0
    cfg.play_target_s = 0.1
    cfg.play_start_prefill_s = 0.02

    from asr import MockAsr
    from llm import EchoLlm
    from tts import ToneTts
    from pipeline import Pipeline as P
    from voice_server import VoiceServer

    pipe = P(cfg, MockAsr(), EchoLlm(), ToneTts())
    srv = VoiceServer(cfg, pipe)
    srv.start()

    received_audio = bytearray()
    received_ctrl: list[int] = []
    hello_ok = threading.Event()

    def device():
        """假 ESP32：连上去、发 hello + 一段语音、收播放音频。"""
        time.sleep(0.3)
        s = socket.create_connection(("127.0.0.1", 18765), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(0.5)

        # ⚠️ 收发必须分开两个线程：PC 会边推音频边回 PING/HELLO_ACK/PONG，
        #    而 TCP 的接收缓冲是有限的。原来这个假设备"先把音频全推完，
        #    再开始读"，几百 KB 堆在缓冲里，PC 那头的 send() 会一直等 ->
        #    读循环被堵住 -> 连握手帧都处理不了（实测踩过的坑）。
        rx_dec = FrameDecoder()

        def reader() -> None:
            deadline = time.time() + 8.0
            while time.time() < deadline:
                try:
                    data = s.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    return
                if not data:
                    return
                for f in rx_dec.feed(data):
                    if f.type == Pkt.TTS_PCM:
                        received_audio.extend(f.payload)
                    elif f.type == Pkt.CTRL and f.payload:
                        received_ctrl.append(f.payload[0])
                        if f.payload[0] == Ctrl.HELLO_ACK:
                            hello_ok.set()

        threading.Thread(target=reader, daemon=True).start()

        # ---- 先说"我上线了"，再推音频 ----
        # 握手（PC 回的 HELLO_ACK）不在这里死等：PC 端的 PING/握手是另一个
        # 线程发的，顺序不保证。下面收音频的那段循环里一起把 HELLO_ACK 收掉。
        s.sendall(encode_ctrl(Ctrl.HELLO, b"esp32s3cam/mock"))

        # 发一段“语音”，最后必须带足够长的静音——VAD 是靠尾部静音判断
        # “这句话说完了”的，发完就停会让这一段永远结束不了。
        stream = np.concatenate(
            [
                make_silence(300),
                make_speech(700, -20.0, seed=5),
                make_silence(1500),  # 尾部静音：触发 SpeechEnd
            ]
        )
        step = 320
        for i in range(0, len(stream), step):
            piece = stream[i : i + step]
            s.sendall(encode(Pkt.MIC_PCM, pcm_to_bytes(piece)))
            time.sleep(0.002)  # 别把服务器喂爆；服务器自己按 20ms 节奏消费

        # 音频推完了，等 reader 线程把回复收全（它自己是 8 秒上限）
        time.sleep(6.0)

    t = threading.Thread(target=device, daemon=True)
    t.start()
    t.join(timeout=15)
    # 让 voice_server 的推流线程把 PLAY_END 也发完
    time.sleep(0.5)

    check("收到 HELLO_ACK（握手通）", hello_ok.is_set())
    check("收到了 TTS 音频帧", len(received_audio) > 0, f"{len(received_audio)} 字节")
    check("收到了 PLAY_BEGIN", Ctrl.PLAY_BEGIN in received_ctrl, f"ctrl={received_ctrl}")
    check("收到了 PLAY_END", Ctrl.PLAY_END in received_ctrl, f"ctrl={received_ctrl}")
    check("音频长度是偶数", len(received_audio) % 2 == 0)

    # 收到的音频应该能正常解码成 16k PCM
    if received_audio:
        pcm = pcm_from_bytes(bytes(received_audio))
        check(
            "收到音频可解码且非静音",
            pcm.size > 0 and float(np.max(np.abs(pcm))) > 0,
            f"{pcm.size} 样本, 峰值 {float(np.max(np.abs(pcm))) if pcm.size else 0}",
        )

    srv.stop()
    pipe.close()


# =====================================================================
# =====================================================================
# 4b. USB 串口链路（不需要硬件的离线回环）
# =====================================================================
class _FakeSerialPort:
    """假的串口对象：只实现 SerialTransport 用到的那几个方法。

    两条方向（都是【串口视角】的缓冲，和真实 pyserial 一致）：
        rx  ← ESP32 写进来的字节（PC 用 read() 取）
        tx  → PC 写出去、等着 ESP32 来取的字节
    测试代码扮演 ESP32：往 rx 里塞、从 tx 里取。
    """

    def __init__(self) -> None:
        self.rx = bytearray()            # ESP32 -> PC
        self.tx = bytearray()            # PC -> ESP32
        self.closed = False

    # pyserial 风格的 API
    def read(self, n: int) -> bytes:
        if not self.rx:
            time.sleep(0.005)
            return b""
        out = bytes(self.rx[:n])
        del self.rx[:n]
        return out

    def write(self, view) -> int:
        data = bytes(view)
        # 模拟"写得比 USB 排空快"：一次只收一半
        take = max(1, len(data) // 2) if len(data) > 1 else len(data)
        self.tx.extend(data[:take])
        return take

    def reset_input_buffer(self) -> None:
        self.rx.clear()

    def set_buffer_size(self, **kwargs) -> None:   # Windows 上没有也正常
        raise OSError("unsupported")

    def close(self) -> None:
        self.closed = True


def test_serial_cam_and_log() -> None:
    print("\n== 4b. USB 串口链路（伪串口，验证抓图/日志/握手）==")

    from config import Config
    from link import SerialTransport
    from protocol import Ctrl, FrameDecoder, Pkt, encode, encode_ctrl, decode_cam_done

    fake = _FakeSerialPort()
    tr = SerialTransport(port="FAKE", log=lambda m: None)   # type: ignore[arg-type]
    tr._ser = fake                                          # 绕过真实打开串口
    tr.port = "FAKE"

    def pump(dec: FrameDecoder, want: int = 1, rounds: int = 200) -> list:
        """像真实读循环那样：反复读字节喂给同一个切帧器，直到凑够 want 帧。

        ⚠️ 必须用同一个 dec 跨轮累积 —— 串口一次只给一点字节，
          这正好验证"半帧跨次读取"这条路径。
        """
        out = []
        for _ in range(rounds):
            data = tr.read(64)
            if data:
                out.extend(dec.feed(data))
            else:
                time.sleep(0.002)
            if len(out) >= want:
                break
        return out

    # ---- 1) 写入被拆成多次也能完整发出去 ----
    hdr = encode(Pkt.TTS_PCM, b"payload-bytes")
    ok = tr.write(hdr)
    check("串口分片写完整", ok and bytes(fake.tx) == hdr,
          f"{bytes(fake.tx)[:16]!r}")

    # ---- 2) 日志帧（PKT_LOG）能切出来 ----
    fake.rx.extend(encode(Pkt.LOG, "[stat] 链路=OK 峰值=1234\n".encode("utf-8")))
    frames = pump(FrameDecoder())
    logs = [f.payload for f in frames if f.type == Pkt.LOG]
    check("收到 PKT_LOG 帧",
          logs == ["[stat] 链路=OK 峰值=1234\n".encode("utf-8")], f"{logs}")

    # ---- 3) 抓图：JPEG 被切成多片，靠 CAM_DONE 收尾 ----
    jpg = b"\xff\xd8" + bytes(range(256)) * 12 + b"\xff\xd9"     # 3076 字节
    chunks = [jpg[i:i + 2048] for i in range(0, len(jpg), 2048)]
    fake.rx.clear()
    for c in chunks:
        fake.rx.extend(encode(Pkt.CAM_JPEG, c))
    fake.rx.extend(encode_ctrl(
        Ctrl.CAM_DONE, bytes([0]) + len(jpg).to_bytes(4, "little") + bytes([7])))

    frames = pump(FrameDecoder(), want=len(chunks) + 1)
    collected = bytearray()
    done_payload = b""
    for f in frames:
        if f.type == Pkt.CAM_JPEG:
            collected.extend(f.payload)
        elif f.type == Pkt.CTRL and f.payload and f.payload[0] == Ctrl.CAM_DONE:
            done_payload = f.payload
    ok_flag, total, req = decode_cam_done(done_payload)
    check("JPEG 分片按序拼回", bytes(collected) == jpg,
          f"{len(collected)}/{len(jpg)}（{len(frames)} 帧 / {len(chunks) + 1} 期望）")
    check("CAM_DONE 字段正确", ok_flag and total == len(jpg) and req == 7,
          f"ok={ok_flag} total={total} req={req}")

    # ---- 4) 少量字节的乱码不会打断后续帧（串口错位自愈）----
    fake.rx.clear()
    fake.rx.extend(b"\x00\x11" + encode(Pkt.LOG, b"after-garbage\n"))
    dec = FrameDecoder()
    frames = pump(dec, want=1)
    logs = [f.payload for f in frames if f.type == Pkt.LOG]
    check("乱码后仍能收到日志帧", logs == [b"after-garbage\n"], f"{logs}")
    check("乱码被计数", dec.bad_bytes == 2, f"bad_bytes={dec.bad_bytes}")

    tr.close()
    check("close() 关掉串口", fake.closed)


# =====================================================================
# 4c. 握手不能变成"回声"（HELLO ↔ HELLO_ACK 无限循环）
# =====================================================================
def test_hello_handshake_no_echo() -> None:
    print("\n== 4c. 握手不会来回刷屏 ==")

    from config import Config
    from protocol import Ctrl, encode_ctrl
    from voice_server import VoiceServer

    class _Pipe:
        """只用来构造 VoiceServer 的假流水线（不跑 VAD/ASR）。"""

        def __init__(self):
            from pipeline import Events
            self.ev = Events()
            self.state = None

        def start(self):
            pass

        def stop(self):
            pass

        def feed_mic(self, block):
            pass

    cfg = Config()
    cfg.transport = "serial"

    srv = VoiceServer(cfg, _Pipe())
    sent: list[int] = []
    # 只截获"发出去的控制帧"，不碰真实串口
    srv._send_ctrl = lambda cmd, extra=b"": sent.append(int(cmd)) or True

    def feed_ctrl(*cmds):
        from protocol import Frame, Pkt
        for c in cmds:
            payload = encode_ctrl(c)[8:]          # 取出 payload 部分
            srv._on_frame(Frame(type=int(Pkt.CTRL), seq=0, payload=payload))

    # 场景 1：ESP32 上线 -> PC 回一次 HELLO_ACK
    feed_ctrl(Ctrl.HELLO)
    check("首次 HELLO 回了一次 HELLO_ACK",
          sent.count(int(Ctrl.HELLO_ACK)) == 1, f"sent={sent}")

    # 场景 2：连来 20 个 HELLO（模拟两边都在"有问必答"的病态情况）
    for _ in range(20):
        feed_ctrl(Ctrl.HELLO)
    check("重复 HELLO 不再回 HELLO_ACK",
          sent.count(int(Ctrl.HELLO_ACK)) == 1, f"sent={sent}")

    # 场景 3：PC 收到 HELLO_ACK 绝对不能回 HELLO（这一步才真正断开环）
    before = list(sent)
    feed_ctrl(Ctrl.HELLO_ACK)
    check("收到 HELLO_ACK 不回 HELLO",
          int(Ctrl.HELLO) not in sent[len(before):],
          f"新增={sent[len(before):]}")

    # 场景 4：PING 仍然正常应答 PONG（保活不能被握手改动破坏）
    feed_ctrl(Ctrl.PING)
    check("PING 仍有 PONG 应答", int(Ctrl.PONG) in sent, f"sent={sent}")


# =====================================================================
# 4d. 抓图请求/回图全链路（PC 侧视角）
# =====================================================================
def test_camera_request_flow() -> None:
    print("\n== 4d. 抓图请求 -> 分片 -> CAM_DONE ==")

    import threading
    from config import Config
    from protocol import Ctrl, Frame, Pkt, encode_ctrl
    from voice_server import VoiceServer

    class _Pipe:
        def __init__(self):
            from pipeline import Events
            self.ev = Events()
            self.state = None

        def start(self):
            pass

        def stop(self):
            pass

        def feed_mic(self, block):
            pass

    cfg = Config()
    cfg.transport = "serial"

    srv = VoiceServer(cfg, _Pipe())
    sent: list[tuple[int, bytes]] = []
    srv._send_ctrl = lambda cmd, extra=b"": (sent.append((int(cmd), extra)) or True)

    jpg = b"\xff\xd8" + bytes(range(256)) * 12 + b"\xff\xd9"

    def device_send(payload_frames, done_payload):
        """模拟 ESP32：先发若干 JPEG 分片，再发 CAM_DONE。"""
        for chunk in payload_frames:
            srv._on_frame(Frame(type=int(Pkt.CAM_JPEG), seq=0, payload=chunk))
        srv._on_frame(Frame(type=int(Pkt.CTRL), seq=0,
                            payload=bytes([int(Ctrl.CAM_DONE)]) + done_payload))

    # ---- 场景 1：正常回一帧 ----
    result: dict = {}

    def ask():
        result["jpg"] = srv.request_jpeg(timeout_s=3.0)

    t = threading.Thread(target=ask, daemon=True)
    t.start()
    time.sleep(0.15)                      # 等它把 CAM_REQ 发出去
    check("PC 发出了 CAM_REQ", any(c == int(Ctrl.CAM_REQ) for c, _ in sent), f"sent={sent}")
    req_id = sent[-1][1][0] if sent and sent[-1][1] else 0

    chunks = [jpg[i:i + 2048] for i in range(0, len(jpg), 2048)]
    device_send(chunks, bytes([0]) + len(jpg).to_bytes(4, "little") + bytes([req_id]))
    t.join(timeout=4.0)

    check("取到完整 JPEG", result.get("jpg") == jpg,
          f"got={len(result.get('jpg') or b'')} 期望 {len(jpg)}")

    # ---- 场景 2：分片丢了 -> 必须判为失败，不能把残缺数据当图 ----
    sent.clear()
    result.clear()
    t = threading.Thread(target=ask, daemon=True)
    t.start()
    time.sleep(0.15)
    req_id = sent[-1][1][0] if sent and sent[-1][1] else 0
    device_send(chunks[:1], bytes([0]) + len(jpg).to_bytes(4, "little") + bytes([req_id]))
    t.join(timeout=4.0)
    check("分片缺失时判为失败（不返回残缺图）", result.get("jpg") is None,
          f"got={len(result.get('jpg') or b'')}")

    # ---- 场景 3：ESP32 明确回报失败 -> 返回 None ----
    sent.clear()
    result.clear()
    t = threading.Thread(target=ask, daemon=True)
    t.start()
    time.sleep(0.15)
    req_id = sent[-1][1][0] if sent and sent[-1][1] else 0
    device_send([], bytes([1]) + (0).to_bytes(4, "little") + bytes([req_id]))
    t.join(timeout=4.0)
    check("ESP32 报失败时返回 None", result.get("jpg") is None)

    # ---- 场景 4：请求号对不上的迟到收尾不能唤醒新的等待者 ----
    sent.clear()
    result.clear()
    t = threading.Thread(target=ask, daemon=True)
    t.start()
    time.sleep(0.15)
    stale = (sent[-1][1][0] + 7) % 256           # 故意用一个错的请求号
    device_send(chunks, bytes([0]) + len(jpg).to_bytes(4, "little") + bytes([stale]))
    t.join(timeout=1.2)                          # 不该被唤醒，会超时
    check("旧请求号的 CAM_DONE 不会串图", t.is_alive() or result.get("jpg") is None,
          f"jpg={None if result.get('jpg') is None else len(result['jpg'])}")


# =====================================================================
def main() -> int:
    print("=" * 60)
    print(" baozi.ears&mouth  PC 端离线自测")
    print("=" * 60)

    test_protocol()
    test_audio()
    test_vad_threshold_not_swallowed()
    test_vad_and_pipeline()
    test_wait_window_cancelled_by_speech()
    test_noise_cannot_kill_pending_reply()
    test_echo_guard_ignores_mic_during_playback()
    test_no_play_callback_does_not_wedge()
    test_pcm_ring_thread_safety()
    test_llm_failure_is_recovered()
    test_loopback_over_tcp()
    test_serial_cam_and_log()
    test_hello_handshake_no_echo()
    test_camera_request_flow()

    print("\n" + "=" * 60)
    print(f" 结果: {PASS} 通过, {FAIL} 失败")
    print("=" * 60)
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
