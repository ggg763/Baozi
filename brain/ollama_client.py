"""
ollama_client.py

包子 Ollama 客户端

职责：
    负责与 Ollama LLM 服务通信

不负责：
    - Prompt构建
    - 对话历史
    - JSON解析
    - 情绪处理
    - 机器人控制

输入：
    messages

输出：
    原始LLM文本
"""


import requests
import time
from typing import List, Dict


def call_ollama(
    messages: List[Dict[str, str]],
    *,
    url: str = "http://localhost:11434/api/chat",
    model: str = "qwen2.5:3b",
    temperature: float = 0.7,
    top_p: float = 0.9,
    timeout: int = 30,
) -> str:
    """Compatibility entry point for the legacy brain's Ollama request.

    ``baozis_brain.py`` does not call this yet.  Keeping this small function
    ready now lets the next change replace only the request implementation,
    while preserving the legacy configuration and its ``str`` return value.
    """
    client = OllamaClient(
        url=url,
        model=model,
        temperature=temperature,
        top_p=top_p,
        timeout=timeout,
    )
    return client.chat(messages)


class OllamaClient:

    def __init__(
        self,
        url: str = "http://localhost:11434/api/chat",
        model: str = "qwen2.5:3b",
        temperature: float = 0.7,
        top_p: float = 0.9,
        timeout: int = 30
    ):
        """
        初始化 Ollama 客户端

        Args:
            url:
                Ollama API地址

            model:
                模型名称

            temperature:
                输出随机性

            top_p:
                核采样参数

            timeout:
                请求超时时间
        """

        self.url = url
        self.model = model
        self.temperature = temperature
        self.top_p = top_p
        self.timeout = timeout


    def chat(
        self,
        messages: List[Dict[str, str]]
    ) -> str:
        """
        调用 Ollama

        Args:
            messages:
            [
                {
                    "role":"system",
                    "content":"..."
                },
                {
                    "role":"user",
                    "content":"..."
                }
            ]

        Returns:
            LLM原始输出文本
        """


        payload = {

            "model": self.model,

            "messages": messages,

            "stream": False,

            "temperature": self.temperature,

            "top_p": self.top_p

        }


        max_retries = 2


        for attempt in range(max_retries):

            try:

                response = requests.post(
                    self.url,
                    json=payload,
                    timeout=self.timeout
                )


                # HTTP错误
                if response.status_code != 200:

                    print(
                        f"[Ollama] HTTP错误: {response.status_code}"
                    )


                    if attempt < max_retries - 1:
                        time.sleep(2)
                        continue

                    return ""


                data = response.json()


                # Ollama首次加载模型
                if (
                    data.get("done")
                    and
                    data.get("done_reason") == "load"
                ):

                    print(
                        "[Ollama] 模型加载中..."
                    )

                    time.sleep(2)
                    continue



                content = (
                    data
                    .get("message", {})
                    .get("content", "")
                    .strip()
                )


                if content:

                    return content



                if attempt < max_retries - 1:

                    time.sleep(2)

                else:

                    return ""



            except requests.exceptions.Timeout:

                print(
                    "[Ollama] 请求超时"
                )

                if attempt < max_retries - 1:

                    time.sleep(2)

                else:

                    return ""



            except Exception as e:

                print(
                    f"[Ollama] 请求异常: {e}"
                )


                if attempt < max_retries - 1:

                    time.sleep(2)

                else:

                    return ""



        return ""


    def test(self) -> bool:
        """
        简单测试 Ollama 是否可用
        """

        try:

            result = self.chat(
                [
                    {
                        "role":"user",
                        "content":"你好"
                    }
                ]
            )

            return bool(result)


        except Exception:

            return False
