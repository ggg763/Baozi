/**
 * motion.c - TD1601 动作组合层实现
 *
 * 动作 = 多个舵机指令按时间序组合。
 * 当前为"顺序阻塞"式实现：每条指令发出后延时等待，
 * 便于真机调试；后续可升级为基于 Motion_Tick 的插值调度。
 */

#include "motion.h"
#include "servo_control.h"

/* 通用延时（毫秒）——由 HAL 提供，真机时替换为 RTOS/定时器延时 */
void HAL_DelayMs(uint32_t ms);   /* 定义于 hal/hal_gpio.c */

void Motion_Init(void)
{
    Motion_ResetPose();
}

void Motion_Tick(uint32_t now_ms)
{
    /* 预留：插值推进。当前动作采用阻塞执行，无需周期驱动。 */
    (void)now_ms;
}

/* ---------- 高层动作实现 ---------- */

void Motion_HappySway(void)
{
    /* 开心：头水平左右轻摆 2 次 */
    Servo_Move(SERVO_HEAD_HORIZONTAL, 110, 300, EASE_IN_OUT);
    HAL_DelayMs(350);
    Servo_Move(SERVO_HEAD_HORIZONTAL, 70, 300, EASE_IN_OUT);
    HAL_DelayMs(350);
    Servo_Move(SERVO_HEAD_HORIZONTAL, SERVO_CENTER, 300, EASE_IN_OUT);
    HAL_DelayMs(300);
}

void Motion_SadNod(void)
{
    /* 难过：头低垂并缓慢回正 */
    Servo_Move(SERVO_HEAD_VERTICAL, 120, 600, EASE_IN_OUT);
    HAL_DelayMs(800);
    Servo_Move(SERVO_HEAD_VERTICAL, SERVO_CENTER, 600, EASE_IN_OUT);
}

void Motion_AngryTilt(void)
{
    /* 生气：快速歪头再回正 */
    Servo_Move(SERVO_HEAD_HORIZONTAL, 70, 150, EASE_STEP);
    HAL_DelayMs(250);
    Servo_Move(SERVO_HEAD_HORIZONTAL, SERVO_CENTER, 200, EASE_IN_OUT);
    HAL_DelayMs(200);
}

void Motion_SurprisedLean(void)
{
    /* 惊讶：头部后仰（垂直回抽） */
    Servo_Move(SERVO_HEAD_VERTICAL, 60, 200, EASE_STEP);
    HAL_DelayMs(400);
    Servo_Move(SERVO_HEAD_VERTICAL, SERVO_CENTER, 300, EASE_IN_OUT);
}

void Motion_LovingLean(void)
{
    /* 亲昵：前倾靠近 */
    Servo_Move(SERVO_HEAD_VERTICAL, 110, 500, EASE_IN_OUT);
    HAL_DelayMs(700);
    Servo_Move(SERVO_HEAD_VERTICAL, SERVO_CENTER, 400, EASE_IN_OUT);
}

void Motion_BoredSlouch(void)
{
    /* 无聊：缓慢歪头（保持） */
    Servo_Move(SERVO_HEAD_HORIZONTAL, 75, 1000, EASE_IN_OUT);
    Servo_Move(SERVO_HEAD_VERTICAL, 85, 1000, EASE_IN_OUT);
}

void Motion_SleepPose(void)
{
    /* 睡眠：头回正后低头 */
    Servo_Move(SERVO_HEAD_HORIZONTAL, SERVO_CENTER, 500, EASE_IN_OUT);
    Servo_Move(SERVO_HEAD_VERTICAL, 95, 1000, EASE_IN_OUT);
}

void Motion_IdlePose(void)
{
    /* 待机：回正 */
    Servo_Move(SERVO_HEAD_HORIZONTAL, SERVO_CENTER, 500, EASE_IN_OUT);
    Servo_Move(SERVO_HEAD_VERTICAL, SERVO_CENTER, 500, EASE_IN_OUT);
}

void Motion_ResetPose(void)
{
    Servo_Move(SERVO_HEAD_HORIZONTAL, SERVO_CENTER, 300, EASE_LINEAR);
    Servo_Move(SERVO_HEAD_VERTICAL, SERVO_CENTER, 300, EASE_LINEAR);
}
