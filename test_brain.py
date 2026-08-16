# -*- coding: utf-8 -*-
"""
test_brain.py - 包子大脑冒烟测试（新版 BaoZiBrain）

用法（需先启动 Ollama）：
    python test_brain.py
"""
import os
from brain.brain import BaoZiBrain

# 读取人设
prompt_path = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "prompts",
    "system_prompt.txt",
)
with open(prompt_path, "r", encoding="utf-8") as f:
    system_prompt = f.read()

brain = BaoZiBrain(system_prompt=system_prompt)
brain.clear_history()

# 第一轮
r1 = brain.chat("你好呀，包子！")
print(r1)
assert "reply" in r1 and "emotion" in r1 and "command" in r1 and "robot_action" in r1

# 第二轮（验证历史记忆生效）
r2 = brain.chat("今天天气如何")
print(r2)
print("✅ 大脑冒烟测试通过")
