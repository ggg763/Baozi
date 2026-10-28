"""PC -> TD1601 情感下发链路。

两种后端（二选一，由 config 决定）：

  A) "esp32"（推荐，任务目标链路）：
       PC --TCP(8765)--> ESP32 --UART(GPIO14)--> TD1601 PB18(UART3 RX)
     用 CTRL_EMOTION / CTRL_INTENT 控制帧把 EMO:xxx / INTENT:xxx 发过去，
     由 ESP32 原样转发。好处：TD1601 的 UART0 空出来专给 SSCOM 看日志。

  B) "serial"（直连）：
       PC --USB-TTL--> TD1601 PA17/PA18(UART0)
     用 pyserial 直接发。代价：SSCOM 与 PC 程序只能二选一占 COM 口。

协议（与 TD1601 端 emotion_fsm.c 的解析对应）：
    EMO:HAPPY | SAD | ANGRY | SURPRISED | LOVING | IDLE     情绪
    EMO:BORED | SLEEPING                                      心情
    INTENT:CHAT | GREETING | QUESTION | COMMAND | NONE        意图
"""

from __future__ import annotations

# 情绪名 -> 协议大写名（与 Baozi_PC/communication.py 完全一致）
EMOTION_MAP = {
    "happy": "HAPPY",
    "sad": "SAD",
    "angry": "ANGRY",
    "surprised": "SURPRISED",
    "loving": "LOVING",
    "neutral": "IDLE",      # neutral 在协议里就是"待机 IDLE"
    "bored": "BORED",
    "sleep": "SLEEPING",
}

INTENT_MAP = {
    "chat": "CHAT",
    "greeting": "GREETING",
    "question": "QUESTION",
    "command": "COMMAND",
    "none": "NONE",
}

_DEFAULT_BAUDRATE = 115200

# ==================== 状态字节编码（2026-09 新增）====================
# 为什么要编码（实测的真实故障）：
#   PC 一轮对话会连发 3 条命令（INTENSITY + EMO + INTENT ≈ 47 字节），
#   ESP32 一次性推给 TD1601，而 TD1601 的 UART3 FIFO 只有几十字节，
#   中途溢出导致命令被截断：
#       "INTENSITY:MEDIUM" + "EMO:SAD"  ->  "INTENSITY:EMO:SAD"
#   表现为：情绪/强度随机失灵、表情不切、强度乱跳。
#
# 解决办法：把三个字段打包成 1 个字节，一次发完（3 帧变 1 帧、47 字节变 3 字节）。
#   code = (emo_id * 8 + intent_id) * 3 + intensity_id     范围 0~215
#   ⚠️ 这三张表必须与 TD1601（UserFolder/esp_link.c 的 k_*_names）完全一致，
#      顺序错一个就会导致情绪/强度整体错位。
EMO_ID = {
    "neutral": 0, "happy": 1, "sad": 2, "angry": 3, "surprised": 4,
    "loving": 5, "idle": 6, "bored": 7, "sleeping": 8, "sleep": 8,
}
INTENT_ID = {
    "none": 0, "greeting": 1, "question": 2, "chat": 3, "command": 4,
    "thanks": 5, "goodbye": 6, "unknown": 7,
}
INTENSITY_ID = {"low": 0, "medium": 1, "high": 2}

_N_INTENT = 8
_N_INTENSITY = 3


def pack_state(emotion: str, intent: str, intensity: str) -> int:
    """把 (情绪, 意图, 强度) 打包成 0~215 的一个字节。

    >>> pack_state("happy", "chat", "medium")
    34
    """
    e = EMO_ID.get(str(emotion).strip().lower(), 0)
    i = INTENT_ID.get(str(intent).strip().lower(), 0)
    n = INTENSITY_ID.get(str(intensity).strip().lower(), 1)
    return (e * _N_INTENT + i) * _N_INTENSITY + n


def state_line(emotion: str, intent: str, intensity: str) -> str:
    """调试用：把打包结果也渲染成可读文本，便于日志比对。"""
    return (f"{emotion}/{intent}/{intensity} "
            f"-> 0x{pack_state(emotion, intent, intensity):02X}")


def emotion_line(emotion: str) -> str:
    """情绪名 -> 协议整行，如 "EMO:HAPPY"。"""
    return f"EMO:{EMOTION_MAP.get(str(emotion).lower(), 'IDLE')}"


def intent_line(intent: str) -> str:
    """意图名 -> 协议整行，如 "INTENT:CHAT"。"""
    return f"INTENT:{INTENT_MAP.get(str(intent).lower(), 'NONE')}"


# 情感强度（intensity）：低/中/高三档，TD1601 用它修正动作幅度与速度。
# 与 EMO: 分开下发是刻意的：情绪决定"演哪段"，强度决定"演多大幅度"，
# 两者更新频率不同（强度常变、情绪少变），分开后一端可独立演进。
INTENSITY_MAP = {
    "low": "LOW",
    "medium": "MEDIUM",
    "high": "HIGH",
}


def intensity_line(intensity: str) -> str:
    """强度名 -> 协议整行，如 "INTENSITY:HIGH"。"""
    return f"INTENSITY:{INTENSITY_MAP.get(str(intensity).lower(), 'MEDIUM')}"


# 命令前缀 -> ESP32 控制帧码（必须与 esp32_baozi/protocol.h 一致）
_CTRL_FOR_PREFIX = {
    "EMO": 0x09,          # CTRL_EMOTION
    "INTENT": 0x0A,       # CTRL_INTENT
    "INTENSITY": 0x0C,    # CTRL_INTENSITY
    # TRACK:ON / TRACK:OFF —— 红外追踪开关。
    # 复用了 CTRL_TD_TEXT（"任意一行，原样转发给 TD1601"），所以 ESP32 侧
    # 一行代码都不用改；TD1601 只是在 emotion_fsm_feed_line() 里多认一个前缀。
    "TRACK": 0x0B,        # CTRL_TD_TEXT
}


class Td1601Link:
    """PC -> TD1601 语义状态下发通道。

    backend:
        "esp32"  -> 经 ESP32 的 TCP 连接转发（需要传入 send_frame 回调）
        "serial" -> 直连 TD1601 串口
        "sim"    -> 只打印（不连硬件）
    """

    def __init__(self, backend: str = "esp32", port: str | None = None,
                 baudrate: int = _DEFAULT_BAUDRATE, log=print,
                 send_frame=None) -> None:
        self.backend = backend
        self.port = port
        self.baudrate = baudrate
        self.log = log
        self._send_frame = send_frame        # 由 main.py 注入（往 ESP32 发控制帧）
        self._serial = None

        if backend == "serial":
            self._connect_serial()
        elif backend == "esp32":
            if send_frame is None:
                self.log("[td] 未注入 send_frame，情感下发将只打印（模拟）")
                self.backend = "sim"
            else:
                self.log("[td] 情感下发走 ESP32 转发（PC->ESP32->TD1601 PB18）")
        else:
            self.log("[td] 模拟模式：只打印 EMO:/INTENT:")

    # ------------------------------------------------------------------
    def _connect_serial(self) -> None:
        try:
            import serial  # pyserial
        except ImportError:
            self.log("[td] 未安装 pyserial，回退到模拟模式（pip install pyserial）")
            self.backend = "sim"
            return
        try:
            self._serial = serial.Serial(self.port, self.baudrate, timeout=0.1)
            self.log(f"[td] 已连接 TD1601 串口 {self.port} @ {self.baudrate}")
        except Exception as exc:  # noqa: BLE001
            self.log(f"[td] 串口打开失败（{exc}），回退到模拟模式")
            self.backend = "sim"

    def close(self) -> None:
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:  # noqa: BLE001
                pass
            self._serial = None
            self.log("[td] 串口已关闭")

    # ------------------------------------------------------------------
    def send_line(self, line: str) -> None:
        """把一整行（如 "EMO:HAPPY"）发给 TD1601。"""
        if self.backend == "esp32" and self._send_frame is not None:
            # 帧格式：payload[0] = 控制码，payload[1..] = 整行文本。
            # ⚠️ 别再用 "是不是 EMO: 就 0x09 否则 0x0A" 这种二选一判断 ——
            #    加入 INTENSITY: 后它会落进 else 被当成意图帧。改成前缀表。
            ctrl = _CTRL_FOR_PREFIX.get(line.split(":", 1)[0].upper())
            if ctrl is None:
                self.log(f"[td] 未知命令前缀，已忽略：{line}")
                return
            payload = bytes([ctrl]) + line.encode("utf-8")
            if not self._send_frame(ctrl, payload):
                self.log(f"[td] 转发失败（ESP32 未连接？）：{line}")
            return

        if self.backend == "serial" and self._serial is not None:
            try:
                self._serial.write((line + "\r\n").encode("utf-8"))
            except Exception as exc:  # noqa: BLE001
                self.log(f"[td] 串口发送失败：{exc}")
            return

        self.log(f"UART> {line}")      # 模拟模式

    def send_emotion(self, emotion: str) -> None:
        self.send_line(emotion_line(emotion))

    def send_intent(self, intent: str) -> None:
        self.send_line(intent_line(intent))

    def send_intensity(self, intensity: str) -> None:
        """下发情感强度（low/medium/high），TD1601 据此修正动作幅度/速度。"""
        self.send_line(intensity_line(intensity))

    # ------------------------------------------------------------------
    def send_state(self, emotion: str, intent: str = "none",
                   intensity: str = "medium") -> None:
        """一次把 (情绪, 意图, 强度) 打包成 1 个字节发给 TD1601。

        这是**正式链路**：一轮对话只发一帧，避免原来三条独立命令
        连发时把 TD1601 的 UART3 FIFO 撑溢出（见文件上方编码表的说明）。

        ESP32 收到 CTRL_STATE 后原样透传为 0xAA + code 两个字节；
        TD1601 解码后又拆回三条文本走既有状态机，串口日志仍可读。

        backend=sim 时只打印可读文本，便于不开硬件调 prompt。
        """
        code = pack_state(emotion, intent, intensity)

        if self.backend == "esp32" and self._send_frame is not None:
            payload = bytes([0x0D, code])      # 0x0D = CTRL_STATE
            if not self._send_frame(0x0D, payload):
                self.log(f"[td] 状态帧转发失败（ESP32 未连接？）：0x{code:02X}")
            return

        if self.backend == "serial" and self._serial is not None:
            # 串口直连时没有 ESP32 帮拆帧，直接发帧头 + 字节
            try:
                self._serial.write(bytes([0xAA, code]))
                self._serial.flush()
            except Exception as exc:  # noqa: BLE001
                self.log(f"[td] 串口发送失败：{exc}")
            return

        self.log(f"UART> state {state_line(emotion, intent, intensity)}")

    @property
    def mode(self) -> str:
        return self.backend
