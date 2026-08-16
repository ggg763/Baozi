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
