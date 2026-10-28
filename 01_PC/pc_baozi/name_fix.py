"""同音字纠正：把 ASR 听到的名字改成已确认过的那个人。

背景（2026-09 用户反馈）：
    语音链路里 "陈若语 / 陈若雨 / 陈若羽" 的音频几乎完全一样，ASR 只能猜一个。
    用户确认过"陈若语"之后，再说起这个人时可能被听成"陈若雨/陈若羽"，
    于是包子就以为是另一个人。

做法：
    拿**已确认的人名表**当热词表。新听到的名字如果与表里某个名字
    拼音完全相同（或同长度下高度相似），就判定"说的就是那个人"，
    自动改写成已确认的写法。这样：
      · 认识的人不会被当成新人重复追问；
      · 信息不会散落到 "陈若雨" 这种同音错名下面。

注意：早期版本还包含"逐字拆字确认"（问用户"是哪几个字"），
但那套交互在真实语音里太啰嗦、状态机也容易乱（用户实测反馈"非常混乱"），
已整体移除。**只保留这里的同音纠正**。
"""

from __future__ import annotations

from typing import Dict, List

# ==================== 拼音（同音判断的基础） ====================
try:
    from pypinyin import lazy_pinyin as _lazy_pinyin
    _HAS_PYPINYIN = True
except ImportError:       # 没装 pypinyin 就退化成字面匹配，不会崩
    _HAS_PYPINYIN = False
    _lazy_pinyin = None


def _pinyin_key(s: str) -> str:
    """取名字的拼音串（无声调），用于同音判断。没有 pypinyin 时返回原串。"""
    if not s:
        return ""
    if not _HAS_PYPINYIN:
        return s
    try:
        return "".join(_lazy_pinyin(s))
    except Exception:  # noqa: BLE001
        return s


def match_known_name(name: str, known: List[str]) -> str:
    """把一个"听到的名字"匹配到已知人名表里最可能的那个。

    >>> match_known_name("陈若雨", ["陈若语"])
    '陈若语'
    >>> match_known_name("陈若雨", ["张三"])
    ''

    返回 "" 表示"不是已知的人"（可能真是新人）。
    匹配优先级：① 完全相同 -> ② 拼音完全相同 -> ③ 同长度下拼音高度相似。
    """
    if not name or not known:
        return ""
    name = str(name).strip()
    if not name:
        return ""

    # ① 字面完全一致
    for k in known:
        if k == name:
            return k

    pk = _pinyin_key(name)
    if pk:
        # ② 拼音完全一致（陈若雨 / 陈若羽 -> chenruoyu，与"陈若语"一致）
        same_py = [k for k in known if _pinyin_key(k) == pk]
        if len(same_py) == 1:
            return same_py[0]          # 只有唯一候选时才改写，避免歧义
        if len(same_py) > 1:
            return ""                  # 多个同音已知名字，不猜

    # ③ 拼音高度相似（ASR 可能把某个字听成不同音）
    #    ⚠️ 只对**长度相同**的名字做模糊匹配。"陈若"(chenruo) 与 "陈若语"
    #    (chenruoyu) 相似度约 0.8，若不限长度就会被错误纠正 —— 用户可能
    #    真的认识一个叫"陈若"的人。长度不同 -> 判为不同的人，更安全。
    import difflib
    best_k, best_r = "", 0.0
    for k in known:
        if len(k) != len(name):
            continue
        r = difflib.SequenceMatcher(None, pk, _pinyin_key(k)).ratio()
        if r > best_r:
            best_k, best_r = k, r
    return best_k if best_r >= 0.8 else ""


def rewrite_names(text: str, known: List[str]) -> str:
    """把整句话里**同音的已知人名**替换成已确认的写法。

    >>> rewrite_names("陈若雨今天来吗", ["陈若语"])
    '陈若语今天来吗'

    按名字长度从长到短处理，避免短名先把长名的一部分换掉。
    """
    if not text or not known:
        return text
    out = text
    for k in sorted(known, key=len, reverse=True):
        if not k:
            continue
        n = len(k)
        i = 0
        while i + n <= len(out):
            seg = out[i:i + n]
            if seg != k and all("\u4e00" <= c <= "\u9fff" for c in seg):
                if match_known_name(seg, [k]) == k:
                    out = out[:i] + k + out[i + n:]
                    i += n
                    continue
            i += 1
    return out
