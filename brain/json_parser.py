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
        "command": str
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
from typing import Dict, Tuple


def parse_output(raw: str) -> Tuple[str, str, str]:
    """Parse the stable brain's response without changing its behaviour.

    This is deliberately a small extraction of ``baozis_brain._parse_output``.
    Keeping the fallback values and the "last JSON object wins" rule identical
    makes this module safe to introduce while the legacy brain remains active.
    """
    json_pattern = r"\{[^{}]*\}"
    matches = list(re.finditer(json_pattern, raw))
    if matches:
        last_json_str = matches[-1].group()
        try:
            data = json.loads(last_json_str)
            reply = data.get("reply", raw.replace(last_json_str, "").strip())
            emotion = data.get("emotion", "neutral")
            command = data.get("command", "NONE")
            if not reply:
                reply = raw.replace(last_json_str, "").strip()
            return reply, emotion, command
        except json.JSONDecodeError:
            pass
    return raw, "neutral", "NONE"


# 包子允许的情绪列表
VALID_EMOTIONS = {
    "happy",
    "sad",
    "angry",
    "surprised",
    "loving",
    "neutral"
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
            "command":""
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
            "command": "NONE"
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
          "command":"NONE"
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


        command = data.get(
            "command",
            "NONE"
        )


        # 防止模型输出奇怪emotion

        if emotion not in VALID_EMOTIONS:

            emotion = "neutral"



        if not command:

            command = "NONE"



        if not reply:

            reply = "嗯，我刚刚有点走神啦。"



        return {

            "reply": reply,

            "emotion": emotion,

            "command": command

        }




    def _fallback(self):

        return {

            "reply":
                "我刚刚好像没有理解你的意思。",

            "emotion":
                "neutral",

            "command":
                "NONE"

        }
