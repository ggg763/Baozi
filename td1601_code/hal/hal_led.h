/**
 * hal_led.h - 硬件抽象：LED
 *
 * 真机接入：在 hal_led.c 中实现 RGB 输出
 * （单色 LED / WS2812 灯带均可在此适配）。
 */

#ifndef HAL_LED_H
#define HAL_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void HAL_LED_Init(void);
void HAL_LED_SetRgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif /* HAL_LED_H */
