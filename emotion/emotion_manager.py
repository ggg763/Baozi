"""
emotion_manager.py

包子的情绪管理模块

职责：

- 保存当前情绪状态
- 接收LLM情绪结果
- 提供统一情绪接口


不负责：

- 生成情绪
- 调用LLM
- 控制硬件


输入：

    "happy"

输出：

    当前情绪状态


"""


from typing import Dict


class EmotionManager:


    # 支持的基础情绪

    VALID_EMOTIONS = {

        "happy",

        "sad",

        "angry",

        "surprised",

        "loving",

        "neutral"

    }



    def __init__(self):

        # 当前情绪

        self.current_emotion = "neutral"



        # 扩展状态

        # 为以后准备

        self.state = {

            "happiness": 0.5,

            "energy": 0.5,

            "curiosity": 0.5

        }



    def update(
        self,
        emotion: str
    ):
        """
        更新情绪

        Args:

            emotion:
                LLM输出情绪标签

        """

        emotion = self.normalize(emotion)



        self.current_emotion = emotion



        self._update_state(emotion)


    def normalize(
        self,
        emotion: str
    ) -> str:
        """Return a TTS-compatible emotion label for a raw LLM value."""
        if emotion not in self.VALID_EMOTIONS:
            return "neutral"

        return emotion



    def get_emotion(
        self
    ) -> str:

        """
        返回当前情绪标签
        """

        return self.current_emotion



    def get_state(
        self
    ) -> Dict[str,float]:

        """
        返回内部状态
        """

        return self.state.copy()



    def _update_state(
        self,
        emotion: str
    ):

        """
        将离散情绪转换为内部状态

        当前只是简单映射

        后续可以升级为连续情绪模型

        """

        mapping = {


            "happy":

            {
                "happiness":0.9,
                "energy":0.8,
                "curiosity":0.7
            },


            "sad":

            {
                "happiness":0.2,
                "energy":0.3,
                "curiosity":0.4
            },


            "angry":

            {
                "happiness":0.2,
                "energy":0.9,
                "curiosity":0.3
            },


            "surprised":

            {
                "happiness":0.6,
                "energy":1.0,
                "curiosity":0.9
            },


            "loving":

            {
                "happiness":0.8,
                "energy":0.6,
                "curiosity":0.7
            },


            "neutral":

            {
                "happiness":0.5,
                "energy":0.5,
                "curiosity":0.5
            }

        }


        self.state = mapping.get(
            emotion,
            mapping["neutral"]
        )



    def reset(self):

        """
        恢复默认状态
        """

        self.current_emotion = "neutral"

        self.state = {

            "happiness":0.5,

            "energy":0.5,

            "curiosity":0.5

        }
