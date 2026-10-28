"""PC 端 UDP 自动发现 —— 与 ESP32 互相广播地址，换网络不用改 IP。

协议（与 ESP32 侧 discovery.cpp 一致，端口 8766）：
    PC    每 2 秒广播 -> 255.255.255.255:8766  "PC:<PC的IP>:<TCP端口>"
    ESP32 每 2 秒广播 -> 255.255.255.255:8766  "ESP32:<ESP32的IP>:<HTTP端口>"

本模块做三件事：
  1) 让 ESP32 知道 PC 在哪（发上面的 PC 信标）—— ESP32 据此连语音 TCP；
  2) 让 PC 知道 ESP32 在哪（收 ESP32 信标）—— 图像链路据此抓图；
  3) **广播被过滤时的兜底**（见下）。

为什么需要第 3 条（2026-10-28 实测）
------------------------------------
不少手机热点会丢掉客户端之间的**广播/多播帧**，只放行单播：板子能 DHCP 拿到
IP、PC 能 ping 通它、HTTP 页面也能打开，但双方的 8766 信标互相收不到，于是
ESP32 永远停在「还没收到 PC 广播」。实测机型：某国产手机热点（Android，2.4GHz）。

兜底做法：PC 侧扫一遍本网段的 80 端口，用首页标题 `包子控制台` 认出板子
（实测 254 个地址约 1.5 秒），拿到 IP 后**再单独给它发单播信标**。
单播能穿透这种热点，ESP32 收到就正常上线。没被过滤的网络里，扫描根本
不会触发（先收到广播就不用扫）。

用法：
    disc = PcDiscovery(tcp_port=8765)
    disc.start()
    ...
    ip = disc.esp32_ip()      # 发现到的 ESP32 地址（可能为 None）
    disc.stop()
"""

from __future__ import annotations

import concurrent.futures
import socket
import threading
import time
from typing import Callable, Optional

DISCOVERY_PORT = 8766
BEACON_INTERVAL_S = 2.0

# ---- 广播被过滤时的单播兜底 ----
ESP32_HTTP_PORT = 80
ESP32_PAGE_MARK = "包子控制台".encode("utf-8")   # web_console.h 的 <title>
SCAN_INTERVAL_S = 12.0      # 两次扫描的最小间隔（只在还没找到板子时扫）
SCAN_TIMEOUT_S = 0.35       # 单个地址的 TCP 连接超时
SCAN_WORKERS = 64


def _probe_esp32(ip: str) -> Optional[str]:
    """若 ip:80 是包子板子的控制台，返回 ip，否则 None。"""
    try:
        with socket.create_connection((ip, ESP32_HTTP_PORT), timeout=SCAN_TIMEOUT_S) as s:
            s.settimeout(0.6)
            s.sendall(
                b"GET / HTTP/1.1\r\nHost: " + ip.encode() + b"\r\nConnection: close\r\n\r\n"
            )
            data = b""
            while len(data) < 4096:
                chunk = s.recv(1024)
                if not chunk:
                    break
                data += chunk
            return ip if ESP32_PAGE_MARK in data else None
    except OSError:
        return None


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
        self._last_scan = 0.0
        self._scanning = False
        self._scanned_once = False

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
        # 扫描兜底从 12 秒后才开始：正常网络 2 秒内就能收到板子广播，
        # 不该为了一个用不上的兜底在每次启动时扫 254 个地址。
        self._last_scan = time.monotonic()
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
    def _maybe_scan(self, now: float) -> None:
        """还没发现板子时，隔一段时间扫一次本网段（在后台线程里做，不阻塞广播）。"""
        if self._scanning or now - self._last_scan < SCAN_INTERVAL_S:
            return
        if self._my_ip == "0.0.0.0":
            return
        self._last_scan = now
        self._scanning = True
        threading.Thread(target=self._scan_worker, name="esp32-scan", daemon=True).start()

    def _scan_worker(self) -> None:
        try:
            prefix = self._my_ip.rsplit(".", 1)[0] + "."
            hosts = [prefix + str(i) for i in range(1, 255)]
            if not self._scanned_once:
                self._scanned_once = True
                self.log(f"[disc] 还没收到 ESP32 广播，扫一遍 {prefix}0/24 找板子"
                         f"（部分手机热点会过滤广播，属正常兜底）")
            hit = None
            with concurrent.futures.ThreadPoolExecutor(max_workers=SCAN_WORKERS) as ex:
                for r in ex.map(_probe_esp32, hosts):
                    if r:
                        hit = r
                        break
            if hit and not self._esp32_ip:
                self._esp32_ip = hit
                self.log(f"[disc] 扫描找到 ESP32: {hit}（广播被过滤，改用单播信标）")
        except Exception as exc:  # noqa: BLE001  —— 兜底逻辑绝不能把发现线程带崩
            self.log(f"[disc] 扫描出错（忽略）：{exc}")
        finally:
            self._scanning = False

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
                # 已经知道板子地址：再补一条单播。热点过滤广播时，只有这条能到。
                if self._esp32_ip:
                    try:
                        self._sock.sendto(pkt, (self._esp32_ip, DISCOVERY_PORT))
                    except OSError:
                        pass
                else:
                    self._maybe_scan(now)

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
