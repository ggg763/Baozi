/**
 * sensor_manager.c - TD1601 传感器管理实现
 *
 * 骨架说明：
 *   - 触摸（TTP223 电容触摸）与红外追踪的输入读取在 HAL_GPIO 层；
 *   - 真机接线后确认引脚映射即可启用，逻辑无需改动。
 */

#include "sensor_manager.h"
#include "hal/hal_gpio.h"
#include "emotion_state.h"
#include "servo_control.h"

/* 触摸检测的边沿防抖：上次读到的电平 */
static bool s_last_touch = false;

void Sensor_Init(void)
{
    HAL_GPIO_Init();
    s_last_touch = HAL_GPIO_ReadTouch();
}

void Sensor_Scan(uint32_t now_ms)
{
    /* ---------- 1. 触摸：上升沿 -> 亲昵情绪 ---------- */
    bool touch = HAL_GPIO_ReadTouch();
    if (touch && !s_last_touch) {
        Emotion_Update(EMO_LOVING);
        Emotion_Touch(now_ms);
    }
    s_last_touch = touch;

    /* ---------- 2. 红外追踪（预留） ----------
     * HAL_GPIO_ReadIRDirection() 返回 -1/0/+1，
     * 真机接入后取消注释并调参。
     */
#if 0
    int8_t dir = HAL_GPIO_ReadIRDirection();
    if (dir != 0) {
        uint8_t cur = Servo_GetAngle(SERVO_HEAD_HORIZONTAL);
        int16_t next = (int16_t)cur + dir * 10;
        if (next >= SERVO_ANGLE_MIN && next <= SERVO_ANGLE_MAX) {
            Servo_Move(SERVO_HEAD_HORIZONTAL, (uint8_t)next, 300, EASE_IN_OUT);
        }
    }
#endif
}
