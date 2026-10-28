/******************************************************************************
 * @file     video_smile_test.c
 * @brief    灵动笑脸：只绘制眼睛与嘴巴（不画脸轮廓），循环演出
 *
 * 底座（已验证稳定）：官方 video.c + 每帧一次全屏 RAMWR 事务，像素由 CPU
 * 逐行软件合成。无 DMA、无局部窗口、无帧缓冲（仅 480x2B 行缓冲）。
 *
 * 表情元素（全部整数几何，无浮点/无 libm）：
 *   眼睛三态：睁眼（圆 + 白高光）/ 眨眼（扁椭圆）/ 笑眼（圆减上移圆 -> 下月牙）
 *   嘴巴三态：微笑弧（下半椭圆环带）/ 张嘴笑（实心半椭圆）/ 惊喜 O 形（椭圆环带）
 *   灵动点：眼睛高光随"看向"偏移；眨眼只持续 3 帧；笑声时眼睛同步变月牙
 *
 * 演出脚本按帧驱动（frame_t 表，循环播放）。
 *
 * 入口：video_smile_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "video.h"              /* video_init / HAL_Delay */

/* video.h 未声明的官方原语（video.c 中定义） */
extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_WritemutileData16_fast(uint16_t num);

/* ==================== 配色（RGB565） ==================== */
#define COL_BG    0xBEFF        /* 背景浅蓝 (186,222,248) */
#define COL_INK   0x49A5        /* 眼睛/嘴巴深棕 (74,52,40) */
#define COL_WHT   0xFFFF        /* 高光白 */

/* ==================== 布局 ==================== */
#define EYE_LX    165           /* 左眼中心 x */
#define EYE_RX    315           /* 右眼中心 x */
#define EYE_CY    190           /* 眼睛中心 y */
#define EYE_R     42            /* 睁眼半径 */
#define EYE_BLINK_RY 9          /* 眨眼时的纵向半径 */
#define EYE_SMILE_DY 18         /* 笑眼月牙厚度（遮罩圆上移量） */
#define HL_R      13            /* 高光半径 */
#define HL_OFF_X  (-12)         /* 高光相对眼中心偏移 */
#define HL_OFF_Y  (-12)

#define MOUTH_CX  240
#define MOUTH_CY  318
#define SMILE_OUT_RX 78         /* 微笑弧：外半轴 */
#define SMILE_OUT_RY 50
#define SMILE_IN_RX  64         /* 微笑弧：内半轴（形成弧带） */
#define SMILE_IN_RY  38
#define BIG_RX    72            /* 张嘴笑：半椭圆 */
#define BIG_RY    48
#define OMOUTH_OUT_RX 45        /* O 形嘴：外半轴 */
#define OMOUTH_OUT_RY 32
#define OMOUTH_IN_RX  27
#define OMOUTH_IN_RY  17

#define GAZE_MAX  12            /* 眼神最大偏移 px */

/* ==================== 表情状态 ==================== */
#define EYE_OPEN   0
#define EYE_BLINK  1
#define EYE_SMILE  2

#define MOUTH_SMILE 0
#define MOUTH_BIG   1
#define MOUTH_O     2

typedef struct {
    uint8_t eye;        /* EYE_* */
    uint8_t mouth;      /* MOUTH_* */
    int8_t  gaze;       /* 眼神偏移（-GAZE_MAX..+GAZE_MAX） */
    uint8_t frames;     /* 持续帧数 */
} frame_t;

/* 演出脚本：常态 -> 眨眼 -> 微笑 -> 张嘴笑 -> 眨眼 -> 看左/看右 -> 惊喜 O 嘴 */
static const frame_t g_anim[] = {
    { EYE_OPEN,  MOUTH_SMILE,   0, 30 },
    { EYE_BLINK, MOUTH_SMILE,   0,  3 },
    { EYE_OPEN,  MOUTH_SMILE,   0, 18 },
    { EYE_SMILE, MOUTH_BIG,     0, 20 },
    { EYE_BLINK, MOUTH_BIG,     0,  3 },
    { EYE_SMILE, MOUTH_BIG,     0, 14 },
    { EYE_OPEN,  MOUTH_SMILE, -12, 12 },
    { EYE_OPEN,  MOUTH_SMILE,  12, 12 },
    { EYE_OPEN,  MOUTH_O,       0, 14 },
    { EYE_OPEN,  MOUTH_SMILE,   0, 26 },
};
#define ANIM_N (int)(sizeof(g_anim) / sizeof(g_anim[0]))

/* ==================== 行缓冲 ==================== */
#define ROW_W 480
static uint16_t g_row[ROW_W];

static void row_fill(uint16_t *row, uint16_t color)
{
    int x;
    for (x = 0; x < ROW_W; x++) row[x] = color;
}

/* ==================== 图元判定（整数，无浮点） ==================== */

/* 点是否在椭圆内（半轴 rx/ry，中心为原点） */
static inline int in_ellipse(int dx, int dy, int rx, int ry)
{
    return (dx * dx * ry * ry + dy * dy * rx * rx) <= (rx * rx * ry * ry);
}

/* 画一只眼睛（含高光）到第 y 行；gaze 为眼神偏移 */
static void draw_eye(uint16_t *row, int y, int ex, int ey, int mode, int gaze)
{
    int ddy = y - ey;
    int x0, x1, x;

    if (mode == EYE_BLINK) {
        if (ddy < -EYE_BLINK_RY || ddy > EYE_BLINK_RY) return;
        x0 = ex - EYE_R; x1 = ex + EYE_R;
    } else {
        if (ddy < -EYE_R || ddy > EYE_R) return;
        x0 = ex - EYE_R; x1 = ex + EYE_R;
    }
    if (x0 < 0) x0 = 0;
    if (x1 > ROW_W - 1) x1 = ROW_W - 1;

    for (x = x0; x <= x1; x++) {
        int dx = x - ex;

        if (mode == EYE_BLINK) {
            if (in_ellipse(dx, ddy, EYE_R, EYE_BLINK_RY)) {
                row[x] = COL_INK;
            }
        } else if (mode == EYE_SMILE) {
            /* 圆减去上移 EYE_SMILE_DY 的同半径圆 -> 下月牙（笑眼） */
            if ((dx * dx + ddy * ddy) <= (EYE_R * EYE_R) &&
                (dx * dx + (ddy + EYE_SMILE_DY) * (ddy + EYE_SMILE_DY)) > (EYE_R * EYE_R)) {
                row[x] = COL_INK;
            }
        } else {
            if ((dx * dx + ddy * ddy) <= (EYE_R * EYE_R)) {
                row[x] = COL_INK;
            }
            /* 高光：随 gaze 移动，让眼神有方向感 */
            {
                int hdx = x - (ex + HL_OFF_X + gaze);
                int hdy = y - (ey + HL_OFF_Y);
                if ((hdx * hdx + hdy * hdy) <= (HL_R * HL_R)) {
                    row[x] = COL_WHT;
                }
            }
        }
    }
}

/* 画嘴巴到第 y 行 */
static void draw_mouth(uint16_t *row, int y, int mode)
{
    int dy = y - MOUTH_CY;
    int x, x0, x1;

    if (mode == MOUTH_SMILE) {
        if (dy < 0 || dy > SMILE_OUT_RY) return;        /* 只取下半 */
        x0 = MOUTH_CX - SMILE_OUT_RX; x1 = MOUTH_CX + SMILE_OUT_RX;
    } else if (mode == MOUTH_BIG) {
        if (dy < 0 || dy > BIG_RY) return;
        x0 = MOUTH_CX - BIG_RX; x1 = MOUTH_CX + BIG_RX;
    } else {
        if (dy > OMOUTH_OUT_RY || dy < -OMOUTH_OUT_RY) return;
        x0 = MOUTH_CX - OMOUTH_OUT_RX; x1 = MOUTH_CX + OMOUTH_OUT_RX;
    }
    if (x0 < 0) x0 = 0;
    if (x1 > ROW_W - 1) x1 = ROW_W - 1;

    for (x = x0; x <= x1; x++) {
        int dx = x - MOUTH_CX;

        if (mode == MOUTH_SMILE) {
            /* 下半椭圆环带 = 微笑弧 */
            if (in_ellipse(dx, dy, SMILE_OUT_RX, SMILE_OUT_RY) &&
                !in_ellipse(dx, dy, SMILE_IN_RX, SMILE_IN_RY)) {
                row[x] = COL_INK;
            }
        } else if (mode == MOUTH_BIG) {
            /* 实心半椭圆 = 张嘴笑 */
            if (in_ellipse(dx, dy, BIG_RX, BIG_RY)) {
                row[x] = COL_INK;
            }
        } else {
            /* 椭圆环带 = 惊讶 O 形嘴 */
            if (in_ellipse(dx, dy, OMOUTH_OUT_RX, OMOUTH_OUT_RY) &&
                !in_ellipse(dx, dy, OMOUTH_IN_RX, OMOUTH_IN_RY)) {
                row[x] = COL_INK;
            }
        }
    }
}

/* ==================== 单行合成 ==================== */
static void render_row(uint16_t *row, int y, int eye_mode, int mouth_mode, int gaze)
{
    row_fill(row, COL_BG);
    draw_eye(row, y, EYE_LX + gaze, EYE_CY, eye_mode, gaze);
    draw_eye(row, y, EYE_RX + gaze, EYE_CY, eye_mode, gaze);
    draw_mouth(row, y, mouth_mode);
}

/* ==================== 测试入口 ==================== */
void video_smile_test(void)
{
    int idx = 0;            /* 当前脚本项 */
    int left = 0;           /* 本项剩余帧数 */

    video_init();           /* 官方初始化 + 三色自检 */

    printf("[smile] eyes + mouth only, lively expression loop\r\n");

    left = g_anim[0].frames;

    for (;;) {
        const frame_t *f = &g_anim[idx];
        int y, x;

        LCD_Set_Window(0, 0, 480, 480);
        LCD_startWriteMutileData();
        for (y = 0; y < 480; y++) {
            render_row(g_row, y, f->eye, f->mouth, f->gaze);
            for (x = 0; x < 480; x++) {
                LCD_WritemutileData16_fast(g_row[x]);
            }
        }
        LCD_endWriteMutileData();

        /* 脚本推进：表情阶段按帧数切换 */
        if (--left <= 0) {
            idx = (idx + 1) % ANIM_N;
            left = g_anim[idx].frames;
        }
        HAL_Delay(20);
    }
}
