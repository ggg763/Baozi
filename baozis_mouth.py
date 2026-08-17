import asyncio
import edge_tts
import hashlib
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
    text_hash = hashlib.md5(text.encode('utf-8')).hexdigest()
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