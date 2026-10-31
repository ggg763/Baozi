"""一键演示的 PC 侧：板子请求播报 -> 放本地预合成 WAV -> 通知板子"可以演了"。

板子（TD1601）没有喇叭，所以演示每一步的解说只能由 PC 代播。整条链路：

    TD1601 ──printf "DEMO:HAPPY:1"──► 本模块
                                        │ ① 读 assets/demo/HAPPY.wav（纯本地）
                                        │ ② 回一行 DEMOWAIT:6800（告诉板子要等多久）
                                        │ ③ PCM ──► pipeline ──► ESP32 ──► 喇叭
                                        │ ④ 放完回一行 DEMORUN
    TD1601 ◄────────────────────────────┘
                                        └─ 收到 DEMORUN 才开始演（表情 + 舵机）

★ 两条设计底线：
  1. **板子永远不能卡在等 PC 上**。音频缺失、主链路忙、ESP32 没连 —— 任何一种
     情况都立刻回 DEMORUN（或什么都不做，让板子的 2.5s 兜底窗口自己到点）。
     宁可没解说，也不能让演示停住。
  2. **不现场合成**。TTS 要联网、要等几秒，当众演示时一次抖动就是"卡在那儿
     不出声"。所有播报都是 demo_voice.py 预生成的 WAV，这里只读盘。

用法（main.py 里由 AppService 接好，一般不用手动调）：

    bridge = DemoAnnouncer(say_pcm=pipe.say_pcm, send_line=link.write_line)
    link.set_demo(bridge)
"""

from __future__ import annotations

import threading
import time

from typing import Callable, Optional

import numpy as np

import demo_voice

from audio import SAMPLE_RATE

# 播放收尾的富余：推流是按 ESP32 的水位限速的，最后一帧从 PC 发出到喇叭出声
# 还有一点缓冲，只等 len(pcm) 会在话还没说完时就让板子开演。
TAIL_SEC = 1.0
# DEMOWAIT 里额外加的富余（在 TAIL 之上再留一点，防 PC 侧调度抖动）
WAIT_EXTRA_SEC = 0.4


def parse_demo_line(line: str) -> "tuple[str, int] | None":
    """解析板子上来的 `DEMO:<NAME>:<IDX>`。不是演示行就返回 None。

    >>> parse_demo_line("DEMO:HAPPY:1")
    ('HAPPY', 1)
    >>> parse_demo_line("DEMO:SLEEP")
    ('SLEEP', -1)
    >>> parse_demo_line("[demo] go 1/10") is None
    True
    """
    s = (line or "").strip()
    if not s.upper().startswith("DEMO:"):
        return None
    parts = s.split(":")
    name = parts[1].strip().upper() if len(parts) > 1 else ""
    idx = -1
    if len(parts) > 2:
        try:
            idx = int(parts[2].strip())
        except ValueError:
            idx = -1
    return (name, idx)


class DemoAnnouncer:
    """把板子的播报请求翻译成"放一段本地音频 + 回一行 DEMORUN"。"""

    def __init__(self, say_pcm: Callable[[np.ndarray, str], bool],
                 send_line: Callable[[str], bool],
                 log: Callable[[str], None] = print) -> None:
        self._say_pcm = say_pcm
        self._send_line = send_line
        self._log = log
        self._lock = threading.Lock()     # 两条播报不重叠（正常不会发生）
        self._announced: "set[str]" = set()   # 只提示一次"缺音频"

    # ---------------------------------------------------------------- 入口
    def on_board_line(self, name: str, idx: int = -1) -> None:
        """串口线程里调：只负责起后台线程，绝不能把串口轮询堵住。"""
        name = (name or "").strip().upper()
        if not name:
            return
        threading.Thread(target=self._run, args=(name, idx),
                         name="demo-say", daemon=True).start()

    # ---------------------------------------------------------------- 内部
    def _send(self, text: str) -> None:
        try:
            self._send_line(text)
        except Exception as exc:  # noqa: BLE001
            self._log("[demo] 下行发送失败（%s）：%r" % (text, exc))

    def _run(self, name: str, idx: int) -> None:
        with self._lock:
            self._announce(name, idx)

    def _announce(self, name: str, idx: int) -> None:
        where = "" if idx < 0 else " 第 %d 步" % (idx + 1)
        pcm = demo_voice.load(name)

        if pcm is None:
            # 缺音频：只提示一次，别每一步都刷一行。
            if name not in self._announced:
                self._announced.add(name)
                known = demo_voice.known(name)
                if known:
                    self._log("[demo] 缺 %s 的播报音频（跑 python demo_voice.py 生成），"
                              "本次只演示不出声" % name)
                else:
                    self._log("[demo] 板子报了没见过的演示项 %r（固件与 demo_voice.py "
                              "的 DEMO_SCRIPTS 对不上？），只演示不出声" % name)
            self._send("DEMORUN")
            return

        dur = len(pcm) / float(SAMPLE_RATE)

        # 先把等待窗口放宽：板子默认只等 2.5s，而一句播报动辄 5 秒。
        # 这一步必须在开始播放【之前】做，否则板子会在我们开口前就开演。
        self._send("DEMOWAIT:%d" % int((dur + TAIL_SEC + WAIT_EXTRA_SEC) * 1000))

        ok = False
        try:
            ok = bool(self._say_pcm(pcm, "演示播报 %s" % name))
        except Exception as exc:  # noqa: BLE001
            self._log("[demo] 播报 %s 失败：%r" % (name, exc))

        if not ok:
            # 主链路忙（正在对话）/ 没接 ESP32：立刻放行，别让板子干等
            self._log("[demo] 播报 %s 没送出去（链路忙或没连 ESP32），直接开演" % name)
            self._send("DEMORUN")
            return

        self._log("[demo] 播报%s %s（%.1fs）" % (where, name, dur))
        time.sleep(dur + TAIL_SEC)
        self._send("DEMORUN")
