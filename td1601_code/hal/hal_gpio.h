/**
 * hal_gpio.h - 硬件抽象：GPIO 与系统延时
 *
 * 传感器：
 *   - 触摸（TTP223）：数字输入，1=被触摸
 *   - 红外追踪：方向输入，-1/0/+1（真机预留）
 *
 * 系统延时 HAL_DelayMs 供 motion.c 等模块使用，
 * 真机应替换为 RTOS 延时或定时器忙等。
 */

#ifndef HAL_GPIO_H
#define HAL_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void HAL_GPIO_Init(void);

/** 读取触摸传感器（true=被触摸） */
bool HAL_GPIO_ReadTouch(void);

/** 读取红外追踪方向：-1 左 / 0 无 / +1 右（真机预留） */
int8_t HAL_GPIO_ReadIRDirection(void);

/** 阻塞延时（毫秒） */
void HAL_DelayMs(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* HAL_GPIO_H */
