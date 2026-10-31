"""一键演示的播报词：预合成 WAV，运行时纯读盘。

板子（TD1601）上没有喇叭，演示每一步开始时要说的话只能由 PC 代播：

    板子 printf "DEMO:HAPPY:1"
        -> PC 读 assets/demo/HAPPY.wav
        -> PCM 推给 ESP32 -> 喇叭

所以这里的 (名字, 文本) 必须和固件 UserFolder/demo/demo_show.c 里 k_steps[]
的 name 一一对应 —— PC 就是靠这个名字找音频文件的。改一边就要改另一边，
否则 PC 会打一行「没有 XX 的播报音频」然后静默放行（演示照演，只是没声音）。

为什么预合成而不是现场 TTS（理由和 boot_voice.py 完全一样）：
    · 演示是要当众跑的，现场合成一抖动就"卡在那儿不出声"；
    · 每句都要一次真实网络调用，10 句就是 10 次机会失败；
    · 音色必须恒定 —— 开场和结尾用不同音色，观感像换了个机器人。

重生成（改了文本 / 想换音色时跑一次，产物进版本库）：
    .venv\\Scripts\\python.exe demo_voice.py              # 生成缺的 / 全部重生成
    .venv\\Scripts\\python.exe demo_voice.py --check      # 只列状态，不联网
    .venv\\Scripts\\python.exe demo_voice.py --voice zh-CN-YunxiNeural
"""

from __future__ import annotations

import json
import sys
import time
import wave

from pathlib import Path

import numpy as np

from audio import SAMPLE_RATE, decode_audio_file
from boot_voice import _synth_pcm, DEFAULT_VOICE, DEFAULT_RATE

# 演示播报词：与固件 k_steps[] 的顺序完全一致。
#
# 写法上的三条经验：
#   1. 一句 15~25 字（约 3~4 秒），太长观众会不耐烦，太短来不及看清动作；
#   2. 只说"现在演的是什么 + 看出来是什么"，不念参数（"300 微秒脉宽"没人想听）；
#   3. 用"我会……"的第一人称 —— 这是包子在介绍自己，不是旁白在念说明书。
DEMO_SCRIPTS: "list[tuple[str, str]]" = [
    ("IDLE",      "没事的时候，我会自己慢慢巡视四周，看看有没有人在旁边。"),
    ("HAPPY",     "开心的时候，我会笑着摇头晃脑，动作也跟着轻快起来。"),
    ("SAD",       "难过的时候，我会低下头，慢慢地摇一摇，像在叹气。"),
    ("ANGRY",     "生气了，我会昂起头，用力甩两下脑袋，表示我很不满意。"),
    ("SURPRISED", "惊讶的时候，我会猛地抬起头，愣在那里好一会儿。"),
    ("LOVING",    "被摸一摸，我会微微抬头、歪着脑袋，跟你撒个娇。"),
    ("EIGHT",     "我的脖子还能画一个横八字，左右和上下一起动。"),
    ("SLEEP",     "困了的话，我会低下头轻轻呼吸，慢慢进入睡眠。"),
    ("WINK",      "我还会眨眼，也能朝你抛个媚眼。"),
    ("GLANCE",    "眼睛可以左右转动，看向你感兴趣的方向。"),
]

_HERE = Path(__file__).resolve().parent
DEMO_DIR = _HERE / "assets" / "demo"
DEMO_MANIFEST = DEMO_DIR / "manifest.json"


def wav_path(name: str) -> Path:
    """某个演示项对应的音频文件。"""
    return DEMO_DIR / ("%s.wav" % str(name).strip().upper())


def _write_wav(path: Path, pcm: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(np.asarray(pcm, dtype="<i2").tobytes())


def load(name: str) -> "np.ndarray | None":
    """读一条演示播报音频；没有这个文件就返回 None（调用方静默放行）。"""
    p = wav_path(name)
    if not p.exists():
        return None
    try:
        pcm = decode_audio_file(p)
    except Exception as exc:  # noqa: BLE001
        print("[demo] 播报音频读不出来（%s）：%r" % (p.name, exc))
        return None
    if pcm.size == 0:
        return None
    return pcm.astype(np.int16, copy=False)


def missing() -> "list[str]":
    """缺哪几条（按 DEMO_SCRIPTS 的顺序）。"""
    return [n for n, _ in DEMO_SCRIPTS if not wav_path(n).exists()]


def status() -> int:
    """打印清单状态，返回缺失条数。"""
    miss = missing()
    total = len(DEMO_SCRIPTS)
    print("演示播报音频：%s" % DEMO_DIR)
    for name, text in DEMO_SCRIPTS:
        p = wav_path(name)
        if p.exists():
            try:
                pcm = decode_audio_file(p)
                ms = pcm.size * 1000.0 / SAMPLE_RATE
                mark = "%5.1fs" % (ms / 1000.0)
            except Exception:  # noqa: BLE001
                mark = "读不出来"
        else:
            mark = "缺"
        print("  %-10s %-8s %s" % (name, mark, text))
    print("合计 %d 条，缺 %d 条" % (total, len(miss)))
    return len(miss)


def generate(voice: str = DEFAULT_VOICE, rate: str = DEFAULT_RATE,
             only_missing: bool = False, quiet: bool = False) -> int:
    """合成演示播报音频，返回成功条数。

    only_missing=True 时只补缺的（日常用）；默认整批重生成（改了文本/音色）。
    """
    DEMO_DIR.mkdir(parents=True, exist_ok=True)
    ok = 0
    for i, (name, text) in enumerate(DEMO_SCRIPTS, 1):
        path = wav_path(name)
        if only_missing and path.exists():
            continue
        print("[demo] 合成 %d/%d %s：%s" % (i, len(DEMO_SCRIPTS), name, text))
        try:
            pcm = _synth_pcm(text, voice, rate)
        except Exception as exc:  # noqa: BLE001
            print("[demo]   失败：%r" % (exc,))
            continue
        _write_wav(path, pcm)
        ms = pcm.size * 1000.0 / SAMPLE_RATE
        print("[demo]   -> %s  %.0f ms / %d 字节" % (path.name, ms, path.stat().st_size))
        ok += 1

    DEMO_MANIFEST.write_text(json.dumps({
        "voice": voice,
        "rate": rate,
        "scripts": DEMO_SCRIPTS,
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    if not quiet:
        print("[demo] 播报音频 %d/%d 条已写入 %s" % (ok, len(DEMO_SCRIPTS), DEMO_DIR))
    return ok


def known(name: str) -> bool:
    """这个名字在不在演示脚本里（板子报了个没见过的名字时用得上）。"""
    n = str(name).strip().upper()
    return any(n == k for k, _ in DEMO_SCRIPTS)


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--check" in argv:
        raise SystemExit(1 if status() else 0)

    voice = DEFAULT_VOICE
    if "--voice" in argv:
        try:
            voice = argv[argv.index("--voice") + 1]
        except IndexError:
            pass
    raise SystemExit(0 if generate(voice=voice, only_missing="--missing" in argv) else 1)
