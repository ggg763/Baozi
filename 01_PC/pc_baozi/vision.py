"""图像理解（眼睛）—— **按需调用**（不是定时抓图）。

★ 2026-11：取图从"WiFi HTTP 拉流"改成"USB 串口请求一帧"。
  ESP32 用 USB 线插在本机，PC 发一个 CTRL_CAM_REQ，板子现抓一帧 JPEG
  分片回传（见 voice_server.py 的 request_jpeg()）。所以：
    · PC 不需要知道 ESP32 的 IP，也不需要和它在同一个局域网；
    · 本机网络只用于外部服务（Ollama 等）。
  HTTP 取图（/capture、/stream）的旧实现保留在下面，只有显式传入
  esp32_ip 且没有注入 frame_provider 时才会用到（WiFi 模式）。

设计（2026-09 改为调用式）：
  平时摄像头只初始化、不拍照，PC 也不取图 —— 零带宽零算力开销。
  只有当用户的语音在语义上"在看图"（如"你看到了什么""这是什么"）时，
  才：
      请求一帧
        -> ① YOLO 目标检测（快、准、带位置）—— 有检出就用它
        -> ② 没检出时才回落 qwen3-vl 做场景描述（可选）
        -> 作为上下文交给 LLM
  由 LLM 结合对话上下文给出最终回复。

为什么加 YOLO（2026-09）：
  VL（qwen3-vl）是生成式的：慢（0.6~2s）、会幻觉、给不出位置。
  YOLO 是判别式的：CPU 上 ~60~150ms、确定性、能给出精确 bbox。
  「你看到了什么」这类问题用 YOLO 又快又准；两者互补而非替代。

为什么要改成"调用式"：
  定时抓图 + 视觉推理，既占带宽也白烧算力（用户大多时候没在看）。
  改成"用户问了才看"，开销只在实际需要时发生。
"""

from __future__ import annotations

import base64
import os
import threading
import time
import urllib.request
from typing import Callable, List, Optional, Tuple

from detector import (
    Detection,
    YoloDetector,
    describe_detections,
    describe_detections_cn,
)

# ⚠️ qwen3-vl 是"思维链"视觉模型：给它长指令/要详细描述时，它会一直输出
# 思考过程（thinking 字段），而 content 为空 —— 现象就是"等很久但没内容"。
# 实测有效的组合是：think=False（关思考）+ 尽量简短的指令。
#
# 定位（2026-10 明确）：
#   YOLO 只负责"突出重点" —— 认出杯子/手机/人这类常见物体时，用它快且准。
#   **没认出任何东西时，不等于"没东西可看"** —— 很多场景（风景、桌面、
#   房间、文字、颜色、光影）本来就没有 COCO 80 类里的物体。
#   这时必须退回让 VL 正常描述整张图，而不是回一句"没看清"。
VISION_PROMPT = (
    "Describe what you see in this photo in ONE short English sentence "
    "(max 20 words). Mention the main subject, colors, and setting. "
    "Plain ASCII, no markdown, no thinking."
)


# =====================================================================
#  "最新一帧"缓存 与 HTTP 模式的 multipart 解析
# =====================================================================
# 说明（2026-11）：默认走 USB 串口，取图是"发一个请求 -> 拿一帧"，
# 下面这套 multipart 解析只有切回 WiFi 模式（HTTP /stream）时才用。
BOUNDARY = b"--frame"

# 照片存这里：脚本同目录下的 photos/（与参考实现一致）
PHOTO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "photos")


def parse_stream(buf: bytearray):
    """从 multipart/x-mixed-replace 缓冲里切出完整 JPEG 帧。

    ESP32 发的是：
        \\r\\n--frame\\r\\n
        Content-Type: image/jpeg\\r\\n
        Content-Length: N\\r\\n
        \\r\\n
        <N 字节 JPEG>
    按 boundary + Content-Length 精确切帧。
    """
    while True:
        i = buf.find(BOUNDARY)
        if i < 0:
            # 没有 boundary：最多留一点尾巴，其余丢掉（防止缓冲无限涨）
            if len(buf) > 4096:
                del buf[:-4096]
            return

        head_end = buf.find(b"\r\n\r\n", i)
        if head_end < 0:
            del buf[:i]           # 头部还没收全
            return
        head = bytes(buf[i:head_end]).decode("latin-1", "replace")

        clen = -1
        for line in head.split("\r\n"):
            if line.lower().startswith("content-length:"):
                try:
                    clen = int(line.split(":", 1)[1].strip())
                except ValueError:
                    clen = -1
                break

        if clen <= 0:
            # 没有 Content-Length，靠下一个 boundary 分界
            del buf[:head_end + 4]
            return

        start = head_end + 4
        if len(buf) < start + clen:
            del buf[:i]           # 这一帧还没收全
            return

        jpg = bytes(buf[start:start + clen])
        del buf[:start + clen]
        yield jpg


class LatestFrame:
    """后台线程收到流后，把「最新一帧 JPEG」放这里。"""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._jpg: Optional[bytes] = None
        self._seq = 0
        self._ts = 0.0
        self.frames = 0          # 累计收了多少帧（判断流是否活着）
        self.bytes = 0

    def put(self, jpg: bytes) -> None:
        with self._lock:
            self._jpg = jpg
            self._seq += 1
            self._ts = time.time()
            self.frames += 1
            self.bytes += len(jpg)

    def get(self) -> Tuple[Optional[bytes], int, float]:
        """返回 (jpeg 字节, seq, 时间戳)。"""
        with self._lock:
            return self._jpg, self._seq, self._ts

    def clear(self) -> None:
        with self._lock:
            self._jpg = None
            self._seq = 0
            self._ts = 0.0


class VisionDescriber:
    """按需抓图 + 描述。调用 look() 才会真正拍照与推理。"""

    def __init__(
        self,
        esp32_ip: str = "",
        model: str = "qwen3-vl:2b",
        timeout_s: float = 30.0,
        save_jpg: str = "captured.jpg",
        num_ctx: int = 8192,
        num_predict: int = 320,
        log: Callable[[str], None] = print,
        ip_provider: Optional[Callable[[], Optional[str]]] = None,
        detector: Optional[YoloDetector] = None,
        vl_fallback: bool = True,
        frame_provider: Optional[Callable[[], Optional[bytes]]] = None,
    ) -> None:
        self.esp32_ip = esp32_ip
        # 可选：动态地址来源（UDP 自动发现）。返回 None 时回退 esp32_ip。
        self._ip_provider = ip_provider
        # ★ 首选取图方式：USB 串口请求一帧（由 main.py 注入
        #   srv.request_jpeg，内部走 CTRL_CAM_REQ / PKT_CAM_JPEG / CTRL_CAM_DONE）。
        #   为 None 时才退回旧的 HTTP 方式。
        self._frame_provider = frame_provider
        self.model = model
        self.timeout_s = timeout_s
        self.save_jpg = save_jpg
        self.num_ctx = int(num_ctx)      # 视觉模型上下文（图像 token 很占，给足）
        # ⚠️ 必须给足：qwen3-vl 会先写一大段思考，太小会在思考阶段被截断、
        #    content 恒为空（详见 describe() 的 docstring）。实测 256~320 合适。
        self.num_predict = int(num_predict)
        self.log = log
        # 不走系统代理，避免局域网请求被代理拦掉
        self._proxies = {"http": None, "https": None}
        self._last_desc = ""
        self._last_ts = 0.0
        # 视觉模型句柄：第一次用时才加载（省启动时间）
        self._ollama = None
        # ① YOLO 目标检测器（首选）。None = 不启用，只走 VL。
        self.detector = detector
        # YOLO 什么都没检出时，是否回落用 VL 描述场景。
        #   开：信息更全（但每次没检出都会跑一次 VL，慢 0.6~2s）
        #   关：只信 YOLO（快），没检出就直说"没看清"
        self.vl_fallback = bool(vl_fallback)
        self._last_dets: List[Detection] = []
        # 是否已经预热过 VL 模型（见 warmup()）
        self._warmed = False

        # ---- 最近一次拿到的帧（USB 模式下"按需请求"的结果）----
        self.frame = LatestFrame()
        self._stream_thread: Optional[threading.Thread] = None
        self._stream_stop = threading.Event()
        self.stream_ok = False          # （仅 HTTP 模式）流当前是否连着
        self.stream_err = ""            # 最近一次断开的原因
        self._photo_lock = threading.Lock()

    # ------------------------------------------------------------------
    def current_ip(self) -> str:
        """当前 ESP32 地址：只有 HTTP 模式才用得上。

        USB 串口模式下图像不走网络，所以这里可能返回空串 —— 那是正常的，
        不要再把它当成"地址配错了"。
        """
        if self._ip_provider is not None:
            ip = self._ip_provider()
            if ip:
                return ip
        return self.esp32_ip

    @property
    def capture_url(self) -> str:
        return f"http://{self.current_ip()}/capture"

    @property
    def stream_url(self) -> str:
        return f"http://{self.current_ip()}/stream"

    def _client(self):
        """惰性获取 ollama 客户端（不用视觉时不加载）。

        ⚠️ 2026-10：重构时这个方法被误删过，导致 describe() 报
            'VisionDescriber' object has no attribute '_client'，
            视觉描述整个失效（YOLO 没检出时就完全没输出）。
        """
        if self._ollama is None:
            import ollama  # 延迟导入：不用视觉时不加载
            self._ollama = ollama
        return self._ollama

    def warmup(self) -> None:
        """预热 VL 模型（后台线程调用，不阻塞启动）。

        为什么需要（2026-10 实测）：
            第一次调 describe() 要让 Ollama 把 qwen3-vl:2b 加载进显存，
            实测 4.7s；加载完之后同样一张图只要 1.37s。
            用户说的"回复还是有点慢"主要就是这个一次性开销。
            启动时先跑一次小图，把它提前吃掉。
        """
        if self._warmed:
            return
        try:
            t0 = time.time()
            # 1x1 的极小 JPEG，只为触发模型加载，不关心输出
            tiny = base64.b64decode(
                b"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8U"
                b"HRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA"
                b"/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEA"
                b"AD8AKwA//9k="
            )
            self.describe(tiny)
            self._warmed = True
            self.log(f"[眼] VL 模型预热完成（{time.time() - t0:.1f}s）"
                     f"—— 之后每次描述约 1.4s")
        except Exception as exc:  # noqa: BLE001
            self.log(f"[眼] VL 预热失败（不影响使用）：{exc!r}")

    # ------------------------------------------------------------------
    def grab_frames(self, want: int = 5, max_wait_s: float = 4.0) -> Optional[bytes]:
        """取一张当前画面（JPEG 字节）。

        ★ USB 串口模式（默认）：直接让 ESP32 现抓一帧（frame_provider 注入的
          request_jpeg），不拉多帧 —— 板子上没有 MJPEG 流，也没什么可"等"的。
        HTTP 模式（未注入 frame_provider）：沿用旧的"临时开流拉几帧取最后一张"。

        为什么 HTTP 模式要拉多帧而不是 1 帧：
            ESP32 用 CAMERA_GRAB_LATEST，第一帧常常还是残留在缓冲里的旧画面。
            拉 3~5 帧能确保拿到的是当前画面。
        """
        if self._frame_provider is not None:
            t0 = time.monotonic()
            try:
                jpg = self._frame_provider()
            except Exception as exc:  # noqa: BLE001
                self.log(f"[眼] 串口取图失败：{exc!r}")
                return None
            if jpg is None:
                return None
            self.frame.put(jpg)
            self.log(f"[眼] 取到一帧 {len(jpg)} 字节"
                     f"（{(time.monotonic() - t0) * 1000:.0f} ms）")
            return jpg

        target = self.stream_url
        last_jpg: Optional[bytes] = None
        got = 0
        t0 = time.monotonic()
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        while got < want and (time.monotonic() - t0) < max_wait_s:
            try:
                req = urllib.request.Request(target, headers={"Connection": "close"})
                with opener.open(req, timeout=3) as resp:
                    buf = bytearray()
                    for chunk in iter(lambda: resp.read(4096), b""):
                        buf.extend(chunk)
                        frames = list(parse_stream(buf))
                        if frames:
                            last_jpg = frames[-1]
                            got += 1
                            break          # ESP32 每次请求只发一帧就断开
                        if len(buf) > 4 * 1024 * 1024:
                            break
            except Exception as exc:  # noqa: BLE001
                self.log(f"[眼] 拉帧失败（{type(exc).__name__}: {exc}）")
                if last_jpg is None:
                    return self._fetch_capture()   # 一帧都没拿到才算真失败

        if last_jpg is None:
            return self._fetch_capture()

        self.log(f"[眼] 按需拉取 {got} 帧（{time.monotonic() - t0:.2f}s，"
                 f"取最后一张 {len(last_jpg)} 字节）")
        return last_jpg

    # ------------------------------------------------------------------
    def get_image(self) -> Optional[bytes]:
        """取一张图（JPEG 字节）。(可选)存盘。失败返回 None。

        USB 模式：向 ESP32 请求一帧；HTTP 模式：临时开流拉几帧取最后一张。
        """
        jpg = self.grab_frames()
        if jpg is not None:
            self._save_jpg(jpg)
        return jpg

    def _save_jpg(self, data: bytes) -> None:
        """把图存成 captured.jpg（调试用，失败不影响识别）。"""
        if not self.save_jpg:
            return
        try:
            with open(self.save_jpg, "wb") as f:
                f.write(data)
        except Exception as exc:  # noqa: BLE001
            self.log(f"[眼] 存图失败（不影响识别）：{exc}")

    def _fetch_capture(self) -> Optional[bytes]:
        """HTTP /capture 现抓一张 —— 仅 WiFi 模式（未注入 frame_provider）用。"""
        if not self.current_ip():
            self.log("[眼] 没配置 ESP32 地址（USB 模式不需要），跳过 HTTP 抓图")
            return None
        try:
            import requests

            resp = requests.get(
                self.capture_url,
                timeout=self.timeout_s,
                proxies=self._proxies,
                headers={"Connection": "close"},
            )
            resp.raise_for_status()
        except Exception as exc:  # noqa: BLE001
            # 区分两类失败，提示要准确：
            #   · 503 = ESP32 收到了请求，但它自己【没有图】（摄像头没起来 /
            #     还没抓到帧）。这时候报"IP 不对"是误导 —— IP 明明是通的。
            #   · 其他（超时/连接拒绝）= 才可能是 IP 或没开机的问题。
            code = getattr(getattr(exc, "response", None), "status_code", None)
            if code == 503:
                self.log(
                    f"[眼] 抓图失败：ESP32 回 503（它暂时没有图）。"
                    f"IP={self.current_ip()} 是通的 —— 说明是【摄像头侧】的问题：\n"
                    f"      · 看 ESP32 串口有没有 'camera init failed' / "
                    f"frame buffer malloc failed\n"
                    f"      · 若是 malloc 失败：把 PSRAM 设成 OPI PSRAM 再烧\n"
                    f"      · 也可能只是还没抓到第一帧（CAM_REFRESH_MS="
                    f"1000ms），过一秒再问一次"
                )
            else:
                self.log(f"[眼] 抓图失败：{exc}"
                         f"（确认 ESP32 已开机、IP={self.current_ip()} 正确）")
            return None

        data = resp.content
        if not data:
            self.log("[眼] 抓图返回空内容")
            return None

        self.log(f"[眼] 已抓图（{len(data)} 字节）")
        self._save_jpg(data)
        return data

    def _strip_thinking_prefix(self, text: str) -> str:
        """剥掉思考口吻的前缀，只留对画面的描述。

        实测 content 的典型形态：
            "Got it, let's see. The photo shows a white shelf..."
            "Let me look at this. It's a close-up of..."
        做法：按句切分，跳过含思考标记的句子，取第一句"描述性"的话。
        若整段都像思考，就退回去用原来的 clean_thinking 提取。
        """
        if not text:
            return ""

        # 先整体判断：不像思考就直接返回
        if not self._looks_like_thinking(text):
            return text

        import re as _re
        parts = [p.strip() for p in _re.split(r"(?<=[.!?])\s+", text) if p.strip()]
        for p in parts:
            if not self._looks_like_thinking(p) and len(p) > 12:
                return p

        return self._clean_thinking(text)

    def _looks_like_thinking(self, text: str) -> bool:
        """判断这段文本是"思考过程"而不是"对画面的描述"。

        qwen3-vl 在被要求描述图片时，思考过程有明显的口吻特征：
            "Got it, let's see." / "I need to..." / "Let me check..."
            "Need to make it one sentence..." / "Wait, ..."
        这些都不是对画面本身的陈述，直接返回给 LLM 会误导它。
        """
        low = text.lower().strip()
        markers = (
            "got it", "let's see", "let me", "i need to", "i should",
            "need to make", "let's check", "okay,", "ok,", "wait,",
            "first,", "so,", "therefore,", "the user",
        )
        head = low[:40]
        return any(m in head for m in markers)

    def _clean_thinking(self, thinking: str) -> str:
        """从思考内容里尽量提取出一句"描述性结论"，而不是推理过程。

        qwen3-vl 偶尔会把结论写在最后一句（如 "So the photo shows ..."），
        这里优先挑这类句子；挑不到就用最后一句，并去掉明显的推理前缀。
        """
        lines = [ln.strip() for ln in thinking.splitlines() if ln.strip()]
        if not lines:
            return ""

        # 优先找"陈述性"句子（shows / there is / I see / the image）
        keys = ("shows", "there is", "there are", "i see", "the photo",
                "the image", "it is", "it's")
        for ln in reversed(lines):
            low = ln.lower()
            if any(k in low for k in keys) and len(ln) > 15:
                # 去掉 "So, " / "Therefore, " 这类连接词
                for pre in ("so, ", "so ", "therefore, ", "thus, ", "finally, "):
                    if low.startswith(pre):
                        ln = ln[len(pre):]
                        break
                return ln[:300]

        # 退而求其次：最后一句，去掉推理口吻
        last = lines[-1]
        for bad in ("wait,", "let me", "let's", "i need to", "identify the",
                    "first,", "okay,", "got it,"):
            if last.lower().startswith(bad):
                return ""
        return last[:300]

    def describe(self, image_bytes: bytes) -> Optional[str]:
        """用 Ollama 视觉模型描述图片字节。

        关键点（全部实测得来，之前踩过的坑）：
          * qwen3-vl 是"思维链"视觉模型：它先输出一大段思考(thinking)，
            思考完才写正文(content)。
          * ⚠️ 因此 num_predict 必须给足（≥256）。给太小（如 96）会导致
            它在思考阶段就被截断（done_reason='length'），content 恒为空
            —— 现象就是"耗时很久却什么都没返回"。实测 256 时 0.9s 就能
            拿到干净正文，比原来快 20 倍。
          * num_ctx 也要给足（图像 token 很占上下文）。
          * content 仍为空时，从 thinking 里兜底提取结论句。
        """
        if not image_bytes:
            return None

        b64 = base64.b64encode(image_bytes).decode("ascii")
        best = ""

        for attempt in (1, 2):
            try:
                ollama = self._client()
                t0 = time.time()
                # ⚠️ 关键：think=False（2026-10 实测）
                #    qwen3-vl 默认先写一大段 thinking，正文排在后面。
                #    num_predict 一旦被思考吃满（done_reason='length'），
                #    正文一个字都不剩 —— 表现就是"描述"其实是思考过程
                #    （"Got it, let's see..."），又慢又没用。
                #    关掉思考链后直接出正文，又快又干净。
                resp = ollama.chat(
                    model=self.model,
                    messages=[{
                        "role": "user",
                        "content": VISION_PROMPT,
                        "images": [b64],
                    }],
                    think=False,
                    options={
                        "num_ctx": self.num_ctx,       # 给足上下文放图像 token
                        "num_predict": self.num_predict,
                    },
                )
                msg = resp.get("message", {})
                text = str(msg.get("content") or "").strip()

                # 后处理：把思考口吻的前缀剥掉，留下真正的描述。
                # （think=False 在模型首次加载/冷启动时偶尔不生效，
                #   实测 5 次里约有 1 次会带出 "Got it, let's see. ..."，
                #   与其重试浪费 1.5s，不如就地清洗 —— 结论句本身是对的。）
                text = self._strip_thinking_prefix(text)

                if text:
                    self.log(f"[眼] 视觉描述（{time.time() - t0:.1f}s）：{text}")
                    return text

                # 只有 thinking 字段时：从里面提取结论句
                picked = self._clean_thinking(str(msg.get("thinking") or ""))
                if picked:
                    best = picked
                    self.log(
                        f"[眼] 正文为空(done={resp.get('done_reason')} "
                        f"eval={resp.get('eval_count')})，从思考提取：{picked}"
                    )
                    break

                self.log(f"[眼] 第{attempt}次视觉描述为空（{time.time() - t0:.1f}s）")
            except Exception as exc:  # noqa: BLE001
                self.log(f"[眼] 视觉识别第{attempt}次失败：{exc}")

        if best:
            return best
        self.log(f"[眼] 视觉识别没拿到可用内容（模型 {self.model}）")
        return None

    # ------------------------------------------------------------------
    def look(self) -> Optional[str]:
        """抓一张图并理解，返回描述文本（给 LLM 当上下文用）。

        定位（2026-10 按用户要求调整）：
            **VL 描述是主体，YOLO 只是"突出重点"。**
              · YOLO 认出常见物体 -> 用它的中文结果（快、准、带位置）
              · YOLO 没认出 -> 不是"没东西可看"，而是这张图里没有 COCO
                80 类物体（风景/桌面/房间/文字/颜色本来就认不出来）。
                这时必须让 VL 正常描述整张画面。
        """
        img = self.get_image()
        if img is None:
            return None

        # ---- ① YOLO：能认出常见物体就用它（突出重点）----
        if self.detector is not None:
            dets = self.detector.detect(img)
            self._last_dets = dets
            if dets:
                desc = describe_detections(dets)
                self.log(f"[眼] YOLO：{describe_detections_cn(dets)}")
                self._last_desc = desc
                self._last_ts = time.time()
                return desc

            # ---- ② 没检出：让 VL 正常描述整体画面 ----
            self.log("[眼] YOLO 未检出常见物体，转 VL 描述整张画面")

        desc = self.describe(img)
        if desc:
            self._last_desc = desc
            self._last_ts = time.time()
            return desc

        # ---- ③ VL 也失败了：如实说"看不清"，不让 LLM 凭空编 ----
        msg = ("The camera view is unclear; I cannot make out the details.")
        self.log("[眼] VL 也没拿到内容，返回兜底描述")
        self._last_desc = msg
        self._last_ts = time.time()
        return msg

    # ------------------------------------------------------------------
    def take_photo(self) -> Optional[str]:
        """语音「拍照」：把当前画面存一张 JPEG 到 photos/，返回文件路径。

        取图方式同 get_image()（USB 模式向 ESP32 请求一帧）。
        文件名 photo_YYYYmmdd_HHMMSS.jpg，同一秒连拍自动加序号。
        """
        data = self.get_image()
        if data is None:
            self.log("[眼] 拍照失败：没拿到画面")
            return None

        try:
            os.makedirs(PHOTO_DIR, exist_ok=True)
        except OSError as exc:
            self.log(f"[眼] 拍照失败：建目录 {PHOTO_DIR} 出错（{exc}）")
            return None

        stamp = time.strftime("%Y%m%d_%H%M%S")
        with self._photo_lock:
            path = os.path.join(PHOTO_DIR, f"photo_{stamp}.jpg")
            n = 1
            while os.path.exists(path):          # 同一秒连拍会重名
                path = os.path.join(PHOTO_DIR, f"photo_{stamp}_{n}.jpg")
                n += 1
            try:
                with open(path, "wb") as f:
                    f.write(data)
            except OSError as exc:
                self.log(f"[眼] 拍照失败：写文件出错（{exc}）")
                return None

        self.log(f"[眼] 拍照已保存 {path}（{os.path.getsize(path)} 字节）")
        return path

    def photo_count(self) -> int:
        """photos/ 里已经存了多少张（给语音回复用）。"""
        try:
            return sum(1 for n in os.listdir(PHOTO_DIR)
                       if n.lower().endswith(".jpg"))
        except OSError:
            return 0

    @property
    def last_detections(self) -> List[Detection]:
        """最近一次的 YOLO 检测结果（含 dx/dy 偏移，供"视觉跟踪"取用）。"""
        return list(self._last_dets)

    @property
    def last_description(self) -> str:
        """最近一次的描述（供调试/复用）。"""
        return self._last_desc
