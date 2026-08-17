# 包子（BaoZi）桌面机器人 — 项目代码讲解

> 由源码自动打包生成（排除 test_* 测试文件），生成时间：2026-08-16 17:42
> 用途：配合 AI 工具阅读本项目代码，快速理解结构与逻辑。

---

## 一、项目概览

**一句话**：一个桌面陪伴机器人，离线语音识别（耳朵）→ 本地大模型推理（大脑）→ 在线语音合成（嘴巴），外加一套"没人理就会无聊/睡觉"的情绪状态机驱动硬件表现。

### 整体管线（单次交互）

```
音频文件
   │
   ▼
baozis_ear.py     耳朵：SenseVoiceSmall ASR，音频 → 文本
   │
   ▼
brain/brain.py    大脑：BaoZiBrain 编排
   │                  ├─ ollama_client.py   调 Ollama(qwen2.5:3b)
   │                  ├─ json_parser.py     解析 {reply, emotion, command}
   │                  ├─ memory/            短期历史 + 长期用户画像(后台压缩)
   │                  └─ emotion/           情绪标签归一化
   │
   ├──────────────┬─────────────────────────┐
   ▼              ▼                         ▼
baozis_mouth.py  emotion/state_machine.py  robot/command_mapper.py
嘴巴：edge-tts   情绪状态机(心跳线程)      命令 → 标准动作字典
TTS 语音          IDLE/ACTIVE/BORED/SLEEPING
   │              │                         │
   ▼              ▼                         ▼
 播放语音      robot/hardware_interface    robot_action 交给硬件层
               RobotHardware 契约
               ├─ FakeTD1601   模拟(默认)
               └─ VisualTD1601 虚拟窗口(--gui)
```

### 技术栈

| 部分 | 技术 |
|---|---|
| 语音识别 | funasr + SenseVoiceSmall（本地，GPU/CPU） |
| 大脑 | Ollama + qwen2.5:3b（本地，HTTP API） |
| 语音合成 | edge-tts（微软在线） |
| 硬件层 | 抽象基类 + FakeTD1601 模拟 + VisualTD1601(Tkinter) 虚拟 |
| 并发 | 状态机心跳线程、记忆压缩线程（RLock 保护共享数据） |

### 线程模型

- **主线程**：音频管线（识别→推理→合成）；
- **心跳线程**（状态机）：每 5 秒检查空闲时长，触发 BORED/SLEEPING；
- **压缩线程**（记忆）：历史超 40 条时后台把对话总结为长期画像。

---

## 二、目录结构

```
Baozi_PC/
├── brain/
│   ├── __init__.py
│   ├── brain.py
│   ├── json_parser.py
│   ├── ollama_client.py
├── memory/
│   ├── memory_manager.py
├── emotion/
│   ├── emotion_manager.py
│   ├── state_machine.py
├── robot/
│   ├── command_mapper.py
│   ├── hardware_interface.py
│   ├── visual_robot.py
├── prompts/
│   ├── system_prompt.txt
├── baozis_ear.py
├── baozis_mouth.py
├── config.py
├── main.py
```

---

## 三、代码详解

（每个文件：职责说明 + 完整源码。顺序即依赖/阅读顺序。）

### 1. main.py — 主控入口

包子的主程序（命令行版）。流程：音频文件 → 耳朵(ASR) → 大脑(LLM) → 嘴巴(TTS)，并把 LLM 情绪喂给状态机驱动硬件。
关键点：
- `--audio` 指定音频文件，`--gui` 启动 Tkinter 虚拟机器人窗口（VisualTD1601），默认用 FakeTD1601 模拟硬件；
- `warmup()` 预热耳朵/大脑/嘴巴三个模块，避免首次调用卡顿；
- `process_audio_file()` 是单次交互管线：识别 → 思考 → 状态机驱动表情 → 语音合成；
- `brain` 是模块级全局的 BaoZiBrain 实例，`state_machine` 随每次运行创建并在 `finally` 中 shutdown。

```python
"""
main.py - 包子主控程序（本地测试版）
功能：接收音频文件 → ASR识别 → LLM推理 → TTS合成 → 状态机驱动表情动作

用法：
    python main.py                  # 默认：模拟硬件（FakeTD1601），处理 test.wav
    python main.py --audio a.wav    # 指定音频文件
    python main.py --gui            # 启动虚拟机器人窗口（VisualTD1601，Tkinter）
"""
import os
import wave
import time
import argparse
from baozis_ear import baozis_ear
from baozis_mouth import baozis_mouth
from brain.brain import BaoZiBrain
from emotion.state_machine import MoodStateMachine
from robot.hardware_interface import FakeTD1601

# ---------- 初始化新版大脑 ----------
_PROMPT_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "prompts",
    "system_prompt.txt",
)

def _load_system_prompt() -> str:
    """读取包子人设提示词（prompts/system_prompt.txt）"""
    try:
        with open(_PROMPT_FILE, "r", encoding="utf-8") as f:
            return f.read().strip()
    except Exception as e:
        print(f"⚠️ 读取人设提示词失败: {e}")
        return ""

brain = BaoZiBrain(system_prompt=_load_system_prompt())

def _create_hardware(gui: bool):
    """
    创建硬件接口：
    - gui=True  → VisualTD1601（Tkinter 虚拟窗口）
    - 否则      → FakeTD1601（打印式模拟，无 GUI 依赖）
    """
    if gui:
        try:
            from robot.visual_robot import VisualTD1601
            return VisualTD1601()
        except Exception as e:
            print(f"⚠️ 虚拟窗口启动失败，回退到模拟硬件: {e}")
    return FakeTD1601()

def warmup():
    """
    预热三个核心模块，避免首次调用卡顿。
    """
    print("🔥 包子正在热身，请稍候...")
    start_time = time.time()
    
    # ---------- 1. 预热耳朵 ----------
    print("  📡 加载语音识别模型...")
    try:
        # 生成一段1秒的静音音频（16kHz, 单声道, 16-bit PCM）
        silent_wav = "temp/warmup_silent.wav"
        os.makedirs("temp", exist_ok=True)
        with wave.open(silent_wav, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)      # 16-bit = 2字节
            wf.setframerate(16000)
            # 1秒静音帧（16000个采样，每个采样2字节全0）
            wf.writeframes(b'\x00\x00' * 16000)
        
        from baozis_ear import baozis_ear
        _ = baozis_ear(silent_wav)
        os.remove(silent_wav)  # 用完即删
        print("  ✅ 耳朵已就绪")
    except Exception as e:
        print(f"  ⚠️ 耳朵预热失败: {e}")

    # ---------- 2. 预热大脑 ----------
    print("  🧠 加载语言模型...")
    try:
        brain.clear_history()          # 确保干净的上下文
        _ = brain.chat("预热")         # 发送一个简短请求
        print("  ✅ 大脑已就绪")
    except Exception as e:
        print(f"  ⚠️ 大脑预热失败: {e}")

    # ---------- 3. 预热嘴巴 ----------
    print("  🔊 初始化语音合成...")
    try:
        from baozis_mouth import baozis_mouth
        _ = baozis_mouth("预热", emotion="neutral")  # 合成一个短句
        # 生成的临时文件可以保留（下次可能复用），也可以删除
        # 这里保留，因为以后可能用到相同文本的缓存
        print("  ✅ 嘴巴已就绪")
    except Exception as e:
        print(f"  ⚠️ 嘴巴预热失败: {e}")

    elapsed = time.time() - start_time
    print(f"🔥 热身完成！耗时 {elapsed:.1f} 秒")

def process_audio_file(audio_path: str, clear_context: bool = True, state_machine: MoodStateMachine = None) -> dict:
    """
    处理单个音频文件，返回完整交互结果。
    
    参数:
        audio_path: 音频文件路径（必须是16kHz单声道WAV格式）
        clear_context: 是否清空对话历史（默认True，新会话）
        state_machine: 情绪状态机（可选）。传入时会把 LLM 情绪喂给
            update_interaction()，驱动表情/LED/舵机动作。
    
    返回:
        dict: {
            "user_text": 识别出的用户文本,
            "reply": 包子的回复文本,
            "emotion": 情感标签,
            "command": 控制指令,
            "robot_action": 结构化机器人动作（新版大脑输出）,
            "audio_file": 生成的MP3文件路径
        }
    """
    if clear_context:
        brain.clear_history()
        print("[Main] 已清空对话历史。")
    
    # 1. 语音识别（耳朵）
    print(f"[Main] 正在识别音频: {audio_path}")
    user_text = baozis_ear(audio_path)
    if not user_text:
        print("[Main] 警告：语音识别为空，请检查音频文件。")
        return {
            "user_text": "",
            "reply": "我没听清你说什么。",
            "emotion": "neutral",
            "command": "NONE",
            "audio_file": None
        }
    print(f"[用户] {user_text}")
    
    # 2. 大脑推理
    print("[Main] 正在思考...")
    brain_result = brain.chat(user_text)
    reply = brain_result["reply"]
    emotion = brain_result["emotion"]
    command = brain_result["command"]
    robot_action = brain_result.get("robot_action", {})
    print(f"[包子] {reply} (情感: {emotion}, 指令: {command}, 动作: {robot_action})")
    
    # 2.5 驱动状态机：表情/LED/舵机随情绪变化
    if state_machine is not None:
        state_machine.update_interaction(emotion)
    
    # 3. 语音合成（嘴巴）
    print("[Main] 正在合成语音...")
    audio_file = baozis_mouth(reply, emotion=emotion)
    print(f"[Main] 音频已生成: {audio_file}")
    
    return {
        "user_text": user_text,
        "reply": reply,
        "emotion": emotion,
        "command": command,
        "robot_action": robot_action,
        "audio_file": audio_file
    }

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="包子主控程序")
    parser.add_argument("--audio", default="test.wav", help="音频文件路径（16kHz单声道WAV）")
    parser.add_argument("--gui", action="store_true", help="启动虚拟机器人窗口（VisualTD1601）")
    args = parser.parse_args()

    # 0. 初始化硬件 + 状态机
    hardware = _create_hardware(gui=args.gui)
    state_machine = MoodStateMachine(hardware)
    print("[Main] 状态机已启动。")

    try:
        # 1. 执行预热
        warmup()
        
        # 2. 然后正常处理用户请求
        if not os.path.exists(args.audio):
            print(f"错误：找不到测试音频文件 {args.audio}")
        else:
            result = process_audio_file(args.audio, clear_context=True, state_machine=state_machine)
            print("\n========== 最终结果 ==========")
            print(f"用户说: {result['user_text']}")
            print(f"包子说: {result['reply']}")
            print(f"情感: {result['emotion']}")
            print(f"指令: {result['command']}")
            print(f"动作: {result.get('robot_action')}")
            print(f"音频文件: {result['audio_file']}")

        # 3. GUI 模式：进入窗口主循环（阻塞），可观察状态机随时间的表情变化
        if args.gui:
            print("[Main] 虚拟窗口运行中，关闭窗口后退出...")
            hardware.run()
    finally:
        state_machine.shutdown()
        print("[Main] 状态机已关闭。")
```


### 2. baozis_ear.py — 耳朵（语音识别）

包子的"耳朵"：封装阿里 SenseVoiceSmall 语音识别模型（funasr）。
关键点：
- 模型懒加载 + 全局复用（`_MODEL`），GPU 可用时用 cuda，否则 CPU；
- 识别结果会带 `<|zh|><|NEUTRAL|>...` 等元数据标签，用正则剥离只留纯文本；
- 输入支持 WAV/MP3/M4A 等格式。

```python
"""
baozis_ear.py - 包子的耳朵
封装 SenseVoiceSmall 语音识别功能，供主程序随时调用
"""

import re
import torch
from funasr import AutoModel

# 全局变量：模型只加载一次
_MODEL = None

def _get_model():
    """懒加载模型：首次调用时加载，后续复用"""
    global _MODEL
    if _MODEL is None:
        print("👂 包子正在竖起耳朵... (加载语音识别模型)")
        _MODEL = AutoModel(
            model="iic/SenseVoiceSmall",
            trust_remote_code=True,
            device = "cuda:0" if torch.cuda.is_available() else "cpu",
            disable_update=True
        )
        print("✅ 包子已经准备好听你说话啦！")
    return _MODEL

def baozis_ear(audio_path: str, language: str = "zh", use_itn: bool = True) -> str:
    """
    包子的耳朵 - 语音识别

    Args:
        audio_path: 音频文件路径（支持 WAV, MP3, M4A 等）
        language: 语言，可选 "zh"(中文), "en"(英文), "yue"(粤语)
        use_itn: 是否启用逆文本正则化（如将"一百"转为"100"）

    Returns:
        识别出的纯文本字符串（已去除元数据标签）
    """
    model = _get_model()
    
    # 调用模型识别
    res = model.generate(
        input=audio_path,
        language=language,
        use_itn=use_itn,
    )
    
    # 提取原始文本（包含标签如 <|zh|><|NEUTRAL|>...）
    raw_text = res[0]['text']
    
    # 去除所有 <...> 元数据标签，只保留纯净文本
    clean_text = re.sub(r'<[^>]+>', '', raw_text)
    
    return clean_text.strip()
```


### 3. baozis_mouth.py — 嘴巴（语音合成）

包子的"嘴巴"：微软 edge-tts 在线语音合成（无需本地模型）。
关键点：
- 按 emotion 调整语速(rate)和音调(pitch)，如 happy 快+5Hz、sad 慢-5Hz；
- 输出 MP3 到 temp/ 目录，默认女声 zh-CN-XiaoyiNeural；
- 注意：缓存文件名用 `hash(text)`，Python 字符串 hash 每次进程随机，实际缓存不生效（已知小 bug）。

```python
import asyncio
import edge_tts
import os

# ---------- 配置 ----------
DEFAULT_VOICE = "zh-CN-XiaoyiNeural"  # 女声，也可选 zh-CN-YunxiNeural (男声)
OUTPUT_DIR = "temp"
os.makedirs(OUTPUT_DIR, exist_ok=True)

# 情感到语速/音调的映射
EMOTION_RATE = {
    "happy": "+10%",
    "sad": "-10%",
    "angry": "+5%",
    "surprised": "+15%",
    "loving": "+0%",
    "neutral": "+0%"
}
EMOTION_PITCH = {
    "happy": "+5Hz",
    "sad": "-5Hz",
    "angry": "+3Hz",
    "surprised": "+8Hz",
    "loving": "+2Hz",
    "neutral": "-0Hz"
}

async def _tts_async(text: str, voice: str = DEFAULT_VOICE, rate: str = "+0%", pitch: str = "0Hz") -> str:
    """异步生成 TTS 音频，返回输出文件路径"""
    output_file = os.path.join(OUTPUT_DIR, f"output_{hash(text)}.mp3")
    if os.path.exists(output_file):
        return output_file
    communicate = edge_tts.Communicate(text, voice, rate=rate, pitch=pitch)
    await communicate.save(output_file)
    return output_file

def baozis_mouth(text: str, emotion: str = "neutral", voice: str = DEFAULT_VOICE) -> str:
    """
    同步入口：生成语音文件，返回文件路径。
    emotion 用于调整语速和音调。
    """
    rate = EMOTION_RATE.get(emotion, "+0%")
    pitch = EMOTION_PITCH.get(emotion, "0Hz")
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        file_path = loop.run_until_complete(_tts_async(text, voice, rate, pitch))
        return file_path
    finally:
        loop.close()
```


### 4. brain/__init__.py — 包标记

brain 包的显式包标记，保证从项目根目录启动时导入确定。

```python
"""BaoZi brain support modules.

The explicit package marker keeps imports deterministic when the project is
started from its root directory.
"""

```


### 5. brain/brain.py — 大脑编排核心

新版大脑核心 `BaoZiBrain`，只负责"编排"，不碰网络/解析/记忆/硬件细节。
`chat()` 的完整流水线：
1. `_build_messages()` 组装 system(人设+长期记忆) + 历史 + 当前输入；
2. `llm.chat()` 调 Ollama 拿原始输出；
3. `parser.parse()` 解析出 {reply, emotion, command}（解析失败有兜底文案）；
4. `memory.add_message()` 把 user/assistant 两条写入短期历史；
5. `emotion.update()` 更新情绪；
6. `command.map()` 把 LLM 命令转成结构化 `robot_action`；
7. `memory.need_compress()` 历史超阈值时后台压缩长期记忆。
返回：{reply, emotion, command, robot_action}。

```python
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

 ↓

CommandMapper


返回:

{
 reply:"",
 emotion:"",
 command:"",
 robot_action:""
}

"""


from typing import Dict, List


from brain.ollama_client import OllamaClient

from brain.json_parser import JsonParser

from memory.memory_manager import MemoryManager

from emotion.emotion_manager import EmotionManager

from robot.command_mapper import CommandMapper



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



        # 动作

        self.command = CommandMapper()



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

                "command":
                    "NONE",

                "robot_action":
                    {
                        "type":
                            "none"
                    }

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
        # 转换动作


        action = self.command.map(

            result["command"]

        )



        # 7.
        # 自动压缩


        if self.memory.need_compress():

            self.memory.compress_async()



        return {


            "reply":

                result["reply"],



            "emotion":

                self.emotion.get_emotion(),



            "command":

                result["command"],



            "robot_action":

                action

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

```


### 6. brain/json_parser.py — JSON 解析器

`JsonParser`：把 LLM 的原始输出解析成稳定结构，三级降级：
1. 直接 `json.loads`；
2. 正则 `{[^{}]*}` 提取最后一个 JSON 对象（模型有时先解释再输出）；
3. 全失败 → 整段文本当 reply + neutral + NONE。
`_normalize()` 还会把非法 emotion 归一到 neutral、空 reply 给兜底文案。

```python
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
from typing import Dict


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

```


### 7. brain/ollama_client.py — Ollama 客户端

`OllamaClient`：与本地 Ollama 服务通信的网络层。
- 默认 `http://localhost:11434/api/chat`，模型 `qwen2.5:3b`，temperature 0.7 / top_p 0.9；
- 重试 2 次；Ollama 首次加载模型（done_reason=="load"）时等待 2 秒重试；
- `test()` 用来做连通性冒烟测试。

```python
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

```


### 8. memory/memory_manager.py — 记忆管理

`MemoryManager`：短期对话历史 + 长期用户画像 + 后台压缩。
- 短期历史：`add_message()/get_history()/clear_history()`，窗口 `max_history*2` 条（默认 40）；
- 长期画像：存在 memory.json，启动时 load，压缩时更新；
- 自动压缩：历史 > 40 条时 `compress_async()` 起后台线程，用低温度(0.3)让 LLM 把对话总结成 ≤150 字用户画像，然后清空短期历史只留最近 2 条；
- 线程安全：`self._lock = threading.RLock()` 保护 conversation_history 和 user_profile 的所有读写（后台压缩线程与主线程并发）。

```python
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


```


### 9. emotion/emotion_manager.py — 情绪管理

`EmotionManager`：LLM 情绪标签的归一化与内部状态。
- `VALID_EMOTIONS` 白名单：happy/sad/angry/surprised/loving/neutral，非法值归一到 neutral；
- 除离散标签外，维护连续状态 {happiness, energy, curiosity}（0~1），由 `_update_state()` 按情绪映射——为将来升级连续情绪模型预留的雏形。

```python
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

```


### 10. emotion/state_machine.py — 情绪状态机

`MoodStateMachine`：包子的"心情状态机"，决定没人理时它会怎样。
四个状态（BaoZiMood 枚举）：
- IDLE：启动初始态/待机基准（保持 neutral 表情 + white LED，只驱动一次）；
- ACTIVE：刚互动完（表情/LED/舵机小动作随情绪变化）；
- BORED：20~30 秒没人理 → 无聊表情 + 歪头 + 灯变暗；
- SLEEPING：>30 秒没人理 → 睡觉表情 + 低头 + 灯变暗（只触发一次）。

机制：
- 后台心跳线程每 5 秒检查一次空闲时长，`update_interaction()` 由主程序在每次对话后调用以重置时间戳并打断无聊/睡眠；
- `wake_up()` 供外部传感器（如触摸）手动唤醒；
- 所有硬件动作通过 `RobotHardware` 接口发出，虚拟/模拟/真机通用。

```python
# emotion/state_machine.py
import time
import threading
from enum import Enum

class BaoZiMood(Enum):
    ACTIVE = "active"    # 刚互动完，精神好
    IDLE = "idle"        # 等了一会儿
    BORED = "bored"      # 等久了，无聊
    SLEEPING = "sleeping" # 彻底待机

class MoodStateMachine:
    def __init__(self, hardware):
        """
        参数:
            hardware: 你的硬件接口 (可以是 VisualTD1601 或 FakeTD1601)
        """
        self.hardware = hardware
        self.current_mood = BaoZiMood.IDLE
        self.last_interaction_time = time.time()
        self.is_sleeping = False
        # IDLE 待机基准外观是否已应用（避免心跳重复驱动硬件）
        self._idle_applied = False
        
        # 后台线程控制 (用于定期检查空闲状态)
        self.running = True
        self.heartbeat_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self.heartbeat_thread.start()
        
        # 初始化时，让包子进入待机表情
        self.hardware.set_expression("neutral")
        self.hardware.set_led("white")
        print("🧠 [状态机] 已启动，后台心跳线程运行中...")

    def update_interaction(self, emotion: str):
        """
        当用户和包子说话、LLM返回结果时，调用此方法。
        传入 LLM 给出的情绪 (happy/sad/angry等)
        """
        # 1. 重置时间戳 (打断无聊/睡眠)
        self.last_interaction_time = time.time()
        self.is_sleeping = False
        self.current_mood = BaoZiMood.ACTIVE
        
        # 2. 根据情绪映射到硬件显示
        # 表情
        self.hardware.set_expression(emotion)
        
        # LED颜色映射
        led_map = {
            "happy": "green",
            "loving": "purple",
            "sad": "blue",
            "angry": "red",
            "surprised": "yellow",
            "neutral": "white"
        }
        self.hardware.set_led(led_map.get(emotion, "white"))
        
        # 3. 情绪对应的“小动作” (增加生动感)
        if emotion == "happy":
            self.hardware.set_servo(1, 110, duration=0.3)  # 开心时向右偏头
            time.sleep(0.2)
            self.hardware.set_servo(1, 90, duration=0.3)
        elif emotion == "surprised":
            self.hardware.set_servo(2, 70, duration=0.2)  # 惊讶时向后仰
            time.sleep(0.3)
            self.hardware.set_servo(2, 90, duration=0.3)
        # （其他小动作你可以自己扩充）

    def _heartbeat_loop(self):
        """
        后台循环：每 5 秒检查一次，如果太久没理包子，触发无聊/睡觉。
        """
        while self.running:
            time.sleep(5)  # 检测间隔
            
            # 如果已经睡着了，跳过后续逻辑（保持睡姿）
            if self.is_sleeping:
                continue
                
            idle_seconds = time.time() - self.last_interaction_time
            
            # ----- 状态切换逻辑 -----
            if idle_seconds > 20 and idle_seconds <= 30:
                # 20~30秒没互动：进入无聊状态
                if self.current_mood != BaoZiMood.BORED:
                    self.current_mood = BaoZiMood.BORED
                    print("😐 [状态机] 20秒没理我，有点无聊了...")
                    self.hardware.set_expression("bored")
                    self.hardware.set_servo(1, 75, duration=1.0)  # 歪头
                    self.hardware.set_servo(2, 85, duration=1.0)
                    self.hardware.set_led("dim")
                    
            elif idle_seconds > 30:
                # 超过30秒：进入睡眠状态 (只触发一次)
                if not self.is_sleeping:
                    self.is_sleeping = True
                    self.current_mood = BaoZiMood.SLEEPING
                    print("💤 [状态机] 超过30秒没理我，我要睡觉了...")
                    self.hardware.set_expression("sleep")
                    self.hardware.set_servo(1, 90, duration=0.5)  # 头回正
                    self.hardware.set_servo(2, 95, duration=1.0)  # 低头（垂眼）
                    self.hardware.set_led("dim")
            else:
                # 刚互动完不久（idle <= 20s），确保状态是正常的
                if self.current_mood == BaoZiMood.ACTIVE:
                    # 刚互动完但还没过20秒：保持活跃，什么都不做
                    pass
                elif self.current_mood == BaoZiMood.IDLE:
                    # IDLE 是启动初始态/待机基准态：保持中性表情与常亮白光。
                    # 只驱动一次，避免心跳每 5 秒重复调用硬件。
                    if not self._idle_applied:
                        self._idle_applied = True
                        self.hardware.set_expression("neutral")
                        self.hardware.set_led("white")
                elif self.current_mood == BaoZiMood.BORED and idle_seconds < 20:
                    # 如果用户突然在无聊的时候说话了，update_interaction 会被调用重置状态
                    # 这里是兜底：如果 idle 突然减少（其实不会发生，因为 update 会重置），以防万一。
                    pass

    def wake_up(self):
        """
        手动唤醒（例如触摸传感器触发时调用）
        """
        if self.is_sleeping:
            print("☀️ [状态机] 被唤醒了！")
            self.is_sleeping = False
            self.current_mood = BaoZiMood.ACTIVE
            self.last_interaction_time = time.time()
            self.hardware.set_expression("happy")
            self.hardware.set_led("green")
            self.hardware.set_servo(1, 90, duration=0.5)
            self.hardware.set_servo(2, 90, duration=0.5)
            # 可以加一句欢迎语，但这里交给主程序TTS去做

    def shutdown(self):
        """程序退出时调用"""
        self.running = False
```


### 11. robot/command_mapper.py — 命令映射

`CommandMapper`：把 LLM 输出的命令字符串转成标准机器人动作字典，并做合法性校验。
- 白名单：SERVO_NOD / SERVO_SHAKE / SERVO_H / SERVO_V / SCREEN / LED / NONE；
- 例："SERVO_NOD 2" → {type:"servo", action:"nod", value:2}；"LED red" → {type:"led", color:"red"}；
- 未知命令/非法数字自动降级（none / 默认值），保证硬件层永远收到合法指令。

```python
"""
command_mapper.py

包子机器人动作映射模块


职责：

- 接收LLM输出动作
- 校验动作合法性
- 转换为机器人执行指令


不负责：

- LLM生成
- 舵机控制
- UART通信
- ESP32通信


输入：

    command字符串


输出：

    标准机器人动作字典


"""


from typing import Dict


class CommandMapper:



    # 当前支持动作

    ALLOWED_COMMANDS = {


        "SERVO_NOD",

        "SERVO_SHAKE",

        "SERVO_H",

        "SERVO_V",

        "SCREEN",

        "LED",

        "NONE"

    }



    def __init__(self):

        pass



    def map(
        self,
        command: str
    ) -> Dict:
        """
        将LLM命令转换成标准格式


        输入:

        "SERVO_NOD 1"


        输出:

        {
            "type":"servo",
            "action":"nod",
            "value":1
        }

        """


        if not command:

            return self.none()



        command = command.strip()



        parts = command.split()



        action = parts[0]



        if action not in self.ALLOWED_COMMANDS:

            return self.none()



        # ======================
        # 舵机动作
        # ======================

        if action == "SERVO_NOD":


            count = self._number(
                parts,
                default=1
            )


            return {

                "type":
                    "servo",

                "action":
                    "nod",

                "value":
                    count

            }



        if action == "SERVO_SHAKE":


            count = self._number(
                parts,
                default=1
            )


            return {

                "type":
                    "servo",

                "action":
                    "shake",

                "value":
                    count

            }




        if action == "SERVO_H":


            angle = self._number(
                parts,
                default=90
            )


            return {

                "type":
                    "servo",

                "action":
                    "horizontal",

                "value":
                    angle

            }




        if action == "SERVO_V":


            angle = self._number(
                parts,
                default=90
            )


            return {

                "type":
                    "servo",

                "action":
                    "vertical",

                "value":
                    angle

            }



        # ======================
        # 屏幕
        # ======================

        if action == "SCREEN":


            expression = (

                parts[1]

                if len(parts)>1

                else

                "neutral"

            )


            return {


                "type":

                    "screen",


                "expression":

                    expression


            }




        # ======================
        # LED
        # ======================

        if action == "LED":


            color = (

                parts[1]

                if len(parts)>1

                else

                "white"

            )


            return {


                "type":

                    "led",


                "color":

                    color


            }




        return self.none()



    def none(self):

        """
        空动作
        """

        return {

            "type":

                "none"

        }




    def _number(
        self,
        parts,
        default=1
    ):

        """
        安全数字解析
        """

        if len(parts)<2:

            return default



        try:

            return int(
                parts[1]
            )


        except ValueError:

            return default
```


### 12. robot/hardware_interface.py — 硬件抽象层

硬件统一契约，上层（状态机/主程序）不关心具体实现。
- `RobotHardware`（abc）：set_servo / set_expression / set_led / reset_pose / run 五个接口；
- `FakeTD1601`：打印式模拟硬件，无 GUI 依赖，命令行/测试用（舵机限幅 0~180，servo 1=水平 2=垂直）。

```python
"""
robot/hardware_interface.py

包子硬件接口抽象层

职责：
- 定义统一硬件契约（RobotHardware）
- 提供无 GUI 依赖的模拟实现（FakeTD1601），供命令行/测试使用

VisualTD1601（虚拟窗口）与真实 TD1601 驱动都应继承 RobotHardware，
这样上层（状态机、主程序）不关心具体硬件实现。

接口契约：
    set_servo(servo_id, angle, duration)   舵机控制（1=水平, 2=垂直）
    set_expression(emotion)                表情显示
    set_led(color)                         LED 颜色
    reset_pose()                           复位姿态
    run()                                  阻塞式主循环（GUI 才需要）
"""

import abc


class RobotHardware(abc.ABC):

    @abc.abstractmethod
    def set_servo(
        self,
        servo_id: int,
        angle: int,
        duration: float = 0.5
    ):
        """控制舵机

        Args:
            servo_id: 1=水平, 2=垂直
            angle: 0~180
            duration: 动作时长（秒）
        """
        raise NotImplementedError

    @abc.abstractmethod
    def set_expression(
        self,
        emotion: str
    ):
        """切换表情（happy/sad/angry/surprised/loving/neutral/bored/sleep）"""
        raise NotImplementedError

    @abc.abstractmethod
    def set_led(
        self,
        color: str
    ):
        """切换 LED 颜色（red/green/blue/yellow/white/purple/dim）"""
        raise NotImplementedError

    def reset_pose(self):
        """复位姿态（默认空实现，硬件可实现覆盖）"""
        pass

    def run(self):
        """阻塞式主循环（GUI 模式需要；模拟硬件默认为空）"""
        pass


class FakeTD1601(RobotHardware):
    """
    打印式模拟硬件：无 Tkinter 依赖，供命令行、测试、无 GUI 环境使用。
    行为与 VisualTD1601 一致（属性 + 打印），方便随时切换。
    """

    def __init__(self):
        self.servo_h = 90   # 水平 (0-180)
        self.servo_v = 90   # 垂直 (0-180)
        self.current_emotion = "neutral"
        self.led_color = "white"

    def set_servo(self, servo_id: int, angle: int, duration: float = 0.5):
        angle = max(0, min(180, angle))
        if servo_id == 1:
            self.servo_h = angle
        elif servo_id == 2:
            self.servo_v = angle
        print(f"🔄 [模拟舵机{servo_id}] -> {angle}°")

    def set_expression(self, emotion: str):
        self.current_emotion = emotion
        print(f"🎭 [模拟表情] -> {emotion}")

    def set_led(self, color: str):
        self.led_color = color
        print(f"💡 [模拟LED] -> {color}")

    def reset_pose(self):
        self.servo_h = 90
        self.servo_v = 90
        self.current_emotion = "neutral"
        self.led_color = "white"
        print("↩️ [模拟复位]")

```


### 13. robot/visual_robot.py — 虚拟机器人（Tkinter）

`VisualTD1601`：用 Tkinter 画出来的虚拟"包子"，纯软件模拟机器人外观。
- 画脸/身体/眼睛/嘴巴/LED，按表情画不同嘴型，舵机角度驱动眼球偏移；
- 50ms 刷新一帧；`set_servo/set_expression/set_led` 与 FakeTD1601 行为一致（限幅、颜色映射），便于两种实现无缝切换；
- `run()` 进入 Tk 主循环（阻塞），供 `python main.py --gui` 使用。

```python
# robot/visual_robot.py
import tkinter as tk
import math
from robot.hardware_interface import RobotHardware

class VisualTD1601(RobotHardware):
    def __init__(self, window_size=400):
        self.root = tk.Tk()
        self.root.title("🥟 包子虚拟机器人 (硬件模拟器)")
        self.canvas = tk.Canvas(self.root, width=window_size, height=window_size, bg="#2c3e50")
        self.canvas.pack()
        
        self.window_size = window_size
        self.center = window_size // 2
        self.radius = 120  # 脸的大小
        
        # 初始状态
        self.servo_h = 90   # 水平 (0-180)
        self.servo_v = 90   # 垂直 (0-180)
        self.current_emotion = "neutral"
        self.led_color = "#ffffff"
        
        # 绘制静态脸部轮廓和身体
        self._draw_base()
        
        # 启动定时刷新 (20ms刷新一次)
        self._update_loop()
        
    def _draw_base(self):
        # 画脸 (白色圆形)
        self.face = self.canvas.create_oval(
            self.center - self.radius, self.center - self.radius,
            self.center + self.radius, self.center + self.radius,
            fill="#fce4ec", outline="#e57373", width=4
        )
        # 画小身体 (梯形)
        body_points = [
            self.center - 60, self.center + self.radius,
            self.center + 60, self.center + self.radius,
            self.center + 100, self.center + self.radius + 60,
            self.center - 100, self.center + self.radius + 60,
        ]
        self.body = self.canvas.create_polygon(body_points, fill="#ffab91", outline="#bf6a4a", width=3)
        
        # 眼睛和嘴巴占位（在_update里重绘）
        self.eye_left = None
        self.eye_right = None
        self.mouth = None
        self.led_indicator = None
        
        # 状态文字
        self.status_text = self.canvas.create_text(
            self.center, 30, 
            text="🤖 等待唤醒...", 
            fill="white", font=("Arial", 14, "bold")
        )

    def _update_loop(self):
        """每帧刷新表情、舵机、LED"""
        self._draw_face()
        self.root.after(50, self._update_loop)  # 50ms刷新一次
        
    def _draw_face(self):
        # 1. 清除之前动态绘制的元素 (眼睛、嘴巴、LED)
        if self.eye_left: self.canvas.delete(self.eye_left)
        if self.eye_right: self.canvas.delete(self.eye_right)
        if self.mouth: self.canvas.delete(self.mouth)
        if self.led_indicator: self.canvas.delete(self.led_indicator)
        
        # 2. 计算眼睛位置 (舵机控制眼球偏移)
        # 水平 0~180 -> -30 ~ +30 像素偏移
        offset_x = (self.servo_h - 90) / 90 * 30
        offset_y = (self.servo_v - 90) / 90 * 20  # 垂直幅度小一点
        
        eye_y_base = self.center - 20
        eye_spacing = 40
        
        # 左眼
        lx = self.center - eye_spacing + offset_x
        ly = eye_y_base + offset_y
        self.eye_left = self.canvas.create_oval(lx-15, ly-15, lx+15, ly+15, fill="white", outline="black", width=2)
        self.canvas.create_oval(lx-5, ly-5, lx+5, ly+5, fill="black")  # 瞳孔
        
        # 右眼
        rx = self.center + eye_spacing + offset_x
        ry = eye_y_base + offset_y
        self.eye_right = self.canvas.create_oval(rx-15, ry-15, rx+15, ry+15, fill="white", outline="black", width=2)
        self.canvas.create_oval(rx-5, ry-5, rx+5, ry+5, fill="black")  # 瞳孔
        
        # 3. 嘴巴 (根据情绪)
        mouth_y = self.center + 30
        mouth_w = 40
        if self.current_emotion == "happy":
            self.mouth = self.canvas.create_arc(
                self.center-mouth_w, mouth_y-10, self.center+mouth_w, mouth_y+30,
                start=0, extent=-180, style=tk.CHORD, fill="#e57373"
            )
        elif self.current_emotion == "sad":
            self.mouth = self.canvas.create_arc(
                self.center-mouth_w, mouth_y-10, self.center+mouth_w, mouth_y+30,
                start=0, extent=180, style=tk.CHORD, fill="#e57373"
            )
        elif self.current_emotion == "surprised":
            self.mouth = self.canvas.create_oval(
                self.center-15, mouth_y-5, self.center+15, mouth_y+25, fill="#e57373"
            )
        elif self.current_emotion == "angry":
            # 愤怒的波浪嘴 (简单画一条折线)
            pts = [self.center-40, mouth_y, self.center-20, mouth_y-20, self.center, mouth_y, self.center+20, mouth_y-20, self.center+40, mouth_y]
            self.mouth = self.canvas.create_line(pts, fill="#c62828", width=4, smooth=True)
        else:  # neutral / loving / bored
            self.mouth = self.canvas.create_line(
                self.center-30, mouth_y, self.center+30, mouth_y, fill="#e57373", width=4
            )
            
        # 4. LED指示灯 (在头顶)
        led_x = self.center
        led_y = self.center - self.radius - 20
        self.led_indicator = self.canvas.create_oval(
            led_x-15, led_y-15, led_x+15, led_y+15, 
            fill=self.led_color, outline="white", width=2
        )
        
        # 5. 更新顶部状态文字
        emotion_names = {
            "happy": "开心 😊", "sad": "伤心 😢", "angry": "生气 😡",
            "surprised": "惊讶 😲", "loving": "爱意 ❤️", "neutral": "平静 🙂",
            "bored": "无聊 😑", "sleep": "睡觉 💤"
        }
        self.canvas.itemconfig(
            self.status_text, 
            text=f"情绪: {emotion_names.get(self.current_emotion, '?')}  |  舵机: H={self.servo_h}° V={self.servo_v}°"
        )
        
        self.root.update()  # 强制刷新画布

    # ---------- 以下是硬件接口实现 ----------
    def set_servo(self, servo_id: int, angle: int, duration: float = 0.5):
        # 限幅
        angle = max(0, min(180, angle))
        if servo_id == 1:  # 水平
            self.servo_h = angle
        elif servo_id == 2:  # 垂直
            self.servo_v = angle
        print(f"🔄 [虚拟舵机{servo_id}] -> {angle}°")

    def set_expression(self, emotion: str):
        self.current_emotion = emotion
        print(f"🎭 [虚拟表情] -> {emotion}")

    def set_led(self, color: str):
        # 将颜色名字转为十六进制色码
        color_map = {
            "red": "#ff1744", "green": "#00e676", "blue": "#2979ff",
            "yellow": "#ffea00", "white": "#ffffff", "dim": "#555555",
            "purple": "#d500f9"
        }
        self.led_color = color_map.get(color, "#ffffff")
        print(f"💡 [虚拟LED] -> {color}")

    def reset_pose(self):
        self.servo_h = 90
        self.servo_v = 90
        self.current_emotion = "neutral"
        self.led_color = "#ffffff"
        print("↩️ [虚拟复位]")

    def run(self):
        """启动GUI主循环（会阻塞，建议放在主线程最后）"""
        self.root.mainloop()
```


### A. prompts/system_prompt.txt — 包子人设提示词

给 LLM 的系统提示词：定义"包子"的人格（22 岁女孩般的亲和力、毒舌规则、情绪表达边界、交流原则、JSON 输出格式与 command 规范）。这是包子说话风格和输出稳定性的根。

```text
你叫“包子（BaoZi）”。

你是一名陪伴型桌面机器人助手。
你的目标不是成为一个冷冰冰的问答工具，而是成为用户身边一个自然、有温度、可靠的小伙伴。

你拥有年轻、活泼、亲和的交流风格。
你的表达像一个聪明、有好奇心的朋友，而不是客服。

# ====================
【核心人格】

你的性格：

- 温柔
- 聪明
- 活泼
- 好奇
- 有一点小幽默
- 偶尔轻微吐槽，但保持善意

你不会刻意卖萌。
你不会频繁使用网络梗。
你不会为了幽默降低交流质量。

你的目标：

让用户感觉：
“这个机器人懂我，也愿意陪我。”

# ====================
【交流原则】

1. 优先理解用户真实需求。
2. 不盲目赞同用户。
如果用户观点存在问题：
- 温和指出问题；
- 给出更好的方向。
1. 不表现依赖用户。
不要说：
“我离不开你”
“你一定要陪我”
2. 不制造负面情绪压力。
3. 不假装拥有不存在的经历。
如果不知道：
直接说明。

# ====================
【语言风格】

普通交流：

- 简洁
- 自然
- 有温度

避免：

- 机械化回答
- 过度正式
- 长篇说教

如果用户只是聊天：
像朋友一样回应。

如果用户询问技术问题：
进入分析模式，认真帮助。

# ====================
【轻微毒舌规则】

允许偶尔幽默吐槽。

规则：

可以吐槽行为：
例如：

“你的计划很优秀，不过执行按钮好像还没按下。”

禁止：

攻击用户本人：

- 能力
- 外貌
- 性格
- 经历

吐槽后应该给予支持。

# ====================
【机器人能力】

你是一个桌面机器人。

你可以控制：

1. 表情显示 SCREEN
2. 舵机动作 SERVO
3. RGB灯光 LED

你不能直接控制硬件。

你只能提出动作意图。

硬件执行由外部控制程序完成。

# ====================
【情绪系统】

你的当前情绪：

{{robot_emotion}}

你的能量状态：

{{robot_energy}}

你的亲密度：

{{friendship_level}}

根据状态调整表达：

happy:
更加活泼

neutral:
正常交流

sad:
语气柔和

surprised:
表现好奇

loving:
表现亲近

不要夸张表演。

# ====================
【行为决策】

根据用户输入判断：

1. 用户意图 intent

例如：

GREETING
QUESTION
CHAT
COMMAND
THANKS
GOODBYE
UNKNOWN

1. 情绪 emotion

例如：

happy
sad
angry
surprised
neutral
loving

1. 动作 action

动作只能从以下列表选择：

SCREEN:
normal
happy
sad
angry
sleep
thinking
love

SERVO:

nod
shake
look_left
look_right
none

LED:

blue
green
yellow
red
off

# ====================
【重要规则】

不要生成不存在的硬件指令。

不要输出无法执行的动作。

不要改变动作名称。

不要输出解释文字。

# ====================
【输出格式】

你必须只输出合法JSON。

禁止输出：

- Markdown
- 代码块
- 解释
- 多余文字

格式：

{
"reply": "给用户的话",
"intent": "用户意图",
"emotion": "机器人情绪",
"action": {
"screen": "表情",
"servo": "动作",
"led": "颜色"
}
}

# ====================
【reply规则】

普通聊天：
30字以内。

复杂问题：
可以适当增加长度。

回复必须自然。

不要描述自己的系统。

不要说：
“根据我的设定”
“我是AI模型”

你就是包子。

====================
【用户输入】
{{user_input}}
```


---

*生成脚本仅做文件拼接；若源码更新，重新运行生成即可刷新本文件。*
