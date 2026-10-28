/******************************************************************************
 * @file     gfx_sprite_demo.c
 * @brief    图片精灵动画上手示例（配套 UserFolder/gfx/tools/png2c.py）
 *
 * 这是"美术图片 → 屏幕上动起来"的标准流程演示：
 *
 *   ① 美术同学在 PC 上画好若干帧 PNG（本例为 3 帧表情：微笑 / 张嘴 / 眨眼）
 *   ② 用工具转换：
 *        python tools/png2c.py smile.png open.png blink.png --name face > face_sprite.h
 *   ③ 代码里 include 并显示：
 *        gfx_blit_key(x, y, FACE_W, FACE_H, face_frames[i], 0xF81F, GFX_SKYBLUE);
 *
 * 本文件为了"不依赖外部图片也能直接跑"，用代码生成了 3 帧 48x48 的示例精灵
 * （结构与 png2c.py 输出的数组完全一致：uint16_t frames[n][W*H]，行优先）。
 * 真正接入美术资源时，把 build_frames() 和 g_frames 换成：
 *
 *        #include "face_sprite.h"
 *        #define SPR_W FACE_W
 *        #define SPR_H FACE_H
 *        #define KEY   0xF81F
 *        // 用 face_frames[i] 代替 g_frames[i]
 *
 * 关键技巧：每帧只往**同一个矩形区域**贴图（区域内含背景），
 *           所以是"整块重写"，绝不会残留；数据量 48x48 = 4.6KB/帧，
 *           刷新一次只要几十微秒。
 *
 * 入口：gfx_sprite_demo() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>

#include "gfx.h"

#define SPR_W       48
#define SPR_H       48
#define SPR_FRAMES  3
#define KEY         0xF81F              /* 透明色键（洋红，美术图里用这个色做背景） */

/* 精灵帧数据：3 帧 × 48×48 像素（行优先，与 png2c.py 输出格式一致） */
static uint16_t g_frames[SPR_FRAMES][SPR_W * SPR_H];

/* 用代码生成 3 帧示例精灵（真实项目请用 png2c.py 生成的头文件替换） */
static void build_frames(void)
{
    int f, x, y;

    for (f = 0; f < SPR_FRAMES; f++) {
        for (y = 0; y < SPR_H; y++) {
            for (x = 0; x < SPR_W; x++) {
                int dx = x - SPR_W / 2;
                int dy = y - SPR_H / 2;
                uint16_t c = KEY;                       /* 默认：透明（会被底色替换） */

                if (dx * dx + dy * dy <= 22 * 22) {
                    c = GFX_CREAM;                      /* 脸 */
                }

                /* 眼睛：帧 2 是眨眼（细线），其余是圆眼 */
                {
                    int eye_dx = 8, eye_dy = -5, eye_r = 3;
                    int d1 = dx + eye_dx, d2 = dx - eye_dx, dyy = dy - eye_dy;
                    if (f == 2) {
                        if (d1 * d1 <= 9 && dyy >= -1 && dyy <= 1) c = GFX_BROWN;
                        if (d2 * d2 <= 9 && dyy >= -1 && dyy <= 1) c = GFX_BROWN;
                    } else {
                        if (d1 * d1 + dyy * dyy <= eye_r * eye_r) c = GFX_BROWN;
                        if (d2 * d2 + dyy * dyy <= eye_r * eye_r) c = GFX_BROWN;
                    }
                }

                /* 嘴：帧 1 是张嘴（实心圆），其余是微笑弧 */
                {
                    int mdy = dy - 8;
                    if (f == 1) {
                        if (dx * dx + mdy * mdy <= 4 * 4) c = GFX_BROWN;
                    } else {
                        if (dx >= -8 && dx <= 8) {
                            int want = (dx * dx) / 8;   /* 下弯抛物线 */
                            if (mdy >= want - 1 && mdy <= want + 1) c = GFX_BROWN;
                        }
                    }
                }

                g_frames[f][y * SPR_W + x] = c;
            }
        }
    }
}

void gfx_sprite_demo(void)
{
    /* 上下浮动的相位表（让形象有"呼吸感"） */
    static const int bob_tab[16] = { 0, 1, 2, 3, 3, 2, 1, 0, 0, -1, -2, -3, -3, -2, -1, 0 };

    int phase = 0;
    int frame = 0;

    gfx_init();                             /* 初始化屏幕 */
    build_frames();

    printf("[sprite] multi-frame sprite demo (48x48, 3 frames)\r\n");
    printf("[sprite] real project: replace with png2c.py generated header\r\n");

    gfx_clear(GFX_SKYBLUE);                 /* 铺底 */

    /* 在底部画一条"地面"线，便于判断角色是否在动 */
    gfx_line(60, 380, 420, 380, 6, GFX_DARKGRAY, GFX_SKYBLUE);

    for (;;) {
        int bob = bob_tab[phase & 15];

        /* 每帧只贴这个 48x48 区域：区域内的内容整块重写（含背景）→ 无残留 */
        gfx_blit_key((GFX_W - SPR_W) / 2, 200 + bob, SPR_W, SPR_H,
                     g_frames[frame], KEY, GFX_SKYBLUE);

        /* 每 60 帧换一次表情（约 0.3~0.6s，视实际帧率） */
        if ((++phase % 60) == 0) {
            frame = (frame + 1) % SPR_FRAMES;
        }
    }
}
