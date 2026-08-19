/**
 * screen_display.c - TD1601 LCD 表情显示层实现
 *
 * 骨架说明：
 *   - 表情帧资源（happy_0.bmp / happy_1.bmp ...）需在真机阶段导入；
 *   - 当前实现把情绪名转发给 HAL_LCD 显示文字占位，
 *     接真实 LCD 驱动后替换为位图动画。
 */

#include "screen_display.h"
#include "hal/hal_lcd.h"

void Screen_Init(void)
{
    HAL_LCD_Init();
    HAL_LCD_Clear();
}

void Screen_ShowEmotion(EmotionState emo, const char *anim_name)
{
    /* emo 参数保留用于查找对应帧序列（真机阶段实现） */
    (void)emo;
    HAL_LCD_ShowText(anim_name != 0 ? anim_name : "neutral");
}

void Screen_Tick(uint32_t now_ms)
{
    /* 预留：帧动画推进（如 500ms 切换下一帧） */
    (void)now_ms;
}
