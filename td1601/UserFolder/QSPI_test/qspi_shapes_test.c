/******************************************************************************
 * @file     qspi_shapes_test.c
 * @brief    最小三角形测试：每轮 = 全屏浅蓝 → 静止黄色三角形 4s → 循环
 *
 * 目的：把测试收敛到"单帧、单图形、静止、可自愈"，便于逐轮观察描述。
 *
 * 每轮流程（串口打印帧号与三角形窗口范围）：
 *   1. 全屏浅蓝（干净起点，也用于自愈上一轮可能的污染）
 *   2. 停留 300ms
 *   3. 画居中的黄色等边三角形（外接半径 R_TRI，窗口范围会打印）
 *   4. 停留 3000ms —— 请在这一帧观察/拍照：三角形是否完整等边、
 *      边线是否直、颜色是否纯黄、窗口范围(打印值)内外有无杂物
 *
 * 可调：改 R_TRI 大小；若怀疑大窗口，先调小（如 80）看是否改善。
 *
 * 入口：qspi_shapes_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/common.h>     /* mdelay */
#include "qspi_lcd.h"

/* ==================== 配色（RGB565） ==================== */
#define COL_BG   QSPI_RGB565(186, 222, 248)   /* 浅蓝背景 */
#define COL_YEL  QSPI_RGB565(255, 232, 64)    /* 三角形：黄 */

/* ==================== 三角形 ==================== */
#define CX       240
#define CY       240
#define R_TRI    150             /* 外接半径（改小可缩小窗口，便于排查） */

/* ==================== 测试入口 ==================== */
void qspi_shapes_test(void)
{
    int x0, y0, x1, y1, x2, y2;

    qspi_lcd_init();

    printf("[tri] minimal static triangle test\r\n");

    /* 三角形三顶点：上 (CX, CY-R)；下两点 (CX±R*cos30, CY+R/2) */
    x0 = CX;                 y0 = CY - R_TRI;
    x1 = CX - 130;           y1 = CY + 75;
    x2 = CX + 130;           y2 = CY + 75;

    for (;;) {
        printf("[tri] bg -> yellow triangle @x[%d..%d] y[%d..%d]\r\n",
               CX - 130, CX + 130, CY - R_TRI, CY + 75);

        /* 1. 全屏浅蓝（干净起点/自愈） */
        qspi_fill_rect(0, 0, QSPI_LCD_W, QSPI_LCD_H, COL_BG);
        mdelay(300);

        /* 2. 静止黄色三角形（单窗口），停留 4s 供观察 */
        qspi_fill_triangle(x0, y0, x1, y1, x2, y2, COL_YEL, COL_BG);
        mdelay(3000);
    }
}
