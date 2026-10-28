/******************************************************************************
 * @file     video_partial_test.c
 * @brief    局部刷新可行性实验（局部窗口 + 严格等长分段 DMA）
 *
 * 已知约束（实测结论）：
 *   - DW DMA 单次传输上限 = 4095 像素（12 位块大小），超出则不完成；
 *   - 面板 RAMWR 的数据条数必须与窗口像素数**严格相等**，否则事务在错误
 *     时机结束 -> 画面错位污染；
 *   - 全屏单事务（0,0,480,480，共 230400 像素）一直可靠。
 *
 * 本实验目的：确认"局部窗口"这条路径能否安全使用（它是提升帧率、降低
 * 数据传输量/发热的关键）。
 *
 * 每步都遵循同一纪律：
 *   LCD_Set_Window(局部) -> start -> 严格发送 w*h 个像素(每段 ≤3840) -> end
 * 并且**每一步之后都用已验证可靠的全屏事务恢复整屏**，用来检查两件事：
 *   (a) 局部块是否正确出现在指定位置；
 *   (b) 局部刷新是否污染了面板（恢复帧能否回到干净背景）。
 *
 * 帧序（每步 2.5 秒，串口打印步骤名；循环播放）：
 *   1. 全屏浅蓝（基线）
 *   2. 局部黄块 (40,40,200,200)
 *   3. 全屏浅蓝（恢复检查）
 *   4. 局部绿块 (240,240,200,200)
 *   5. 全屏浅蓝（恢复检查）
 *   6. 局部小黄块 (40,40,20,20)
 *   7. 全屏浅蓝（恢复检查）
 *   8. 局部黄细条 (100,300,280,60)
 *   9. 全屏浅蓝（恢复检查）
 *  10. 局部动画：蓝底 + 一个小黄方块左右往返移动（每帧两次局部刷新）
 *      —— 若前面步骤都正常，这一步能直接看出"局部刷新"的实用价值
 *      （数据量极小 → 帧率极高）
 *
 * 入口：video_partial_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "video.h"              /* video_init / HAL_Delay */

#include <drv/ospi.h>
#include <drv/pin.h>
#include <drv/pwm.h>
#include <drv/spi.h>
#include <drv/common.h>
#include <dw_dma_ll.h>
#include <dw_ospi_ll.h>
#include <dw_spi_ll.h>

extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_WritemutileData16_fast(uint16_t num);

/* ==================== DMA（与 src/video.c 一致，带超时） ==================== */
#define DWDMA     ((dw_dma_regs_t *)DW_DMA_BASE)
#define CH_IDX    2
#define DWDMA_CH  ((dw_dma_ch_regs_t *)(DW_DMA_BASE + (CH_IDX * 0x58)))

static void dma_clear_flags(void)
{
    DWDMA->ClearBlock   = (1 << CH_IDX);
    DWDMA->ClearTfr     = (1 << CH_IDX);
    DWDMA->ClearSrcTran = (1 << CH_IDX);
    DWDMA->ClearDstTran = (1 << CH_IDX);
}

static void dma_start(uint16_t *pdata, int len)
{
    dw_dma_set_transfer_size(DWDMA_CH, len);
    dw_dma_set_sar(DWDMA_CH, (uint32_t)pdata);
    csi_dcache_clean_invalid_range((uint32_t *)pdata, len * 2);
    dw_dma_set_channel_en(DWDMA, 1 << CH_IDX);
}

static int dma_wait(void)
{
    volatile uint32_t guard = 0;
    while (!DWDMA->RawBlock) {
        if (++guard > 20000000u) { dma_clear_flags(); return -1; }
    }
    dma_clear_flags();
    return 0;
}

/* ==================== 配色 ==================== */
#define COL_BG    0xBEFF        /* 浅蓝 (186,222,248) */
#define COL_YEL   0xFF48        /* 黄 (255,232,64) */
#define COL_GRN   0x27E0        /* 绿 (0,200,90) */

/* ==================== 行缓冲（8 行 × 480 = 3840 像素 = 7680B） ==================== */
#define ROWS_PER_BAND   8
#define BAND_PIX_MAX    (ROWS_PER_BAND * 480)
static uint16_t g_line[BAND_PIX_MAX];

/** 全屏单色填充（已验证可靠的路径，用作基线与恢复） */
static int full_fill(uint16_t color)
{
    int y = 0;
    int i;

    LCD_Set_Window(0, 0, 480, 480);
    LCD_startWriteMutileData();
    while (y < 480) {
        int rows = (480 - y < ROWS_PER_BAND) ? (480 - y) : ROWS_PER_BAND;
        int n = rows * 480;
        for (i = 0; i < n; i++) g_line[i] = color;
        dma_start(&g_line[0], n);
        if (dma_wait() != 0) { LCD_endWriteMutileData(); return -1; }
        y += rows;
    }
    LCD_endWriteMutileData();
    return 0;
}

/** 局部发送：缓冲内容由调用者填充（行优先，ww*wh 像素） */
static int partial_send(int wx, int wy, int ww, int wh)
{
    int done = 0;

    LCD_Set_Window((uint16_t)wx, (uint16_t)wy, (uint16_t)ww, (uint16_t)wh);
    LCD_startWriteMutileData();
    while (done < wh) {
        int rows = (wh - done < ROWS_PER_BAND) ? (wh - done) : ROWS_PER_BAND;
        int n = rows * ww;
        dma_start(&g_line[done * ww], n);
        if (dma_wait() != 0) { LCD_endWriteMutileData(); return -1; }
        done += rows;
    }
    LCD_endWriteMutileData();
    udelay(60);                 /* 事务间隔：避免背靠背窗口事务被面板漏处理 */
    return 0;
}

/** 局部单色填充：窗口 (wx,wy,ww,wh)，严格发送 ww*wh 个像素 */
static int partial_fill(int wx, int wy, int ww, int wh, uint16_t color)
{
    int done = 0;
    int i;

    LCD_Set_Window((uint16_t)wx, (uint16_t)wy, (uint16_t)ww, (uint16_t)wh);
    LCD_startWriteMutileData();
    while (done < wh) {
        int rows = (wh - done < ROWS_PER_BAND) ? (wh - done) : ROWS_PER_BAND;
        int n = rows * ww;                       /* 与窗口内容严格等长 */
        for (i = 0; i < n; i++) g_line[i] = color;
        dma_start(&g_line[0], n);
        if (dma_wait() != 0) { LCD_endWriteMutileData(); return -1; }
        done += rows;
    }
    LCD_endWriteMutileData();
    udelay(60);
    return 0;
}

/* ==================== 测试入口 ==================== */
void video_partial_test(void)
{
    int step = 0;
    int rc;
    int frame_no_anim = 0;

    video_init();               /* 官方初始化 + 三色自检 */

    printf("[partial] local-window refresh feasibility test\r\n");

    for (;;) {
        switch (step) {
        case 0:
            printf("[partial] 1 baseline: fullscreen BG\r\n");
            rc = full_fill(COL_BG);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 1:
            printf("[partial] 2 local yellow block (40,40,200,200)\r\n");
            rc = partial_fill(40, 40, 200, 200, COL_YEL);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 2:
            printf("[partial] 3 restore: fullscreen BG (pollution check)\r\n");
            rc = full_fill(COL_BG);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 3:
            printf("[partial] 4 local green block (240,240,200,200)\r\n");
            rc = partial_fill(240, 240, 200, 200, COL_GRN);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 4:
            printf("[partial] 5 restore: fullscreen BG\r\n");
            rc = full_fill(COL_BG);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 5:
            printf("[partial] 6 tiny yellow block (40,40,20,20)\r\n");
            rc = partial_fill(40, 40, 20, 20, COL_YEL);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 6:
            printf("[partial] 7 restore: fullscreen BG\r\n");
            rc = full_fill(COL_BG);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 7:
            printf("[partial] 8 local yellow bar (100,300,280,60)\r\n");
            rc = partial_fill(100, 300, 280, 60, COL_YEL);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        case 8:
            printf("[partial] 9 restore: fullscreen BG\r\n");
            rc = full_fill(COL_BG);
            printf("[partial]   rc=%d\r\n", rc);
            HAL_Delay(2500);
            break;

        default:
            /* 10. 局部刷新动画：小方块左右往返
             *     关键：每帧只做**一次**局部事务——把"旧位置 ∪ 新位置"的
             *     矩形一次刷好（新位置黄、其余背景），避免背靠背两次窗口
             *     事务被面板漏处理而残留竖条 */
            {
                int x = 0, oldx = 0, dir = 1;
                int frame, i, j;
                printf("[partial] 10 local-refresh animation (single transaction/frame)\r\n");
                full_fill(COL_BG);

                for (frame = 0; frame < 400; frame++) {
                    int ux = (x < oldx) ? x : oldx;              /* 合并区域左边界（相对块原点） */
                    int uw = 40 + ((x < oldx) ? (oldx - x) : (x - oldx));
                    int rc;

                    if (uw * 40 > BAND_PIX_MAX) uw = BAND_PIX_MAX / 40;
                    if (uw > 480 - 60 - ux) uw = 480 - 60 - ux;  /* 窗口不出屏 */

                    /* 填缓冲：窗口内每行，新块位置黄、其余背景 */
                    for (i = 0; i < 40; i++) {
                        uint16_t *row = &g_line[i * uw];
                        for (j = 0; j < uw; j++) {
                            int px = ux + j;
                            row[j] = (px >= x && px < x + 40) ? COL_YEL : COL_BG;
                        }
                    }

                    rc = partial_send(60 + ux, 200, uw, 40);
                    oldx = x;

                    x += dir * 8;
                    if (x <= 0) { x = 0; dir = 1; }
                    if (x >= 360) { x = 360; dir = -1; }

                    if ((++frame_no_anim % 20) == 0) {
                        printf("[partial] anim frame %d x=%d uw=%d rc=%d\r\n",
                               frame_no_anim, x, uw, rc);
                    }
                }
                printf("[partial] animation done, loop back to step 1\r\n");
            }
        }

        step = (step + 1) % 10;
    }
}
