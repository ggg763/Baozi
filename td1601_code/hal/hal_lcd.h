/**
 * hal_lcd.h - 硬件抽象：LCD 显示
 *
 * 真机接入步骤：
 *   1. 在 hal_lcd.c 中按 LCD 驱动（SPI/并口）实现初始化与绘制；
 *   2. HAL_LCD_ShowText 仅作占位，真机应改为显示表情位图
 *      （见 screen_display.c 注释）。
 */

#ifndef HAL_LCD_H
#define HAL_LCD_H

#ifdef __cplusplus
extern "C" {
#endif

void HAL_LCD_Init(void);
void HAL_LCD_Clear(void);
void HAL_LCD_ShowText(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* HAL_LCD_H */
