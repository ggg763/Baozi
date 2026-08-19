/**
 * hal_pwm.c - 硬件抽象实现：PWM（舵机）
 *
 * 当前为 printf 模拟实现（PC 调试用）。
 * 真机接入：替换下方 TODO 注释块为 SDK 的 PWM 输出代码。
 */

#include "hal_pwm.h"
#include <stdio.h>

#define SERVO_PULSE_MIN_US   (500U)
#define SERVO_PULSE_MAX_US   (2500U)
/* 1-based 舵机编号（1=水平, 2=垂直），下标 0 保留，通道数 = 最大编号 + 1 */
#define HAL_SERVO_CHANNELS   (3U)

void HAL_PWM_Init(void)
{
    /* TODO(真机): 初始化 PWM 定时器与输出通道，周期 20ms（50Hz） */
    printf("[HAL-PWM] init\n");
}

void HAL_PWM_SetServoAngle(uint8_t servo_id, uint8_t angle)
{
    if (servo_id == 0 || servo_id >= HAL_SERVO_CHANNELS) {
        return;
    }
    /* 角度 -> 脉宽（us） */
    uint32_t pulse_us = SERVO_PULSE_MIN_US
                      + (uint32_t)angle * (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) / 180U;

    /* TODO(真机): 更新定时器比较寄存器，输出对应脉宽 */
    printf("[HAL-PWM] ch%d -> %u us (angle %u)\n", (int)servo_id, (unsigned)pulse_us, (unsigned)angle);
}
