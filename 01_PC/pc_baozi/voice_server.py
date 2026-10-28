"""ESP32 <-> PC 流水线的服务端（传输方式：USB 串口 / TCP 二选一）。

职责很窄：
  * 打开与 ESP32 的链路（link.py：默认 USB 串口）；
  * 把收到的 PKT_MIC_PCM 喂给 Pipeline；
  * 把 Pipeline 产出的 TTS PCM 按 ESP32 的播放缓冲水位限速推回去；
  * 转发控制帧（PLAY_BEGIN / PLAY_END / PLAY_ABORT / 情绪 / 抓图请求）；
  * 把 ESP32 发来的日志帧（PKT_LOG）按行打印出来。

VAD / ASR / LLM / TTS 全在 pipeline.py 那边，这里只搬字节。
程序入口是 server.py / main.py，本文件只提供 :class:`VoiceServer`。

★ 2026-11 改动：
  * 传输从"TCP 服务端"改成"USB 串口"，PC 不再需要和 ESP32 同网段；
  * 摄像头图像也走这根线（request_jpeg()），不再用 HTTP；
  * ESP32 的调试日志走 PKT_LOG 帧，由这里打 [esp] 前缀输出。
    —— 因为串口这根线在跑二进制帧，裸文本会把帧流冲乱。
"""

from __future__ import annotations

import threading
import time
from typing import List, Optional

import numpy as np

from audio import SAMPLE_RATE, PcmRing, pcm_to_bytes
from link import TransportError, TcpTransport, make_transport  # noqa: F401
from pipeline import Events, Pipeline, State
from protocol import (
    Ctrl,
    Frame,
    FrameDecoder,
    MAX_PAYLOAD,
    Pkt,
    decode_cam_done,
    encode,
    encode_ctrl,
)

# 每秒字节数：16k * 16bit * 单声道
BYTES_PER_SEC = SAMPLE_RATE * 2

# PC 主动保活间隔。ESP32 侧"这么久没收到 PC 的帧就当 PC 没在跑"（15 秒），
# 所以这里 2 秒一次，留足余量。
HEARTBEAT_S = 2.0


class VoiceServer:
    def __init__(self, cfg, pipeline: Pipeline) -> None:
        self.cfg = cfg
        self.pipeline = pipeline
        self._transport = make_transport(cfg)
        self._send_lock = threading.Lock()
        self._stop = threading.Event()
        self._device_hello = threading.Event()
        # 重复 HELLO 的提示限频（秒），避免异常时刷屏
        self._hello_dup_ts = 0.0

        # 收到但还没喂给流水线的麦克风数据
        self._mic_ring = PcmRing(SAMPLE_RATE * 5)

        # “ESP32 还剩多少音频没播”——按字节数推算的水位，
        # 用来给推流限速：既不会撑爆它的 1.5s 缓冲，也不会让它饿死。
        # 会被 worker 线程（_on_play_*）和 push 线程同时读写，必须加锁。
        # ⚠️ 这是【开环推算】，只在收不到 ESP32 上报时当兜底用（见 level()）。
        self._unplayed = 0
        self._unplayed_lock = threading.Lock()

        # ---- ESP32 上报的真实播放状态（闭环）----
        # payload[1] = active，payload[2..5] = buffered(uint32)。
        #   _esp_buffered = -1 表示"从没收到过上报"（老固件）-> 退回推算。
        self._esp_active = False
        self._esp_buffered = -1
        self._esp_state_ts = 0.0

        # ---- 日志帧（PKT_LOG）的按行拼装 ----
        self._log_buf = ""
        self._log_lock = threading.Lock()

        # ---- 摄像头：一次只允许一个请求在飞 ----
        self._cam_lock = threading.Lock()
        self._cam_chunks: Optional[bytearray] = None
        self._cam_expected = 0
        self._cam_ok = False
        self._cam_event = threading.Event()
        self._cam_req_id = 0

        # 同一时间只允许一个推流线程：否则旧的推流线程跑完调
        # notify_play_done()，会把新一轮说话的 SPEAKING 状态误清掉。
        self._push_lock = threading.Lock()

        self._threads: List[threading.Thread] = []

        # 把流水线的回调接到链路上。注意必须用 update() 而不是整个替换
        # Events 对象——调用方（比如自测脚本）可能自己挂了 on_log 之类的钩子。
        pipeline.ev.update(
            on_play_pcm=self._on_play_pcm,
            on_play_begin=self._on_play_begin,
            on_play_end=self._on_play_end,
            on_play_abort=self._on_play_abort,
            on_transcript=lambda t: self._send_text(t),
            on_state=self._on_state,
            on_log=lambda m: print(f"[pipe] {m}", flush=True),
        )

    # ==================================================================
    # 生命周期
    # ==================================================================
    def start(self) -> None:
        self._transport.open()          # 串口打不开会抛 TransportError
        self.pipeline.start()

        for target, name in (
            (self._read_loop, "esp-read"),
            (self._pump_loop, "esp-pump"),
            (self._heartbeat_loop, "esp-heart"),
        ):
            t = threading.Thread(target=target, name=name, daemon=True)
            t.start()
            self._threads.append(t)

        if isinstance(self._transport, TcpTransport):
            # WiFi 模式：等 ESP32 连进来（accept 超时会自己重试）
            t = threading.Thread(target=self._accept_loop, name="accept", daemon=True)
            t.start()
            self._threads.append(t)
        else:
            self._handshake()

    def stop(self) -> None:
        self._stop.set()
        try:
            self.pipeline.stop()
        except Exception:  # noqa: BLE001
            pass
        self._transport.wait_tx_idle(1.0)   # 让 PLAY_END 之类写完再走
        # 退出时用 shutdown()（TCP 才会真正关监听口）；串口没有这个方法，回落 close()
        shutdown = getattr(self._transport, "shutdown", None)
        (shutdown or self._transport.close)()

    @property
    def connected(self) -> bool:
        return self._transport.is_open

    # ==================================================================
    # 握手：确认对面确实是我们的 ESP32
    # ==================================================================
    def _handshake(self, timeout_s: float = 4.0) -> bool:
        """发 HELLO_ACK，等 ESP32 回 HELLO。

        为什么要握手：打开串口会让大部分 ESP32 板子复位一次，
        复位期间的乱码/半帧都要清掉；而且万一插错串口（比如插着 TD1601
        的 USB-TTL），早点报出来比后面"音频发出去没人应"更好排查。
        握手失败不阻断启动 —— ESP32 可能还没上电，后面它会自己重连。
        """
        for _ in range(3):
            self._send_ctrl(Ctrl.HELLO_ACK)
            if self._device_hello.wait(timeout_s / 3):
                return True
            time.sleep(0.2)
        print(
            "[link] 还没收到 ESP32 的握手（HELLO）。"
            "检查：USB 线是数据线 / 板子已烧 USB 模式固件 / 串口号对不对。"
            "语音链路仍会继续等待 ESP32 上线。",
            flush=True,
        )
        return False

    def _accept_loop(self) -> None:
        """WiFi 模式：反复等 ESP32 连进来。"""
        while not self._stop.is_set():
            if self._transport.is_open:
                time.sleep(0.2)
                continue
            if self._transport.accept():
                self._mic_ring.clear()
            else:
                time.sleep(0.05)

    # ==================================================================
    # 收：ESP32 -> PC
    # ==================================================================
    def _read_loop(self) -> None:
        dec = FrameDecoder()
        reported_bad = False
        was_open = False

        while not self._stop.is_set():
            # 连接断过（板子重启/WiFi 抖动）就换一个干净的解码器：
            # 上一轮可能停在半帧上，残留字节会让重连后的前 100 多字节全部对不齐
            # （实测重连后报"收到 117 个无法对齐的字节"就是这么来的）。
            if was_open and not self._transport.is_open:
                dec = FrameDecoder()
                reported_bad = False
            was_open = self._transport.is_open

            data = self._transport.read(4096)
            if not data:
                if isinstance(self._transport, TcpTransport):
                    # WiFi 模式：没有数据不一定是断线（recv 超时也会返回空）。
                    # 只有【真的连着 + 连续 15 秒一个字节都没收到】才按断线处理。
                    # ⚠️ 必须判 is_open：ESP32 还没连上来时 _conn 是 None，
                    #    这时若也判"读超时"，_drop() 会把监听 socket 一起关掉，
                    #    之后 ESP32 永远连不进来（实测踩过的坑）。
                    if (self._transport.is_open
                            and self._transport.rx_idle_s() > 15.0):
                        self._drop("读超时")
                elif not self._transport.is_open:
                    time.sleep(0.1)
                continue

            for frame in dec.feed(data):
                self._on_frame(frame)

            # 协议对不上时（比如插错了串口）提示一次，别刷屏
            if dec.bad_bytes and not reported_bad:
                reported_bad = True
                print(
                    f"[link] 注意：收到 {dec.bad_bytes} 个无法对齐的字节。"
                    "如果一直在涨，说明这个串口上的设备不是本工程的 ESP32。",
                    flush=True,
                )

    def _on_frame(self, frame: Frame) -> None:
        if frame.type == Pkt.MIC_PCM:
            pcm = np.frombuffer(frame.payload, dtype="<i2").astype(np.int16, copy=False)
            if pcm.size:
                self._mic_ring.push(pcm)
            return

        if frame.type == Pkt.LOG:
            self._on_log(frame.payload)
            return

        if frame.type == Pkt.CAM_JPEG:
            if self._cam_chunks is not None:
                self._cam_chunks.extend(frame.payload)
            return

        if frame.type == Pkt.CTRL:
            if not frame.payload:
                return
            cmd = frame.payload[0]
            if cmd == Ctrl.PING:
                self._send_ctrl(Ctrl.PONG)
            elif cmd == Ctrl.HELLO:
                dev = frame.payload[1:].decode("utf-8", "replace")
                # HELLO_ACK 每次连接只回一次：ESP32 侧收到它就不再发 HELLO。
                # ⚠️ 绝对不能"每次收到 HELLO 都回 HELLO_ACK"——如果固件那边也
                #    "有问必答"，两边就会 HELLO ↔ HELLO_ACK 无限回声，把串口
                #    带宽吃满、PC 终端"设备上线"疯狂刷屏（2026-11 实测踩过）。
                if not self._device_hello.is_set():
                    print(f"[link] 设备上线: {dev}", flush=True)
                    # 提示固件版本对不上时的大概率症状：取图请求发出去了却没人应。
                    if "fw/" in dev:
                        print(f"[link] 设备固件信息: {dev.split('fw/')[-1]}", flush=True)
                    self._device_hello.set()
                    self._send_ctrl(Ctrl.HELLO_ACK)
                else:
                    # 已经握过手，只吞掉不再应答，最多 10 秒提示一次，
                    # 免得真出问题时被日志淹没、也免得刷屏。
                    now = time.monotonic()
                    if now - self._hello_dup_ts > 10.0:
                        self._hello_dup_ts = now
                        print(f"[link] 又收到 HELLO（已忽略，不再回应）：{dev}",
                              flush=True)
            elif cmd == Ctrl.AUDIO_STATE:
                # ESP32 报的真实播放状态（闭环）。payload[1] = active(0/1)；
                # payload[2..5] = buffered(uint32 小端)。
                if len(frame.payload) >= 2:
                    self._esp_active = frame.payload[1] != 0
                    if len(frame.payload) >= 6:
                        self._esp_buffered = int.from_bytes(
                            frame.payload[2:6], "little")
                    self._esp_state_ts = time.monotonic()
            elif cmd == Ctrl.CAM_DONE:
                ok, total, req_id = decode_cam_done(frame.payload)
                self._on_cam_done(ok, total, req_id)
            # PONG 不需要处理
            return

        # 其他类型忽略（PC -> ESP32 的方向）

    # ---- ESP32 日志 ----
    def _on_log(self, payload: bytes) -> None:
        """PKT_LOG 是一条条文本片段（固件按 2048 字节切片），这里按行拼。"""
        text = payload.decode("utf-8", "replace")
        with self._log_lock:
            self._log_buf += text
            while True:
                idx = self._log_buf.find("\n")
                if idx < 0:
                    break
                line = self._log_buf[:idx].rstrip("\r")
                self._log_buf = self._log_buf[idx + 1:]
                if line:
                    print(f"[esp] {line}", flush=True)
            # 兜底：固件万一发来一大段没有换行的内容，别把缓冲撑爆
            if len(self._log_buf) > 8192:
                print(f"[esp] {self._log_buf[:8192]}", flush=True)
                self._log_buf = ""

    # ---- 摄像头 ----
    def _on_cam_done(self, ok: bool, total: int, req_id: int) -> None:
        self._cam_ok = ok
        self._cam_expected = total
        if req_id != self._cam_req_id:
            # 上一轮（超时/被丢弃）的迟到收尾，别让它唤醒新的等待者
            return
        self._cam_event.set()

    def _drop(self, why: str) -> None:
        if self._transport.is_open:
            print(f"[link] 断开连接（{why}）", flush=True)
        self._transport.close()
        with self._unplayed_lock:
            self._unplayed = 0
        # 连接没了，上报自然也不再更新：清掉，避免拿旧值当"真实水位"。
        self._esp_buffered = -1
        self._esp_active = False
        self._mic_ring.clear()
        self._cam_chunks = None

    def _esp_report_fresh(self) -> bool:
        """ESP32 的播放状态上报是不是"还新鲜"（0.5s 内收到过）。"""
        return (self._esp_buffered >= 0
                and (time.monotonic() - self._esp_state_ts) < 0.5)

    # ==================================================================
    # 喂流水线：单独一个线程，按 20ms 的节奏把麦克风数据交出去
    # ==================================================================
    def _pump_loop(self) -> None:
        step_samples = SAMPLE_RATE * 20 // 1000

        while not self._stop.is_set():
            if len(self._mic_ring) >= step_samples:
                block = self._mic_ring.pop(step_samples)
                try:
                    self.pipeline.feed_mic(block)
                except Exception as exc:  # noqa: BLE001
                    print(f"[pipe] feed_mic 出错: {exc!r}", flush=True)
            else:
                time.sleep(0.002)

    # ==================================================================
    # 保活：让 ESP32 知道"PC 还在跑"（它靠这个决定要不要往串口写音频）
    # ==================================================================
    def _heartbeat_loop(self) -> None:
        while not self._stop.is_set():
            if self._transport.is_open:
                self._send_ctrl(Ctrl.PING)
            time.sleep(HEARTBEAT_S)

    # ==================================================================
    # 发：PC -> ESP32
    # ==================================================================
    _SEND_STALL_MAX_S = 10.0
    _PLAY_DRAIN_MAX_S = 20.0

    def _send_frame(self, type_: int, payload: bytes) -> bool:
        """发一帧（超长 payload 自动切片）。

        串口没有"连不上"这个状态：写不进去基本只有两种可能 ——
        ESP32 还没起来（它的缓冲没人排空）或 USB 线松了。两种情况都只
        放弃这一帧：音频丢一帧无所谓，阻塞流水线才是灾难。
        """
        with self._send_lock:
            ok = True
            for off in range(0, len(payload), MAX_PAYLOAD):
                if not self._transport.write(
                    encode(type_, payload[off : off + MAX_PAYLOAD])
                ):
                    ok = False
                    break
            if not ok:
                print("[link] 写失败（USB 被拔 / ESP32 复位中），本轮放弃", flush=True)
            return ok

    def _send_ctrl(self, cmd: int, extra: bytes = b"") -> bool:
        return self._send_frame(Pkt.CTRL, bytes([int(cmd)]) + extra)

    # ------------------------------------------------------------------
    # 供上层（main.py）复用的发送接口
    # ------------------------------------------------------------------
    def send_ctrl_payload(self, ctrl: int, line: str) -> bool:
        """把一个控制帧发给 ESP32：payload[0]=控制码，payload[1..]=整行文本。

        用于情感链路：把 "EMO:HAPPY" / "INTENT:CHAT" 交给 ESP32，
        由它经 UART 转发给 TD1601。链路没开时返回 False。
        """
        payload = bytes([ctrl]) + line.encode("utf-8", "replace")
        return self._send_frame(Pkt.CTRL, payload)

    def send_debug_line(self, line: str) -> bool:
        """把一行文本交给 ESP32（如 "+" 调麦克风增益），走 CTRL_TD_TEXT 通道。"""
        return self.send_ctrl_payload(Ctrl.TD_TEXT, line)

    def _send_text(self, text: str) -> None:
        """把识别结果当调试信息发给 ESP32（它会打进日志）。"""
        self._send_frame(Pkt.TEXT, text.encode("utf-8", "replace")[:MAX_PAYLOAD])

    # ------------------------------------------------------------------
    # 摄像头：请求一帧 JPEG（USB 模式下图像的唯一通路）
    # ------------------------------------------------------------------
    def request_jpeg(self, timeout_s: float = 8.0) -> Optional[bytes]:
        """让 ESP32 现抓一帧，返回 JPEG 字节；失败返回 None。

        协议：PC 发 CTRL_CAM_REQ（带一个递增请求号）-> ESP32 回若干个
        PKT_CAM_JPEG 分片 -> 最后 CTRL_CAM_DONE 收尾（带总字节数和请求号）。
        用请求号区分"上一轮的迟到收尾"，避免拿到上一次的图。
        """
        with self._cam_lock:
            self._cam_req_id = (self._cam_req_id % 255) + 1
            req_id = self._cam_req_id
            self._cam_chunks = bytearray()
            self._cam_expected = 0
            self._cam_ok = False
            self._cam_event.clear()

            t0 = time.monotonic()
            if not self._send_ctrl(Ctrl.CAM_REQ, bytes([req_id])):
                self._cam_chunks = None
                return None

            got = self._cam_event.wait(timeout_s)
            chunks = self._cam_chunks
            self._cam_chunks = None

        if not got:
            print(
                f"[眼] ESP32 没在 {timeout_s:.0f}s 内回图（请求号 {req_id}）。排查顺序：\n"
                f"      1) 看上面有没有 [esp] [cam] 行：\n"
                f"         · 有「已发一帧」-> 是我们这边的接收问题（分片没拼上/请求号对不上）\n"
                f"         · 有「抓帧失败 / 未就绪」-> 摄像头侧问题，见那条日志\n"
                f"         · 完全没有 -> 固件没在处理抓图请求（固件是不是 USB 模式的旧版本？）\n"
                f"      2) [esp] [stat] 里 摄像头=无 -> 摄像头初始化失败（多因 PSRAM 没设成 OPI）\n"
                f"      3) 请求期间主循环被卡住（麦克风上传也会停）：看采集块计数是否停止增长",
                flush=True,
            )
            return None
        if not self._cam_ok or chunks is None or not chunks:
            print("[眼] ESP32 抓图失败（它会说明原因，见 [esp] [cam] 行）", flush=True)
            return None

        data = bytes(chunks)
        if self._cam_expected and len(data) != self._cam_expected:
            print(f"[眼] JPEG 不完整：收到 {len(data)} / 声明 {self._cam_expected} 字节"
                  f"（请求号 {req_id}）—— 多半是分片在串口上被截断，重试一次通常就好",
                  flush=True)
            return None
        if data[:2] != b"\xff\xd8":
            print("[眼] 收到的数据不是 JPEG（开头不是 FFD8），丢弃", flush=True)
            return None

        print(f"[眼] 从串口取到一帧 {len(data)} 字节"
              f"（{(time.monotonic() - t0) * 1000:.0f} ms）", flush=True)
        return data

    # ---- 播放 ----
    def _on_play_begin(self) -> None:
        with self._unplayed_lock:
            self._unplayed = 0
        self._send_ctrl(Ctrl.PLAY_BEGIN)

    def _on_play_end(self) -> None:
        self._send_ctrl(Ctrl.PLAY_END)

    def _on_play_abort(self) -> None:
        with self._unplayed_lock:
            self._unplayed = 0
        self._send_ctrl(Ctrl.PLAY_ABORT)

    def _on_play_pcm(self, pcm: np.ndarray) -> None:
        """在后台线程里限速推流，避免阻塞流水线的工作线程。"""
        # 先清掉已经结束的线程对象，免得反复说话时无限堆积
        if len(self._threads) > 32:
            self._threads = [x for x in self._threads if x.is_alive()]
        t = threading.Thread(
            target=self._push_audio, args=(pcm,), name="esp-push", daemon=True
        )
        t.start()
        self._threads.append(t)

    def _push_audio(self, pcm: np.ndarray) -> None:
        # 串行化：同一时刻只有一个推流线程，避免两个线程同时改 _unplayed，
        # 也避免旧线程结束后把新线程刚设好的 SPEAKING 状态清掉。
        with self._push_lock:
            self._push_audio_locked(pcm)

    def _push_audio_locked(self, pcm: np.ndarray) -> None:
        data = pcm_to_bytes(pcm)
        total = len(data)
        sent = 0
        chunk = SAMPLE_RATE * 2 * 40 // 1000  # 每次 40ms

        target = max(1, int(BYTES_PER_SEC * self.cfg.play_target_s))
        prefill = max(1, int(BYTES_PER_SEC * self.cfg.play_start_prefill_s))
        hard_max = max(target, int(BYTES_PER_SEC * self.cfg.play_max_s))

        t0 = time.monotonic()
        last_drain = t0
        aborted = False

        def level() -> int:
            """当前"ESP32 还欠播多少字节"。

            闭环优先：ESP32 每 100ms 上报真实缓冲量，直接用它 —— 以前纯靠
            按时间推算，pace 判定偏慢时 ESP32 缓冲会饿死（声音断续），
            偏快时又撞上传输背压。上报断了（>0.5s 没收到）就退回开环推算值。
            """
            if self._esp_report_fresh():
                return self._esp_buffered
            with self._unplayed_lock:
                return self._unplayed

        while sent < total and not self._stop.is_set():
            if not self._transport.is_open:
                print("[link] 播放期间链路断开，放弃本轮推流", flush=True)
                break

            # 打断时流水线会把状态置回 IDLE，这里要立刻停下别继续推
            if self.pipeline.state is not State.SPEAKING:
                aborted = True
                break

            unplayed = level()

            # 水位控制：ESP32 缓冲越接近 target，我们推得越慢
            if unplayed < prefill:
                pace = 0.5  # 缓冲很空，加把劲
            elif unplayed < target:
                pace = 0.9  # 正常节奏
            else:
                pace = 1.4  # 快满了，等一会儿

            piece = data[sent : sent + chunk]
            if not self._send_frame(Pkt.TTS_PCM, piece):
                break
            sent += len(piece)
            with self._unplayed_lock:
                self._unplayed += len(piece)

            # 按“音频时长 × pace”等待；ESP32 以真实速率消耗，下面同步扣减水位
            time.sleep(max(0.0, len(piece) / BYTES_PER_SEC * pace))

            now = time.monotonic()
            with self._unplayed_lock:
                self._unplayed = max(
                    0, self._unplayed - int(BYTES_PER_SEC * (now - last_drain) * 0.98)
                )
                drained = self._unplayed
            last_drain = now

            if drained > hard_max:
                time.sleep(min(0.3, (drained - target) / BYTES_PER_SEC))
                last_drain = time.monotonic()

        if aborted:
            print("[link] 本轮推流被用户打断", flush=True)
            return

        # 推完了，通知 ESP32 本轮结束（USB 模式下先等串口把队列写空，
        # 否则最后的 PLAY_END 可能还压在缓冲里）
        if self._transport.is_open:
            self._send_ctrl(Ctrl.PLAY_END)
            self._transport.wait_tx_idle(2.0)

        # 等 ESP32【真的】把缓冲放完，再回待机。
        #
        # ⚠️ 这一步以前是纯推算（remaining/速率 + 0.25s）。推算偏小就会提前
        #    进入 LISTENING，而 ESP32 还在响 —— 于是：
        #      · 它把自己的喇叭尾音当用户说话（"幻听"）；
        #      · 或者把用户真正说的话当回环丢掉（"话录不进去"）。
        #    现在有 ESP32 上报的真实 active/buffered，就等它明确报
        #    "不响了且缓冲空"；收不到上报（老固件）才退回原来的推算。
        if self._esp_report_fresh():
            deadline = time.monotonic() + self._PLAY_DRAIN_MAX_S
            while time.monotonic() < deadline and not self._stop.is_set():
                if not self._transport.is_open:
                    break
                if (self._esp_report_fresh() and not self._esp_active
                        and self._esp_buffered <= 0):
                    break
                time.sleep(0.05)
        else:
            remaining = level()
            deadline = time.monotonic() + min(
                remaining / BYTES_PER_SEC + 0.25, self._PLAY_DRAIN_MAX_S)
            while time.monotonic() < deadline and not self._stop.is_set():
                if not self._transport.is_open:
                    break
                time.sleep(0.05)
                with self._unplayed_lock:
                    self._unplayed = max(
                        0, self._unplayed - int(BYTES_PER_SEC * 0.05))

        self.pipeline.notify_play_done()
        print(
            f"[link] 本轮播放结束（推送 {sent} 字节，耗时 {time.monotonic() - t0:.2f}s）",
            flush=True,
        )

    def _on_state(self, st: State) -> None:
        print(f"[state] {st.name}", flush=True)
