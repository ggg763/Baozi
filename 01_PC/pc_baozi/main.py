# noinspection LanguageDetectionInspection
"""包子机器人 · PC 端主入口。
一个进程里同时跑三条链路（全部经过【一根 USB 线】与 ESP32 通信）：
  1) 语音对话：ESP32 <-> USB 串口 <-> VAD/ASR/LLM/TTS
  2) 图像描述：PC 发抓图请求 -> ESP32 现抓一帧 JPEG 回传 -> YOLO/qwen3-vl 描述
  3) 情感下发：LLM 输出的情绪/意图 -> ESP32 -> UART -> TD1601 行为层（舵机/表情）

★ 2026-11 改动：ESP32 暂时用 USB 线直连这台电脑，PC 的【网络只用于外部服务】
  （edge-tts / 天气 / Ollama 等），不再需要和 ESP32 组局域网、也不需要 IP。
  想切回 WiFi 模式：固件把 USE_USB_LINK 设成 0，这里加 --transport tcp。

情感状态机分两部分：
  · PC 侧（emotion.py）：作为大语言模型的 memory，短期调控情绪；
    让 LLM 在回复文本之外，按固定 JSON 输出 {reply, emotion, intent}；
  · TD1601 侧（UserFolder/emotion/）：收到 EMO:xxx 后自主决定动作与强度
    （强度由 TD1601 用 emotion_manager 自行推算，PC 不下发强度）。

情感下行链路（默认走 ESP32 转发）：
    PC --USB 串口--> ESP32 --UART(GPIO14)--> TD1601 PB18(UART3 RX)
  TD1601 的 UART0(PA17/PA18) 因此空出来，可以一直挂 SSCOM 看日志。

用法::

    python main.py                              # USB 模式，串口自动检测
    python main.py --serial-port COM5           # 指定串口
    python main.py --list-ports                 # 看本机有哪些串口
    python main.py --td1601-backend serial --td1601-port COM7   # 改回串口直连
    python main.py --no-vision --no-td1601                      # 只跑语音
    python main.py --asr-backend mock --llm-backend echo --tts-backend tone
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
import threading
import time

from server import BANNER, apply_extra_overrides, check_dependencies, make_config


# ---------------------------------------------------------------------
#  单实例保护：main.py 要独占串口（COM7/COM16 等）和 8765/8766 端口。
#  双击 run.bat 多次、或旧实例没退干净就重开，会造成多个实例抢串口、
#  互相抢发现广播 —— 现象是"串口打不开 / ESP32 连不上 / 屏幕收不到指令"。
#
#  做法：往临时目录写一个锁文件，里面存 PID。启动时若锁还在、且那个 PID
#  还活着，就直接退出并提示；锁文件里的 PID 已死（残留）则覆盖它。
#  锁文件随进程退出自动清掉（atexit），正常 Ctrl+C / stop() 都会触发。
# ---------------------------------------------------------------------
_LOCK_NAME = "baozi_main.lock"


def _acquire_single_instance() -> None:
    lock_path = os.path.join(tempfile.gettempdir(), _LOCK_NAME)

    def _pid_alive(pid: int) -> bool:
        if os.name != "nt":
            try:
                os.kill(pid, 0)
                return True
            except OSError:
                return False
        # Windows：用 tasklist 探测该 PID 是否还在（不做权限敏感的 os.kill）。
        import subprocess
        try:
            out = subprocess.run(
                ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                capture_output=True, text=True, timeout=5,
            ).stdout
            return str(pid) in out
        except Exception:
            return True  # 查不到就不冒险抢，按"还活着"处理

    if os.path.exists(lock_path):
        try:
            old_pid = int(open(lock_path, encoding="utf-8").read().strip() or "0")
        except (OSError, ValueError):
            old_pid = 0
        if old_pid and _pid_alive(old_pid) and old_pid != os.getpid():
            print(f"[!] 检测到另一个 main.py 正在运行（PID {old_pid}）。", file=sys.stderr)
            print("    包子一次只能跑一个实例（要独占串口和端口）。", file=sys.stderr)
            print("    若确认旧实例已死，删掉这个文件后重试：", file=sys.stderr)
            print(f"    {lock_path}", file=sys.stderr)
            sys.exit(1)

    with open(lock_path, "w", encoding="utf-8") as f:
        f.write(str(os.getpid()))

    import atexit
    atexit.register(lambda: os.path.exists(lock_path) and os.remove(lock_path))


def _parse_extra(argv):
    """本程序特有的参数（图像链路 + 情感下发），其余原样交给 server 的参数解析。"""
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--esp32-ip", default=None,
                    help="ESP32 局域网 IP（仅 --transport tcp 的图像链路用）")
    ap.add_argument("--vision-interval", type=float, default=None, help="取图间隔（秒）")
    ap.add_argument("--vision-model", default=None, help="Ollama 视觉模型名")
    ap.add_argument("--no-vision", action="store_true", help="关闭图像描述")
    ap.add_argument("--no-yolo", action="store_true", help="关闭 YOLO 目标检测（只走 VL）")
    ap.add_argument("--yolo-model", default=None, help="YOLO 权重路径，如 weights/yolov8n.pt")
    ap.add_argument("--yolo-imgsz", type=int, default=None,
                    help="YOLO 推理尺寸（CPU 嫌慢可设 320）")
    ap.add_argument("--yolo-once", action="store_true",
                    help="自检：抓一张图跑一次 YOLO 并打印结果，然后退出")
    ap.add_argument("--td1601-backend", default=None,
                    choices=["esp32", "serial", "sim"],
                    help="情感下发链路：esp32(默认) | serial | sim")
    ap.add_argument("--td1601-port", default=None, help="backend=serial 时的串口，如 COM5")
    ap.add_argument("--no-td1601", action="store_true", help="关闭情感下发")
    ap.add_argument("--with-app", dest="with_app", action="store_true", default=None,
                    help="启用组员 app 整合层（习惯/日程/提醒/小程序），"
                         "默认由 config.json 的 app_enabled 决定")
    ap.add_argument("--no-app", action="store_true",
                    help="关闭 app 整合层（只跑语音/视觉/情感）")
    ap.add_argument("--no-qr", action="store_true",
                    help="开机不把配网二维码推到 TD1601 屏幕（默认会推一次）")
    extra, rest = ap.parse_known_args(argv)
    return extra, rest


def _yolo_once(cfg, frame_provider=None) -> int:
    """自检：抓一张图跑一次 YOLO 检测，打印结果（不启动语音链路）。

    用途：验证「ESP32 能不能取到图 + YOLO 在这个分辨率下认不认得出目标」，
    避免一上来就开着语音链路来回试。用法：
        python main.py --yolo-once
    """
    from vision import VisionDescriber
    from detector import YoloDetector, describe_detections, describe_detections_cn

    print("[自检] 只跑一次 YOLO 检测（不启动语音链路）")
    det = YoloDetector(model=cfg.vision_yolo_model, conf=cfg.vision_yolo_conf,
                       imgsz=cfg.vision_yolo_imgsz)
    v = VisionDescriber(esp32_ip=cfg.esp32_ip, timeout_s=cfg.vision_timeout_s,
                        save_jpg=cfg.vision_save_jpg, detector=det,
                        frame_provider=frame_provider)

    t0 = time.time()
    img = v.get_image()
    if img is None:
        print(f"[自检] 取图失败（{time.time() - t0:.1f}s）。"
              f"确认 ESP32 已插好 USB、串口没被别的程序占用。")
        return 2
    print(f"[自检] 取图成功：{len(img)} 字节（{time.time() - t0:.1f}s）")

    t1 = time.time()
    dets = det.detect(img)
    dt = (time.time() - t1) * 1000
    print(f"[自检] YOLO 耗时 {dt:.0f} ms")
    if not dets:
        print("[自检] 没检出目标。可尝试：① 把物体靠近镜头 ② 降低阈值 "
              "vision_yolo_conf ③ 提高分辨率（需改 ESP32 的 frame_size）")
        return 0

    print(f"[自检] 检出 {len(dets)} 个目标：")
    for d in dets:
        print(f"    {d.label:8} 置信 {d.conf:.2f}  {d.position():6}  "
              f"{d.distance_hint():6}  dx={d.dx:+.3f} dy={d.dy:+.3f}")
    print(f"\n[自检] 给 LLM 的上下文: {describe_detections(dets)}")
    print(f"[自检] 中文摘要: {describe_detections_cn(dets)}")
    print("\n[自检] dx/dy 就是做“视觉跟随”时要发给 TD1601 的偏移量。")
    return 0


def _start_boot_greeting(cfg, srv, pipe) -> None:
    """接通 ESP32 后，放一次开机自我介绍。

    三个作用：
      1. **热身** —— 代替原来那句合成完就扔掉的"预热"；
      2. **自检** —— 听到了就说明 PC -> ESP32 -> 喇叭 整条音频链路是通的，
         比事后猜"为什么没声音"强得多；
      3. 语音用的是**预生成 WAV**（assets/boot_hello.wav），纯本地读盘，
         和 TTS / 网络完全解耦 —— 开机时网络没起来也照样能说。

    独立线程 + 轮询连接状态：ESP32 可能比 PC 晚几十秒上电，
    这里最多等 3 分钟，等不到就安静放弃（不阻塞、不报错）。
    """

    def _run() -> None:
        if not getattr(cfg, "boot_voice_enabled", True):
            return
        try:
            from boot_voice import load_boot_voice
            pcm = load_boot_voice()
        except Exception as exc:  # noqa: BLE001
            print(f"[boot] 开机语音加载失败（跳过）：{exc!r}")
            return
        if pcm is None:
            print("[boot] 没有 assets/boot_hello.wav，开机不出声"
                  "（跑 python boot_voice.py 生成一次）")
            return

        deadline = time.monotonic() + 180.0
        while time.monotonic() < deadline:
            if srv.connected:
                break
            time.sleep(0.5)
        else:
            print("[boot] 180s 内没等到 ESP32，开机语音跳过")
            return

        time.sleep(1.0)          # 给握手和播放缓冲一点稳定时间
        try:
            ok = pipe.say_pcm(pcm, "开机语音")
        except Exception as exc:  # noqa: BLE001
            print(f"[boot] 开机语音播放失败（忽略）：{exc!r}")
            return
        if ok:
            print(f"[boot] 开机问候已送出（{len(pcm) / 16000:.1f}s）", flush=True)
        else:
            print("[boot] 开机问候没送出去（链路忙或没有播放通路）", flush=True)

    threading.Thread(target=_run, name="boot-greeting", daemon=True).start()


def _start_provision_qr(cfg, srv, td_link) -> None:
    """接通 ESP32 后，把本机 WiFi 的配网二维码推到 TD1601 屏幕（NET 页）一次。

    为什么需要它：TD1601 的 NET 页只要一次都没收到过 `QR:` 行，就永远显示
    `waiting for network / from ESP32 over UART3`（见 UserFolder/qr/net_card.c
    的 !s_has_qr 分支）。而 PC 侧过去**根本没人发** —— 屏上那句话其实是在等
    一件没有实现的事。

    链路和 TRACK:ON 完全一样：send_line -> CTRL_TD_TEXT -> ESP32 原样转发 ->
    TD1601 的 PB18，所以 ESP32 固件一行都不用改。

    只推一次、不周期重推：板子每收到一条 QR: 都会 ui_app_show_net() 把屏幕
    拉回 NET 页（那是为了让人举着手机扫），而 NET 页按设计不自动回落 ——
    周期重推会让人正常聊天时被反复抢屏。

    独立线程 + 轮询连接状态，和 _start_boot_greeting 同一套路：ESP32 可能比
    PC 晚几十秒上电，最多等 3 分钟，等不到就安静放弃。
    """

    def _run() -> None:
        if not getattr(cfg, "provision_qr_enabled", True):
            return

        deadline = time.monotonic() + 180.0
        while time.monotonic() < deadline:
            if srv.connected:
                break
            time.sleep(0.5)
        else:
            print("[qr] 180s 内没等到 ESP32，配网二维码这次不推了")
            return

        # 等板子把 ui_app / net_link 初始化完再推：net_link_init() 里会
        # net_card_reset()，推早了会被那次 reset 清掉，屏幕又回到 waiting。
        time.sleep(3.0)

        try:
            from wifi_cred import local_provision_lines

            lines, ssid, has_pwd = local_provision_lines()
        except Exception as exc:                             # noqa: BLE001
            print(f"[qr] 配网二维码跳过：{exc}")
            return

        try:
            for line in lines:
                td_link.send_line(line)
                time.sleep(0.25)      # 两行别粘成一行（板子按行解析）
        except Exception as exc:                             # noqa: BLE001
            print(f"[qr] 推到屏幕失败（忽略）：{exc!r}")
            return

        print(f"[qr] 配网码已推给屏幕：SSID={ssid!r}"
              f"（{'带口令' if has_pwd else '无口令'}，{len(lines)} 行）。"
              "屏上应停在 NET 页的二维码上，点一下可放大成全屏码。", flush=True)
        print("[qr] 如果屏上还是 waiting for network，看板子 UART0 日志里有没有 "
              "[NET] qr set（有=编码成功，是显示/跳页问题）/ [NET] qr rejected"
              "（编码失败）/ 什么都没有（UART3 那一段没传到）", flush=True)

    threading.Thread(target=_run, name="provision-qr", daemon=True).start()


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)

    _acquire_single_instance()

    print(BANNER)
    extra, rest = _parse_extra(argv)

    cfg = make_config(rest)
    # 整合版特有的开关（--no-vision / --no-yolo / --no-td1601 …）写回 cfg。
    # ⚠️ 必须真的写回，否则这些开关会静默失效（见 server.apply_extra_overrides）。
    apply_extra_overrides(cfg, extra)

    # app 整合层开关：命令行优先于 config.json
    if extra.no_app:
        cfg.app_enabled = False
    elif extra.with_app:
        cfg.app_enabled = True

    # 开机自动推配网二维码（连上 ESP32 后推一次，见 _start_provision_qr）
    if getattr(extra, "no_qr", False):
        cfg.provision_qr_enabled = False

    # 情感下行后端（关闭时退化为 sim，只打印）
    td_backend = cfg.td1601_backend if cfg.td1601_enabled else "sim"
    if td_backend not in ("esp32", "serial"):
        td_backend = "sim"

    usb_mode = str(cfg.transport).lower() != "tcp"

    print("配置：")
    if usb_mode:
        print(f"  链路  USB 串口 {cfg.serial_port or '自动检测'} @ {cfg.serial_baud}"
              f"（PC 只用外网，不需要 ESP32 的 IP）")
    else:
        print(f"  链路  WiFi/TCP 监听 {cfg.host}:{cfg.port}")
    print(f"  语音  ASR={cfg.asr_backend}  LLM={cfg.llm_model}  TTS={cfg.tts_backend}")
    if cfg.vision_enabled:
        src = "USB 串口（发请求现抓一帧）" if usb_mode else f"HTTP {cfg.esp32_ip}"
        print(f"  图像  开  取图={src}")
        if getattr(cfg, "vision_yolo_enabled", True):
            print(f"  识别  YOLO({cfg.vision_yolo_model}, imgsz={cfg.vision_yolo_imgsz})"
                  f" 优先" +
                  (f"，未检出回落 {cfg.vision_model}" if getattr(cfg, "vision_vl_fallback", True)
                   else "，不做 VL 兜底"))
        else:
            print(f"  识别  {cfg.vision_model}（YOLO 已关）")
    else:
        print("  图像  关（--no-vision）")
    print(f"  情感  {'开' if cfg.td1601_enabled else '关'}  后端={td_backend}"
          + (f"  串口={cfg.td1601_port}" if td_backend == "serial" else
             "  (经 ESP32 转发 -> TD1601 PB18)" if td_backend == "esp32" else
             "  (模拟，只打印 EMO:/INTENT:)"))
    print(f"  配网  {'开' if getattr(cfg, 'provision_qr_enabled', True) else '关'}"
          "（连上 ESP32 后把本机 WiFi 的二维码推到屏幕 NET 页，--no-qr 可关）")
    if cfg.app_enabled:
        print(f"  App   {'开' if cfg.app_enabled else '关'}"
              f"  后端={cfg.app_backend_host}:{cfg.app_backend_port}"
              f"{'(自动启动)' if cfg.app_backend_autostart else '(手动启动)'}"
              f"  屏幕串口={cfg.app_board_port or '自动识别'}"
              f"  提醒语音=主链路(say)")
    else:
        print("  App   关（--no-app 或 config.json 的 app_enabled=false）")
    print()

    # ---- 自检：抓一张图跑一次 YOLO，打完结果就退出（不启动语音链路）----
    if extra.yolo_once:
        # 自检也要能连上 ESP32 取图：直接开一个链路拿一帧。
        frame_provider = None
        if usb_mode:
            from link import TransportError, make_transport

            transport = make_transport(cfg)
            try:
                transport.open()
            except TransportError as exc:
                print(f"[自检] {exc}", file=sys.stderr)
                return 2
            from protocol import FrameDecoder, Pkt, encode_ctrl, Ctrl

            def frame_provider():        # noqa: F811
                """在自检模式里直接用传输层请求一帧（不启动完整 VoiceServer）。"""
                transport.write(encode_ctrl(Ctrl.CAM_REQ, b"\x01"))
                dec = FrameDecoder()
                chunks = bytearray()
                deadline = time.time() + cfg.vision_timeout_s
                while time.time() < deadline:
                    for f in dec.feed(transport.read(4096)):
                        if f.type == Pkt.CAM_JPEG:
                            chunks.extend(f.payload)
                        elif f.type == Pkt.CTRL and f.payload and f.payload[0] == Ctrl.CAM_DONE:
                            return bytes(chunks) if chunks else None
                return bytes(chunks) if chunks else None

            try:
                return _yolo_once(cfg, frame_provider)
            finally:
                transport.close()
        return _yolo_once(cfg)

    check_dependencies(cfg)

    # ---- 1) 语音服务（先建，情感链路要借它的链路往 ESP32 发控制帧）----
    from asr import make_asr
    from tts import make_tts
    from link import TransportError
    from pipeline import Pipeline
    from voice_server import VoiceServer

    # 情绪内存（PC 侧情感状态机的第一部分）要先于 LLM 建好，供其注入 prompt
    from emotion import EmotionMemory
    emotion_memory = EmotionMemory()

    # 对话记忆（短期历史 + 长期画像），参照 Baozi_PC/memory
    mem = None
    if cfg.memory_enabled:
        from memory import MemoryManager
        mem = MemoryManager(
            memory_file=cfg.memory_file,
            max_history=cfg.memory_max_history,
            llm_base_url=cfg.llm_base_url,
            llm_api_key=cfg.llm_api_key,
            model_name=cfg.llm_model,
        )
        print(f"[mem] 对话记忆已启用（短期 {cfg.memory_max_history} 轮，"
              f"长期画像存 {cfg.memory_file}）")

    from llm import make_llm

    # ⚠️ 下面两步是启动里最慢的（合计首次约 20~40 秒）：加载 SenseVoice 模型、
    #    把 7B 读进内存预热。不打进度的话，窗口会长时间停在上一行，
    #    用户会以为"卡死了 / 没反应"（实测被问过）。
    print("[启动] 加载语音识别模型（SenseVoice，首次约 10~30 秒）...", flush=True)
    asr = make_asr(cfg)
    print("[启动] 连接大模型与语音合成 ...", flush=True)
    llm = make_llm(cfg, emotion_memory, memory=mem)   # 文本 + 情绪/意图 + 记忆
    tts = make_tts(cfg)

    # 预热大模型：把 7B 先加载进显存并常驻，避免第一句话等 10 秒以上
    if cfg.llm_warmup and hasattr(llm, "warmup"):
        print("[启动] 预热大模型（首次约 10~60 秒，之后秒回）...", flush=True)
        llm.warmup()

    # ---- UDP 自动发现：只在 WiFi 模式需要（USB 直连不需要 IP）----
    if usb_mode:
        disc = None
    else:
        from discovery import PcDiscovery
        disc = PcDiscovery(tcp_port=cfg.port)
        disc.start()

    # 图像（按需调用）：只在用户问"看到了什么"时才取一帧，不做定时循环
    vision = None
    if cfg.vision_enabled:
        from vision import VisionDescriber

        # YOLO 检测器（首选通路）。模型第一次用时才加载，不拖慢启动。
        detector = None
        if getattr(cfg, "vision_yolo_enabled", True):
            from detector import YoloDetector
            detector = YoloDetector(
                model=cfg.vision_yolo_model,
                conf=cfg.vision_yolo_conf,
                imgsz=cfg.vision_yolo_imgsz,
            )

        vision = VisionDescriber(
            esp32_ip=cfg.esp32_ip,
            model=cfg.vision_model,
            timeout_s=cfg.vision_timeout_s,
            save_jpg=cfg.vision_save_jpg,
            # USB 模式：取图走串口请求（srv.request_jpeg）；WiFi 模式才用 IP 发现
            ip_provider=(None if usb_mode else disc.esp32_ip),
            detector=detector,
            vl_fallback=getattr(cfg, "vision_vl_fallback", True),
            frame_provider=None,        # 下面建好 srv 后再注入（见 _bind_vision）
        )
        if usb_mode:
            print("[眼] 取图走 USB 串口：问「你看到了什么」时才向 ESP32 请求一帧")
        else:
            print(f"[眼] 取图走 HTTP（ESP32={cfg.esp32_ip}）")
        if detector is not None:
            print(f"[眼] 识别顺序：YOLO({cfg.vision_yolo_model}) 先行（重点物体），"
                  f"未检出时由 {cfg.vision_model} 描述整张画面")
        # ★ 后台预热 VL 模型：第一次推理要把 qwen3-vl 加载进显存，
        #   实测 4.7s；预热后同样一张图只要 1.37s。
        #   放后台线程，不拖慢启动。
        threading.Thread(target=vision.warmup, name="vl-warmup", daemon=True).start()

    pipe = Pipeline(cfg, asr, llm, tts, vision=vision)
    srv = VoiceServer(cfg, pipe)

    # 把"向 ESP32 请求一帧"的能力注入视觉模块（USB 模式的核心接线）
    if vision is not None and usb_mode:
        vision._frame_provider = srv.request_jpeg

    # ---- 2) 情感下行链路：把 EMO:/INTENT: 送到 TD1601 ----
    from td1601 import Td1601Link

    def _send_frame(ctrl: int, payload: bytes) -> bool:
        """往 ESP32 发一个控制帧（复用语音连接）。payload[0]=控制码，[1..]=整行。"""
        try:
            line = payload[1:].decode("utf-8", "replace")
            return srv.send_ctrl_payload(ctrl, line)
        except Exception:  # noqa: BLE001
            return False

    td_link = Td1601Link(
        backend=td_backend,
        port=cfg.td1601_port or None,
        baudrate=cfg.td1601_baud,
        send_frame=_send_frame if td_backend == "esp32" else None,
    )

    # ---- 状态下发：三个字段打包成 1 个字节发（避免 3 帧连发撑溢出 FIFO）----
    # 背景：pipeline 会在同一轮里连续调用 on_emotion / on_intent / on_intensity。
    # 若各自独立发一帧，一轮就是 3 帧 ≈ 47 字节，TD1601 的 UART3 FIFO 会溢出、
    # 命令被截断（实测故障）。这里把三者缓存起来，等最后一个回调到齐再一次性发。
    _st = {"emo": None, "intent": None, "inten": None, "sent": False}

    def _flush_state() -> None:
        """三个字段都到齐（或超时）后发一帧。"""
        if _st["sent"]:
            return
        if _st["emo"] is None or _st["intent"] is None or _st["inten"] is None:
            return
        _st["sent"] = True
        td_link.send_state(_st["emo"], _st["intent"], _st["inten"])

    def _new_turn() -> None:
        _st.update(emo=None, intent=None, inten=None, sent=False)

    def on_emotion(emo: str) -> None:
        """LLM 给出的情绪：写回情绪内存 + 攒着，等齐了一起下发。"""
        emotion_memory.update(emo)
        _st["emo"] = emo
        _flush_state()

    def on_intent(intent: str) -> None:
        """LLM 给出的意图：攒着，等齐了一起下发。"""
        _st["intent"] = intent
        _flush_state()

    def on_intensity(intensity: str) -> None:
        """LLM 给出的情感强度：攒着，等齐了一起下发。"""
        _st["inten"] = intensity
        _flush_state()

    def on_track(on: bool) -> None:
        """红外追踪开关 -> TRACK:ON / TRACK:OFF。

        走 CTRL_TD_TEXT（"任意一行，原样转发给 TD1601"），所以 ESP32 侧
        一行都不用改。刻意【不】攒进 _st：它不是状态字段而是一次即时动作，
        拖到"三个字段凑齐"才发没有意义 —— 用户说完就该动。
        """
        td_link.send_line("TRACK:ON" if on else "TRACK:OFF")

    def on_reply(reply: str) -> None:
        """LLM 给出的回复：打印到控制台，再清空状态缓存（避免串轮）。

        之前这里只调 _new_turn()，回复文字、情绪、意图一个都没打印 ——
        于是"包子到底听懂没、回了什么、为什么配了这个表情"在 PC 串口里
        完全看不到，出了乱码/错答也没法定位。补上可见性。
        """
        print(f"[reply] {reply!r}", flush=True)
        if reply:
            print(f"[判] 情绪={_st.get('emo')} 意图={_st.get('intent')} "
                  f"强度={_st.get('inten')}", flush=True)
        _new_turn()

    # VoiceServer 只覆盖它自己那几个回调键，不会动这几个
    pipe.ev.update(on_emotion=on_emotion, on_intent=on_intent,
                   on_intensity=on_intensity, on_track=on_track,
                   on_reply=on_reply)

    # ---- 3) 图像已在上面按需注入 Pipeline，无需后台线程 ----

    # ---- 4) 组员 app 整合层：后端(:8000) + TD1601 屏幕桥接（习惯/日程/提醒）----
    # speaker=pipe.say：提醒到点时借【主语音链路】说一句（TTS -> ESP32 -> 喇叭）。
    # 不再自起 TCP 8765 语音服务端 —— 那个端口归 voice_server，起第二个必撞。
    app_svc = None

    try:
        srv.start()

        if cfg.app_enabled:
            from app.service import AppService

            # say_pcm=pipe.say_pcm：一键演示的播报走这条（预合成 WAV -> ESP32 -> 喇叭，
            # 不经过 TTS）。没有它演示照样能跑，只是每一步没有解说。
            app_svc = AppService(cfg, speaker=pipe.say, say_pcm=pipe.say_pcm)
            app_svc.start()

        # 开机自我介绍：等 ESP32 连上来就放一次（热身 + 音频链路自检）
        _start_boot_greeting(cfg, srv, pipe)

        # 配网二维码：连上后把本机 WiFi 的 WIFI: 码推到屏幕 NET 页（--no-qr 关）
        _start_provision_qr(cfg, srv, td_link)

        print("[link] 就绪。语音：对着麦克风说话（说「拍照」可存图）；"
              "视觉：问「你看到了什么」即从串口取一帧并描述；"
              "情感：下发 TD1601"
              + ("；App：习惯/日程卡片 + 到点提醒" if app_svc is not None else "")
              + "。Ctrl+C 退出", flush=True)
        while True:
            time.sleep(0.5)
    except TransportError as exc:
        print(f"\n[link] {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\n[link] 收到 Ctrl+C，正在退出 ...", flush=True)
    finally:
        if app_svc is not None:
            app_svc.stop()
        if disc is not None:
            disc.stop()
        td_link.close()
        srv.stop()
        pipe.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
