/**
 * led_control.h - TD1601 LED 灯效控制层
 *
 * 职责：
 *   设置 LED 颜色/亮度（RGB），按情绪映射。
 *   颜色定义见 robot_types.h 的 RgbColor。
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include "robot_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LED
 */
void LED_Init(void);

/**
 * 设置 LED 颜色
 * @param color RGB 颜色
 */
void LED_SetColor(RgbColor color);

/**
 * 设置亮度（0-100%）
 */
void LED_SetBrightness(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /* LED_CONTROL_H */
