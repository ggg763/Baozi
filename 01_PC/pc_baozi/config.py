"""PC 端配置。命令行参数 > config.json > 这里的默认值。"""

from __future__ import annotations

import json
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict

SAMPLE_RATE = 16000  # 全链路统一：16k / 16bit / 单声道
FRAME_MS = 20  # 与固件 MIC_BLOCK_MS 对齐（只用于统计/日志）


@dataclass
class Config:
    # ---------------- 通信链路（ESP32 <-> PC）----------------
    # ★ 2026-11：默认走【USB 串口】。ESP32 用 USB 线插在这台电脑上，
    #   与 ESP32 有关的语音 / 图像 / 情感全走这根线；
    #   本机的网络只用来访问外部服务（edge-tts、天气、Ollama 等），
    #   不需要和 ESP32 处于同一局域网，也不需要知道它的 IP。
    #
    #   "serial" -> USB 串口（备用，需配合固件 USE_USB_LINK=1）
    #   "tcp"    -> WiFi + TCP（默认，当前用这个；需配合固件 USE_USB_LINK=0）
    transport: str = "tcp"

    # ---- 传输 = serial 时用 ----
    # 留空 = 自动找（按 ESP32-S3 的 USB VID 找，找不到就找第一个可用串口）。
    # 想看有哪些口：python main.py --list-ports
    serial_port: str = ""
    # USB CDC 实际忽略波特率；用 CH340/CP2102 这类 USB-UART 桥时才是真的，
    # 那时 115200 带不动 64KB/s 的音频+图像，必须调高（两端要一致）。
    serial_baud: int = 921600

    # ---- 传输 = tcp 时用（WiFi 模式）----
    host: str = "0.0.0.0"
    port: int = 8765
    # 只允许这个 IP 连进来（留空 = 不限制）。设成 ESP32 的 IP 更安全。
    allow_client_ip: str = ""

    # ---------------- 设备 ----------------
    # 发送音频的“说话方”方向：默认就是唯一的连接。多连接时按 hello 里的
    # 设备名匹配；匹配不到就用第一个连接。
    device_match: str = "esp32s3cam"

    # ---------------- VAD ----------------
    # 注意：webrtcvad 在 Python 3.14 上装不上（C 扩展不支持），
    # 所以这里用"双门限能量 VAD"来兼顾灵敏度与抗噪：
    #   * 判定"开始说话"用【灵敏】门限（太低会吞掉开头几个字）
    #   * 判定"说话中/结束"用【保守】门限（太低会被噪声拖着不结束）
    vad_backend: str = "energy"
    vad_threshold_db: float = -30.0        # 兼容旧配置（未填下面两项时用它）
    # 开始判定门限：越接近噪声底越灵敏。-38 让正常说话音量的起头就能触发。
    vad_start_threshold_db: float = -38.0
    # 持续判定门限：更保守，避免噪声/余音把一句话无限拖长。
    vad_end_threshold_db: float = -28.0
    # 连续多少毫秒的语音才认为"真的开始说话"。
    # 500→120：尽快进入录音状态（否则一句话的前半截会被丢掉）。
    vad_start_ms: int = 120
    # 说话中途停顿多少毫秒算"说完了"。
    # 700→900：停顿容限放宽一点，减少"一句话被切成两段"。
    vad_end_ms: int = 900
    # 一句最长多少毫秒，超了就强制切一段去识别（防止一直说个不停）
    # ⚠️ 这是【兜底】而不是常态：如果日志里频繁出现"原因=maxlen"，
    #    说明"说话结束"判不出来（门限太高 / 噪声底盖住 hold_db / 麦克风削顶），
    #    别靠调大它绕过——那样只会让每句话都变成 15 秒的长录音。
    #    这里从 15000 收到 10000：真人一句话很少超过 10 秒，兜底切得早点
    #    也能让"卡住"更快自愈。
    vad_max_utterance_ms: int = 10000
    # 说话前保留多少毫秒，避免把首字切掉。
    # 300→600：判定触发时把之前 600ms 的音频一起带上，开头不会被切。
    vad_preroll_ms: int = 600

    # ---------------- ASR ----------------
    # 默认用 SenseVoice（阿里 FunASR），中文识别明显强于 whisper-tiny，
    # 而且是纯本地推理、模型从 ModelScope（国内站）下载。
    # 想换回 whisper：--asr-backend faster-whisper
    asr_backend: str = "sensevoice"  # sensevoice | faster-whisper | openai-whisper | mock

    # SenseVoice 模型（ModelScope 上的 ID，不要改成 HuggingFace 的名字）
    sensevoice_model: str = "iic/SenseVoiceSmall"
    # tiny 已随项目准备好（本机缓存里就有），加载只需几秒、不联网，
    # 适合先把整条链路跑通。中文识别质量到 small 才明显变好，
    # 但 small 约 480MB，国内镜像下载容易卡住 —— 想升级见 README 的
    # 「换更大的 whisper 模型」。
    whisper_model: str = "tiny"  # tiny / base / small / medium / large-v3
    whisper_device: str = "auto"  # auto | cpu | cuda
    whisper_compute_type: str = "auto"  # auto | int8 | float16 | float32
    asr_language: str = "zh"  # 留空 = 自动检测
    asr_min_ms: int = 300  # 太短的片段直接丢掉不识别

    # ---------------- 大模型（OpenAI 兼容）----------------
    # 默认本地 Ollama。base_url 要带 /v1。
    llm_backend: str = "openai"  # openai | echo | mock
    llm_base_url: str = "http://localhost:11434/v1"
    llm_api_key: str = "ollama"
    # 默认走本地 Ollama 的纯文本模型。不要用带思维链的模型（如 qwen3 系列）：
    # 它们会把 token 全花在推理上、content 返回空，语音助手场景下不可用。
    llm_model: str = "qwen2.5:7b"
    # 温度：越低输出越稳定、越可预测。交互体验优先 -> 0.3（原 0.7 偏随机）。
    llm_temperature: float = 0.3
    # 回复要念出来，两三句话足够；上限调小能明显减少生成时间（512 → 200）。
    llm_max_tokens: int = 320
    llm_timeout_s: float = 120.0
    # 上下文窗口。语音助手不需要长上下文，调小可省显存、加快首 token。
    llm_num_ctx: int = 2048
    # 启动时先"预热"一次（把模型加载进显存并常驻），避免第一句话等 10 秒以上。
    llm_warmup: bool = True
    # 固定人设提示词：留空则从 pc_baozi/prompts/system_prompt.txt 加载
    # （包子的完整人格 · 见 LLM 类里的 _render_system 会填充 {robot_emotion} 等占位符）
    system_prompt: str = ""
    # 保留最近多少轮对话作为上下文（0 = 无记忆）。
    # 8 轮会让 prompt 越来越长、越来越慢；语音助手 4 轮足够（8 → 4）。
    history_turns: int = 4

    # ---------------- TTS ----------------
    tts_backend: str = "edge-tts"  # edge-tts | none | tone
    tts_voice: str = "zh-CN-XiaoxiaoNeural"
    tts_rate: str = "+0%"  # 语速，例如 +10% / -10%
    tts_volume: str = "+0%"

    # ---------------- 播放节奏 ----------------
    # ESP32 播放缓冲 1.5 秒。推流时尽量把 ESP32 端缓冲维持在
    # play_target_s 左右：既不会撑爆，也不会让它饿死。
    play_target_s: float = 0.35
    play_start_prefill_s: float = 0.25  # 开始新一轮播放前先预填这么多
    play_max_s: float = 1.20  # 硬上限，绝不把 ESP32 缓冲写满

    # ---------------- 打断 / 抢话 ----------------
    # 注意：现在【不再】在播放中途打断。旧做法太不可靠 —— VAD 门限低到
    # 喇叭自身的声音/呼吸/键盘声都算"语音"，包子刚开口就被打断，
    # 只说得出开头几个字。改成下面的"备好 -> 等一会儿 -> 再开口"。
    #
    # 话已备好后，等这么多毫秒看用户会不会接着说：
    #   期间用户【真的在说话】-> 丢掉这段话，继续听
    #   否则到点就开口
    # ⚠️ 这个窗口越长，"回复被环境噪声挤掉"的概率越大。
    #    这里从 1000 降到 250：备好就尽快开口，不给噪声太多插空子的机会。
    #    想彻底不等，设为 0（备好立即开口）。
    pre_speak_wait_ms: int = 250
    # WAITING 窗口里，多长的声音才算"用户真的又说话了"（要取消本次回复）。
    # 短于这个时长的声音（咳嗽、碰桌、键盘）不会取消回复。
    # 600ms 大约是一个短句的开头，能滤掉绝大多数瞬时噪声。
    wait_cancel_min_ms: int = 600

    # ---------------- 声学回环（喇叭声音被自己麦克风收进去）----------------
    # 喇叭离麦克风只有几厘米时，回环必然很强：包子会听到自己说的话，
    # 把它当成用户输入 -> ASR -> LLM -> 又生成一句，于是自己跟自己对话。
    # 本机没有回声消除(AEC)能力，所以用最简单可靠的办法：
    #   从进入 SPEAKING 起，到播放结束后再等 echo_guard_ms，这段时间内
    #   收到的麦克风数据全部丢弃（不喂给 VAD）。
    #
    # 代价：这段时间你插不上话。但对"几厘米间距"的硬件来说这是唯一稳的做法。
    # 想缩短"说不进去"的窗口，就调小这个值（但太小会把喇叭余音当成用户说话）。
    echo_guard_ms: int = 600

    # ---------------- 录音存档（可选）----------------
    save_audio_dir: str = ""  # 留空 = 不存

    # ---------------- 图像描述（眼睛）----------------
    # 整合进来的"图片"链路：PC 从 ESP32 抓图 -> YOLO 目标检测（首选）
    # -> 没检出时才回落 qwen3-vl 做场景描述。
    # 设 vision_enabled = False 就只跑语音对话，不开图像。
    vision_enabled: bool = True
    # ESP32 地址：WiFi 模式下起步时用 UDP 发现拿真实地址，
    # 发现失败时才回落到这个值（兜底）。
    esp32_ip: str = "10.16.117.204"
    vision_model: str = "qwen3-vl:2b"      # Ollama 视觉模型（YOLO 没检出时的兜底）
    vision_interval_s: float = 5.0         # 每隔多少秒抓一次图并描述
    vision_timeout_s: float = 30.0         # 抓图 HTTP 超时（秒）
    vision_save_jpg: str = "captured.jpg"  # 抓到的图存这里（留空 = 不存）

    # ---- YOLO 目标检测（首选通路；qwen3-vl 太慢且会幻觉、给不出位置）----
    # 模型：yolov8n.pt（COCO 80 类，含人 / 杯子 / 手机 / 书 / 猫狗…）
    #       首次运行会自动下载（~6MB，从 GitHub）。换成自己的模型填路径即可。
    vision_yolo_enabled: bool = True
    vision_yolo_model: str = "yolov8n.pt"
    vision_yolo_conf: float = 0.25          # 置信度阈值（低=更敏感但误报多）
    vision_yolo_imgsz: int = 640            # 推理尺寸。CPU 嫌慢可降到 320（快约 4 倍）
    # YOLO 什么都没检出时，是否回落 VL 描述场景。
    #   True  信息更全，但每次没检出都会多花 0.6~2s
    #   False 只信 YOLO（快），没检出就如实说"没看清"
    vision_vl_fallback: bool = True

    # ---------------- TD1601 行为层（情感下发）----------------
    # 情感/意图怎么送到 TD1601：
    #   "esp32"  -> PC --(USB 串口)--> ESP32 --UART(GPIO14)--> TD1601 PB18（推荐）
    #                同一根 USB 线，PC 只发控制帧，ESP32 原样转发；
    #                TD1601 的 UART0 空出来专给 SSCOM 看日志
    #   "serial" -> PC 用 USB-TTL 直连 TD1601 UART0（PA17/PA18）
    #               代价：SSCOM 与 PC 程序只能二选一占 COM 口
    #   "sim"    -> 只打印 EMO:/INTENT:，不连硬件
    td1601_enabled: bool = True
    td1601_backend: str = "esp32"     # esp32 | serial | sim
    td1601_port: str = ""             # backend=serial 时才用，例如 "COM5"
    td1601_baud: int = 115200

    # ---------------- 对话记忆（参照 Baozi_PC/memory）----------------
    # 短期历史：保留最近若干轮对话（让包子记得刚才聊了什么）；
    # 长期画像：历史太长时由 LLM 压缩成一段"用户画像"存盘（记住用户是谁/偏好），
    #           之后每轮注入 system prompt。这样长聊也不会撑爆上下文。
    memory_enabled: bool = True
    memory_file: str = "memory.json"     # 长期记忆（人物档案）存这里（重启仍在）
    memory_max_history: int = 20         # 短期最多保留多少轮（1 轮 = 用户+助手）

    # ---------------- 联网检索（天气）----------------
    # 包子（本地模型）自己没有联网能力，问天气只会编。
    # 这里在 PC 端调 wttr.in 查到实时数据，再注入给模型转述。
    # 数据源 wttr.in 免费、免注册、免 API key。关掉就把 weather_enabled 设 False。
    weather_enabled: bool = True
    weather_default_city: str = "武汉"    # 用户没说城市时用这个（如"冷不冷"）
    weather_timeout_s: float = 6.0       # 联网超时（秒）；超时就直接放弃，不拖慢对话

    # ---------------- 组员 app 服务（习惯 / 日程 / 提醒 / 小程序）----------------
    # 深度整合：由 main.py 统一拉起「后端 + 屏幕桥接」（实现见 app/service.py）。
    # 这套东西管三件事：
    #   1) 手机小程序（app/miniprogram）通过后端读写习惯/日程
    #   2) 后端数据 -> A5 帧 -> TD1601 屏幕卡片（习惯看板 / 今日日程）
    #   3) 到点提醒：屏幕弹卡 + 借主语音链路说一句话
    # ⚠️ 提醒的语音出口是 pipeline.say()，不是独立的 TCP 8765 服务端 ——
    #    那个端口归 voice_server.py 独占，起第二个会直接撞"地址已在使用"。
    app_enabled: bool = True             # 总开关：False = 完全不碰 app 那一套
    app_backend_autostart: bool = True   # 自动起 FastAPI 后端（已有人在跑则跳过）
    app_backend_host: str = "127.0.0.1"
    app_backend_port: int = 8000
    app_board_port: str = ""             # TD1601 屏幕串口，空 = 自动握手识别
    app_board_baud: int = 115200
    app_board_dry: bool = False          # True = 不开串口，只打印将要下发的帧
    app_sync_interval_s: float = 5.0     # 多久回后端拉一次习惯/日程
    app_resync_s: float = 300.0          # 多久强制全量重发（防丢帧/板子重启）
    app_board_log: bool = False          # 是否打印板子 printf 日志（排障时开）

    # ---------------- 日志 ----------------
    debug: bool = False

    # ------------------------------------------------------------------
    @classmethod
    def load(cls, path: str | Path | None = None) -> "Config":
        """从 JSON 文件加载，未出现的键用默认值。"""
        cfg = cls()
        if not path:
            return cfg

        p = Path(path)
        if not p.exists():
            return cfg

        data: Dict[str, Any] = json.loads(p.read_text(encoding="utf-8"))
        known = {f for f in asdict(cfg)}
        for k, v in data.items():
            if k in known:
                setattr(cfg, k, v)
        return cfg

    def apply_overrides(self, **kwargs) -> "Config":
        """命令行覆盖：只认不是 None 的值，键名用下划线形式。"""
        for k, v in kwargs.items():
            if v is None:
                continue
            if hasattr(self, k):
                setattr(self, k, v)
        return self

    def resolved_compute_type(self) -> str:
        if self.whisper_compute_type != "auto":
            return self.whisper_compute_type
        # CPU 上用 int8 最快，GPU 上用 float16
        return "int8" if self.whisper_device in ("cpu", "auto") else "float16"
