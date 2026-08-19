/**
 * hal_pwm.h - 硬件抽象：PWM（舵机）
 *
 * 真机接入步骤：
 *   1. 在 hal_pwm.c 中按目标 SDK（如 CH32V 定时器 + DMA）实现
 *      HAL_PWM_SetServoAngle 的脉宽输出；
 *   2. 确认舵机脉宽范围（默认 500us~2500us 对应 0~180°）。
 */

#ifndef HAL_PWM_H
#define HAL_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void HAL_PWM_Init(void);

/**
 * 设置某路舵机角度
 * @param servo_id 舵机编号（0 起）
 * @param angle    角度 0-180
 */
void HAL_PWM_SetServoAngle(uint8_t servo_id, uint8_t angle);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PWM_H */
