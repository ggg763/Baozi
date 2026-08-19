/**
 * hal_gpio.c - 硬件抽象实现：GPIO 与系统延时
 *
 * 当前为 printf 模拟实现。
 * 真机接入：替换 TODO 注释块为 SDK GPIO/定时器代码。
 */

#include "hal_gpio.h"
#include <stdio.h>

void HAL_GPIO_Init(void)
{
    /* TODO(真机): 初始化触摸/红外输入引脚 */
    printf("[HAL-GPIO] init\n");
}

bool HAL_GPIO_ReadTouch(void)
{
    /* TODO(真机): 读取触摸 GPIO（TTP223 输出） */
    return false;
}

int8_t HAL_GPIO_ReadIRDirection(void)
{
    /* TODO(真机): 读取红外传感器方向（预留） */
    return 0;
}

void HAL_DelayMs(uint32_t ms)
{
    /* TODO(真机): 替换为 RTOS delay 或定时器忙等。
     * PC 调试期使用标准 C 的 clock 忙等，保证行为可观测。 */
    volatile uint32_t i;
    for (; ms > 0U; ms--) {
        for (i = 0; i < 20000U; i++) {
            /* 空转 */
        }
    }
}
