from funasr import AutoModel

# 1. 加载模型（首次运行会自动下载，约 900MB）
print("正在加载模型...")
model = AutoModel(
    model="iic/SenseVoiceSmall",
    trust_remote_code=True,
    device="cuda:0"  
)

# 2. 识别音频文件（必须是 16kHz 单声道 WAV 格式）
print("开始识别...")
res = model.generate(
    input="test.wav",        # 👈 替换为你的音频文件路径
    language="zh",           # 中文：zh | 英文：en | 粤语：yue
    use_itn=True,            # 数字转文本（如 "一百" → "100"）
    # ban_emo_unk=True,      # 取消注释可禁用情感标签输出
)

# 3. 打印结果
print("识别结果：")
print(res)