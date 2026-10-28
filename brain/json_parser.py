"""
json_parser.py

包子 JSON 输出解析器

职责：
    将 LLM 原始输出解析成标准机器人响应格式

输入：
    LLM返回的字符串

输出：
    {
        "reply": str,
        "emotion": str,
        "intent": str
    }


不负责：
    - 调用LLM
    - Prompt
    - Memory
    - 情绪逻辑
    - 硬件控制
"""


import json
import re
from typing import Dict


# 包子允许的情绪列表（LLM 只能输出这 6 种）
VALID_EMOTIONS = {
    "happy",
    "sad",
    "angry",
    "surprised",
    "loving",
    "neutral"
}

# 情绪同义词表：LLM 输出不严谨（中文/英文变体/口语）时归一到标准 6 种
EMOTION_ALIASES = {
    "happy": ["开心", "高兴", "快乐", "愉快", "喜悦", "愉悦", "兴奋", "欢快",
              "欣喜", "爽", "glad", "joy", "joyful", "delighted", "excited",
              "cheerful", "pleased", "great", "good"],
    "sad": ["难过", "伤心", "悲伤", "忧郁", "沮丧", "失落", "低落", "委屈",
            "悲哀", "sad", "sorrow", "unhappy", "sadness", "down", "gloomy"],
    "angry": ["生气", "愤怒", "恼火", "发火", "气愤", "暴躁", "恼人", "不爽",
              "angry", "mad", "furious", "annoyed"],
    "surprised": ["惊讶", "吃惊", "惊奇", "意外", "震惊", "surprised",
                  "shocked", "amazed", "astonished", "surprise"],
    "loving": ["喜欢", "喜爱", "亲昵", "温馨", "心动", "爱",
               "loving", "love", "affectionate", "tender", "fond"],
    "neutral": ["平静", "平常", "一般", "无所谓", "淡然", "neutral",
                "calm", "ok", "fine", "normal", "serene"],
}

# 意图中文映射：LLM 输出中文意图时归一到标准意图
INTENT_ALIASES = {
    "chat": ["聊天", "闲聊", "聊"],
    "greeting": ["问候", "打招呼", "你好", "您好"],
    "question": ["提问", "询问", "问题", "求助", "问"],
    "command": ["命令", "指令", "要求"],
    "thanks": ["感谢", "谢谢", "多谢"],
    "goodbye": ["再见", "告别", "拜拜", "再见"],
    "unknown": ["不知道", "随便", "没想好"],
}


def _is_chinese(text: str) -> bool:
    """是否包含中文字符"""
    return any('\u4e00' <= ch <= '\u9fff' for ch in text)


def _emotion_exact(text: str):
    """精确匹配：白名单 + 同义词表，返回标准情绪；未命中返回 None"""
    if text in VALID_EMOTIONS:
        return text
    for standard, aliases in EMOTION_ALIASES.items():
        if text in aliases:
            return standard
    return None


def _emotion_contains(text: str):
    """中文包含匹配（如"很开心"→happy）；英文别名只精确匹配，避免误判"""
    for standard, aliases in EMOTION_ALIASES.items():
        for alias in aliases:
            if len(alias) >= 2 and _is_chinese(alias) and alias in text:
                return standard
    return None


def normalize_emotion(raw) -> str:
    """LLM 情绪归一化：清洗 → 精确匹配 → 否定前缀 → 中文包含 → neutral

    顺序很重要：否定前缀必须在包含匹配之前判断，
    否则"不开心"会先命中包含的"开心"而错误归为 happy。
    """
    if not raw:
        return "neutral"
    text = str(raw).strip().lower()
    for ch in "!！。.，,、~～ ":
        text = text.replace(ch, "")
    if not text:
        return "neutral"

    # 1. 精确匹配（白名单 + 同义词）
    std = _emotion_exact(text)
    if std:
        return std

    # 2. 否定前缀："不开心"→sad、"不生气"→neutral
    if text.startswith("不") and len(text) > 1:
        core = text[1:]
        core_std = _emotion_exact(core) or _emotion_contains(core)
        if core_std in ("happy", "loving"):
            return "sad"
        if core_std in ("sad", "angry"):
            return "neutral"

    # 3. 中文包含匹配（如"很开心"→happy）
    std = _emotion_contains(text)
    if std:
        return std

    return "neutral"


def normalize_intent(raw) -> str:
    """LLM 意图归一化：中文映射 + 白名单，非法值归一到 none"""
    if not raw:
        return "none"
    text = str(raw).strip().lower()
    if text in VALID_INTENTS:
        return text
    # 同义词精确匹配（含中文意图词）
    for standard, aliases in INTENT_ALIASES.items():
        if text in aliases:
            return standard
    # 中文包含匹配（如"想聊天"→chat）
    for standard, aliases in INTENT_ALIASES.items():
        for alias in aliases:
            if len(alias) >= 2 and _is_chinese(alias) and alias in text:
                return standard
    return "none"

# 允许的意图列表
VALID_INTENTS = {
    "greeting",
    "question",
    "chat",
    "command",
    "thanks",
    "goodbye",
    "unknown",
    "none"
}


class JsonParser:


    def __init__(self):
        pass



    def parse(
        self,
        raw: str
    ) -> Dict[str, str]:
        """
        解析LLM输出

        Args:
            raw:
                LLM原始字符串


        Returns:

        {
            "reply":"",
            "emotion":"",
            "intent":""
        }

        """

        if not raw:

            return self._fallback()



        # 第一层：
        # 尝试直接JSON解析

        result = self._try_json(raw)

        if result:

            return self._normalize(result)



        # 第二层：
        # 正则提取JSON

        result = self._extract_json(raw)

        if result:

            return self._normalize(result)



        # 第三层：
        # 安全降级

        return {
            "reply": raw.strip(),
            "emotion": "neutral",
            "intent": "none"
        }




    def _try_json(
        self,
        text: str
    ):

        try:

            data = json.loads(text)

            if isinstance(data, dict):

                return data


        except json.JSONDecodeError:

            pass


        return None




    def _extract_json(
        self,
        text: str
    ):

        """
        从混杂文本中寻找JSON对象

        例如：

        好啦：
        {
          "reply":"你好",
          "emotion":"happy",
          "intent":"chat"
        }

        """

        pattern = r"\{[^{}]*\}"


        matches = re.findall(
            pattern,
            text,
            re.S
        )


        if not matches:

            return None



        # 取最后一个JSON
        # 因为模型有时前面会解释

        for item in reversed(matches):

            try:

                data = json.loads(item)

                if isinstance(data, dict):

                    return data


            except json.JSONDecodeError:

                continue



        return None





    def _normalize(
        self,
        data: Dict
    ) -> Dict[str, str]:

        """
        保证输出字段稳定
        """

        reply = str(
            data.get(
                "reply",
                ""
            )
        ).strip()


        emotion = data.get(
            "emotion",
            "neutral"
        )


        intent = str(
            data.get(
                "intent",
                "none"
            )
        ).strip().lower()


        # 情绪归一化：同义词/中文口语/否定前缀 → 标准 6 种
        emotion = normalize_emotion(emotion)


        # 意图归一化：中文映射 + 白名单，非法值归一到 none
        intent = normalize_intent(intent)


        if not reply:

            reply = "嗯，我刚刚有点走神啦。"



        return {

            "reply": reply,

            "emotion": emotion,

            "intent": intent

        }




    def _fallback(self):

        return {

            "reply":
                "我刚刚好像没有理解你的意思。",

            "emotion":
                "neutral",

            "intent":
                "none"

        }
