/**
 * servo_control.h - TD1601 舵机控制层
 *
 * 职责：
 *   - PWM 生成与角度控制（通过 HAL 层）
 *   - 角度限幅 0-180
 *   - 缓动（easing）计算骨架
 */

#ifndef SERVO_CONTROL_H
#define SERVO_CONTROL_H

#include "robot_types.h"

/* 舵机编号约定：1 = 水平(SERVO_HEAD_HORIZONTAL)，2 = 垂直(SERVO_HEAD_VERTICAL)
 * 数组下标 0 保留不用，因此存储数组大小 = SERVO_COUNT + 1。 */
#define SERVO_COUNT   (2)   /**< 舵机数量（最大合法编号） */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化舵机（所有舵机回到 90°）
 */
void Servo_Init(void);

/**
 * 控制单个舵机
 * @param servo_id  舵机编号（SERVO_HEAD_HORIZONTAL / SERVO_HEAD_VERTICAL）
 * @param angle     目标角度（0-180，自动限幅）
 * @param duration  动作时长（毫秒）
 * @param easing    缓动方式
 */
void Servo_Move(uint8_t servo_id, uint8_t angle, uint16_t duration_ms, EasingType easing);

/**
 * 立即读取当前角度
 */
uint8_t Servo_GetAngle(uint8_t servo_id);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_CONTROL_H */
