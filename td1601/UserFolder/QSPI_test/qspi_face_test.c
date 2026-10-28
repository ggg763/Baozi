/******************************************************************************
 * @file     qspi_face_test.c
 * @brief    轻量可爱表情动画测试（QSPI 480x480 屏幕，循环播放）
 *
 * 硬件：TD1601 EVB 扩展板 CON2 QSPI 屏（TR230S 480x480），
 *       驱动见同目录 qspi_lcd.c（移植自 QSPI_480 LCD 模板）。
 *
 * 内容：一个"包子脸"大头表情，循环播放 4 个可爱表情帧：
 *         ① 睁眼微笑  ② 眯眯眼微笑  ③ 睁眼张嘴大笑  ④ 眯眯眼张嘴大笑
 *       每帧全量重绘（先刷背景再画脸），简单可靠、无需逐帧差分。
 *
 * 画圆一律用 qspi_fill_circle（单次窗口事务，圆内/圆外双色），避免逐行
 * 小窗口造成漏画/花屏；全部几何为整数运算，坐标集中在顶部宏可调。
 *
 * 入口：qspi_face_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/common.h>     /* mdelay */
#include "qspi_lcd.h"

/* ==================== 配色（RGB565） ==================== */
#define COL_BG      QSPI_RGB565(255, 244, 205)   /* 背景：米白 */
#define COL_SKIN    QSPI_RGB565(255, 201, 128)   /* 脸：橙黄（与背景对比明显） */
#define COL_INK     QSPI_RGB565(74, 52, 40)      /* 眼/嘴：深棕 */
#define COL_BLUSH   QSPI_RGB565(255, 118, 136)   /* 腮红：粉 */
#define COL_HILITE  QSPI_COLOR_WHITE             /* 眼睛高光 */

/* ==================== 脸部几何参数（480x480 居中） ==================== */
#define FACE_CX     240
#define FACE_CY     258
#define FACE_R      178

#define EYE_Y       205
#define EYE_DX      82          /* 眼中心到脸中心的水平距离 */
#define EYE_R       25          /* 睁眼半径 */
#define HL_DX       14          /* 高光相对眼中心偏移（朝脸内侧上方） */
#define HL_DY       11
#define HL_R        8

#define BLUSH_DX    148         /* 腮红水平偏移 */
#define BLUSH_Y     300
#define BLUSH_R     30

/* ==================== 表情状态 ==================== */
#define EYE_OPEN    0           /* 睁眼：大圆眼 + 高光 */
#define EYE_SQUINT  1           /* 眯眯笑眼：下弯弧点列 */

#define MOUTH_SMILE 0           /* 微笑：二次曲线点列 */
#define MOUTH_OPEN  1           /* 张嘴大笑：深色大圆嘴 */

/* ==================== 基础绘图 ==================== */

/* 二次曲线点列：用一排小方块拼出下弯弧（∪，嘴微笑 / 眯眯眼用）。
 * cx,cy 为弧两端所在的基线（两端 y=cy），中点下垂 drop。
 * 共 N 个点，每点 (step)x(step) 像素见方。 */
static void draw_arc_dots(int cx, int cy, int span_half, int drop,
                          int step, uint16_t color)
{
    /* t 从 -n 到 n：x 等距，y = cy + (n^2 - t^2)*drop/n^2
     * 即两端(t=±n)在基线、中点(t=0)下垂最多 drop */
    int n = (span_half + step / 2) / step;
    int t;
    for (t = -n; t <= n; t++) {
        int xx = cx + t * step;
        int yy = cy + ((n * n - t * t) * drop) / (n * n);
        qspi_fill_rect((uint16_t)(xx - step / 2), (uint16_t)(yy - step / 2),
                       (uint16_t)step, (uint16_t)step, color);
    }
}

/* ==================== 脸部绘制 ==================== */
static void draw_face(int eye_state, int mouth_state)
{
    int ex_l = FACE_CX - EYE_DX;          /* 左眼中心 x */
    int ex_r = FACE_CX + EYE_DX;          /* 右眼中心 x */

    /* 1. 背景 + 脸（圆外接矩形其余处填背景色，盖掉旧帧） */
    qspi_fill_rect(0, 0, QSPI_LCD_W, QSPI_LCD_H, COL_BG);
    qspi_fill_circle(FACE_CX, FACE_CY, FACE_R, COL_SKIN, COL_BG);

    /* 2. 腮红（常驻，画在脸上） */
    qspi_fill_circle(FACE_CX - BLUSH_DX, BLUSH_Y, BLUSH_R, COL_BLUSH, COL_SKIN);
    qspi_fill_circle(FACE_CX + BLUSH_DX, BLUSH_Y, BLUSH_R, COL_BLUSH, COL_SKIN);

    /* 3. 眼睛 */
    if (eye_state == EYE_OPEN) {
        qspi_fill_circle(ex_l, EYE_Y, EYE_R, COL_INK, COL_SKIN);
        qspi_fill_circle(ex_r, EYE_Y, EYE_R, COL_INK, COL_SKIN);
        /* 高光：小白点（外接矩形其余填黑=仅挖出高光），眼神更亮 */
        qspi_fill_circle(ex_l + HL_DX, EYE_Y - HL_DY, HL_R, COL_HILITE, COL_INK);
        qspi_fill_circle(ex_r - HL_DX, EYE_Y - HL_DY, HL_R, COL_HILITE, COL_INK);
    } else {
        /* 眯眯笑眼：下弯短弧（下垂 16px） */
        draw_arc_dots(ex_l, EYE_Y, 34, 16, 9, COL_INK);
        draw_arc_dots(ex_r, EYE_Y, 34, 16, 9, COL_INK);
    }

    /* 4. 嘴 */
    if (mouth_state == MOUTH_SMILE) {
        /* 微笑弧（每点 11x11，半宽 66、下垂 36） */
        draw_arc_dots(FACE_CX, FACE_CY + 72, 66, 36, 11, COL_INK);
    } else {
        /* 张嘴大笑：深色大圆嘴（画在脸上） */
        qspi_fill_circle(FACE_CX, FACE_CY + 100, 46, COL_INK, COL_SKIN);
    }
}

/* ==================== 测试入口（循环播放） ==================== */
void qspi_face_test(void)
{
    qspi_lcd_init();

    printf("[face] cute expression animation start\r\n");

    for (;;) {
        /* ① 睁眼微笑 */
        draw_face(EYE_OPEN, MOUTH_SMILE);
        mdelay(1500);
        /* ② 眯眯眼微笑 */
        draw_face(EYE_SQUINT, MOUTH_SMILE);
        mdelay(600);
        /* ③ 睁眼张嘴大笑 */
        draw_face(EYE_OPEN, MOUTH_OPEN);
        mdelay(1000);
        /* ④ 眯眯眼张嘴大笑 */
        draw_face(EYE_SQUINT, MOUTH_OPEN);
        mdelay(600);
    }
}
