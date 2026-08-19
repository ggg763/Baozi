"""
brain.py

BaoZi 大脑核心

负责：

- 组织AI推理流程
- 调度各模块


不负责：

- 网络请求
- JSON解析
- 记忆保存
- 情绪计算
- 硬件通信


流程：

user text

 ↓

MemoryManager

 ↓

OllamaClient

 ↓

JsonParser

 ↓

EmotionManager


返回:

{
 reply:"",
 emotion:"",
 intent:""
}

说明：

- 只输出语义（回复文本/情绪/意图），不输出任何动作指令；
- 动作（表情/舵机/LED）由 TD1601 行为层自主决策。

"""


from typing import Dict, List


from brain.ollama_client import OllamaClient

from brain.json_parser import JsonParser

from memory.memory_manager import MemoryManager

from emotion.emotion_manager import EmotionManager



class BaoZiBrain:


    def __init__(
        self,
        system_prompt: str
    ):


        # LLM

        self.llm = OllamaClient()



        # JSON解析

        self.parser = JsonParser()



        # 记忆

        self.memory = MemoryManager()



        # 情绪

        self.emotion = EmotionManager()



        # 人设Prompt

        self.system_prompt = system_prompt




    # ==========================
    # 对外接口
    # ==========================


    def chat(
        self,
        user_input: str
    ) -> Dict[str,str]:

        """
        包子聊天入口

        """



        # 1.
        # 构建消息

        messages = self._build_messages(
            user_input
        )



        # 2.
        # 调用LLM


        raw = self.llm.chat(
            messages
        )



        if not raw:

            return {

                "reply":
                    "呀，我刚刚卡住了一下。",

                "emotion":
                    "neutral",

                "intent":
                    "none"

            }




        # 3.
        # JSON解析


        result = self.parser.parse(
            raw
        )



        # 4.
        # 更新记忆


        self.memory.add_message(

            "user",

            user_input

        )


        self.memory.add_message(

            "assistant",

            result["reply"]

        )




        # 5.
        # 更新情绪


        self.emotion.update(

            result["emotion"]

        )




        # 6.
        # 自动压缩


        if self.memory.need_compress():

            self.memory.compress_async()



        return {


            "reply":

                result["reply"],



            "emotion":

                self.emotion.get_emotion(),



            "intent":

                result["intent"]

        }




    # ==========================
    # Prompt构建
    # ==========================


    def _build_messages(
        self,
        user_input:str
    ) -> List[Dict[str,str]]:


        messages = []


        # system

        system = self.system_prompt



        # 加入长期记忆


        profile = (
            self.memory.get_profile()
        )


        if profile:


            system += f"""


【用户长期记忆】

{profile}

请结合这些信息，
让回复更加自然。


"""



        messages.append(

            {

                "role":
                    "system",

                "content":
                    system

            }

        )



        # 历史

        history = (
            self.memory.get_history()
        )


        messages.extend(
            history
        )



        # 当前输入

        messages.append(

            {

                "role":
                    "user",

                "content":
                    user_input

            }

        )



        return messages




    def clear_history(self):

        """
        清空短期聊天
        """

        self.memory.clear_history()
