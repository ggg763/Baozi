/******************************************************************************
 * @file     qspi_lcd.h
 * @brief    轻量 QSPI LCD 驱动接口（480x480, TR230S）
 *
 * 移植自 QSPI_480/projects/LCD 模板（已点亮验证），做了轻量化：
 *   - 去掉 DMA / 中断回调，像素用轮询发送，逻辑更简单
 *   - 保留模板的命令前缀字节协议与 QSPI 16bit 数据时序
 *
 * 接线（TD1601 EVB 扩展板 CON2）：
 *   SCK=PA3  D0~D3=PB4~PB7(SPI1 mux)
 *   CS=PA2(低有效)  RST=PB15  WAIT=PB10(输入忙信号)  BL=PA31(背光,高亮)
 ******************************************************************************/

#ifndef _QSPI_LCD_H_
#define _QSPI_LCD_H_

#include <stdint.h>
#include <stdbool.h>

/* 屏幕尺寸（模板按 480x480 全屏填充） */
#define QSPI_LCD_W       480
#define QSPI_LCD_H       480

/* 常用 16bit RGB565 颜色 */
#define QSPI_RGB565(r, g, b)  ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define QSPI_COLOR_WHITE       0xFFFF
#define QSPI_COLOR_BLACK       0x0000
#define QSPI_COLOR_RED         0xF800
#define QSPI_COLOR_GREEN       0x07E0
#define QSPI_COLOR_BLUE        0x001F

/* LCD 初始化（引脚/OSPI/复位/背光/初始化命令），成功后屏幕可正常填充 */
void qspi_lcd_init(void);

/* 区域单色填充：以 (x,y) 为左上角，宽 w 高 h，颜色 RGB565 */
void qspi_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* 实心圆填充（一次窗口完成，比逐行小窗口更稳更快）：
 * 圆心(cx,cy)、半径 r，圆内填 fill，圆外接矩形其余区域填 out（out 应传
 * 该圆所在表面的底色，用于盖掉旧内容） */
void qspi_fill_circle(int cx, int cy, int r, uint16_t fill, uint16_t out);

/* 实心椭圆填充（单窗口双色，语义同 qspi_fill_circle，rx/ry 为横纵半轴） */
void qspi_fill_ellipse(int cx, int cy, int rx, int ry, uint16_t fill, uint16_t out);

/* 实心三角形填充（单窗口双色）：三点 (x0,y0)(x1,y1)(x2,y2)，边函数光栅化，
 * 三角形内填 fill、外接矩形其余填 out（out 传表面底色） */
void qspi_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                        uint16_t fill, uint16_t out);

#endif /* _QSPI_LCD_H_ */
