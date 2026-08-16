"""
baozis_ear.py - 包子的耳朵
封装 SenseVoiceSmall 语音识别功能，供主程序随时调用
"""

import re
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
            device="cuda:0"  # 如果有 GPU 可以取消注释
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