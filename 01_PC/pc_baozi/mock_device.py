"""假 ESP32：在电脑上模拟设备端，用来在没接硬件时验证 PC 服务端。

它做三件事：
  1) 按协议连上 server.py，发 HELLO；
  2) 发一段合成的“语音”（静音 + 类语音信号），触发 VAD/ASR/LLM/TTS；
  3) 把收到的 TTS PCM 存成 WAV，方便你直接试听（默认 mock_device_reply.wav）。

用法::

    # 终端 1
    python server.py --asr-backend mock --llm-backend echo --tts-backend tone

    # 终端 2
    python mock_device.py --host 127.0.0.1 --port 8765

也可以用它来测真实后端（把 server.py 换成默认的 whisper/ollama/edge-tts），
只是第 2 步的“语音”是合成的，识别结果是乱的就对了。
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

from audio import SAMPLE_RATE, encode_wav, pcm_from_bytes, pcm_to_bytes  # noqa: E402
from protocol import Ctrl, FrameDecoder, Pkt, encode, encode_ctrl  # noqa: E402


def make_speech(ms: int, level_db: float = -20.0, seed: int = 0) -> np.ndarray:
    """合成一段类似语音的信号（几个谐波 + 噪声 + 幅度调制）。"""
    rng = np.random.default_rng(seed)
    n = SAMPLE_RATE * ms // 1000
    t = np.arange(n) / SAMPLE_RATE
    sig = (
        0.6 * np.sin(2 * np.pi * 180 * t)
        + 0.3 * np.sin(2 * np.pi * 320 * t)
        + 0.2 * rng.standard_normal(n)
    )
    amp = 10 ** (level_db / 20.0) * 32768.0
    sig = sig / (np.sqrt(np.mean(sig**2)) + 1e-9) * amp
    sig *= 0.6 + 0.4 * np.sin(2 * np.pi * 4 * t)
    return np.clip(sig, -32768, 32767).astype(np.int16)


def make_silence(ms: int, level_db: float = -75.0, seed: int = 1) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = SAMPLE_RATE * ms // 1000
    amp = 10 ** (level_db / 20.0) * 32768.0
    return np.clip(rng.standard_normal(n) * amp, -32768, 32767).astype(np.int16)


def main() -> int:
    ap = argparse.ArgumentParser(description="模拟 ESP32 语音节点")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--speech-ms", type=int, default=900, help="合成语音长度")
    ap.add_argument("--listen-s", type=float, default=8.0, help="等待回复的秒数")
    ap.add_argument("--out", default="mock_device_reply.wav", help="收到的音频存这里")
    args = ap.parse_args()

    print(f"[mock] 连接 {args.host}:{args.port} ...")
    s = socket.create_connection((args.host, args.port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.5)
    print("[mock] 已连接")

    s.sendall(encode_ctrl(Ctrl.HELLO, b"esp32s3cam/mock-device"))

    # ---- 发一段“语音”：400ms 静音 + N ms 说话 + 1s 静音收尾 ----
    # 最后的静音很关键：VAD 靠尾部静音判断“这句话说完了”。
    stream = np.concatenate(
        [make_silence(400), make_speech(args.speech_ms, -20.0, seed=11), make_silence(1500)]
    )
    step = 320  # 20ms
    print(f"[mock] 推送 {len(stream) / SAMPLE_RATE:.2f} 秒模拟音频 ...")
    for i in range(0, len(stream), step):
        s.sendall(encode(Pkt.MIC_PCM, pcm_to_bytes(stream[i : i + step])))
        time.sleep(0.020)  # 按真实时间推，别把服务器喂爆

    # ---- 收回复 ----
    audio = bytearray()
    ctrl_seen: list[str] = []
    dec = FrameDecoder()
    deadline = time.time() + args.listen_s

    print(f"[mock] 等待回复（最多 {args.listen_s} 秒）...")
    while time.time() < deadline:
        try:
            data = s.recv(4096)
        except socket.timeout:
            continue
        if not data:
            print("[mock] 服务器关闭了连接")
            break
        for f in dec.feed(data):
            if f.type == Pkt.TTS_PCM:
                audio.extend(f.payload)
            elif f.type == Pkt.CTRL and f.payload:
                name = Ctrl(f.payload[0]).name if f.payload[0] in Ctrl._value2member_map_ else str(f.payload[0])
                if name not in ctrl_seen:
                    ctrl_seen.append(name)
                    print(f"[mock] <-- CTRL {name}")
            elif f.type == Pkt.TEXT:
                print(f"[mock] <-- TEXT {f.payload.decode('utf-8', 'replace')}")

    s.close()

    print(f"[mock] 收到音频 {len(audio)} 字节（{len(audio)/2/SAMPLE_RATE:.2f} 秒）")
    print(f"[mock] 控制帧: {ctrl_seen}")

    if audio:
        out = Path(args.out)
        encode_wav(pcm_from_bytes(bytes(audio)), out)
        print(f"[mock] 已保存到 {out.resolve()}，可以直接播放试听")
    else:
        print("[mock] 没收到音频——检查服务器日志里的 VAD 门限和识别结果")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
