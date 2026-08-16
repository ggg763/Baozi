"""
main.py - 包子主控程序（本地测试版）
功能：接收音频文件 → ASR识别 → LLM推理 → TTS合成
"""
import os
import wave
import time
from baozis_ear import baozis_ear
from baozis_mouth import baozis_mouth
from brain.brain import BaoZiBrain

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

def process_audio_file(audio_path: str, clear_context: bool = True) -> dict:
    """
    处理单个音频文件，返回完整交互结果。
    
    参数:
        audio_path: 音频文件路径（必须是16kHz单声道WAV格式）
        clear_context: 是否清空对话历史（默认True，新会话）
    
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
    # 1. 执行预热
    warmup()
    
    # 2. 然后正常处理用户请求
    test_audio = "test.wav"
    if not os.path.exists(test_audio):
        print(f"错误：找不到测试音频文件 {test_audio}")
    else:
        result = process_audio_file(test_audio, clear_context=True)
        print("\n========== 最终结果 ==========")
        print(f"用户说: {result['user_text']}")
        print(f"包子说: {result['reply']}")
        print(f"情感: {result['emotion']}")
        print(f"指令: {result['command']}")
        print(f"动作: {result.get('robot_action')}")
        print(f"音频文件: {result['audio_file']}")