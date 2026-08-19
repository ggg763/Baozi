/**
 * hal_led.c - 硬件抽象实现：LED
 *
 * 当前为 printf 模拟实现。
 * 真机接入：替换 TODO 注释块为 GPIO/灯带驱动代码。
 */

#include "hal_led.h"
#include <stdio.h>

void HAL_LED_Init(void)
{
    /* TODO(真机): 初始化 LED 引脚 / 灯带 */
    printf("[HAL-LED] init\n");
}

void HAL_LED_SetRgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* TODO(真机): 输出 RGB（PWM 或 WS2812 协议） */
    printf("[HAL-LED] rgb(%u,%u,%u)\n", (unsigned)r, (unsigned)g, (unsigned)b);
}
