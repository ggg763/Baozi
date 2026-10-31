"""TTS 后端：把回复文本合成成 16k / 单声道 / int16 的 PCM。

  * ``edge-tts`` —— 微软在线语音，中文音色自然，免费（需联网）。
                    输出是 MP3，用 miniaudio 解码后重采样到 16k。
  * ``sapi``     —— Windows 自带离线语音（System.Speech）。断网时兜底，
                    音质一般但永远可用。
  * ``tone``     —— 用正弦音代替语音，用来测链路。
  * ``none``     —— 静音，不做 TTS。

★ 网络维护（2026-10-31 重写 EdgeTts）：
  原来的实现在国内"合成失败率很高"，实测原因有四条，逐条对着改：

  1. 超时写死 6s，但合成耗时随字数线性涨（实测 t ≈ 1.0 + 0.019×字数）——
     300 字就要 5.2s、450 字 13.9s，长回复必然超时被吞。
     → 改成按字数算预算，且**整句话的总预算有硬上限**（不是每次尝试都重算）。
  2. 每次合成都 asyncio.run() 新建事件循环，超时后那个后台线程还挂在 socket 上
     永远不退出（旧实现在反复超时会越积越多）。
     → 改成**常驻 event loop**，超时直接 cancel 协程，连接当场释放。
     （没有做连接池复用：edge_tts 内部每个 ClientSession 都会把传进去的
      connector 一起关掉，硬要复用得 monkeypatch 它的私有实现，不值当。
      实测建连成本约 0.4~0.7s，现在的超时预算留足了这部分余量。）
  3. 零重试，一次丢包整句报废（好网也有 ~5% 超时底噪）。
     → 快失败立刻重试；卡死且没有备用线路就直接放弃，不空耗。
  4. 一次失败就永久降级到 SAPI（音色突兀、机器味重），再也不会自己恢复。
     → 失败只记一笔，下一次照常先用 edge；配了备用线路的话再加后台探测，
       通了自动切回来。

  ⚠️ 关于代理：**默认不挂代理，只走直连**。实测 edge-tts 国内直连是通的
  （直连 19/20，唯一那次失败是本地 6s 超时掐的，不是网络断）。最初以为是
  "被墙要挂 Clash" 是误判 —— 那台机器挂 Clash 是为了 GitHub，跟语音无关；
  把语音塞进代理只会多一条会挂的链路。要强制走代理就显式配 tts_proxy。
"""

from __future__ import annotations

import asyncio
import concurrent.futures
import re
import subprocess
import tempfile
import threading
import time

from pathlib import Path

import numpy as np

from audio import SAMPLE_RATE, decode_audio_file, pcm_from_bytes, resample
# 兜底话术的文本和预生成音频都由 boot_voice.py 管（那边不依赖 tts，无环）
from boot_voice import FALLBACK_PHRASES, load_fallback_pool

# 朗读前把 Markdown / 表情之类的“念不出来”的东西清掉
_MD_PATTERNS = [
    (re.compile(r"```.*?```", re.S), ""),
    (re.compile(r"`([^`]*)`"), r"\1"),
    (re.compile(r"\*\*([^*]*)\*\*"), r"\1"),
    (re.compile(r"\*([^*]*)\*"), r"\1"),
    (re.compile(r"^#{1,6}\s*", re.M), ""),
    (re.compile(r"^\s*[-*+]\s+", re.M), ""),
    (re.compile(r"^\s*\d+\.\s+", re.M), ""),
    (re.compile(r"\[([^\]]*)\]\([^)]*\)"), r"\1"),
]
_EMOJI = re.compile(
    "[\U0001F300-\U0001FAFF\U00002600-\U000027BF\U0001F000-\U0001F0FF]"
)


def clean_for_speech(text: str) -> str:
    for pat, repl in _MD_PATTERNS:
        text = pat.sub(repl, text)
    text = _EMOJI.sub("", text)
    return re.sub(r"\s+", " ", text).strip()


class Tts:
    name = "base"

    def synthesize(self, text: str) -> np.ndarray:
        """返回 16k/单声道/int16 的 PCM。"""
        raise NotImplementedError


# ---------------------------------------------------------------------
# 网络线路：默认**只走直连**。
#
# 为什么不默认挂代理（2026-10-31 修正）：
#   一开始以为"连不上微软"要挂 Clash，实测是误判 —— edge-tts 在国内
#   直连是通的（直连 19/20，失败那次是本地 6s 超时掐的，不是网络断）。
#   用户挂 Clash 是为了 GitHub，跟语音不是一回事；把语音塞进代理反而
#   多一跳、多一条会挂的链路，还会被规则随手改掉。
#   所以默认只有「直连」一条线路。真要强制走代理，显式配置 tts_proxy；
#   想留个备用线路，显式配置 tts_alt_proxy（例如 Clash 的 7897）。
# ---------------------------------------------------------------------
class _Route:
    """一条到微软语音服务器的线路。"""

    __slots__ = ("label", "proxy")

    def __init__(self, label: str, proxy: "str | None") -> None:
        self.label = label
        self.proxy = proxy

    def __repr__(self) -> str:      # 只用于日志
        return self.label


class EdgeTts(Tts):
    """微软在线语音（edge-tts），带线路维护和自动恢复。

    线程模型：
        所有网络 IO 跑在一个**常驻**的后台 event loop 上（`_loop`）。
        调用方线程通过 `run_coroutine_threadsafe(...).result(timeout)` 拿结果，
        超时就 cancel —— 既不会无限等（aiohttp 的 sock 超时管不住 DNS/TLS 握手），
        也不会像原来那样留一个永远不退出的 daemon 线程占着 socket。
    """

    name = "edge-tts"

    def __init__(self, voice: str = "zh-CN-XiaoxiaoNeural",
                 rate: str = "+0%", volume: str = "+0%",
                 timeout_s: float = 0.0, retry: int = 2,
                 proxy: str = "", alt_proxy: str = "") -> None:
        try:
            import edge_tts
        except ImportError as exc:
            raise RuntimeError("edge-tts 未安装：pip install edge-tts") from exc

        self._mod = edge_tts
        self._voice = voice
        self._rate = rate
        self._volume = volume
        # timeout_s <= 0 表示按字数自动算（推荐）；显式配置则原样尊重
        self._timeout_s = float(timeout_s or 0.0)
        self._retry = max(0, int(retry))

        self._routes = self._build_routes(proxy, alt_proxy)
        self._route_i = 0
        self._fail_streak = 0

        self._lock = threading.RLock()
        self._loop: "asyncio.AbstractEventLoop | None" = None
        self._stop = threading.Event()
        self._wd: "threading.Thread | None" = None

        print(f"[tts] edge-tts 音色={voice} 语速={rate} "
              f"超时={'按字数' if not self._timeout_s else f'{self._timeout_s:.0f}s'} "
              f"线路={self._routes[0].label}")
        if len(self._routes) > 1:
            print(f"[tts] 备用线路={self._routes[1].label}（主线路连续失败时自动切换）")

    # ------------------------------------------------------------------
    # 线路
    # ------------------------------------------------------------------
    def _build_routes(self, proxy: str, alt_proxy: str) -> "list[_Route]":
        """决定有哪些线路可走。

        默认（两个都空）只有「直连」一条 —— 这是实测出来的正确配置，
        edge-tts 国内直连是通的，套代理只会多一条会挂的链路。

        显式给了 proxy：就走它（直连留作备用线路，代理挂了还能自己爬回来）。
        显式给了 alt_proxy：直连优先，失败后切到它。
        """
        if proxy:
            routes = [_Route(f"代理 {proxy}", proxy)]
            if alt_proxy and alt_proxy != proxy:
                routes.append(_Route("直连", None))
            return routes
        routes = [_Route("直连", None)]
        if alt_proxy:
            routes.append(_Route(f"代理 {alt_proxy}", alt_proxy))
        return routes

    @property
    def _current(self) -> _Route:
        return self._routes[self._route_i]

    def _alt_index(self) -> int:
        """备用线路下标；只有一条线路时返回 -1。"""
        if len(self._routes) < 2:
            return -1
        return (self._route_i + 1) % len(self._routes)

    def _attempt_plan(self) -> "list[int]":
        """这一次按什么顺序试哪些线路。

        **两条线路交替**，不是"同一条重试三次再说"：
        实测首选线路指向一个没人监听的端口时，三次重试全打在死路上，
        备用线路一次都没试到，整句白等 6 秒直接失败 —— 交替就没这问题。

        retry=2 且有两条线路 -> [当前, 备用, 当前]
        """
        cur = self._route_i
        alt = self._alt_index()
        if alt < 0:
            return [cur] * (1 + self._retry)
        return [cur if k % 2 == 0 else alt for k in range(1 + self._retry)]

    def _adopt(self, idx: int, why: str = "") -> None:
        """切换并记住能用的那条线路。"""
        with self._lock:
            if idx == self._route_i or idx >= len(self._routes):
                return
            old = self._current.label
            self._route_i = idx
            self._fail_streak = 0
            print(f"[tts] 线路切换：{old} -> {self._current.label}"
                  f"{('（' + why + '）') if why else ''}", flush=True)

    # ------------------------------------------------------------------
    # 常驻 event loop
    # ------------------------------------------------------------------
    def _ensure_loop(self) -> asyncio.AbstractEventLoop:
        with self._lock:
            if self._loop is not None and self._loop.is_running():
                return self._loop
            loop = asyncio.new_event_loop()
            t = threading.Thread(target=loop.run_forever, name="tts-net", daemon=True)
            t.start()
            self._loop = loop
            return loop

    async def _synth_once(self, text: str, route: _Route, timeout: float) -> bytes:
        communicate = self._mod.Communicate(
            text, self._voice,
            rate=self._rate, volume=self._volume,
            proxy=route.proxy,
            connect_timeout=max(3, int(timeout)),
            receive_timeout=max(5, int(timeout * 2)),
        )
        chunks = bytearray()
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                chunks.extend(chunk["data"])
        return bytes(chunks)

    def _call(self, text: str, route: _Route, timeout: float) -> bytes:
        """在常驻 loop 上跑一次合成，超时就取消（调用方线程最多等 timeout 秒）。"""
        loop = self._ensure_loop()
        fut = asyncio.run_coroutine_threadsafe(
            self._synth_once(text, route, timeout), loop)
        # 取一次异常，否则取消/失败时 asyncio 会打一堆
        # "Task exception was never retrieved" 的噪声到控制台
        fut.add_done_callback(lambda f: f.cancelled() or f.exception())
        try:
            return fut.result(timeout)
        except concurrent.futures.TimeoutError:
            fut.cancel()
            raise TimeoutError(f"{timeout:.1f}s 内没合成完") from None

    # ------------------------------------------------------------------
    # 超时预算
    # ------------------------------------------------------------------
    def _base_timeout(self, text: str) -> float:
        """按字数算单次预算。

        实测（直连，单句）：t ≈ 1.0 + 0.019×字数 秒。
        给到 4 + 0.02×字数，50 字约 5s、100 字约 6s，留一倍余量。
        """
        if self._timeout_s > 0:
            return self._timeout_s
        return min(20.0, max(4.0, 4.0 + 0.02 * len(text)))

    # ------------------------------------------------------------------
    # 对外
    # ------------------------------------------------------------------
    def synthesize(self, text: str) -> np.ndarray:
        text = clean_for_speech(text)
        if not text:
            return np.zeros(0, dtype=np.int16)

        base = self._base_timeout(text)
        # 整句话的总预算，含所有重试。原来最坏情况会卡 3 倍预算，
        # 这里硬封顶 1.6 倍，避免"出一句话先等 15 秒"。
        deadline = time.monotonic() + base * 1.6
        start = time.monotonic()
        plan = self._attempt_plan()

        last_err = "未知原因"
        i = 0
        while i < len(plan):
            remain = deadline - time.monotonic()
            if remain <= 1.0:            # 剩这点时间不可能合成完，别硬试
                last_err += "（预算耗尽）"
                break
            # 前两次已经花掉大半预算还没通 -> 别硬堆第三次。
            # 实测两条线路全死时，每次尝试要 ~2s，"当前/备用/当前"
            # 三次就是 6s 白等；早点把空结果交回去，用户不用干熬。
            if i >= 2 and (time.monotonic() - start) > base * 0.8:
                last_err += "（前两次已耗尽大半预算，不再重试）"
                break
            idx = plan[i]
            route = self._routes[idx]
            budget = min(base, remain)
            t0 = time.monotonic()
            try:
                data = self._call(text, route, budget)
            except Exception as exc:     # noqa: BLE001
                took = time.monotonic() - t0
                last_err = f"{type(exc).__name__}: {exc}"
                print(f"[tts] {route.label} 失败（{took:.1f}s）：{last_err}", flush=True)
                # 只有一条线路时，超时之后重试同一条路毫无意义（大概率再超一次），
                # 直接放弃把空结果交回去，别让用户干等第二轮预算。
                if took > base * 0.8 and self._alt_index() < 0:
                    last_err += "（超时且无备用线路，不再重试）"
                    break
                i += 1
                continue

            if not data:
                last_err = "返回空音频"
                print(f"[tts] {route.label} 返回空音频（{len(text)} 字）", flush=True)
                i += 1
                continue

            if idx != self._route_i:
                self._adopt(idx, "合成成功")
            self._fail_streak = 0
            return self._decode(data)

        # ---- 全都没成：记一笔，让后台探测线程接手 ----
        with self._lock:
            self._fail_streak += 1
            streak = self._fail_streak
        print(f"[tts] 本句合成失败（连续 {streak} 次）：{last_err}", flush=True)
        self._ensure_watchdog()
        return np.zeros(0, dtype=np.int16)

    def _decode(self, mp3: bytes) -> np.ndarray:
        """edge-tts 只吐 MP3，落盘后交给 miniaudio 解码。"""
        with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as f:
            f.write(mp3)
            tmp = Path(f.name)
        try:
            return decode_audio_file(tmp)
        finally:
            tmp.unlink(missing_ok=True)

    # ------------------------------------------------------------------
    # 后台探测：失败了也要能自己爬回来，不许一直"脱机"
    # ------------------------------------------------------------------
    _WD_INTERVAL_S = 15.0

    def _ensure_watchdog(self) -> None:
        if self._wd is not None and self._wd.is_alive():
            return
        if len(self._routes) < 2:
            return
        with self._lock:
            if self._wd is not None and self._wd.is_alive():
                return
            self._wd = threading.Thread(target=self._watchdog,
                                        name="tts-watchdog", daemon=True)
            self._wd.start()

    def _watchdog(self) -> None:
        """连续失败后，每隔一会儿拿一句话去探备用线路，通了就切回来。

        为什么用真合成而不是 ping：能连上不代表能合成（DRM 403、音色下线
        都会让连接成功但没音频），只有真拿到音频才算"这条线路活着"。
        """
        while not self._stop.is_set():
            time.sleep(self._WD_INTERVAL_S)
            if self._stop.is_set():
                return
            # 只失败一次不算"脱机"（网络抖一下而已），连续两次才动手 —— 
            # 否则偶尔一次超时就把线路切走，来回跳比不切更不稳。
            if self._fail_streak < 2:
                return
            alt = self._alt_index()
            if alt < 0:
                return
            route = self._routes[alt]
            try:
                data = self._call("嗯", route, 5.0)
            except Exception:               # noqa: BLE001
                continue
            if data:
                self._adopt(alt, "后台探测成功")
                print("[tts] 网络已恢复，后台探测线程退出", flush=True)
                return

    def close(self) -> None:
        self._stop.set()
        loop = self._loop
        if loop is not None and loop.is_running():
            loop.call_soon_threadsafe(loop.stop)


# ---------------------------------------------------------------------
class SapiTts(Tts):
    """Windows 自带离线语音（System.Speech / SAPI）。

    为什么需要（2026-10 实测的真实故障）：
        edge-tts 走 wss://speech.platform.bing.com（微软海外服务器），
        国内网络经常 ConnectionTimeout，而且 asyncio 会无限等待 ——
        表现就是"说完话卡十几秒、包子不出声"。
        本后端【完全离线】，不依赖任何网络，作为兜底保证"至少能说话"。

    必需：Windows 中文语音包。本机已有 "Microsoft Huihui Desktop"。
    音质不如 edge-tts（机器感明显），但胜在永远可用。
    """

    name = "sapi"

    def __init__(self, voice: str = "", rate: int = 0) -> None:
        self._voice = voice          # 空 = 自动挑第一个中文语音
        self._rate = rate            # SAPI 语速 -10~10，0 为正常

    def synthesize(self, text: str) -> np.ndarray:
        text = clean_for_speech(text)
        if not text:
            return np.zeros(0, dtype=np.int16)

        # 走 PowerShell 调 System.Speech：不需要额外 pip 包
        base = Path(tempfile.gettempdir()) / f"sapi_{int(time.time() * 1000)}"
        ps = base.with_suffix(".ps1")
        wav = base.with_suffix(".wav")

        voice_sel = (
            f"$s.SelectVoice('{self._voice}')" if self._voice else
            "$zh = $s.GetInstalledVoices() | "
            "Where-Object { $_.VoiceInfo.Culture.Name -like 'zh*' } | "
            "Select-Object -First 1;"
            "if ($zh) { $s.SelectVoice($zh.VoiceInfo.Name) }"
        )
        script = (
            "Add-Type -AssemblyName System.Speech\n"
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer\n"
            f"{voice_sel}\n"
            f"$s.Rate = {int(self._rate)}\n"
            f"$s.SetOutputToWaveFile('{wav}')\n"
            # 文本从 stdin 读，避免引号/换行把脚本写坏
            "$s.Speak([Console]::In.ReadToEnd())\n"
            "$s.Dispose()\n"
        )

        try:
            ps.write_text(script, encoding="utf-8-sig")
            subprocess.run(
                ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                 "-File", str(ps)],
                input=text, text=True, encoding="utf-8",
                capture_output=True, timeout=30, check=False,
            )
            if not wav.exists() or wav.stat().st_size < 100:
                print("[tts] SAPI 未产生音频（检查中文语音包是否已安装）")
                return np.zeros(0, dtype=np.int16)
            return decode_audio_file(wav)
        except Exception as exc:  # noqa: BLE001
            print(f"[tts] SAPI 合成失败：{exc!r}")
            return np.zeros(0, dtype=np.int16)
        finally:
            ps.unlink(missing_ok=True)
            wav.unlink(missing_ok=True)


# ---------------------------------------------------------------------
class FallbackTts(Tts):
    """主后端失败时自动降级到备用后端。

    用于「edge-tts（在线，音质好）→ sapi（离线，音质一般）」这种组合：
    网络正常时用好的，网络挂了也不会哑掉。
    """

    name = "fallback"

    def __init__(self, primary: Tts, backup: Tts) -> None:
        self._primary = primary
        self._backup = backup
        self._primary_fail = 0
        print(f"[tts] 主={primary.name}  备用={backup.name}")

    def synthesize(self, text: str) -> np.ndarray:
        pcm = self._primary.synthesize(text)
        if pcm.size > 0:
            self._primary_fail = 0
            return pcm

        self._primary_fail += 1
        if self._primary_fail == 1:
            print(f"[tts] {self._primary.name} 失败，降级到 {self._backup.name}")
        return self._backup.synthesize(text)


# ---------------------------------------------------------------------
class CachedFallbackTts(Tts):
    """合成失败时，播放一段【本机预生成好】的话术，而不是哑掉或播乱码。

    背景（2026-10 实测）：
        主后端 edge-tts 连不上微软服务器会降级到 sapi；sapi 也可能因为
        缺中文语音包 / 临时故障返回空 PCM。此时如果静默返回，用户听到的
        就是"上一句的残响"或干脆没声音 —— 表现为包子突然哑了。
        所以备几句固定的话，主合成失败时改播它，保证"至少有个像样的回应"。

    ★ 为什么是读盘，不是每次开机现场合成（2026-10-31 改）：
        最早是开机时用 TTS 合成这 3 句再存内存。问题有三个：
          · 网络没起来就全灭，池子空着，真出故障时反而没话可说；
          · 每句都是真实网络调用，白白让启动慢几秒；
          · 音色取决于"启动那一刻哪条链路通"，可能混进 SAPI 的机器音。
        现在这 3 句是 `assets/fallback/phrase_*.wav`，由 boot_voice.py
        一次性生成、跟代码走。运行时纯本地读盘：0 网络、0 等待、音色恒定，
        断网也照样能播。文件缺了才退回现场合成。

    注意：
        · 它本身不接主/备后端，而是被 FallbackTts 之外再包一层，
          或者直接替换 FallbackTts（见 make_tts）。
    """

    name = "cached-fallback"

    # 话术文本的唯一来源在 boot_voice.py（那边改了要重跑 --fallback）
    DEFAULT_PHRASES = list(FALLBACK_PHRASES)

    def __init__(self, synth: Tts, phrases: "Optional[list]" = None,
                 pool_synth: "Tts | None" = None,
                 pool_voice: str = "") -> None:
        self._synth = synth
        # 话术池用哪个后端现场补合成（只在磁盘文件缺失时才会用到）。
        # ★ 和 synth 分开是有意的：synth 是"所有都失败时"的最后一条路（含 SAPI），
        #   而话术池必须**只用 edge**。否则池子里会混进 SAPI 的惠惠音色
        #   （机器人味、慢半拍、还小 4.7dB）—— 比哑掉还怪。
        self._pool_synth = pool_synth or synth
        self._phrases = list(phrases or self.DEFAULT_PHRASES)
        self._rng = __import__("random")

        # ① 先从本机读（毫秒级，无网络）
        self._cache = load_fallback_pool(self._phrases, voice=pool_voice)
        n_ok = sum(1 for c in self._cache if c is not None)

        # ② 有缺的才现场补 —— 正常情况下这一步永远不会发生
        if n_ok < len(self._cache):
            print(f"[tts] 兜底话术本机只有 {n_ok}/{len(self._cache)} 句，"
                  f"后台补合成（建议跑 python boot_voice.py --fallback）")
            threading.Thread(target=self._prewarm_later, name="tts-prewarm",
                             daemon=True).start()
        else:
            ms = sum(len(c) for c in self._cache) * 1000 / SAMPLE_RATE
            print(f"[tts] 兜底话术本机就绪 {n_ok}/{len(self._cache)} 句"
                  f"（共 {ms / 1000:.1f}s，不联网）")

    def _prewarm_later(self) -> None:
        time.sleep(2.0)          # 先让启动流程走完，别抢连接
        self.prewarm()

    def prewarm(self) -> int:
        """把还没备好的兜底话术补上，返回本次补成几句。

        只在磁盘文件缺失/损坏时才会走到这里。补完不会写回磁盘 ——
        想让它进版本库就手动跑 `python boot_voice.py --fallback`。
        """
        filled = 0
        for i, p in enumerate(self._phrases):
            if self._cache[i] is not None:
                continue
            try:
                pcm = self._pool_synth.synthesize(p)
            except Exception as exc:  # noqa: BLE001
                print(f"[tts] 补合成兜底话术失败（{p[:8]}…）：{exc!r}")
                pcm = None
            if pcm is not None and pcm.size > 0:
                self._cache[i] = pcm
                filled += 1
        n_ok = sum(1 for c in self._cache if c is not None)
        if filled:
            print(f"[tts] 兜底话术 {n_ok}/{len(self._cache)} 句就绪（临时补的，没写盘）")
        return filled

    def synthesize(self, text: str) -> np.ndarray:
        return self._synth.synthesize(text)

    def fallback_pcm(self) -> np.ndarray:
        """取一段本机预生成的兜底话术 PCM；一句都没备成才现场合成一次。"""
        ready = [c for c in self._cache if c is not None]
        if ready:
            return self._rng.choice(ready)
        # 兜底话术也没备成（比如 sapi 也坏）：现场再试一句，仍失败就空
        for p in self._phrases:
            try:
                pcm = self._synth.synthesize(p)
                if pcm.size > 0:
                    return pcm
            except Exception:  # noqa: BLE001
                continue
        return np.zeros(0, dtype=np.int16)

    def close(self) -> None:
        for t in (self._synth, self._pool_synth):
            closer = getattr(t, "close", None)
            if callable(closer):
                try:
                    closer()
                except Exception:  # noqa: BLE001
                    pass


# ---------------------------------------------------------------------
class ToneTts(Tts):
    """不合成语音，只按文本长度生成一段正弦音。用于纯链路测试。"""

    name = "tone"

    def __init__(self, freq: float = 440.0, ms_per_char: float = 60.0) -> None:
        self._freq = freq
        self._ms_per_char = ms_per_char

    def synthesize(self, text: str) -> np.ndarray:
        n = int(SAMPLE_RATE * max(1, len(text)) * self._ms_per_char / 1000.0)
        t = np.arange(n, dtype=np.float64) / SAMPLE_RATE
        # 加淡入淡出，避免开关声
        wave = np.sin(2 * np.pi * self._freq * t)
        fade = min(400, n // 4)
        if fade > 0:
            wave[:fade] *= np.linspace(0, 1, fade)
            wave[-fade:] *= np.linspace(1, 0, fade)
        return (wave * 8000).astype(np.int16)


class NoneTts(Tts):
    name = "none"

    def synthesize(self, text: str) -> np.ndarray:
        return np.zeros(0, dtype=np.int16)


class WavTts(Tts):
    """生成一段静音，随后可被外部文件覆盖；主要给离线自测用。"""

    name = "silence"

    def __init__(self, ms: int = 200) -> None:
        self._ms = ms

    def synthesize(self, text: str) -> np.ndarray:
        return np.zeros(SAMPLE_RATE * self._ms // 1000, dtype=np.int16)


# ---------------------------------------------------------------------
def _edge_from_cfg(cfg) -> EdgeTts:
    return EdgeTts(
        voice=cfg.tts_voice,
        rate=cfg.tts_rate,
        volume=cfg.tts_volume,
        timeout_s=getattr(cfg, "tts_timeout_s", 0.0),
        retry=getattr(cfg, "tts_retry", 2),
        proxy=getattr(cfg, "tts_proxy", "") or "",
        alt_proxy=getattr(cfg, "tts_alt_proxy", "") or "",
    )


def make_tts(cfg) -> Tts:
    backend = (cfg.tts_backend or "edge-tts").lower()

    if backend == "edge-tts":
        primary = _edge_from_cfg(cfg)
        # ★ 自动兜底：edge-tts 连不上微软服务器时（国内很常见），
        #   降级到 Windows 自带离线语音，保证包子不会哑掉。
        #   写 "edge-tts" 就自动获得这个行为，不用改配置。
        try:
            backup = SapiTts()
            fb = FallbackTts(primary, backup)
            # ★ 再包一层缓存兜底：万一 edge-tts 和 sapi 都失败，
            #   改播预先合成好的"抱歉我没听清"话术，而不是静默/乱码。
            #   话术音频在 assets/fallback/，本机读盘不走网络；
            #   pool_synth 只在文件缺失时才用来现场补，
            #   ⚠️ 且只准用 edge —— SAPI 的惠惠音色机器人味太重，比哑掉还怪。
            return CachedFallbackTts(fb, pool_synth=primary,
                                     pool_voice=cfg.tts_voice)
        except Exception as exc:  # noqa: BLE001
            print(f"[tts] 离线兜底不可用（{exc}），只用 edge-tts")
            return CachedFallbackTts(primary)
    if backend == "cached-fallback":
        # 显式启用：主+备都失败时播预生成话术（与 edge-tts 行为一致，
        # 但便于测试 / 单独指定）
        primary = _edge_from_cfg(cfg)
        return CachedFallbackTts(FallbackTts(primary, SapiTts()),
                                 pool_synth=primary,
                                 pool_voice=cfg.tts_voice)
    if backend == "sapi":
        return SapiTts(voice=getattr(cfg, "tts_voice", "") or "")
    if backend == "tone":
        return ToneTts()
    if backend == "none":
        return NoneTts()
    if backend == "silence":
        return WavTts()

    raise ValueError(f"未知的 tts_backend: {cfg.tts_backend}")


__all__ = [
    "Tts",
    "EdgeTts",
    "SapiTts",
    "FallbackTts",
    "CachedFallbackTts",
    "ToneTts",
    "NoneTts",
    "WavTts",
    "make_tts",
    "clean_for_speech",
    "pcm_from_bytes",
    "resample",
    "SAMPLE_RATE",
]
