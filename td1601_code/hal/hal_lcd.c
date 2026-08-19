/**
 * hal_lcd.c - 硬件抽象实现：LCD 显示
 *
 * 当前为 printf 模拟实现。
 * 真机接入：替换 TODO 注释块为 LCD 驱动代码。
 */

#include "hal_lcd.h"
#include <stdio.h>

void HAL_LCD_Init(void)
{
    /* TODO(真机): LCD 初始化（复位、背光、清屏） */
    printf("[HAL-LCD] init\n");
}

void HAL_LCD_Clear(void)
{
    /* TODO(真机): 清屏 */
    printf("[HAL-LCD] clear\n");
}

void HAL_LCD_ShowText(const char *text)
{
    /* TODO(真机): 改为显示表情位图动画（happy_0.bmp ...） */
    printf("[HAL-LCD] show: %s\n", text != 0 ? text : "");
}
