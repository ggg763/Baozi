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