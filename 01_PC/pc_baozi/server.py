"""PC 端入口：启动语音服务器。

用法::

    python server.py                         # 用 config.json / 内置默认值
    python server.py --port 8765
    python server.py --asr-backend mock --llm-backend echo --tts-backend tone
    python server.py --config config.json --debug

依赖（按后端选装）：
    必装  numpy
    识别  faster-whisper          （推荐，CPU int8 也能实时）
    对话  openai                  （走 Ollama 的 OpenAI 兼容接口）
    合成  edge-tts miniaudio      （在线中文语音 + MP3 解码）
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from config import Config  # noqa: E402

BANNER = r"""
  ____                  _         _   _
 | __ )  __ _  ___ ___ (_)       | | | | ___  _ __ ___
 |  _ \ / _` |/ _ \_  \| |  ears | |_| |/ _ \| '__/ _ \
 | |_) | (_| | (_) / / | |       |  _  | (_) | | |  __/
 |____/ \__,_|\___/___|_| |      |_| |_|\___/|_|  \___|
        ESP32-S3-CAM 耳朵 + 嘴  ·  VAD/ASR/LLM/TTS on PC
"""


def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="ESP32 语音节点服务器（VAD -> ASR -> 大模型 -> TTS）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--config", default=None, help="JSON 配置文件路径")

    g = p.add_argument_group("通信链路（ESP32）")
    g.add_argument("--transport", choices=["serial", "tcp"], default=None,
                   help="serial=USB 串口（默认）；tcp=WiFi 模式")
    g.add_argument("--serial-port", default=None,
                   help="串口号，如 COM5（留空=自动找 ESP32）")
    g.add_argument("--serial-baud", type=int, default=None,
                   help="端口波特率（USB CDC 时无意义，CH340 之类才有用）")
    g.add_argument("--list-ports", action="store_true",
                   help="列出本机串口后退出")

    g = p.add_argument_group("网络（仅 --transport tcp 时用）")
    g.add_argument("--host", default=None)
    g.add_argument("--port", type=int, default=None)
    g.add_argument("--allow-client-ip", default=None)

    g = p.add_argument_group("VAD")
    g.add_argument("--vad-backend", choices=["energy", "webrtc"], default=None)
    g.add_argument("--vad-threshold-db", type=float, default=None)
    g.add_argument("--vad-start-ms", type=int, default=None)
    g.add_argument("--vad-end-ms", type=int, default=None)

    g = p.add_argument_group("ASR")
    g.add_argument(
        "--asr-backend",
        choices=["sensevoice", "faster-whisper", "openai-whisper", "mock"],
        default=None,
    )
    g.add_argument("--sensevoice-model", default=None, help="ModelScope 上的模型 ID")
    g.add_argument("--whisper-model", default=None)
    g.add_argument("--whisper-device", choices=["auto", "cpu", "cuda"], default=None)
    g.add_argument("--asr-language", default=None)

    g = p.add_argument_group("大模型")
    g.add_argument("--llm-backend", choices=["openai", "echo", "mock"], default=None)
    g.add_argument("--llm-base-url", default=None)
    g.add_argument("--llm-api-key", default=None)
    g.add_argument("--llm-model", default=None)

    g = p.add_argument_group("TTS")
    g.add_argument(
        "--tts-backend", choices=["edge-tts", "tone", "none", "silence"], default=None
    )
    g.add_argument("--tts-voice", default=None)
    g.add_argument("--tts-rate", default=None)

    g = p.add_argument_group("其他")
    g.add_argument("--save-audio-dir", default=None, help="把 mic/tts 音频存成 WAV")
    g.add_argument("--no-echo-guard", action="store_true",
                   help="关闭播放期间的麦克风屏蔽（会听到自己说话，一般不要用）")
    g.add_argument("--debug", action="store_true")
    return p


def make_config(argv=None) -> Config:
    args = build_argparser().parse_args(argv)

    # ★ 2026-10-28 修：没显式给 --config 时，自动加载与本文件同目录的 config.json。
    #
    # 原实现直接把 args.config（默认 None）交给 Config.load()，而
    # Config.load(None) 会立刻返回内置默认值 —— 也就是说 config.json
    # **从来没有被读取过**。表现是：在 config.json 里调好的 vad_*、play_*、
    # llm_model、transport 全部不生效，程序实际用的是 config.py 里的硬编码默认，
    # 其中 llm_model 默认还是本机不存在的 qwen2.5:7b（对话直接失败）。
    #
    # 显式 --config 仍然优先；文件不存在则退回内置默认（保持可移植）。
    cfg_path = args.config
    if cfg_path is None:
        default_cfg = Path(__file__).resolve().with_name("config.json")
        if default_cfg.is_file():
            cfg_path = str(default_cfg)
            print(f"[cfg] 使用配置文件: {cfg_path}")

    cfg = Config.load(cfg_path)

    cfg.apply_overrides(
        transport=args.transport,
        serial_port=args.serial_port,
        serial_baud=args.serial_baud,
        host=args.host,
        port=args.port,
        allow_client_ip=args.allow_client_ip,
        vad_backend=args.vad_backend,
        vad_threshold_db=args.vad_threshold_db,
        vad_start_ms=args.vad_start_ms,
        vad_end_ms=args.vad_end_ms,
        asr_backend=args.asr_backend,
        sensevoice_model=args.sensevoice_model,
        whisper_model=args.whisper_model,
        whisper_device=args.whisper_device,
        asr_language=args.asr_language,
        llm_backend=args.llm_backend,
        llm_base_url=args.llm_base_url,
        llm_api_key=args.llm_api_key,
        llm_model=args.llm_model,
        tts_backend=args.tts_backend,
        tts_voice=args.tts_voice,
        tts_rate=args.tts_rate,
        save_audio_dir=args.save_audio_dir,
    )
    if args.no_echo_guard:
        cfg.echo_guard_ms = 0
    if args.debug:
        cfg.debug = True

    return cfg


def apply_extra_overrides(cfg: Config, extra) -> Config:
    """把 main.py 里那些"只属于整合版"的参数并进 cfg。

    ⚠️ 为什么要有这个函数：main.py 是先用 _parse_extra() 单独解析自己的
    参数、再把剩下的交给 make_config() 的。如果只把这几个开关记在
    Namespace 里不写回 cfg，它们就会【静默失效】——
    实测 `python main.py --no-vision` 仍然会开视觉，
    对没有摄像头的板子来说等于直接抛异常。
    """
    if getattr(extra, "esp32_ip", None):
        cfg.esp32_ip = extra.esp32_ip
    if getattr(extra, "vision_interval", None) is not None:
        cfg.vision_interval_s = extra.vision_interval
    if getattr(extra, "vision_model", None):
        cfg.vision_model = extra.vision_model
    if getattr(extra, "no_vision", False):
        cfg.vision_enabled = False
    if getattr(extra, "no_yolo", False):
        cfg.vision_yolo_enabled = False
    if getattr(extra, "yolo_model", None):
        cfg.vision_yolo_model = extra.yolo_model
    if getattr(extra, "yolo_imgsz", None) is not None:
        cfg.vision_yolo_imgsz = extra.yolo_imgsz
    if getattr(extra, "td1601_backend", None):
        cfg.td1601_backend = extra.td1601_backend
    if getattr(extra, "td1601_port", None):
        cfg.td1601_port = extra.td1601_port
    if getattr(extra, "no_td1601", False):
        cfg.td1601_enabled = False
    return cfg


def check_dependencies(cfg: Config) -> None:
    """缺依赖时给一句人话，而不是让 import 报栈。"""
    missing: list[str] = []

    try:
        import numpy  # noqa: F401
    except ImportError:
        missing.append("numpy  ->  pip install numpy")

    # ESP32 走 USB 串口时必须有 pyserial
    if str(getattr(cfg, "transport", "serial")).lower() != "tcp":
        try:
            import serial  # noqa: F401
        except ImportError:
            missing.append("pyserial  ->  pip install pyserial   （USB 串口链路必需）")

    if cfg.asr_backend in ("sensevoice", "funasr"):
        try:
            import funasr  # noqa: F401
        except ImportError:
            missing.append(
                "funasr  ->  pip install funasr modelscope torch torchaudio"
                "   （SenseVoice 语音识别，模型会从 ModelScope 自动下载）"
            )
    elif cfg.asr_backend == "faster-whisper":
        try:
            import faster_whisper  # noqa: F401
        except ImportError:
            missing.append("faster-whisper  ->  pip install faster-whisper")
    elif cfg.asr_backend == "openai-whisper":
        try:
            import whisper  # noqa: F401
        except ImportError:
            missing.append("openai-whisper  ->  pip install openai-whisper")

    if cfg.llm_backend == "openai":
        try:
            import openai  # noqa: F401
        except ImportError:
            missing.append("openai  ->  pip install openai")

    if cfg.tts_backend == "edge-tts":
        try:
            import edge_tts  # noqa: F401
        except ImportError:
            missing.append("edge-tts  ->  pip install edge-tts")
        try:
            import miniaudio  # noqa: F401
        except ImportError:
            missing.append("miniaudio  ->  pip install miniaudio   （解码 edge-tts 的 MP3）")

    if missing:
        print("缺少以下依赖，请先安装：", file=sys.stderr)
        for m in missing:
            print("  " + m, file=sys.stderr)
        print(
            "\n只想先验证链路的话，可以用模拟后端全跑通：\n"
            "  python server.py --asr-backend mock --llm-backend echo --tts-backend tone\n",
            file=sys.stderr,
        )
        raise SystemExit(2)


def main(argv=None) -> int:
    print(BANNER)
    cfg = make_config(argv)

    if "--list-ports" in (argv if argv is not None else sys.argv[1:]):
        from link import SerialTransport

        ports = SerialTransport.available_ports()
        if not ports:
            print("一个串口都没找到。检查 USB 线（要数据线）和驱动。")
        else:
            print("本机串口：")
            for dev, desc in ports:
                print(f"  {dev}  {desc}")
            print("\nESP32 一般在 VID 303A（芯片内置 USB）或 1A86（CH340）。")
        return 0

    kind = str(cfg.transport).lower()
    if kind == "tcp":
        link_desc = f"WiFi/TCP   监听 {cfg.host}:{cfg.port}"
    else:
        link_desc = (f"USB 串口   {cfg.serial_port or '自动检测'} "
                     f"@ {cfg.serial_baud}（PC 只用外网，不需要 ESP32 的 IP）")

    print("配置：")
    print(f"  链路            {link_desc}")
    print(f"  VAD             {cfg.vad_backend} (门限 {cfg.vad_threshold_db} dBFS, "
          f"起 {cfg.vad_start_ms}ms / 止 {cfg.vad_end_ms}ms)")
    print(f"  ASR             {cfg.asr_backend} (模型 {cfg.sensevoice_model if cfg.asr_backend in ('sensevoice','funasr') else cfg.whisper_model}, 语言 {cfg.asr_language or 'auto'})")
    print(f"  LLM             {cfg.llm_backend} ({cfg.llm_base_url} / {cfg.llm_model})")
    print(f"  TTS             {cfg.tts_backend} ({cfg.tts_voice})")
    print(f"  播放策略        {'等 ' + str(cfg.pre_speak_wait_ms) + 'ms 再开口' if cfg.pre_speak_wait_ms > 0 else '立即开口'}")
    print(f"  防回环          播放期间+结束后 {cfg.echo_guard_ms}ms 屏蔽麦克风")
    print(f"  录音存档        {cfg.save_audio_dir or '关'}")
    print()

    check_dependencies(cfg)

    # 延迟 import：check_dependencies 过了再加载重依赖
    from asr import make_asr
    from llm import make_llm
    from tts import make_tts
    from link import TransportError
    from pipeline import Pipeline
    from voice_server import VoiceServer

    asr = make_asr(cfg)
    llm = make_llm(cfg)
    tts = make_tts(cfg)
    pipe = Pipeline(cfg, asr, llm, tts)

    srv = VoiceServer(cfg, pipe)
    try:
        srv.start()
        print("[link] 就绪。对着麦克风说话即可（Ctrl+C 退出）", flush=True)
        while True:
            time.sleep(0.5)
    except TransportError as exc:
        print(f"\n[link] {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\n[link] 收到 Ctrl+C，正在退出 ...", flush=True)
    finally:
        srv.stop()
        pipe.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
