/******************************************************************************
 * @file     gfx.h
 * @brief    屏幕图形库（TD1601 + 480x480 QSPI 屏）—— 给图形设计同学使用
 *
 * 用法三步：
 *      gfx_init();                                  // 初始化屏幕
 *      gfx_clear(GFX_SKYBLUE);                      // 刷背景
 *      gfx_fill_circle(240, 240, 80, GFX_RED, GFX_SKYBLUE);   // 画图形
 *
 * 设计约定（重要，请务必了解）：
 *  1. 颜色统一用 RGB565（16bit），用宏 GFX_RGB565(r,g,b) 或下面预定义色。
 *  2. 本库的绘制是"立即写屏"，每次调用后画面立刻更新，无需 flush。
 *  3. **局部绘制的背景参数**：屏幕不支持"读回像素"，所以往局部区域写入时
 *     必须提供该窗口内**其余像素**的颜色（bg 参数）。例如在浅蓝底上画圆，
 *     传 bg = GFX_SKYBLUE，圆外接矩形的四角就会被填成浅蓝（与底色一致，
 *     看不出差别）。若在图片上叠加圆，请先用 gfx_blit 贴整图，再传对应
 *     bg，或自行用 blit 合成复杂画面。
 *  4. 坐标可超屏，库内部会自动裁剪（但窗口必须至少 1x1 像素）。
 *  5. 性能（实测）：
 *       - 全屏刷新（gfx_clear / 整屏 blit）：约 20ms 一帧（≈50fps 上限）；
 *       - 局部刷新（小图元 / 小图 blit）：几十微秒~几百微秒，可轻松 60fps+；
 *     因此动画请优先"只画变化的区域"。
 *
 * 线程/中断安全：请在主循环里调用；不要在中断里直接调用本库。
 ******************************************************************************/

#ifndef _GFX_H_
#define _GFX_H_

#include <stdint.h>

/* ============================ 屏幕尺寸 ============================ */
#define GFX_W 480
#define GFX_H 480

/* ============================ 颜色（RGB565） ============================ */
#define GFX_RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define GFX_BLACK        0x0000
#define GFX_WHITE        0xFFFF
#define GFX_RED          0xF800
#define GFX_GREEN        0x07E0
#define GFX_BLUE         0x001F
#define GFX_YELLOW       0xFFE0
#define GFX_CYAN         0x07FF
#define GFX_MAGENTA      0xF81F
#define GFX_GRAY         0x8410
#define GFX_DARKGRAY     0x4208
#define GFX_ORANGE       GFX_RGB565(255, 140, 40)
#define GFX_SKYBLUE      GFX_RGB565(186, 222, 248)   /* 默认柔和背景 */
#define GFX_CREAM        GFX_RGB565(255, 240, 211)
#define GFX_BROWN        GFX_RGB565(150, 100, 70)
#define GFX_PINK         GFX_RGB565(255, 118, 136)
#define GFX_DARKBLUE     GFX_RGB565(30, 40, 70)

/* ============================ 初始化 ============================ */
/* 初始化屏幕（含官方 LCD 初始化与三色自检），调用一次即可 */
void gfx_init(void);

/* ============================ 基础绘制 ============================ */
/* 全屏填充（会整屏刷新，约 20ms） */
void gfx_clear(uint16_t color);

/* 纯色矩形：窗口内全部是该颜色，不需要背景参数 */
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);

/* 实心圆：bg 为圆外接矩形内、圆以外的颜色（通常是底色） */
void gfx_fill_circle(int cx, int cy, int r, uint16_t color, uint16_t bg);

/* 实心三角形：bg 同上 */
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                       uint16_t color, uint16_t bg);

/* 线段：thick 为线宽（像素），bg 同上 */
void gfx_line(int x0, int y0, int x1, int y1, int thick, uint16_t color, uint16_t bg);

/* ============================ 位图 / 精灵 ============================ */
/* 贴一张位图：pix 为 w*h 个 RGB565 像素（行优先、连续内存）
 * 用途：把美术做好的图片/精灵直接显示到屏幕任意位置 */
void gfx_blit(int x, int y, int w, int h, const uint16_t *pix);

/* 贴图 + 透明色键：pix 中等于 key 的像素不绘制（用 bg 填充该位置）
 * 用途：不带 alpha 通道的精灵图（把背景色作为 key） */
void gfx_blit_key(int x, int y, int w, int h, const uint16_t *pix,
                  uint16_t key, uint16_t bg);

#endif /* _GFX_H_ */
