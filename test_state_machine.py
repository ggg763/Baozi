# -*- coding: utf-8 -*-
"""
test_state_machine.py - 状态机与虚拟硬件接口测试

覆盖范围：
    1. RobotHardware 抽象契约（不可直接实例化）
    2. FakeTD1601 模拟硬件（舵机限幅/编号、表情、LED、复位）
    3. VisualTD1601 虚拟窗口硬件（mock 掉 tkinter，不弹真实窗口）
    4. MoodStateMachine 状态机（假时钟 + 单步心跳，覆盖全部状态切换）

不依赖任何外部服务（Ollama/网络/TTS/ASR），可随时重复运行。

运行：
    python test_state_machine.py
    python -m unittest test_state_machine -v
"""
import sys
import os
import io
import time as _time
import unittest
from unittest import mock

# 控制台 UTF-8 输出（Windows GBK 终端下避免 emoji 报错）
if sys.stdout and hasattr(sys.stdout, "buffer"):
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
if sys.stderr and hasattr(sys.stderr, "buffer"):
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from robot.hardware_interface import RobotHardware, FakeTD1601
from emotion.state_machine import MoodStateMachine, BaoZiMood

try:
    from robot.visual_robot import VisualTD1601
    _HAS_TK = True
except ImportError:
    VisualTD1601 = None
    _HAS_TK = False


# ==========================
# 测试工具
# ==========================

class _StopHeartbeat(Exception):
    """用于单步心跳：第 2 次 sleep(5) 抛异常结束循环"""


class FakeHardware:
    """记录所有硬件调用的假硬件（供状态机测试）"""

    def __init__(self):
        self.calls = []

    def set_expression(self, e):
        self.calls.append(("expression", e))

    def set_led(self, c):
        self.calls.append(("led", c))

    def set_servo(self, sid, angle, duration=0.5):
        self.calls.append(("servo", sid, angle))


class HeartbeatMixin:
    """假时钟 + 免真实等待的单步心跳。

    _heartbeat_loop 的结构是 while running: sleep(5); <检查逻辑>，
    这里让第 1 次 sleep(5) 直接返回（模拟等待 5 秒）、第 2 次抛异常退出，
    这样每次 step_heartbeat() 恰好执行一次状态检查。
    """

    def setUp(self):
        self.now = [1000.0]
        self.sleep_count = [0]
        self._p1 = mock.patch("emotion.state_machine.time.time", lambda: self.now[0])
        self._p2 = mock.patch("emotion.state_machine.time.sleep", self._fake_sleep)
        self._p3 = mock.patch("threading.Thread.start", lambda self: None)  # 不启动真后台线程
        self._p1.start()
        self._p2.start()
        self._p3.start()

    def tearDown(self):
        self._p1.stop()
        self._p2.stop()
        self._p3.stop()

    def _fake_sleep(self, seconds):
        if seconds >= 1:  # 心跳的 sleep(5)
            self.sleep_count[0] += 1
            if self.sleep_count[0] > 1:
                raise _StopHeartbeat()
        # 互动里的短 sleep(0.2/0.3)：直接跳过

    def step_heartbeat(self, sm):
        self.sleep_count[0] = 0
        try:
            sm._heartbeat_loop()
        except _StopHeartbeat:
            pass
        except Exception as e:
            raise AssertionError(f"心跳检查异常: {e}")

    def advance(self, seconds):
        self.now[0] += seconds


# ==========================
# 1. 抽象契约
# ==========================

class TestHardwareContract(unittest.TestCase):

    def test_abstract_cannot_instantiate(self):
        with self.assertRaises(TypeError):
            RobotHardware()

    def test_fake_implements_contract(self):
        for name in ("set_servo", "set_expression", "set_led", "reset_pose", "run"):
            self.assertTrue(hasattr(FakeTD1601, name), f"FakeTD1601 缺少 {name}()")
        self.assertTrue(issubclass(FakeTD1601, RobotHardware))


# ==========================
# 2. FakeTD1601 模拟硬件
# ==========================

class TestFakeTD1601(unittest.TestCase):

    def test_servo_limit_and_channel(self):
        hw = FakeTD1601()
        hw.set_servo(1, 200)   # 超上限 → 180
        self.assertEqual(hw.servo_h, 180)
        hw.set_servo(2, -10)   # 低于下限 → 0
        self.assertEqual(hw.servo_v, 0)
        hw.set_servo(1, 45)
        hw.set_servo(2, 135)
        self.assertEqual(hw.servo_h, 45)
        self.assertEqual(hw.servo_v, 135)

    def test_expression_and_led(self):
        hw = FakeTD1601()
        hw.set_expression("happy")
        self.assertEqual(hw.current_emotion, "happy")
        hw.set_led("red")
        self.assertEqual(hw.led_color, "red")
        self.assertEqual((hw.servo_h, hw.servo_v), (90, 90), "默认姿态应为 90/90")

    def test_reset_pose(self):
        hw = FakeTD1601()
        hw.set_servo(1, 30)
        hw.set_servo(2, 150)
        hw.set_expression("angry")
        hw.set_led("blue")
        hw.reset_pose()
        self.assertEqual((hw.servo_h, hw.servo_v), (90, 90))
        self.assertEqual(hw.current_emotion, "neutral")
        self.assertEqual(hw.led_color, "white")


# ==========================
# 3. VisualTD1601 虚拟窗口（mock tkinter）
# ==========================

class _FakeCanvas:
    def create_oval(self, *a, **k): return 1
    def create_polygon(self, *a, **k): return 2
    def create_text(self, *a, **k): return 3
    def create_arc(self, *a, **k): return 4
    def create_line(self, *a, **k): return 5
    def delete(self, *a, **k): pass
    def itemconfig(self, *a, **k): pass
    def pack(self, *a, **k): pass


class _FakeRoot:
    def __init__(self):
        self.canvas = _FakeCanvas()
    def after(self, ms, cb): pass
    def update(self): pass
    def title(self, *a): pass


@unittest.skipUnless(_HAS_TK, "tkinter 不可用，跳过 VisualTD1601 测试")
class TestVisualTD1601(unittest.TestCase):

    def _make(self):
        with mock.patch("robot.visual_robot.tk.Tk", return_value=_FakeRoot()), \
             mock.patch("robot.visual_robot.tk.Canvas", return_value=_FakeCanvas()):
            return VisualTD1601()

    def test_inherits_contract(self):
        self.assertTrue(issubclass(VisualTD1601, RobotHardware))

    def test_servo_limit_and_channel(self):
        v = self._make()
        v.set_servo(1, 200)
        v.set_servo(2, -10)
        self.assertEqual(v.servo_h, 180)
        self.assertEqual(v.servo_v, 0)

    def test_led_color_mapping(self):
        v = self._make()
        v.set_led("red")
        self.assertEqual(v.led_color, "#ff1744")
        v.set_led("dim")
        self.assertEqual(v.led_color, "#555555")
        v.set_led("unknown")  # 未知颜色回退白色
        self.assertEqual(v.led_color, "#ffffff")

    def test_expression_and_reset(self):
        v = self._make()
        v.set_expression("surprised")
        self.assertEqual(v.current_emotion, "surprised")
        v.reset_pose()
        self.assertEqual(v.current_emotion, "neutral")
        self.assertEqual((v.servo_h, v.servo_v), (90, 90))
        self.assertEqual(v.led_color, "#ffffff")


# ==========================
# 4. MoodStateMachine 状态机
# ==========================

class TestMoodStateMachine(HeartbeatMixin, unittest.TestCase):

    def test_init_idle(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        self.assertEqual(sm.current_mood, BaoZiMood.IDLE)
        self.assertIn(("expression", "neutral"), hw.calls)
        self.assertIn(("led", "white"), hw.calls)

    def test_idle_kept_on_short_idle(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        hw.calls.clear()
        self.step_heartbeat(sm)          # 空闲 0s
        self.assertEqual(sm.current_mood, BaoZiMood.IDLE)
        self.assertIn(("expression", "neutral"), hw.calls)
        # IDLE 基准外观只驱动一次
        self.step_heartbeat(sm)
        self.assertEqual(hw.calls.count(("expression", "neutral")), 1)

    def test_interaction_happy(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        sm.update_interaction("happy")
        self.assertEqual(sm.current_mood, BaoZiMood.ACTIVE)
        self.assertIn(("expression", "happy"), hw.calls)
        self.assertIn(("led", "green"), hw.calls)
        self.assertIn(("servo", 1, 110), hw.calls)
        self.assertIn(("servo", 1, 90), hw.calls)

    def test_emotion_led_mapping(self):
        expected = {
            "happy": "green", "loving": "purple", "sad": "blue",
            "angry": "red", "surprised": "yellow", "neutral": "white",
        }
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        for emotion, color in expected.items():
            hw.calls.clear()
            sm.update_interaction(emotion)
            self.assertIn(("led", color), hw.calls, f"{emotion} 应映射到 {color}")

    def test_bored_after_25s(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        sm.update_interaction("neutral")
        hw.calls.clear()
        self.advance(25)
        self.step_heartbeat(sm)
        self.assertEqual(sm.current_mood, BaoZiMood.BORED)
        self.assertIn(("expression", "bored"), hw.calls)
        self.assertIn(("led", "dim"), hw.calls)

    def test_sleeping_after_35s_once(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        sm.update_interaction("neutral")
        hw.calls.clear()
        self.advance(35)
        self.step_heartbeat(sm)
        self.assertEqual(sm.current_mood, BaoZiMood.SLEEPING)
        self.assertTrue(sm.is_sleeping)
        self.assertIn(("expression", "sleep"), hw.calls)
        # 睡眠中心跳应跳过，不重复驱动
        hw.calls.clear()
        self.step_heartbeat(sm)
        self.assertEqual(hw.calls, [])

    def test_interaction_breaks_sleep(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        sm.update_interaction("neutral")
        self.advance(35)
        self.step_heartbeat(sm)
        self.assertTrue(sm.is_sleeping)
        sm.update_interaction("surprised")
        self.assertEqual(sm.current_mood, BaoZiMood.ACTIVE)
        self.assertFalse(sm.is_sleeping)
        self.assertIn(("led", "yellow"), hw.calls)

    def test_wake_up(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        sm.is_sleeping = True
        sm.current_mood = BaoZiMood.SLEEPING
        sm.wake_up()
        self.assertFalse(sm.is_sleeping)
        self.assertEqual(sm.current_mood, BaoZiMood.ACTIVE)
        self.assertIn(("expression", "happy"), hw.calls)

    def test_shutdown(self):
        hw = FakeHardware()
        sm = MoodStateMachine(hw)
        sm.shutdown()
        self.assertFalse(sm.running)


if __name__ == "__main__":
    unittest.main(verbosity=2)
