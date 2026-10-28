"""PC 端 UDP 自动发现 —— 与 ESP32 互相广播地址，换网络不用改 IP。

协议（与 ESP32 侧 discovery.cpp 一致，端口 8766）：
    PC    每 2 秒广播 -> 255.255.255.255:8766  "PC:<PC的IP>:<TCP端口>"
    ESP32 每 2 秒广播 -> 255.255.255.255:8766  "ESP32:<ESP32的IP>:<HTTP端口>"

本模块做两件事：
  1) 让 ESP32 知道 PC 在哪（发上面的 PC 广播）—— ESP32 据此连语音 TCP；
  2) 让 PC 知道 ESP32 在哪（收 ESP32 广播）—— 图像链路据此抓图。

用法：
    disc = PcDiscovery(tcp_port=8765)
    disc.start()
    ...
    ip = disc.esp32_ip()      # 发现到的 ESP32 地址（可能为 None）
    disc.stop()
"""

from __future__ import annotations

import socket
import threading
import time
from typing import Callable, Optional

DISCOVERY_PORT = 8766
BEACON_INTERVAL_S = 2.0


def _local_ip() -> str:
    """取本机在局域网里的 IP（用一个不出网的 UDP connect 探测，不发送实际数据）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "0.0.0.0"
    finally:
        s.close()


class PcDiscovery:
    """PC 侧地址广播 + ESP32 发现。"""

    def __init__(self, tcp_port: int = 8765, log: Callable[[str], None] = print) -> None:
        self.tcp_port = tcp_port
        self.log = log
        self._esp32_ip: Optional[str] = None
        self._stop = False
        self._thread: Optional[threading.Thread] = None
        self._sock: Optional[socket.socket] = None
        self._my_ip = _local_ip()

    # ------------------------------------------------------------------
    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.bind(("", DISCOVERY_PORT))
            s.settimeout(0.5)
        except OSError as exc:  # noqa: BLE001
            self.log(f"[disc] UDP 发现启动失败：{exc}（图像链路需手动填 --esp32-ip）")
            self._sock = None
            return

        self._sock = s
        self._stop = False
        self._thread = threading.Thread(target=self._loop, name="discovery", daemon=True)
        self._thread.start()
        self.log(f"[disc] UDP 发现已启动：本机 {self._my_ip}:{self.tcp_port}"
                 f"（广播 {DISCOVERY_PORT} -> {', '.join(self._broadcast_targets())}）")

    def stop(self) -> None:
        self._stop = True
        if self._thread:
            self._thread.join(timeout=1.5)
            self._thread = None
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    # ------------------------------------------------------------------
    def esp32_ip(self) -> Optional[str]:
        """发现到的 ESP32 地址；还没发现到返回 None。"""
        return self._esp32_ip

    def my_ip(self) -> str:
        return self._my_ip

    # ------------------------------------------------------------------
    def _broadcast_targets(self) -> list:
        """广播目的地。回到纯 WiFi 单网卡：只需受限广播地址。"""
        return ["255.255.255.255"]

    # ------------------------------------------------------------------
    def _loop(self) -> None:
        last_beacon = 0.0
        while not self._stop and self._sock is not None:
            # ---- 发：每 BEACON_INTERVAL_S 广播自己，让 ESP32 找到 PC ----
            now = time.monotonic()
            if now - last_beacon >= BEACON_INTERVAL_S:
                last_beacon = now
                # 网络可能换过，刷新一次本机 IP（单网卡：默认路由即可）
                self._my_ip = _local_ip()
                pkt = f"PC:{self._my_ip}:{self.tcp_port}".encode("ascii")
                for dst in self._broadcast_targets():
                    try:
                        self._sock.sendto(pkt, (dst, DISCOVERY_PORT))
                    except OSError:
                        pass

            # ---- 收：ESP32 的广播 ----
            try:
                data, _addr = self._sock.recvfrom(128)
            except socket.timeout:
                continue
            except OSError:
                break

            msg = data.decode("utf-8", "replace")
            if not msg.startswith("ESP32:"):
                continue
            parts = msg.split(":")
            if len(parts) >= 2 and parts[1]:
                ip = parts[1]
                if ip != self._esp32_ip:
                    self._esp32_ip = ip
                    self.log(f"[disc] 发现 ESP32: {ip}")
