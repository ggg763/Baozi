/******************************************************************************
 * @file     gfx_demo.c
 * @brief    gfx 库使用示例：静态图元 + 局部刷新的灵动小脸动画
 *
 * 演示要点：
 *   1. gfx_clear / gfx_fill_rect / gfx_fill_circle / gfx_fill_triangle /
 *      gfx_line 的用法（颜色用 GFX_* 或 GFX_RGB565）；
 *   2. **局部刷新动画**：每帧只重画 96x96 的小区域（一个"小脸"上下浮动 +
 *      眨眼），数据量极小（2304 像素/帧）→ 帧率极高、控制器发热低；
 *   3. 局部区域的画面是"整块重写"（含背景），因此不存在残影问题；
 *      这也是做精灵动画的推荐方式：把角色活动范围划成一个矩形区域，
 *      每帧在这个区域内重新合成角色。
 *
 * 入口：gfx_demo() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/common.h>          /* mdelay */

#include "gfx.h"

/* 小脸活动区域（屏幕上的一个矩形） */
#define AREA_W      96
#define AREA_H      96
#define AREA_X      ((GFX_W - AREA_W) / 2)      /* 居中 */
#define AREA_Y      180

/* 小脸颜色 */
#define FACE_COLOR  GFX_CREAM
#define EYE_COLOR   GFX_BROWN
#define MOUTH_COLOR GFX_BROWN

/* 区域帧缓冲（96*96 像素 = 18KB） */
static uint16_t g_area[AREA_W * AREA_H];

/* 在区域内画"小脸"：bob = 上下浮动量，blink = 是否眨眼 */
static void build_area_frame(int bob, int blink)
{
    int y, x;

    for (y = 0; y < AREA_H; y++) {
        for (x = 0; x < AREA_W; x++) {
            uint16_t c = GFX_SKYBLUE;               /* 区域底色 */
            int fx = x - AREA_W / 2;
            int fy = y - (AREA_H / 2 + bob);        /* 脸中心随 bob 动 */
            int r2 = fx * fx + fy * fy;

            if (r2 <= 38 * 38) {
                c = FACE_COLOR;                     /* 脸 */
            }

            /* 眼睛（随 bob 移动）；眨眼时变成扁椭圆 */
            {
                int eye_dx = 16, eye_dy = -8, eye_r = 5;
                int dx1 = fx + eye_dx, dx2 = fx - eye_dx;
                int dy = fy + eye_dy;

                if (blink) {
                    if (dx1 * dx1 <= eye_r * eye_r && dy >= -1 && dy <= 1) c = EYE_COLOR;
                    if (dx2 * dx2 <= eye_r * eye_r && dy >= -1 && dy <= 1) c = EYE_COLOR;
                } else {
                    if (dx1 * dx1 + dy * dy <= eye_r * eye_r) c = EYE_COLOR;
                    if (dx2 * dx2 + dy * dy <= eye_r * eye_r) c = EYE_COLOR;
                }
            }

            /* 微笑嘴：一段下弯的弧 */
            {
                int mx = fx;
                int my = fy - 14;                   /* 相对脸中心 */
                int arc_y = 6;                      /* 弧的基线 */
                int t = mx;
                if (t >= -16 && t <= 16) {
                    int want_y = arc_y + (t * t) / 32;    /* 抛物线（下弯） */
                    if (my >= want_y - 2 && my <= want_y + 2) {
                        c = MOUTH_COLOR;
                    }
                }
            }

            g_area[y * AREA_W + x] = c;
        }
    }
}

void gfx_demo(void)
{
    int phase = 0;

    gfx_init();                             /* 初始化屏幕 */

    printf("[gfx] demo: primitives + local-refresh face animation\r\n");

    /* ---------- 1. 静态图元演示 ---------- */
    gfx_clear(GFX_SKYBLUE);
    gfx_fill_rect(40, 40, 140, 90, GFX_ORANGE);
    gfx_fill_circle(330, 110, 70, GFX_YELLOW, GFX_SKYBLUE);
    gfx_fill_triangle(240, 230, 150, 350, 330, 350, GFX_GREEN, GFX_SKYBLUE);
    gfx_line(40, 430, 440, 430, 8, GFX_DARKGRAY, GFX_SKYBLUE);
    gfx_line(40, 400, 440, 400, 2, GFX_WHITE, GFX_SKYBLUE);
    printf("[gfx] primitives drawn (rect/circle/triangle/line)\r\n");

    mdelay(2500);

    /* ---------- 2. 局部刷新动画：小脸上下浮动 + 眨眼 ---------- */
    printf("[gfx] local-refresh animation in %dx%d area @(%d,%d)\r\n",
           AREA_W, AREA_H, AREA_X, AREA_Y);

    for (;;) {
        /* 上下浮动：±8px 正弦；每 40 帧眨一次眼（持续 3 帧） */
        int bob_tab[16] = { 0, 3, 6, 8, 8, 6, 3, 0, 0, -3, -6, -8, -8, -6, -3, 0 };
        int bob = bob_tab[phase & 15];
        int blink = ((phase % 120) < 3) ? 1 : 0;

        build_area_frame(bob, blink);
        gfx_blit(AREA_X, AREA_Y, AREA_W, AREA_H, g_area);   /* 一次局部事务 */

        phase++;
        /* 不加延时，直接看局部刷新的真实速度 */
    }
}
