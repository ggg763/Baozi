"""包子的情绪内存（PC 侧）—— 移植自 Baozi_PC/emotion/emotion_manager.py
与 Baozi_PC/brain/json_parser.py 的情绪归一化。

职责（= 任务3 里"情感状态机的第一部分：PC 上作为大语言模型的 memory，短期调控情绪"）：
  - 保存当前情绪 + 连续状态 {happiness, energy, curiosity}（0~1）；
  - 把 LLM 输出的情绪（可能中文/同义词/口语）归一化到标准 6 种；
  - 提供 prompt 片段，把"当前情绪/能量"喂回给大模型，形成短期情绪记忆。

不负责：驱动硬件 / 下发串口（那在 td1601.py）与 TTS（那在 tts.py）。
"""

from __future__ import annotations

from typing import Dict

# 标准 6 种情绪（与 TD1601 端 emotion_manager.h 的 emo_id_t 一一对应）
VALID_EMOTIONS = {"happy", "sad", "angry", "surprised", "loving", "neutral"}

# 同义词表：LLM 输出不严谨（中文/英文变体/口语）时归一到标准 6 种
EMOTION_ALIASES = {
    "happy": ["开心", "高兴", "快乐", "愉快", "喜悦", "愉悦", "兴奋", "欢快",
              "欣喜", "爽", "好玩", "glad", "joy", "joyful", "delighted", "excited",
              "cheerful", "pleased", "great", "good", "playful", "amused"],
    # ⚠️ 共情类词（sympathetic / caring / concerned）实测是模型的高频输出：
    #    它想表达"我同情你"，但标准 6 情绪里没有这一档，语义上最贴近 sad。
    #    若不收录，这些词会被 normalize 丢成 neutral —— 表现为"用户诉苦，包子却一脸平静"。
    "sad": ["难过", "伤心", "悲伤", "忧郁", "沮丧", "失落", "低落", "委屈",
            "悲哀", "心疼", "同情", "抱歉", "不好意思", "sad", "sorrow", "unhappy",
            "sadness", "down", "gloomy", "sympathetic", "sympathy", "caring",
            "concerned", "empathetic", "apologetic", "sorry"],
    "angry": ["生气", "愤怒", "恼火", "发火", "气愤", "暴躁", "恼人", "不爽",
              "恼火", "不满", "angry", "mad", "furious", "annoyed", "irritated",
              "frustrated", "upset"],
    "surprised": ["惊讶", "吃惊", "惊奇", "意外", "震惊", "surprised", "shocked",
                  "amazed", "astonished", "surprise", "stunned", "startled"],
    "loving": ["喜欢", "喜爱", "亲昵", "温馨", "心动", "爱", "撒娇", "宠溺",
               "loving", "love", "affectionate", "tender", "fond", "caring_love",
               "sweet", "warm"],
    "neutral": ["平静", "平常", "一般", "无所谓", "淡然", "neutral",
                "calm", "ok", "fine", "normal", "serene", "plain"],
}

# 情绪 -> 连续状态（与 Baozi_PC/emotion/emotion_manager.py 的 _update_state 一致）
STATE_MAP: Dict[str, Dict[str, float]] = {
    "happy":     {"happiness": 0.9, "energy": 0.8, "curiosity": 0.7},
    "sad":       {"happiness": 0.2, "energy": 0.3, "curiosity": 0.4},
    "angry":     {"happiness": 0.2, "energy": 0.9, "curiosity": 0.3},
    "surprised": {"happiness": 0.6, "energy": 1.0, "curiosity": 0.9},
    "loving":    {"happiness": 0.8, "energy": 0.6, "curiosity": 0.7},
    "neutral":   {"happiness": 0.5, "energy": 0.5, "curiosity": 0.5},
}


def _is_chinese(text: str) -> bool:
    return any("\u4e00" <= ch <= "\u9fff" for ch in text)


def _emotion_exact(text: str):
    if text in VALID_EMOTIONS:
        return text
    for standard, aliases in EMOTION_ALIASES.items():
        if text in aliases:
            return standard
    return None


def _emotion_contains(text: str):
    for standard, aliases in EMOTION_ALIASES.items():
        for alias in aliases:
            if len(alias) >= 2 and _is_chinese(alias) and alias in text:
                return standard
    return None


def normalize_emotion(raw) -> str:
    """清洗 -> 精确匹配 -> 否定前缀 -> 中文包含 -> neutral（同 Baozi_PC）。"""
    if not raw:
        return "neutral"
    text = str(raw).strip().lower()
    for ch in "!！。.，,、~～ ":
        text = text.replace(ch, "")
    if not text:
        return "neutral"

    std = _emotion_exact(text)
    if std:
        return std

    if text.startswith("不") and len(text) > 1:
        core = text[1:]
        core_std = _emotion_exact(core) or _emotion_contains(core)
        if core_std in ("happy", "loving"):
            return "sad"
        if core_std in ("sad", "angry"):
            return "neutral"

    std = _emotion_contains(text)
    if std:
        return std
    return "neutral"


# ---------------- 关键词兜底推测情绪 ----------------
# 用途（2026-09 新增）：模型偶尔只返回 {"reply": ...} 而漏掉 emotion 字段
# （实测：历史里的 JSON 格式会被模仿，字段缺失时 emotion 本该"不知道"）。
# 与其一律降级成 neutral，不如从 reply 文本里做一次保守的关键词推测 ——
# 只认非常明确的词，猜不出仍然返回 neutral，避免瞎猜。
_REPLY_EMOTION_HINTS = (
    # (关键词元组, 情绪) —— 顺序即优先级，靠前的先匹配
    (("哈哈", "嘿嘿", "开心", "高兴", "太好了", "真棒", "好耶", "嘻嘻", "耶"), "happy"),
    (("难过", "伤心", "呜呜", "好想哭", "想哭", "委屈"), "sad"),
    (("生气", "哼", "讨厌", "气死", "不高兴", "火大"), "angry"),
    (("哇", "天啊", "天哪", "真的吗", "居然", "没想到", "吃惊", "惊讶"), "surprised"),
    (("想你", "爱你", "喜欢你", "抱抱", "亲亲", "宝贝", "小可爱"), "loving"),
)


def guess_emotion_from_reply(reply: str) -> str:
    """从回复文本里保守地推测情绪；猜不出返回 "neutral"。"""
    if not reply:
        return "neutral"
    for words, emo in _REPLY_EMOTION_HINTS:
        for w in words:
            if w in reply:
                return emo
    return "neutral"


# ---------------- 情感强度（intensity）归一化 ----------------
# 三级强度：low / medium / high。由 LLM 在回复时一并给出，"当前这句话的情绪有多强"。
# 下游用法：PC 经 ESP32 下发给 TD1601，TD1601 用它修正动作幅度/速度
# （见 UserFolder/emotion/emotion_fsm.c 的强度层）。
# 注意与 intent 的区别：intent 描述"用户在干什么"（提问/命令…），
# intensity 描述"包子此刻情绪多重"，两者都是可选的低成本补充信息。
VALID_INTENSITIES = {"low", "medium", "high"}

INTENSITY_ALIASES = {
    "low": ["低", "弱", "轻微", "一点点", "稍微", "小幅", "low", "mild", "slight", "weak"],
    "medium": ["中", "中等", "一般", "普通", "适中", "medium", "moderate", "normal", "mid"],
    "high": ["高", "强", "强烈", "非常", "极度", "超高", "high", "strong", "intense", "extreme"],
}

# 强度默认值：模型没给时视作 medium（既不夸张也不冷淡）
DEFAULT_INTENSITY = "medium"


def normalize_intensity(raw) -> str:
    """情感强度归一化：三级取值 + 中英文别名，非法/缺失 → medium。"""
    if not raw:
        return DEFAULT_INTENSITY
    text = str(raw).strip().lower()
    for ch in "!！。.，,、~～ ":
        text = text.replace(ch, "")
    if not text:
        return DEFAULT_INTENSITY

    if text in VALID_INTENSITIES:
        return text
    for standard, aliases in INTENSITY_ALIASES.items():
        if text in aliases:
            return standard
    return DEFAULT_INTENSITY


# ---------------- 意图（intent）归一化 ----------------
# 与 TD1601 侧 emotion_fsm 的 INTENT: 解析、Baozi_PC/communication.py 的
# INTENT_MAP 保持同一套取值。
VALID_INTENTS = {
    "greeting", "question", "chat", "command",
    "thanks", "goodbye", "unknown", "none",
}

INTENT_ALIASES = {
    "chat": ["聊天", "闲聊", "聊"],
    "greeting": ["问候", "打招呼", "你好", "您好"],
    "question": ["提问", "询问", "问题", "求助", "问"],
    "command": ["命令", "指令", "要求"],
    "thanks": ["感谢", "谢谢", "多谢"],
    "goodbye": ["再见", "告别", "拜拜"],
    "unknown": ["不知道", "随便", "没想好"],
}


def normalize_intent(raw) -> str:
    """LLM 意图归一化：中文映射 + 白名单，非法值归一到 none。"""
    if not raw:
        return "none"
    text = str(raw).strip().lower()
    if text in VALID_INTENTS:
        return text
    for standard, aliases in INTENT_ALIASES.items():
        if text in aliases:
            return standard
    for standard, aliases in INTENT_ALIASES.items():
        for alias in aliases:
            if len(alias) >= 2 and _is_chinese(alias) and alias in text:
                return standard
    return "none"


# ==================== 情绪命令识别（代码层兜底）====================
# 为什么需要（2026-09 实测）：
#   用户说"你生气一下"时，模型会把它理解成**"用户生气了，我要安慰他"**，
#   于是输出 loving/sad —— 实测 6 次只中 1 次。
#   原因是陪伴机器人的"安慰人"人设是很强的先验，光靠 prompt 压不住
#   （试过加强措辞 + 加主语判据，仍是 1/6）。
#   所以这里在代码层直接识别"命令你演某种情绪"的句式并覆盖 emotion。
#
# 判据：**"你"字开头 + 情绪词 + 命令语气** → 命令包子演这个情绪。
#   对照："我生气了"（"我"开头）= 用户在表达情绪，不归这里管。
#
# 顺序很重要：先在**用户输入**上匹配（这才是命令），
# 而不是在模型输出上事后纠正 —— 后者会让 reply 和 emotion 不匹配。

#: 情绪词 -> 标准 emotion。顺序按长度倒序匹配，避免"不开心"被"开心"截胡。
_CMD_EMOTION_WORDS = (
    ("不高兴", "sad"), ("不开心", "sad"), ("生气", "angry"), ("愤怒", "angry"),
    ("发火", "angry"), ("恼火", "angry"), ("伤心", "sad"), ("难过", "sad"),
    ("悲伤", "sad"), ("忧郁", "sad"), ("失落", "sad"),
    ("开心", "happy"), ("高兴", "happy"), ("快乐", "happy"), ("愉快", "happy"),
    ("兴奋", "happy"), ("喜悦", "happy"),
    ("惊讶", "surprised"), ("吃惊", "surprised"), ("震惊", "surprised"),
    ("意外", "surprised"),
    ("撒娇", "loving"), ("可爱", "loving"), ("亲昵", "loving"), ("喜欢", "loving"),
)

#: 命令的语气标志（出现在句末附近）
_CMD_TAILS = ("一下", "一点", "一些", "点儿", "看看", "试试", "嘛", "吧", "啊",
              "个娇", "撒娇")

#: 明显的"夸奖/陈述"标志 —— 出现这些就不是命令（如"你很可爱""你真好"）
_NOT_CMD = ("很可爱", "真可爱", "好可爱", "很棒", "真好", "厉害", "不错",
            "有点", "是不是", "吗", "么")

#: 明显不是命令的前缀（"我"开头 = 用户在说自己）
_SELF_PREFIX = ("我", "咱", "人家")


def parse_emotion_command(text: str):
    """识别"命令包子演某种情绪"的句式，返回 (emotion, intensity) 或 None。

    >>> parse_emotion_command("你生气一下")
    ('angry', 'medium')
    >>> parse_emotion_command("你给我开心一点")
    ('happy', 'medium')
    >>> parse_emotion_command("我非常生气")      # "我"开头 -> 用户在表达，不接管
    >>> parse_emotion_command("今天天气不错")
    """
    if not text:
        return None
    t = str(text).strip().strip("。！？!?，,、 ")

    # 必须以"你"开头（或"包子你…"）：命令的对象是包子
    body = t
    for pre in ("包子", "宝贝", "亲爱的"):
        if body.startswith(pre):
            body = body[len(pre):]
    if not body.startswith("你"):
        return None
    if body.startswith(_SELF_PREFIX):     # 双保险
        return None

    rest = body[1:]                        # 去掉开头的"你"

    # 先排除"夸奖/陈述"（"你很可爱""你真好"是在夸你，不是命令你演）
    for bad in _NOT_CMD:
        if bad in rest:
            return None

    # 去掉"给我/快点/赶紧"这类插入语
    for filler in ("给我", "快点", "赶紧", "马上", "现在", "也", "要", "再"):
        if rest.startswith(filler):
            rest = rest[len(filler):]

    # 找情绪词（按长度倒序，先匹配长词）
    # 注意 `rest` 里可能夹口语填充字（"撒个娇""开个心"），
    # 所以匹配时把"个/一下/一点"这类先剔除再找。
    probe = rest
    for filler in ("个", "一下", "一点", "一些", "点儿"):
        probe = probe.replace(filler, "")
    hit = ""
    for word, emo in sorted(_CMD_EMOTION_WORDS, key=lambda x: -len(x[0])):
        if word in rest or word in probe:
            hit, emotion = word, emo
            break
    if not hit:
        return None

    # 语气校验：句子要像"命令"而不是陈述
    #   有这些标志 -> 命令；没有的话，仅当整句很短（≤6字）也接受（如"你生气"）
    looks_cmd = any(tail in rest for tail in _CMD_TAILS) or len(rest) <= 6
    if not looks_cmd:
        return None

    # 强度：出现"非常/特别/超/特别"等 -> high
    intensity = "medium"
    for strong in ("非常", "特别", "超级", "超", "极其", "巨", "死了", "很"):
        if strong in rest:
            intensity = "high"
            break
    # "有一点点/稍微" -> low
    for weak in ("一点点", "一点小", "稍微", "略"):
        if weak in rest:
            intensity = "low"
            break

    return emotion, intensity


class EmotionMemory:
    """当前情绪 + 连续状态（= Baozi_PC 的 EmotionManager）。"""
    def __init__(self) -> None:
        self.current_emotion = "neutral"
        self.state = dict(STATE_MAP["neutral"])

    def update(self, emotion: str) -> str:
        emotion = normalize_emotion(emotion)
        self.current_emotion = emotion
        self.state = dict(STATE_MAP.get(emotion, STATE_MAP["neutral"]))
        return emotion

    def get_emotion(self) -> str:
        return self.current_emotion

    def get_state(self) -> Dict[str, float]:
        return dict(self.state)

    def reset(self) -> None:
        self.current_emotion = "neutral"
        self.state = dict(STATE_MAP["neutral"])

    # ------------------------------------------------------------------
    def prompt_block(self) -> str:
        """把当前情绪/能量喂回给大模型（短期情绪记忆）。"""
        return (
            "\n【你当前的情绪记忆】\n"
            f"- 当前情绪：{self.current_emotion}\n"
            f"- 愉悦度(happiness)：{self.state['happiness']:.2f}\n"
            f"- 精力(energy)：{self.state['energy']:.2f}\n"
            f"- 好奇(curiosity)：{self.state['curiosity']:.2f}\n"
            "请让回复的语气与当前情绪保持一致，但不要夸张表演。\n"
        )
