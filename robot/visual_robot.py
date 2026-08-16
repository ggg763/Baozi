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