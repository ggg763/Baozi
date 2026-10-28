"""实测 SenseVoice：用真实音频对比识别效果。

用法：
    python verify_sensevoice.py                     # 自动找可用的测试音频
    python verify_sensevoice.py <音频文件>          # 指定文件
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np

from audio import SAMPLE_RATE, decode_audio_file, pcm_duration_ms

CANDIDATES = [
    r"D:\CDK\baozi.ears.mouth\code\projects\Baozi_PC\test.wav",
    r"D:\CDK\baozi.ears.mouth\code\projects\test\新录音 69.m4a",
]


def main() -> int:
    from config import Config
    from asr import make_asr

    cfg = Config()

    # 找测试音频
    if len(sys.argv) > 1:
        files = [Path(sys.argv[1])]
    else:
        files = [Path(p) for p in CANDIDATES if Path(p).exists()]

    if not files:
        print("没找到测试音频。请把音频文件路径作为参数传进来。")
        return 2

    print("=" * 60)
    print(f" 后端: {cfg.asr_backend}   模型: {cfg.sensevoice_model}")
    print("=" * 60)

    t0 = time.time()
    asr = make_asr(cfg)
    print(f"\n模型加载耗时: {time.time() - t0:.1f} s\n")

    for f in files:
        print("-" * 60)
        print(f"音频: {f.name}")

        try:
            pcm = decode_audio_file(f)
        except Exception as exc:
            print(f"  读取失败: {exc!r}")
            continue

        print(f"  时长: {pcm_duration_ms(pcm):.0f} ms ({len(pcm)} 样本)")

        t = time.time()
        try:
            text = asr.transcribe(pcm)
        except Exception as exc:
            print(f"  识别失败: {exc!r}")
            continue
        dt = time.time() - t

        rtf = dt / (len(pcm) / SAMPLE_RATE) if len(pcm) else 0
        print(f"  识别耗时: {dt:.2f} s   (实时率 {rtf:.2f}x, <1 表示比实时快)")
        print(f"  结果: {text!r}")
        print()

    if hasattr(asr, "close"):
        asr.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
