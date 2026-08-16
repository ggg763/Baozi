from baozis_brain import baozis_brain, clear_history

clear_history()
print(baozis_brain("你好呀，包子！"))
# 输出示例: {'reply': '你好呀！今天心情真好！', 'emotion': 'happy', 'command': 'SERVO_NOD 1'}
print(baozis_brain("今天天气如何"))