/**
 * servo_control.c - TD1601 舵机控制层实现
 *
 * 通过 HAL_PWM 输出脉冲宽度（PWM 占空比 -> 舵机角度）。
 * 角度 -> 脉宽换算公式（典型舵机，500us~2500us 对应 0~180°）：
 *   pulse_us = 500 + angle * 2000 / 180
 * 真机需按舵机型号确认，可在此处集中调整。
 */

#include "servo_control.h"
#include "hal/hal_pwm.h"

#define SERVO_PULSE_MIN_US   (500U)
#define SERVO_PULSE_MAX_US   (2500U)

/* 1-based 舵机编号：下标 0 保留，数组大小 = SERVO_COUNT + 1 */
static uint8_t s_angle[SERVO_COUNT + 1] = { 0, SERVO_CENTER, SERVO_CENTER };

/* 限幅 */
static uint8_t clamp_angle(int angle)
{
    if (angle < SERVO_ANGLE_MIN) {
        return (uint8_t)SERVO_ANGLE_MIN;
    }
    if (angle > SERVO_ANGLE_MAX) {
        return (uint8_t)SERVO_ANGLE_MAX;
    }
    return (uint8_t)angle;
}

void Servo_Init(void)
{
    uint8_t i;
    for (i = 1; i <= SERVO_COUNT; i++) {
        s_angle[i] = SERVO_CENTER;
        HAL_PWM_SetServoAngle(i, SERVO_CENTER);
    }
}

void Servo_Move(uint8_t servo_id, uint8_t angle, uint16_t duration_ms, EasingType easing)
{
    /* 合法编号：1..SERVO_COUNT */
    if (servo_id == 0 || servo_id > SERVO_COUNT) {
        return;
    }
    uint8_t target = clamp_angle((int)angle);

    /*
     * 骨架实现：忽略缓动，直接跳变。
     * 真机进阶：在 Motion_Tick / 定时器中断中按 easing 曲线逐步逼近 target，
     * 参考 ease_in_out 公式（此处预留）：
     *   t<0.5 ? 2t^2 : 1-((-2t+2)^2)/2
     */
    (void)duration_ms;
    (void)easing;

    s_angle[servo_id] = target;
    HAL_PWM_SetServoAngle(servo_id, target);
}

uint8_t Servo_GetAngle(uint8_t servo_id)
{
    if (servo_id == 0 || servo_id > SERVO_COUNT) {
        return SERVO_CENTER;
    }
    return s_angle[servo_id];
}
