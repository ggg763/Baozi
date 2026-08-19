/**
 * led_control.c - TD1601 LED 灯效控制层实现
 *
 * 骨架说明：
 *   - 通过 HAL_LED 输出 RGB；
 *   - 真机若为单色 LED 或 WS2812 灯带，在 HAL_LED_SetRgb 内适配。
 */

#include "led_control.h"
#include "hal/hal_led.h"

static RgbColor s_color = { 255, 255, 255 };
static uint8_t  s_brightness = 100U;

void LED_Init(void)
{
    s_color.r = 255U;
    s_color.g = 255U;
    s_color.b = 255U;
    s_brightness = 100U;
    HAL_LED_Init();
    HAL_LED_SetRgb(s_color.r, s_color.g, s_color.b);
}

void LED_SetColor(RgbColor color)
{
    s_color = color;
    HAL_LED_SetRgb(
        (uint8_t)((uint32_t)s_color.r * s_brightness / 100U),
        (uint8_t)((uint32_t)s_color.g * s_brightness / 100U),
        (uint8_t)((uint32_t)s_color.b * s_brightness / 100U)
    );
}

void LED_SetBrightness(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    s_brightness = percent;
    LED_SetColor(s_color);   /* 重算并刷新输出 */
}
