"""预生成语音资产：开机自我介绍 + TTS 兜底话术。

共同点是「固定的几句话」，所以一次性合成好、存成 16k/单声道/int16 的 WAV，
跟着代码走（assets/ 下），运行时**纯本地读盘**，不再联网合成。

为什么不在启动时合成（2026-10-31 改）：
    原来开机时用 TTS 现场合成 3 句兜底话术，问题是——
      · 网络没起来就全灭，池子空着，真出故障时反而没话可说；
      · 每句话都要一次真实网络调用，白白让启动慢几秒；
      · 音色取决于"启动那一刻哪条链路是通的"，可能混进 SAPI 的机器音。
    存成本机文件后：0 网络、0 等待、音色恒定，断网也照样能播。

改了文本或音色怎么办（重跑一次，产物进版本库）：
    .venv\\Scripts\\python.exe boot_voice.py                    # 重生成开机问候
    .venv\\Scripts\\python.exe boot_voice.py --fallback          # 重生成兜底话术
    .venv\\Scripts\\python.exe boot_voice.py --all               # 两个都重生成
    .venv\\Scripts\\python.exe boot_voice.py "想换的开机词"        # 换开机词

    文件里带 manifest.json 记录"这批音频是用哪句话、哪个音色生成的"，
    话术被改过但没重新生成时，运行时会打一行明显的警告。
"""

from __future__ import annotations

import json
import sys
import time
import wave

from pathlib import Path

import numpy as np

from audio import SAMPLE_RATE, decode_audio_file

DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"
DEFAULT_RATE = "+0%"

# 开机说的一句。控制在 30 字以内：太长了开机要干等，还不容易听清。
BOOT_TEXT = "你好，我是包子，你的陪伴型智能桌面机器人，很高兴见到你"

# 兜底话术池：TTS 彻底失败时随机抽一句播，避免"哑掉"或播上一句的残响。
# ⚠️ 改这里之后要跑 `python boot_voice.py --fallback` 重新生成音频。
FALLBACK_PHRASES = [
    "抱歉，我没听清楚，可以再讲一遍吗？",
    "不好意思，刚才信号有点问题，你能再说一次吗？",
    "我没太听懂，麻烦你再说一遍。",
]

# 固定放在 assets/ 下，跟着代码走；不在 temp/ 里，不会被清掉。
_HERE = Path(__file__).resolve().parent
ASSET_DIR = _HERE / "assets"
BOOT_WAV = ASSET_DIR / "boot_hello.wav"
FALLBACK_DIR = ASSET_DIR / "fallback"
FALLBACK_MANIFEST = FALLBACK_DIR / "manifest.json"


# ---------------------------------------------------------------------
# 读
# ---------------------------------------------------------------------
def load_boot_voice() -> np.ndarray | None:
    """读预生成的开机语音，返回 16k/单声道/int16 的 PCM。

    读不到就返回 None —— 调用方安静跳过即可，绝不能因为一个问候语
    把整个启动流程搞挂。
    """
    if not BOOT_WAV.exists():
        return None
    try:
        pcm = decode_audio_file(BOOT_WAV)
    except Exception as exc:  # noqa: BLE001
        print(f"[boot] 开机语音读不出来（{BOOT_WAV.name}）：{exc!r}")
        return None
    if pcm.size == 0:
        return None
    return pcm.astype(np.int16, copy=False)


def _read_manifest() -> dict:
    try:
        return json.loads(FALLBACK_MANIFEST.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return {}


def load_fallback_pool(phrases: "list[str] | None" = None,
                       voice: str = "") -> list:
    """读本机预生成的兜底话术，返回与 phrases 等长的 [PCM | None, ...]。

    某一句文件缺了就留 None，调用方会自己决定要不要现场补合成。
    话术文本/音色和 manifest 对不上（改了代码没重新生成音频）时打警告，
    但**仍然把这些文件用上** —— 播一句措辞过时的"没听清"，总比哑掉好。
    """
    phrases = list(phrases or FALLBACK_PHRASES)
    out: list = [None] * len(phrases)

    if not FALLBACK_DIR.exists():
        return out

    man = _read_manifest()
    if man.get("phrases") and list(man["phrases"]) != phrases:
        print("[boot] ⚠️ 兜底话术文本已改动，但音频还是旧的。"
              "跑 `python boot_voice.py --fallback` 重新生成。")
    if voice and man.get("voice") and man["voice"] != voice:
        print(f"[boot] ⚠️ 兜底话术是 {man['voice']} 生成的，当前音色是 {voice}。"
              "想统一就重跑 `python boot_voice.py --fallback`。")

    for i in range(len(phrases)):
        p = FALLBACK_DIR / f"phrase_{i + 1}.wav"
        if not p.exists():
            continue
        try:
            pcm = decode_audio_file(p)
        except Exception as exc:  # noqa: BLE001
            print(f"[boot] 兜底话术 {p.name} 读不出来：{exc!r}")
            continue
        if pcm.size:
            out[i] = pcm.astype(np.int16, copy=False)
    return out


# ---------------------------------------------------------------------
# 生成
# ---------------------------------------------------------------------
def _synth_pcm(text: str, voice: str, rate: str) -> np.ndarray:
    """调 edge-tts 合成一句话，直接返回 16k/单声道/int16 的 PCM。"""
    import asyncio
    import tempfile

    import edge_tts

    async def _run() -> bytes:
        buf = bytearray()
        async for chunk in edge_tts.Communicate(
                text, voice, rate=rate).stream():
            if chunk["type"] == "audio":
                buf.extend(chunk["data"])
        return bytes(buf)

    mp3 = asyncio.run(_run())
    if not mp3:
        raise RuntimeError("edge-tts 没有返回音频")

    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as f:
        f.write(mp3)
        tmp = Path(f.name)
    try:
        return decode_audio_file(tmp)
    finally:
        tmp.unlink(missing_ok=True)


def _write_wav(path: Path, pcm: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(np.asarray(pcm, dtype="<i2").tobytes())


def generate(text: str = BOOT_TEXT, voice: str = DEFAULT_VOICE,
             rate: str = DEFAULT_RATE) -> Path:
    """重新生成开机自我介绍。"""
    print(f"[boot] 合成开机语音：{text}")
    pcm = _synth_pcm(text, voice, rate)
    _write_wav(BOOT_WAV, pcm)
    ms = pcm.size * 1000.0 / SAMPLE_RATE
    print(f"[boot] 已写入 {BOOT_WAV}  （{ms:.0f} ms / {BOOT_WAV.stat().st_size} 字节）")
    return BOOT_WAV


def generate_fallback(phrases: "list[str] | None" = None,
                      voice: str = DEFAULT_VOICE,
                      rate: str = DEFAULT_RATE) -> int:
    """重新生成整套兜底话术，返回成功几句。"""
    phrases = list(phrases or FALLBACK_PHRASES)
    FALLBACK_DIR.mkdir(parents=True, exist_ok=True)
    ok = 0
    for i, text in enumerate(phrases, 1):
        path = FALLBACK_DIR / f"phrase_{i}.wav"
        print(f"[boot] 合成兜底话术 {i}/{len(phrases)}：{text}")
        try:
            pcm = _synth_pcm(text, voice, rate)
        except Exception as exc:  # noqa: BLE001
            print(f"[boot]   失败：{exc!r}")
            continue
        _write_wav(path, pcm)
        ms = pcm.size * 1000.0 / SAMPLE_RATE
        print(f"[boot]   -> {path.name}  {ms:.0f} ms / {path.stat().st_size} 字节")
        ok += 1

    # manifest 只记录"这批音频对应哪句话、哪个音色"，用来查过期
    FALLBACK_MANIFEST.write_text(json.dumps({
        "voice": voice,
        "rate": rate,
        "phrases": phrases,
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[boot] 兜底话术 {ok}/{len(phrases)} 句已写入 {FALLBACK_DIR}")
    return ok


# ---------------------------------------------------------------------
if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--all" in argv:
        generate()
        generate_fallback()
    elif "--fallback" in argv:
        generate_fallback()
    else:
        custom = [a for a in argv if not a.startswith("--")]
        generate(custom[0] if custom else BOOT_TEXT)
