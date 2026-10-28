"""与 ESP32 之间的传输层。

★ 2026-11：默认改成 USB 串口，PC 的网络只用于外部服务。

两种实现，对外接口一致（open/close/is_open/read/write/wait_tx_idle/describe）：

  SerialTransport
      ESP32 用 USB 线插在本机。语音、图像、情感/日志全部走这根线。
      不需要 IP，也不需要和 ESP32 在同一个局域网。
      · 如果板子走的是芯片内置 USB（USB CDC，Arduino IDE 里
        "USB CDC On Boot = Enabled"），波特率无意义（USB 全速 12Mbps）；
      · 如果板子接的是 CH340/CP2102 这类 USB-UART 桥，波特率是真的，
        115200 带不动 64KB/s 的音频+图像，必须调高（默认 921600，两端一致）。

  TcpTransport
      原来的 WiFi + TCP 服务端（ESP32 当客户端连进来）。保留下来，方便
      哪天要切回无线（固件把 USE_USB_LINK 设成 0）。

为什么不直接用 pyserial 的 read()/write()：
  · 串口写是【非阻塞】的：write() 返回的字节数可能小于请求，剩下的要等；
  · 打开串口会把 DTR 拉低，很多 ESP32 板子的自动复位电路会因此复位一次，
    所以打开后要等它重启完（约 1.5 秒，应用层再做握手确认）。
  这些细节都封在这里，上层（voice_server.py）只管搬字节。
"""

from __future__ import annotations

import socket
import threading
import time
from typing import Callable, Optional

# ESP32-S3 的 USB VID：
#   0x303A = Espressif（芯片内置 USB CDC / USB Serial/JTAG）
#   0x1A86 = 沁恒 CH340/CH343（很多开发板的 USB-UART 桥）
#   0x10C4 = 芯科 CP210x
KNOWN_VIDS = (0x303A, 0x1A86, 0x10C4)

Log = Callable[[str], None]

# "串口被别的程序占用"时最多等多久（秒）。
# 典型场景：烧完固件忘了关 Arduino 串口监视器就直接跑 main.py。
# 等这么久还没释放才真正报错退出。
PORT_BUSY_RETRY_S_DEFAULT = 60.0


class TransportError(RuntimeError):
    """打开/读写失败（消息里带给人看的提示）。"""


# =====================================================================
#  USB 串口
# =====================================================================
class SerialTransport:
    def __init__(
        self,
        port: str = "",
        baudrate: int = 921600,
        log: Log = print,
        packetsize: int = 4096,
    ) -> None:
        self.port = (port or "").strip()
        self.baudrate = int(baudrate or 921600)
        self.log = log
        self.packetsize = int(packetsize)
        self._ser = None
        self._lock = threading.Lock()
        self._writing = threading.Event()
        # 串口被占用时等待多久再放弃（见 open()）
        self._PORT_BUSY_RETRY_S = PORT_BUSY_RETRY_S_DEFAULT

    # ------------------------------------------------------------------
    @staticmethod
    def _is_port_busy(exc: BaseException) -> bool:
        """这个异常是不是"端口被别的程序占着"。

        Windows: PermissionError(13, '拒绝访问。')
        Linux  : SerialException('[Errno 16] could not open port ... Device or resource busy')
        因为 Windows 上被占用报的是 PermissionError 而不是 SerialException，
        这里按 Windows 错误码 / 关键字两种方式一起认。
        """
        if isinstance(exc, PermissionError):
            return True
        if getattr(exc, "winerror", None) in (5, 32):     # 5=拒绝访问 32=正在使用
            return True
        text = str(exc).lower()
        return any(k in text for k in ("busy", "拒绝访问", "permission", "access is denied", "占用"))

    # ------------------------------------------------------------------
    @staticmethod
    def available_ports() -> list[tuple[str, str]]:
        """[(设备名, 说明)]，说明里带 VID:PID，便于认出哪一个是 ESP32。"""
        try:
            from serial.tools import list_ports  # type: ignore
        except ImportError:
            return []
        out: list[tuple[str, str]] = []
        for p in list_ports.comports():
            vid = p.vid or 0
            pid = p.pid or 0
            desc = f"{p.description}  VID:PID={vid:04X}:{pid:04X}"
            out.append((p.device, desc))
        return out

    @staticmethod
    def _score(device: str) -> int:
        """给候选串口打分：ESP32 的 USB 口优先。"""
        try:
            from serial.tools import list_ports  # type: ignore
        except ImportError:
            return 0
        for p in list_ports.comports():
            if p.device != device:
                continue
            if (p.vid or 0) in KNOWN_VIDS:
                return 100
            desc = (p.description or "").lower()
            if "usb" in desc or "serial" in desc:
                return 10
            return 0
        return 0

    def autodetect(self) -> str:
        """自动挑一个串口；挑不到返回空串。"""
        ports = self.available_ports()
        if not ports:
            return ""
        # 有 ESP32 的 VID 就只在这些里面挑，否则退回"第一个能打分的"
        best = max(ports, key=lambda p: self._score(p[0]))
        if self._score(best[0]) <= 0:
            return ""
        return best[0]

    def _resolve_port(self) -> str:
        if self.port:
            return self.port
        found = self.autodetect()
        if not found:
            listing = "\n".join(f"    {d}  {n}" for d, n in self.available_ports()) or \
                "    （一个串口都没找到）"
            raise TransportError(
                "找不到 ESP32 的串口。请确认：\n"
                "  1) USB 线是【数据线】并插在 ESP32 的 USB 口（不是只充电的线）；\n"
                "  2) Arduino IDE 里 USB CDC On Boot = Enabled，已经烧进板子；\n"
                "  3) 没有别的程序占着这个串口（Arduino 串口监视器 / SSCOM 要先关掉）。\n"
                f"  当前可用的串口：\n{listing}\n"
                "也可以手动指定：python main.py --serial-port COM5"
            )
        return found

    # ------------------------------------------------------------------
    def open(self) -> None:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise TransportError(
                "缺少 pyserial：python -m pip install pyserial"
            ) from exc

        dev = self._resolve_port()

        # ⚠️ 串口是【独占】的：Arduino 串口监视器 / SSCOM 开着时打不开
        #    （Windows 上是 PermissionError 13，Linux 上是 [Errno 16] busy）。
        #    这是最常见的使用顺序问题——烧完固件忘了关监视器就直接跑 main.py。
        #    所以这里不能一撞就退出：按 _PORT_BUSY_RETRY_S 一直等，用户关掉
        #    占用程序后自动接上；真的等超时了才报错，并把排查步骤写清楚。
        deadline = time.monotonic() + self._PORT_BUSY_RETRY_S
        warned = False
        last_exc: Optional[BaseException] = None
        while True:
            try:
                self._ser = serial.Serial(
                    port=dev,
                    baudrate=self.baudrate,
                    timeout=0.02,      # 读超时给短一点：读循环要能被 stop() 打断
                    write_timeout=None,  # 写永不超时：写满了就等（USB 迟早会排空）
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    rtscts=False,
                    dsrdtr=False,
                    xonxoff=False,
                )
                break
            except (serial.SerialException, PermissionError, OSError) as exc:
                self._ser = None
                last_exc = exc
                if not self._is_port_busy(exc):
                    # 不是"被占用"（比如根本没有这个端口、驱动异常）就别等了
                    raise TransportError(
                        f"打开串口 {dev} 失败：{exc}\n"
                        "  · 端口号写错？先跑 python main.py --list-ports 看有哪些\n"
                        "  · USB 线是数据线吗？板子插好了吗？"
                    ) from exc
                if not warned:
                    warned = True
                    self.log(
                        f"[link] 串口 {dev} 被占用（多半是 Arduino 串口监视器 / "
                        f"SSCOM 还开着）。正在等待它释放，最多等 "
                        f"{self._PORT_BUSY_RETRY_S:.0f} 秒……关掉那个程序就会自动接上。"
                    )
                if time.monotonic() >= deadline:
                    raise TransportError(
                        f"串口 {dev} 一直被占用（等了 "
                        f"{self._PORT_BUSY_RETRY_S:.0f} 秒）：{exc}\n"
                        "  1) 关掉 Arduino IDE 的【串口监视器】（它不会自己释放端口）\n"
                        "  2) 关掉 SSCOM / XCOM / PuTTY 等串口工具\n"
                        "  3) 有别的 python 进程还开着？任务管理器里结束它\n"
                        "  4) 想确认是哪个口：python main.py --list-ports"
                    ) from exc
                time.sleep(self._PORT_BUSY_RETRY_S / 30.0)   # 约 0.3s 探一次

        # 打开串口会拉低 DTR：绝大多数 ESP32 开发板的自动复位电路会因此
        # 复位一次。清掉输入缓冲，避免把复位瞬间的乱码当成协议帧。
        try:
            self._ser.reset_input_buffer()
        except Exception:  # noqa: BLE001
            pass
        try:
            self._ser.set_buffer_size(rx_size=1 << 18, tx_size=1 << 16)
        except Exception:  # noqa: BLE001 —— Windows 上不支持就算了
            pass

        self.port = dev
        self.log(
            f"[link] 已打开 USB 串口 {dev} @ {self.baudrate}"
            f"（USB CDC 时波特率无意义；换 CH340 之类的桥片时才是真的）"
        )

    def close(self) -> None:
        with self._lock:
            ser = self._ser
            self._ser = None
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
            self.log(f"[link] 串口 {self.port} 已关闭")

    @property
    def is_open(self) -> bool:
        return self._ser is not None

    # ------------------------------------------------------------------
    def read(self, n: int) -> bytes:
        ser = self._ser
        if ser is None:
            return b""
        try:
            return ser.read(n)
        except Exception as exc:  # noqa: BLE001
            # USB 线被拔：pyserial 会抛 SerialException
            self.log(f"[link] 串口读取失败（USB 被拔？）：{exc}")
            return b""

    def write(self, data: bytes) -> bool:
        """把 data 全部写出去（内部处理"写得比缓冲慢"）。

        返回 False = 串口不可用（已经断开）。
        """
        with self._lock:
            ser = self._ser
            if ser is None:
                return False
            view = memoryview(data)
            self._writing.set()
            try:
                while len(view):
                    try:
                        n = ser.write(view)
                    except Exception as exc:  # noqa: BLE001
                        self.log(f"[link] 串口写入失败（USB 被拔？）：{exc}")
                        return False
                    if not n:
                        # write_timeout=None 时一般不会走到这，保险起见让一下 CPU
                        time.sleep(0.001)
                        continue
                    view = view[n:]
                return True
            finally:
                self._writing.clear()

    def wait_tx_idle(self, timeout_s: float = 2.0) -> bool:
        """等"手上这一帧写完了"。断开连接时要先等它，否则刚发出去的 PLAY_END
        之类的帧会留在缓冲区里跟着串口一起消失。"""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if not self._writing.is_set():
                return True
            time.sleep(0.005)
        return not self._writing.is_set()

    def describe(self) -> str:
        return f"USB 串口 {self.port or '自动检测'} @ {self.baudrate}"


# =====================================================================
#  TCP（WiFi 模式，保留）
# =====================================================================
class TcpTransport:
    """ESP32 作为客户端连进来的 TCP 服务端。"""

    def __init__(
        self,
        host: str = "0.0.0.0",
        port: int = 8765,
        allow_client_ip: str = "",
        log: Log = print,
    ) -> None:
        self.host = host
        self.port = int(port)
        self.allow_client_ip = allow_client_ip or ""
        self.log = log
        self._sock: Optional[socket.socket] = None
        self._conn: Optional[socket.socket] = None
        self._send_lock = threading.Lock()
        self._pending: list = []          # [(时间, 字节)] 等 ESP32 连上再补发
        # 最近一次真正收到数据的时间（WiFi 模式下判"链路是否还活着"用）
        self._last_rx = 0.0

    def open(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((self.host, self.port))
        self._sock.listen(2)
        self._sock.settimeout(0.5)
        self.log(f"[link] 监听 {self.host}:{self.port}，等待 ESP32 连接 ...")

    def close(self) -> None:
        """只断开当前客户端，**保留监听端口**。

        2026-10-28 修：原来这里把监听 socket 也一起关了，于是 ESP32 只要重启
        一次（升级固件、按复位键、WiFi 掉线 15 秒），PC 就再也接不进来——
        必须重启整个 PC 程序，现场极难看出原因。监听口只在 shutdown() 里关。
        """
        if self._conn:
            try:
                self._conn.close()
            except Exception:  # noqa: BLE001
                pass
        self._conn = None

    def shutdown(self) -> None:
        """彻底关闭（程序退出用）：断客户端 + 关监听。"""
        self.close()
        if self._sock:
            try:
                self._sock.close()
            except Exception:  # noqa: BLE001
                pass
        self._sock = None

    @property
    def is_open(self) -> bool:
        return self._conn is not None

    def accept(self) -> bool:
        """等一个客户端连进来（带超时）。返回 True = 新连接就绪。"""
        if self._sock is None:
            return False
        try:
            conn, addr = self._sock.accept()
        except socket.timeout:
            return False
        except OSError:
            return False

        ip = addr[0]
        if self.allow_client_ip and ip != self.allow_client_ip:
            self.log(f"[link] 拒绝未授权连接: {ip}")
            conn.close()
            return False

        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.settimeout(0.5)
        # 板子重启后往往在 PC 判超时之前就重新连上来（15 秒窗口内）。
        # 这时旧连接还挂着，必须先关掉，否则 fd 泄漏、且读线程可能继续读旧 socket。
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:  # noqa: BLE001
                pass
        self._conn = conn
        # ⚠️ 重新计时：_last_rx 是"距上次收到数据多久"，上一轮连接留下的旧值
        #    会让新连接一上来就被判成"15 秒读超时"并立刻断开（实测踩过的坑）。
        self._last_rx = time.monotonic()
        self.log(f"[link] ESP32 已连接: {ip}:{addr[1]}")
        # 把连上之前攒下的 PING / HELLO_ACK 补发过去（ESP32 靠它们判断"PC 在"）
        with self._send_lock:
            pending, self._pending = self._pending, []
            for _ts, data in pending:
                if not self._write_now(conn, data):
                    break
        return True

    @staticmethod
    def _write_now(conn: socket.socket, data: bytes) -> bool:
        """把 data 全部塞进 socket（会阻塞到发完）。调用方负责持锁。"""
        view = memoryview(data)
        while len(view):
            try:
                n = conn.send(view)
            except socket.timeout:
                continue                     # 背压：等一等
            except OSError:
                return False
            if n <= 0:
                return False
            view = view[n:]
        return True

    # 读超时（0.5s）只是让读线程能周期性醒来看 stop 标志，不代表对端关闭。
    # ⚠️ 曾经把"超时"和"对端关闭(recv 返回 b'')"混为一谈，结果 PC 每 0.5 秒就
    #    判定一次"对端关闭"并主动断开 —— 表现为握手帧时通时不通、音频却照发
    #    （断线后写入进了待发队列）。这里统一返回 b""（= 暂时没数据），
    #    真断线由上层"15 秒没有任何数据"来判。
    def read(self, n: int) -> bytes:
        conn = self._conn
        if conn is None:
            return b""
        try:
            data = conn.recv(n)
        except socket.timeout:
            return b""            # 只是暂时没数据，不是断线
        except OSError:
            self._conn = None     # 真断了（RST / 对端关闭）
            return b""
        if not data:
            self._conn = None     # recv 返回 b"" = 对端关闭了
            return b""
        self._last_rx = time.monotonic()
        return data

    def rx_idle_s(self) -> float:
        """距离上次真正收到数据过了多久（秒）；从未收到返回 inf。"""
        if not self._last_rx:
            return float("inf")
        return time.monotonic() - self._last_rx

    # ------------------------------------------------------------------
    # ⚠️ 延迟入队：PC 在 ESP32 连上之前发的帧不能直接丢掉。
    #    真实场景：ESP32 复位后要几秒才连上来，而 PC 的 heartbeat 线程
    #    每 2 秒就在发 PING / HELLO_ACK。以前这些帧直接 return False，
    #    于是 ESP32 只会看到"刚连上时恰好发来的一两条"，握手全丢。
    #    现在先把帧攒住（只留最近 2 秒、最多 64KB），accept() 一成功就补发。
    # ------------------------------------------------------------------
    _QUEUE_MAX_AGE_S = 2.0
    _QUEUE_MAX_BYTES = 64 * 1024

    def _queue(self, data: bytes) -> None:
        now = time.monotonic()
        self._pending = [(t, d) for (t, d) in self._pending
                         if now - t < self._QUEUE_MAX_AGE_S]
        self._pending.append((now, data))
        total = sum(len(d) for _, d in self._pending)
        while total > self._QUEUE_MAX_BYTES and self._pending:
            total -= len(self._pending.pop(0)[1])

    def write(self, data: bytes) -> bool:
        with self._send_lock:
            conn = self._conn
            if conn is None:
                self._queue(data)        # ESP32 还没连上：先攒着，连上补发
                return True
            return self._write_now(conn, data)

    def wait_tx_idle(self, timeout_s: float = 2.0) -> bool:
        return True

    def describe(self) -> str:
        return f"TCP {self.host}:{self.port}"


def make_transport(cfg, log: Log = print):
    """按配置创建传输层对象（不打开）。"""
    kind = str(getattr(cfg, "transport", "tcp") or "tcp").strip().lower()
    if kind == "tcp":
        return TcpTransport(
            host=getattr(cfg, "host", "0.0.0.0"),
            port=getattr(cfg, "port", 8765),
            allow_client_ip=getattr(cfg, "allow_client_ip", ""),
            log=log,
        )
    return SerialTransport(
        port=getattr(cfg, "serial_port", ""),
        baudrate=getattr(cfg, "serial_baud", 921600),
        log=log,
    )
