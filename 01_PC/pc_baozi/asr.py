"""语音识别（ASR）后端。

  * ``sensevoice``     —— 推荐（中文效果最好）。阿里 FunASR 的
                          SenseVoiceSmall，本地推理，从 ModelScope（国内）下载；
  * ``faster-whisper`` —— whisper 的 CTranslate2 实现，CPU 也够快（int8）；
  * ``openai-whisper`` —— 原版 whisper 包；
  * ``mock``           —— 不做识别，直接返回占位文本，用来测整条链路。

所有后端都接受 16k / 单声道 / int16 的 numpy 数组，内部按需转换。

关于模型下载（国内网络友好）：
  * SenseVoice 走 ModelScope（modelscope.cn，阿里国内站），不去 HuggingFace；
  * faster-whisper 走 HuggingFace，官方源连不上时自动切 hf-mirror.com 镜像。
"""

from __future__ import annotations

import os
import time
from typing import Optional

import numpy as np

from audio import SAMPLE_RATE

# 国内可用的 HuggingFace 镜像
DEFAULT_HF_MIRROR = "https://hf-mirror.com"


def ensure_hf_endpoint() -> None:
    """确保 HuggingFace 下载配置能在当前网络下工作。

    国内直连 huggingface.co 会超时，且 huggingface_hub >= 1.0 默认走 Xet
    存储（cas-server.xethub.hf.co），那个域名在国内也连不上，会报 401。
    所以这里做三件事：
      1. 把 HF_ENDPOINT 指向 hf-mirror.com（如果官方源连不上）；
      2. 设 HF_HUB_DISABLE_XET=1，强制回退到普通 HTTP 下载；
      3. 设 HF_HUB_DISABLE_SYMLINKS_WARNING=1，消掉 Windows 无符号链接权限
         时那条纯噪音的 UserWarning（只影响缓存效率，不影响功能）。

    已经由用户显式设置过的环境变量不会被覆盖。
    """
    # ---- 0) Windows 上的符号链接警告：纯噪音，关掉 ----
    # 首次加载模型时 huggingface_hub 会警告"缓存目录不支持符号链接"，
    # 原因是 Windows 普通账户默认没有创建符号链接的权限。
    # 它只是说缓存会多占一点磁盘，模型照样能正常用，所以直接静音。
    os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

    # ---- 1) Xet：默认关掉 ----
    # 不关的话，hf-mirror 镜像也救不了，会卡在 cas-server.xethub.hf.co 上报 401
    if not os.environ.get("HF_HUB_DISABLE_XET"):
        os.environ["HF_HUB_DISABLE_XET"] = "1"
        print("[asr] 已禁用 HuggingFace Xet 传输（回退到普通 HTTP）")

    # ---- 2) 镜像 ----
    if os.environ.get("HF_ENDPOINT"):
        print(f"[asr] 使用已设置的 HF_ENDPOINT = {os.environ['HF_ENDPOINT']}")
        return

    mirror = os.environ.get("BAOZI_HF_ENDPOINT", DEFAULT_HF_MIRROR)

    def reachable(url: str, timeout: float = 6.0) -> bool:
        try:
            import urllib.request

            req = urllib.request.Request(url, method="HEAD")
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return 200 <= resp.status < 400
        except Exception:
            return False

    if reachable("https://huggingface.co"):
        print("[asr] huggingface.co 可直连，使用官方源")
        # 官方源能连上就不用镜像了
        return

    if reachable(mirror):
        os.environ["HF_ENDPOINT"] = mirror
        print(f"[asr] huggingface.co 连不上，已自动切到镜像 {mirror}")
        return

    print(
        "[asr] 警告：huggingface.co 和镜像都连不上。\n"
        "       如果模型不在本地缓存里，加载会失败。\n"
        "       可以手动设置镜像后重试：$env:HF_ENDPOINT='https://hf-mirror.com'"
    )


class Asr:
    """ASR 的统一接口。实现类只需实现 :meth:`transcribe`。"""

    name = "base"

    def transcribe(self, pcm: np.ndarray) -> str:
        raise NotImplementedError

    # 让 with 语句/显式释放都能工作（whisper 模型挺占内存）
    def close(self) -> None:  # pragma: no cover
        pass


# ---------------------------------------------------------------------
class SenseVoiceAsr(Asr):
    """阿里 FunASR 的 SenseVoiceSmall（本地推理，中文效果好）。

    相比 whisper-tiny：中文准确率高很多，而且模型小（~900MB 含所有依赖），
    CPU 上速度也够用。模型从 ModelScope（国内站）下载，不走 HuggingFace。
    """

    name = "sensevoice"

    #: 默认模型（ModelScope 上的 ID）
    DEFAULT_MODEL = "iic/SenseVoiceSmall"

    def __init__(
        self,
        model: str = DEFAULT_MODEL,
        device: str = "auto",
        language: Optional[str] = "zh",
        use_itn: bool = True,
    ) -> None:
        try:
            from funasr import AutoModel  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "funasr 未安装：pip install funasr modelscope\n"
                "  （SenseVoice 还需要 torch，一般会连带装上）"
            ) from exc

        if device == "auto":
            device = self._detect_device()

        self._language = language or "zh"
        self._use_itn = use_itn

        print(f"[asr] 加载 SenseVoice 模型 '{model}' (device={device}) ...")

        # ---- 本地缓存优先：有完整缓存就**完全不联网** ----
        # 为什么需要（2026-09 实测）：
        #   模型明明已经下好了（~896MB 在 modelscope 缓存里），但只要
        #   `modelscope.cn` 的 DNS 临时解析不了，funasr 就会抛
        #   "model 'iic/SenseVoiceSmall' is not registered" 直接启动失败 ——
        #   这是把**联网校验**当成了加载前提，对已经下载好的机器毫无必要。
        #   这里先找本地目录，找到就直接用**绝对路径**加载，
        #   彻底绕开联网校验（`disable_update=True` 并不阻止它查远端）。
        local_dir = self._find_local_model(model)
        if local_dir:
            print(f"[asr] 找到本地模型缓存，离线加载: {local_dir}")
            model = local_dir
        else:
            print("[asr] 本地无缓存，将从 ModelScope 下载模型（约 900MB），请耐心等待")

        try:
            self._model = AutoModel(
                model=model,
                trust_remote_code=True,
                device=device,
                disable_update=True,   # 别每次启动都去检查更新（省时间、少联网）
            )
        except Exception as exc:
            raise RuntimeError(
                f"加载 SenseVoice 模型失败：{exc}\n"
                f"  常见原因：\n"
                f"    * 首次运行需要联网从 ModelScope 下载（modelscope.cn，国内可直连）\n"
                f"    * funasr / torch 没装好：pip install funasr modelscope torch torchaudio\n"
                f"  手动下载模型（可选，提前把模型拉好）:\n"
                f"    python -c \"from funasr import AutoModel; AutoModel(model='{model}')\"\n"
                f"  想改用 whisper 作为后备：python server.py --asr-backend faster-whisper"
            ) from exc

        self._language = language or "zh"
        print("[asr] 模型就绪")

    @staticmethod
    def _detect_device() -> str:
        try:
            import torch  # type: ignore

            return "cuda:0" if torch.cuda.is_available() else "cpu"
        except Exception:
            return "cpu"

    @staticmethod
    def _find_local_model(model: str) -> str:
        """在 ModelScope 缓存里找已下载好的模型目录，返回绝对路径或 ""。

        缓存布局（modelscope 新版）：
            ~/.cache/modelscope/models/<org>--<name>/snapshots/<rev>/
        旧版：
            ~/.cache/modelscope/hub/<org>/<name>/

        判定"完整"的标准：存在 model.pt（权重）和 configuration.json。
        """
        import os

        if os.path.isdir(model):        # 用户直接给了路径
            return model

        cache_root = os.environ.get("MODELSCOPE_CACHE") or os.path.join(
            os.path.expanduser("~"), ".cache", "modelscope")
        org, _, name = model.partition("/")
        cands: List[str] = []

        # 新版：models/<org>--<name>/snapshots/<rev>
        base = os.path.join(cache_root, "models", f"{org}--{name}", "snapshots")
        if os.path.isdir(base):
            for rev in sorted(os.listdir(base)):
                cands.append(os.path.join(base, rev))
        # 旧版：hub/<org>/<name>
        cands.append(os.path.join(cache_root, "hub", org, name))
        cands.append(os.path.join(cache_root, "hub", "models", org, name))

        for d in cands:
            if (os.path.isfile(os.path.join(d, "model.pt"))
                    and os.path.isfile(os.path.join(d, "configuration.json"))):
                return d
        return ""

    def transcribe(self, pcm: np.ndarray) -> str:
        import re

        # SenseVoice 要的是【归一化到 -1~1 的 float32】。
        # 注意：FunASR 对 np.ndarray 分支不做 /32768 归一化（只对 kaldi_ark
        # 分支做），所以必须我们自己转，否则幅度会放大 32768 倍。
        audio = pcm.astype(np.float32) / 32768.0

        res = self._model.generate(
            input=audio,
            language=self._language,     # zh / en / yue ...
            use_itn=self._use_itn,       # 逆文本正则化："一百" -> "100"
            batch_size_s=60,
        )

        if not res:
            return ""

        raw = res[0].get("text", "")
        # 去掉 SenseVoice 输出的元数据标签（如 <|zh|><|NEUTRAL|><|Speech|>）
        clean = re.sub(r"<[^>]+>", "", raw)
        return clean.strip()


# ---------------------------------------------------------------------
class FasterWhisperAsr(Asr):
    name = "faster-whisper"

    def __init__(
        self,
        model: str = "small",
        device: str = "auto",
        compute_type: str = "auto",
        language: Optional[str] = "zh",
    ) -> None:
        ensure_hf_endpoint()

        try:
            from faster_whisper import WhisperModel  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "faster-whisper 未安装：pip install faster-whisper"
            ) from exc

        if device == "auto":
            device = self._detect_device()

        if compute_type == "auto":
            compute_type = "float16" if device == "cuda" else "int8"

        print(f"[asr] 加载 faster-whisper 模型 '{model}' (device={device}, compute={compute_type}) ...")
        self._model = self._load_with_retry(model, device, compute_type)
        self._language = language or None
        print("[asr] 模型就绪")

    @staticmethod
    def _load_with_retry(model: str, device: str, compute_type: str, attempts: int = 3):
        """加载模型；如果本地没有需要下载，失败就重试。

        为什么要重试：hf-mirror 偶尔会在下载到一半时卡住（连接不动也不报错），
        huggingface_hub 会一直等下去，表现为程序"卡在那里没反应"。
        重试时已下载的分片会被复用（hf 会续传），所以重跑是有意义的。

        模型已在本地的正常情况走不到这里，直接一次成功。
        """
        from faster_whisper import WhisperModel  # type: ignore

        last: Exception | None = None
        for i in range(1, attempts + 1):
            try:
                return WhisperModel(model, device=device, compute_type=compute_type)
            except Exception as exc:
                last = exc
                if i < attempts:
                    print(
                        f"[asr] 第 {i} 次加载失败（{type(exc).__name__}），"
                        f"{2 * i} 秒后重试（已下载的部分会续传）..."
                    )
                    time.sleep(2 * i)

        hint = (
            f"加载 whisper 模型 '{model}' 失败（已重试 {attempts} 次）：{last}\n"
            f"  常见原因：模型需要联网下载，而网络不通/超时。\n"
            f"  本机会自动使用镜像 hf-mirror.com；若仍失败，可以手动下载：\n"
            f"    $env:HF_ENDPOINT='https://hf-mirror.com'\n"
            f"    $env:HF_HUB_DISABLE_XET='1'\n"
            f"    python -c \"from faster_whisper import WhisperModel; WhisperModel('{model}')\"\n"
            f"  或者先用已下载好的小模型把链路跑通：--whisper-model tiny"
        )
        raise RuntimeError(hint)

    @staticmethod
    def _detect_device() -> str:
        # 不做重依赖探测，能 import torch 且 cuda 可用就用 cuda
        try:
            import torch  # type: ignore

            return "cuda" if torch.cuda.is_available() else "cpu"
        except Exception:
            return "cpu"

    def transcribe(self, pcm: np.ndarray) -> str:
        audio_f32 = pcm.astype(np.float32) / 32768.0

        segments, _info = self._model.transcribe(
            audio_f32,
            language=self._language,
            beam_size=1,          # 实时交互场景：贪心解码，快
            vad_filter=False,     # 我们已经做过 VAD 了
            condition_on_previous_text=False,
        )
        return "".join(seg.text for seg in segments).strip()


# ---------------------------------------------------------------------
class OpenaiWhisperAsr(Asr):
    name = "openai-whisper"

    def __init__(
        self,
        model: str = "small",
        device: str = "auto",
        compute_type: str = "auto",  # 这个后端用不上，为了接口一致保留
        language: Optional[str] = "zh",
    ) -> None:
        try:
            import whisper  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "openai-whisper 未安装：pip install openai-whisper"
            ) from exc

        if device == "auto":
            device = FasterWhisperAsr._detect_device()

        print(f"[asr] 加载 whisper 模型 '{model}' (device={device}) ...")
        self._model = whisper.load_model(model, device=device)
        self._language = language or None
        print("[asr] 模型就绪")

    def transcribe(self, pcm: np.ndarray) -> str:
        audio_f32 = pcm.astype(np.float32) / 32768.0
        result = self._model.transcribe(
            audio_f32,
            language=self._language,
            fp16=False,
            condition_on_previous_text=False,
        )
        return str(result.get("text", "")).strip()


# ---------------------------------------------------------------------
class MockAsr(Asr):
    """不识别，返回固定文本。用于在没有模型的情况下跑通整条链路。"""

    name = "mock"
    counter = 0

    def transcribe(self, pcm: np.ndarray) -> str:
        MockAsr.counter += 1
        ms = len(pcm) * 1000 // SAMPLE_RATE
        return f"（模拟识别结果第 {MockAsr.counter} 句，音频 {ms} 毫秒）"


# ---------------------------------------------------------------------
def make_asr(cfg) -> Asr:
    backend = (cfg.asr_backend or "sensevoice").lower()

    if backend in ("sensevoice", "funasr"):
        return SenseVoiceAsr(
            model=cfg.sensevoice_model,
            device=cfg.whisper_device,     # 复用同一个设备配置（auto/cpu/cuda）
            language=cfg.asr_language,
        )
    if backend == "faster-whisper":
        return FasterWhisperAsr(
            model=cfg.whisper_model,
            device=cfg.whisper_device,
            compute_type=cfg.whisper_compute_type,
            language=cfg.asr_language,
        )
    if backend in ("openai-whisper", "whisper"):
        return OpenaiWhisperAsr(
            model=cfg.whisper_model,
            device=cfg.whisper_device,
            language=cfg.asr_language,
        )
    if backend == "mock":
        return MockAsr()

    raise ValueError(f"未知的 asr_backend: {cfg.asr_backend}")
