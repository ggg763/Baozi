# -*- coding: utf-8 -*-
"""
communication.py - PC ↔ TD1601 通信层

架构调整 v1.0：PC 只下发"语义状态"（emotion / intent），
不下发动作指令；具体行为由 TD1601 行为层自主决策。

协议（见 td1601_code/docs/protocol.md）：
    轻量协议：EMO:HAPPY\\r\\n   /   INTENT:CHAT\\r\\n
    JSON 备选：{"type":"emotion","value":"happy"}

两种后端：
    - 模拟模式（默认）：把下行消息打印为 "UART> ..."，无硬件可跑；
    - 串口模式：通过 pyserial 发送到真实 TD1601（需安装 pyserial）。

用法：
    from communication import Communication
    comm = Communication(port="COM3")      # 不传 port 即模拟模式
    comm.send_emotion("happy")
    comm.send_intent("chat")
    comm.close()
"""

import os

# 情绪名 -> 协议大写名
EMOTION_MAP = {
    "happy": "HAPPY",
    "sad": "SAD",
    "angry": "ANGRY",
    "surprised": "SURPRISED",
    "loving": "LOVING",
    "neutral": "IDLE",
    "bored": "BORED",
    "sleep": "SLEEPING",
}

# 意图名 -> 协议大写名
INTENT_MAP = {
    "chat": "CHAT",
    "greeting": "GREETING",
    "question": "QUESTION",
    "command": "COMMAND",
    "none": "NONE",
}

_DEFAULT_BAUDRATE = 115200


class Communication:
    """PC → TD1601 语义状态下发通道。"""

    def __init__(self, port=None, baudrate=_DEFAULT_BAUDRATE, use_json=False):
        """
        Args:
            port: 串口设备名（如 "COM3" / "/dev/ttyUSB0"）。
                  None 时使用模拟模式（仅打印，不连接硬件）。
            baudrate: 波特率（默认 115200）。
            use_json: True 时使用 JSON 协议，否则轻量协议。
        """
        self.port = port
        self.baudrate = baudrate
        self.use_json = use_json
        self._serial = None
        self._mode = "sim"

        if port:
            self._connect()

    # ---------- 连接管理 ----------
    def _connect(self):
        try:
            import serial  # pyserial
        except ImportError:
            print("⚠️ 未安装 pyserial，串口模式不可用，回退到模拟模式。")
            print("   安装：pip install pyserial")
            return
        try:
            self._serial = serial.Serial(self.port, self.baudrate, timeout=0.1)
            self._mode = "serial"
            print(f"🔌 已连接 TD1601 串口: {self.port} @ {self.baudrate}")
        except Exception as e:
            print(f"⚠️ 串口打开失败（{e}），回退到模拟模式。")

    def close(self):
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:
                pass
            self._serial = None
            print("🔌 串口已关闭。")

    # ---------- 发送 ----------
    def send_emotion(self, emotion: str):
        """下发情绪状态（TD1601 据此自主决策行为）。"""
        name = EMOTION_MAP.get(str(emotion).lower(), "IDLE")
        if self.use_json:
            self._send_line(f'{{"type":"emotion","value":"{name.lower()}"}}')
        else:
            self._send_line(f"EMO:{name}")

    def send_intent(self, intent: str):
        """下发意图（预留，TD1601 当前优先响应 emotion）。"""
        name = INTENT_MAP.get(str(intent).lower(), "NONE")
        if self.use_json:
            self._send_line(f'{{"type":"intent","value":"{name.lower()}"}}')
        else:
            self._send_line(f"INTENT:{name}")

    def send_line(self, line: str):
        """直接发送一行原始消息（调试用）。"""
        self._send_line(line)

    # ---------- 内部 ----------
    def _send_line(self, line: str):
        payload = line + "\r\n"
        if self._mode == "serial" and self._serial is not None:
            try:
                self._serial.write(payload.encode("utf-8"))
            except Exception as e:
                print(f"⚠️ 串口发送失败: {e}")
        else:
            # 模拟模式：打印下行消息，便于无硬件联调观察
            print(f"UART> {line}")

    # ---------- 便捷入口（与旧测试兼容） ----------
    @property
    def mode(self) -> str:
        return self._mode


# 简单自测：python communication.py
if __name__ == "__main__":
    comm = Communication()          # 模拟模式
    comm.send_emotion("happy")
    comm.send_emotion("loving")
    comm.send_intent("chat")
    comm.use_json = True
    comm.send_emotion("sad")
    comm.close()
    print("communication.py 自测完成（模拟模式）")
