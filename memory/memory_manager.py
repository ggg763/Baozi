"""
memory_manager.py

包子的记忆管理模块

职责：

1. 短期对话历史管理
2. 长期用户画像保存
3. 提供上下文
4. 后台记忆压缩


不负责：

- LLM聊天流程
- JSON解析
- 情绪判断
- 硬件控制


接口目标：

MemoryManager

    load()

    save()

    get_context()

    add_message()

    clear_history()

    compress_if_needed()

"""

import json
import os
import threading
import requests
import time
from typing import Dict, List


class MemoryManager:


    def __init__(
        self,
        memory_file: str = "memory.json",
        max_history: int = 20,
        ollama_url: str = "http://localhost:11434/api/chat",
        model_name: str = "qwen2.5:3b"
    ):

        self.memory_file = memory_file

        # 短期记忆轮数
        self.max_history = max_history

        # LLM配置
        self.ollama_url = ollama_url
        self.model_name = model_name


        # 对话历史

        self.conversation_history: List[
            Dict[str,str]
        ] = []


        # 并发保护：后台压缩线程与主线程共享
        # conversation_history 与 user_profile

        self._lock = threading.RLock()


        # 用户长期画像

        self.user_profile = ""


        # 防止重复压缩

        self.compressing = False


        self.load()



    # ==========================
    # 长期记忆
    # ==========================


    def load(self):
        """
        从文件加载长期记忆
        """

        if not os.path.exists(
            self.memory_file
        ):
            return


        try:

            with open(
                self.memory_file,
                "r",
                encoding="utf-8"
            ) as f:

                data = json.load(f)


            with self._lock:

                self.user_profile = data.get(
                    "profile",
                    ""
                )

                if self.user_profile:

                    print(
                        f"🧠 已加载长期记忆: "
                        f"{self.user_profile[:30]}..."
                    )


        except Exception as e:

            print(
                f"⚠️ 加载记忆失败: {e}"
            )



    def save(self):

        """
        保存长期记忆
        """

        try:

            with self._lock:

                profile = self.user_profile


            with open(
                self.memory_file,
                "w",
                encoding="utf-8"
            ) as f:

                json.dump(
                    {
                        "profile":
                            profile
                    },
                    f,
                    ensure_ascii=False,
                    indent=2
                )


        except Exception as e:

            print(
                f"⚠️ 保存记忆失败: {e}"
            )



    def get_profile(self):

        with self._lock:

            return self.user_profile



    def update_profile(
        self,
        profile: str
    ):

        with self._lock:

            self.user_profile = profile

        self.save()



    # ==========================
    # 短期历史
    # ==========================


    def add_message(
        self,
        role: str,
        content: str
    ):

        with self._lock:

            self.conversation_history.append(
                {
                    "role": role,
                    "content": content
                }
            )


    def get_history(self):

        with self._lock:

            return self.conversation_history[
                -self.max_history * 2:
            ]



    def clear_history(self):

        """
        清空短期记忆
        保留长期画像
        """

        with self._lock:

            self.conversation_history = []

        print(
            "🗑️ 短期记忆已清空，长期记忆保留"
        )



    # ==========================
    # Prompt上下文
    # ==========================


    def get_context(self):

        """
        提供给Brain的上下文
        """

        return {

            "profile":
                self.user_profile,

            "history":
                self.get_history()

        }



    # ==========================
    # 自动压缩
    # ==========================


    def need_compress(self):

        with self._lock:

            return len(
                self.conversation_history
            ) > self.max_history * 2



    def compress_async(self):

        """
        后台启动压缩
        """

        if self.compressing:

            return


        self.compressing = True


        thread = threading.Thread(

            target=self._compress_history,

            daemon=True

        )


        thread.start()



    def _compress_history(self):

        try:

            with self._lock:

                if len(
                    self.conversation_history
                ) < 10:

                    return


                history_text = json.dumps(
                    self.conversation_history,
                    ensure_ascii=False
                )


            prompt = f"""
请阅读以下我和用户的对话历史，提取出关于该用户的**核心性格特征、偏好、习惯和长期情感模式**。
请用不超过150字的第三人称中文总结（例如：“用户喜欢科技话题，情绪容易焦虑，需要鼓励...”）。
只输出总结，不要输出JSON，不要有多余的话。

对话历史：
{history_text}
"""


            payload = {

                "model":
                    self.model_name,

                "messages":
                    [
                        {
                            "role":
                                "user",

                            "content":
                                prompt
                        }
                    ],

                "stream":
                    False,

                "temperature":
                    0.3

            }


            response = requests.post(

                self.ollama_url,

                json=payload,

                timeout=60

            )


            if response.status_code == 200:


                data = response.json()


                summary = (

                    data
                    .get(
                        "message",
                        {}
                    )
                    .get(
                        "content",
                        ""
                    )
                    .strip()

                )


                if len(summary) > 10:


                    with self._lock:

                        self.user_profile = summary

                        # 清空短期历史，只保留最近两条消息。

                        self.conversation_history = (
                            self.conversation_history[-2:]
                        )


                    self.save()


                    print(
                        "✅ 长期记忆更新:"
                    )

                    print(summary)



        except Exception as e:

            print(
                f"⚠️ 记忆压缩失败: {e}"
            )


        finally:

            self.compressing = False

