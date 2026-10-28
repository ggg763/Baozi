"""TTS 后端：把回复文本合成成 16k / 单声道 / int16 的 PCM。

  * ``edge-tts`` —— 微软在线语音，中文音色自然，免费（需联网）；
                    输出是 MP3，用 miniaudio 解码后重采样到 16k。
  * ``sapi``     —— Windows 自带离线语音（System.Speech）。断网时兜底，
                    音质一般但永远可用。
  * ``tone``     —— 用正弦音代替语音，用来测链路。
  * ``none``     —— 静音，不做 TTS。
"""

from __future__ import annotations

import asyncio
import re
import subprocess
import tempfile
import threading
import time

from pathlib import Path

import numpy as np

from audio import SAMPLE_RATE, decode_audio_file, pcm_from_bytes, resample

# 朗读前把 Markdown / 表情之类的“念不出来”的东西清掉
_MD_PATTERNS = [
    (re.compile(r"```.*?```", re.S), ""),
    (re.compile(r"`([^`]*)`"), r"\1"),
    (re.compile(r"\*\*([^*]*)\*\*"), r"\1"),
    (re.compile(r"\*([^*]*)\*"), r"\1"),
    (re.compile(r"^#{1,6}\s*", re.M), ""),
    (re.compile(r"^\s*[-*+]\s+", re.M), ""),
    (re.compile(r"^\s*\d+\.\s+", re.M), ""),
    (re.compile(r"\[([^\]]*)\]\([^)]*\)"), r"\1"),
]
_EMOJI = re.compile(
    "[\U0001F300-\U0001FAFF\U00002600-\U000027BF\U0001F000-\U0001F0FF]"
)


def clean_for_speech(text: str) -> str:
    for pat, repl in _MD_PATTERNS:
        text = pat.sub(repl, text)
    text = _EMOJI.sub("", text)
    return re.sub(r"\s+", " ", text).strip()


class Tts:
    name = "base"

    def synthesize(self, text: str) -> np.ndarray:
        """返回 16k/单声道/int16 的 PCM。"""
        raise NotImplementedError


# ---------------------------------------------------------------------
class EdgeTts(Tts):
    name = "edge-tts"

    def __init__(self, voice: str = "zh-CN-XiaoxiaoNeural",
                 rate: str = "+0%", volume: str = "+0%",
                 timeout_s: float = 6.0) -> None:
        try:
            import edge_tts  # noqa: F401
        except ImportError as exc:
            raise RuntimeError("edge-tts 未安装：pip install edge-tts") from exc

        self._voice = voice
        self._rate = rate
        self._volume = volume
        # 硬超时：微软服务器在国内经常连不上，卡住时必须能放弃并降级
        self._timeout_s = float(timeout_s)
        print(f"[tts] edge-tts 音色={voice} 语速={rate} 超时={self._timeout_s:.0f}s")

    def synthesize(self, text: str) -> np.ndarray:
        text = clean_for_speech(text)
        if not text:
            return np.zeros(0, dtype=np.int16)

        # ⚠️ 必须有硬超时（2026-10 实测的真实故障）：
        #    edge-tts 走 wss://speech.platform.bing.com（微软海外服务器），
        #    网络稍差就 ConnectionTimeout，而 asyncio.run() 会【无限等待】——
        #    表现为"说完话后卡十几秒才恢复"，把整条流水线拖死。
        #    这里套一层线程 + join(timeout)，到点直接放弃，让上层降级。
        mp3 = self._run_with_timeout(text, self._timeout_s)
        if not mp3:
            return np.zeros(0, dtype=np.int16)

        # edge-tts 只吐 MP3，落盘后再解码
        with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as f:
            f.write(mp3)
            tmp = Path(f.name)
        try:
            return decode_audio_file(tmp)
        finally:
            tmp.unlink(missing_ok=True)

    def _run_with_timeout(self, text: str, timeout_s: float) -> bytes:
        """在线程里跑 asyncio，超时就放弃（不阻塞调用方）。

        为什么不能只靠 asyncio.wait_for：
            aiohttp 的连接阶段有时卡在 DNS/TLS 握手，wait_for 不一定能及时
            打断底层 socket。用线程 + join(timeout) 更硬：超时后直接放弃这个
            线程（daemon，不阻止进程退出），调用方立刻拿到空结果。
        """
        result: dict = {"data": b"", "err": None}

        def _worker() -> None:
            try:
                result["data"] = asyncio.run(self._synth_async(text))
            except Exception as exc:  # noqa: BLE001
                result["err"] = exc

        t = threading.Thread(target=_worker, daemon=True)
        t.start()
        t.join(timeout_s)
        if t.is_alive():
            print(f"[tts] edge-tts 超过 {timeout_s:.0f}s 未返回，放弃本次合成")
            return b""
        if result["err"] is not None:
            print(f"[tts] edge-tts 失败：{result['err']!r}")
            return b""
        return result["data"]

    async def _synth_async(self, text: str) -> bytes:
        import edge_tts

        communicate = edge_tts.Communicate(
            text, self._voice, rate=self._rate, volume=self._volume
        )
        chunks = bytearray()
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                chunks.extend(chunk["data"])
        return bytes(chunks)


# ---------------------------------------------------------------------
class SapiTts(Tts):
    """Windows 自带离线语音（System.Speech / SAPI）。

    为什么需要（2026-10 实测的真实故障）：
        edge-tts 走 wss://speech.platform.bing.com（微软海外服务器），
        国内网络经常 ConnectionTimeout，而且 asyncio 会无限等待 ——
        表现就是"说完话卡十几秒、包子不出声"。
        本后端【完全离线】，不依赖任何网络，作为兜底保证"至少能说话"。

    必需：Windows 中文语音包。本机已有 "Microsoft Huihui Desktop"。
    音质不如 edge-tts（机器感明显），但胜在永远可用。
    """

    name = "sapi"

    def __init__(self, voice: str = "", rate: int = 0) -> None:
        self._voice = voice          # 空 = 自动挑第一个中文语音
        self._rate = rate            # SAPI 语速 -10~10，0 为正常

    def synthesize(self, text: str) -> np.ndarray:
        text = clean_for_speech(text)
        if not text:
            return np.zeros(0, dtype=np.int16)

        # 走 PowerShell 调 System.Speech：不需要额外 pip 包
        base = Path(tempfile.gettempdir()) / f"sapi_{int(time.time() * 1000)}"
        ps = base.with_suffix(".ps1")
        wav = base.with_suffix(".wav")

        voice_sel = (
            f"$s.SelectVoice('{self._voice}')" if self._voice else
            "$zh = $s.GetInstalledVoices() | "
            "Where-Object { $_.VoiceInfo.Culture.Name -like 'zh*' } | "
            "Select-Object -First 1;"
            "if ($zh) { $s.SelectVoice($zh.VoiceInfo.Name) }"
        )
        script = (
            "Add-Type -AssemblyName System.Speech\n"
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer\n"
            f"{voice_sel}\n"
            f"$s.Rate = {int(self._rate)}\n"
            f"$s.SetOutputToWaveFile('{wav}')\n"
            # 文本从 stdin 读，避免引号/换行把脚本写坏
            "$s.Speak([Console]::In.ReadToEnd())\n"
            "$s.Dispose()\n"
        )

        try:
            ps.write_text(script, encoding="utf-8-sig")
            subprocess.run(
                ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                 "-File", str(ps)],
                input=text, text=True, encoding="utf-8",
                capture_output=True, timeout=30, check=False,
            )
            if not wav.exists() or wav.stat().st_size < 100:
                print("[tts] SAPI 未产生音频（检查中文语音包是否已安装）")
                return np.zeros(0, dtype=np.int16)
            return decode_audio_file(wav)
        except Exception as exc:  # noqa: BLE001
            print(f"[tts] SAPI 合成失败：{exc!r}")
            return np.zeros(0, dtype=np.int16)
        finally:
            ps.unlink(missing_ok=True)
            wav.unlink(missing_ok=True)


# ---------------------------------------------------------------------
class FallbackTts(Tts):
    """主后端失败时自动降级到备用后端。

    用于「edge-tts（在线，音质好）→ sapi（离线，音质一般）」这种组合：
    网络正常时用好的，网络挂了也不会哑掉。
    """

    name = "fallback"

    def __init__(self, primary: Tts, backup: Tts) -> None:
        self._primary = primary
        self._backup = backup
        self._primary_fail = 0
        print(f"[tts] 主={primary.name}  备用={backup.name}")

    def synthesize(self, text: str) -> np.ndarray:
        pcm = self._primary.synthesize(text)
        if pcm.size > 0:
            self._primary_fail = 0
            return pcm

        self._primary_fail += 1
        if self._primary_fail == 1:
            print(f"[tts] {self._primary.name} 失败，降级到 {self._backup.name}")
        return self._backup.synthesize(text)


# ---------------------------------------------------------------------
class ToneTts(Tts):
    """不合成语音，只按文本长度生成一段正弦音。用于纯链路测试。"""

    name = "tone"

    def __init__(self, freq: float = 440.0, ms_per_char: float = 60.0) -> None:
        self._freq = freq
        self._ms_per_char = ms_per_char

    def synthesize(self, text: str) -> np.ndarray:
        n = int(SAMPLE_RATE * max(1, len(text)) * self._ms_per_char / 1000.0)
        t = np.arange(n, dtype=np.float64) / SAMPLE_RATE
        # 加淡入淡出，避免开关声
        wave = np.sin(2 * np.pi * self._freq * t)
        fade = min(400, n // 4)
        if fade > 0:
            wave[:fade] *= np.linspace(0, 1, fade)
            wave[-fade:] *= np.linspace(1, 0, fade)
        return (wave * 8000).astype(np.int16)


class NoneTts(Tts):
    name = "none"

    def synthesize(self, text: str) -> np.ndarray:
        return np.zeros(0, dtype=np.int16)


class WavTts(Tts):
    """生成一段静音，随后可被外部文件覆盖；主要给离线自测用。"""

    name = "silence"

    def __init__(self, ms: int = 200) -> None:
        self._ms = ms

    def synthesize(self, text: str) -> np.ndarray:
        return np.zeros(SAMPLE_RATE * self._ms // 1000, dtype=np.int16)


# ---------------------------------------------------------------------
def make_tts(cfg) -> Tts:
    backend = (cfg.tts_backend or "edge-tts").lower()

    if backend == "edge-tts":
        primary = EdgeTts(voice=cfg.tts_voice, rate=cfg.tts_rate,
                          volume=cfg.tts_volume)
        # ★ 自动兜底：edge-tts 连不上微软服务器时（国内很常见），
        #   降级到 Windows 自带离线语音，保证包子不会哑掉。
        #   写 "edge-tts" 就自动获得这个行为，不用改配置。
        try:
            backup = SapiTts()
            return FallbackTts(primary, backup)
        except Exception as exc:  # noqa: BLE001
            print(f"[tts] 离线兜底不可用（{exc}），只用 edge-tts")
            return primary
    if backend == "sapi":
        return SapiTts(voice=getattr(cfg, "tts_voice", "") or "")
    if backend == "tone":
        return ToneTts()
    if backend == "none":
        return NoneTts()
    if backend == "silence":
        return WavTts()

    raise ValueError(f"未知的 tts_backend: {cfg.tts_backend}")


__all__ = [
    "Tts",
    "EdgeTts",
    "SapiTts",
    "FallbackTts",
    "ToneTts",
    "NoneTts",
    "WavTts",
    "make_tts",
    "clean_for_speech",
    "pcm_from_bytes",
    "resample",
    "SAMPLE_RATE",
]
