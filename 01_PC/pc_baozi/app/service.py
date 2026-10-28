"""AppService —— 把组员 app 那一套（习惯 / 日程 / 提醒 / 小程序）并进主进程。

与其他模块的关系：

    pc_baozi/main.py
        └─ AppService.start()
             ├─ 后端子进程      app/backend/main.py（FastAPI + SQLite，默认 :8000）
             │                     ↑ 手机小程序读写这里
             └─ 桥接线程        app/link.py（串口 -> TD1601 屏幕）
                                 ↑ 提醒到点时调 speaker(text) 说话
                                   speaker = pipeline.say（主语音链路）

线程/进程边界为什么这么划：
  · 后端放【子进程】：FastAPI/uvicorn 出问题（端口冲突、依赖缺失、异常）不会
    把实时语音链路一起拖死；清理也简单（terminate + wait）。
  · 桥接放【线程】：它和主进程共享 speaker 回调，且只是一根串口的轮询循环。

用法（main.py 里已经接好，一般不用手动调）：

    svc = AppService(cfg, speaker=pipe.say)
    svc.start()
    ...
    svc.stop()
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from types import SimpleNamespace
from typing import Callable, Optional

APP_DIR = os.path.dirname(os.path.abspath(__file__))
BACKEND_DIR = os.path.join(APP_DIR, "backend")


class AppService:
    def __init__(self, cfg, speaker: Optional[Callable[[str], bool]] = None,
                 log: Callable[[str], None] = print) -> None:
        self.cfg = cfg
        self.speaker = speaker
        self.log = log
        self._backend: Optional[subprocess.Popen] = None
        self._backend_thread: Optional[threading.Thread] = None
        self._bridge_thread: Optional[threading.Thread] = None
        self._started = False

    # ------------------------------------------------------------------ 状态
    @property
    def running(self) -> bool:
        return self._started

    @property
    def api_base(self) -> str:
        host = getattr(self.cfg, "app_backend_host", "127.0.0.1")
        port = int(getattr(self.cfg, "app_backend_port", 8000))
        return f"http://{host}:{port}"

    # ------------------------------------------------------------------ 启停
    def start(self) -> bool:
        if self._started:
            return True
        if not getattr(self.cfg, "app_enabled", True):
            self.log("[app] 未启用（config.json 的 app_enabled=false）")
            return False

        self._started = True

        if getattr(self.cfg, "app_backend_autostart", True):
            self._start_backend()
        else:
            self.log("[app] 不自动启动后端（app_backend_autostart=false），"
                     "请手动运行 app/backend/start.bat")

        self._bridge_thread = threading.Thread(
            target=self._bridge_main, name="app-bridge", daemon=True)
        self._bridge_thread.start()
        return True

    def stop(self) -> None:
        if not self._started:
            return
        self._started = False

        # 1) 桥接线程：让它自己从常驻循环里退出来（它会关串口）
        try:
            from . import link
            link.set_speaker(None)
            link.stop()
        except Exception:  # noqa: BLE001 —— 停不下来也不能挡住退出
            pass
        if self._bridge_thread is not None:
            self._bridge_thread.join(timeout=3.0)

        # 2) 后端子进程
        self._stop_backend()

    # -------------------------------------------------------------- 后端子进程
    def _port_in_use(self) -> bool:
        host = getattr(self.cfg, "app_backend_host", "127.0.0.1")
        port = int(getattr(self.cfg, "app_backend_port", 8000))
        try:
            with socket.create_connection((host, port), timeout=0.3):
                return True
        except OSError:
            return False

    def _start_backend(self) -> None:
        if self._port_in_use():
            self.log(f"[app] 后端已在运行（{self.api_base}），本次不再启动")
            return

        if not self._backend_deps_ok():
            self.log("[app] 后端依赖缺失（fastapi / uvicorn），跳过后端启动。"
                     "手机小程序与提醒卡片将不可用；"
                     "需要时执行：pip install -r app/backend/requirements.txt")
            return

        host = getattr(self.cfg, "app_backend_host", "127.0.0.1")
        port = str(int(getattr(self.cfg, "app_backend_port", 8000)))
        cmd = [sys.executable, "-m", "uvicorn", "main:app",
               "--host", host, "--port", port, "--log-level", "warning"]
        kwargs = {"cwd": BACKEND_DIR, "stdout": subprocess.DEVNULL,
                  "stderr": subprocess.STDOUT}
        if os.name == "nt":
            # 不弹黑窗口：后端日志走 link 的 stdout 会太吵，这里只报出问题
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            self._backend = subprocess.Popen(cmd, **kwargs)
        except Exception as exc:  # noqa: BLE001
            self.log(f"[app] 后端启动失败：{exc!r}")
            self._backend = None
            return

        self.log(f"[app] 后端已启动（{self.api_base}，pid={self._backend.pid}）")
        self._backend_thread = threading.Thread(
            target=self._watch_backend, name="app-backend", daemon=True)
        self._backend_thread.start()

    def _watch_backend(self) -> None:
        """盯着后端子进程：起来了就说一声，挂了就报出来（不重启，避免死循环）。"""
        proc = self._backend
        if proc is None:
            return
        deadline = time.time() + 15.0
        while time.time() < deadline and self._started:
            if proc.poll() is not None:
                self.log(f"[app] 后端进程提前退出（code={proc.returncode}），"
                         f"手机端会连不上。手动排查：cd app/backend && python main.py")
                return
            if self._http_ok("/api/v1/settings"):
                self.log("[app] 后端就绪：手机小程序 / 提醒卡片可用")
                return
            time.sleep(0.5)
        if self._started:
            self.log("[app] 后端 15 秒内没就绪（端口被占 / 依赖缺失？），"
                     "屏幕桥接会自动重试拉数据")

    def _stop_backend(self) -> None:
        proc, self._backend = self._backend, None
        if proc is None or proc.poll() is not None:
            return
        try:
            proc.terminate()
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.kill()
        except Exception:  # noqa: BLE001
            pass

    # ------------------------------------------------------------------ 桥接线程
    def _bridge_main(self) -> None:
        try:
            from . import link
        except Exception as exc:  # noqa: BLE001 —— 缺 pyserial / Pillow 之类
            self.log(f"[app] 屏幕桥接不可用（{exc}）；习惯/日程卡片不会下发")
            return

        link.set_speaker(self.speaker)

        args = SimpleNamespace(
            port=(getattr(self.cfg, "app_board_port", "") or None),
            baud=int(getattr(self.cfg, "app_board_baud", 115200)),
            api=self.api_base,
            interval=float(getattr(self.cfg, "app_sync_interval_s", 5.0)),
            resync=float(getattr(self.cfg, "app_resync_s", 300.0)),
            dry=bool(getattr(self.cfg, "app_board_dry", False)),
            once=False,
            show_log=bool(getattr(self.cfg, "app_board_log", False)),
        )
        try:
            link.run(args)
        except (RuntimeError, ImportError) as exc:
            # 串口没找到/被占用、依赖缺失：只影响屏幕卡片，主链路照常
            self.log(f"[app] 屏幕桥接未启动：{exc}")
        except Exception as exc:  # noqa: BLE001
            self.log(f"[app] 屏幕桥接异常退出：{exc!r}")
        finally:
            link.set_speaker(None)

    # ------------------------------------------------------------------ 小工具
    def _http_ok(self, path: str) -> bool:
        try:
            with urllib.request.urlopen(self.api_base + path, timeout=1.0) as r:
                return 200 <= getattr(r, "status", 200) < 300
        except Exception:  # noqa: BLE001
            return False

    @staticmethod
    def _backend_deps_ok() -> bool:
        try:
            import fastapi  # noqa: F401
            import uvicorn  # noqa: F401
            return True
        except Exception:  # noqa: BLE001
            return False
