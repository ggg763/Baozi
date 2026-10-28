/******************************************************************************
 * @file     video_tetra_test.c
 * @brief    3D 旋转正四面体 + 双缓冲流水线 DMA（帧率优化版）
 *
 * 已确认的硬件限制（实测结论）：
 *   - DW DMA 单次传输上限 = 4095 像素（12 位块大小）：
 *     一次 8 行 = 3840 像素 -> 正常；16 行 = 7680 像素 -> 超限不完成。
 *   - 任何一段 DMA 失败都会让面板 RAMWR 数据流长度不符 -> 画面错位污染，
 *     因此**段长必须固定 8 行**、且每帧都完整写满 480 行。
 *
 * 本版优化：
 *   1. 段长固定 8 行（3840 像素），每帧 60 段；
 *   2. **双缓冲流水线**：dma_start(上一段) 之后 CPU 立即合成下一段到另一块
 *      缓冲，再 dma_wait 上一段 -> 传输时间与合成时间重叠，
 *      帧时间 ≈ max(传输 ~20ms, 合成 ~10ms) → 期望 35~45fps；
 *   3. 保留超时保护（超时会打印 TIMEOUT，不再死等）。
 *
 * 渲染：软件 3D（整数定点）——正四面体绕 X/Y 旋转、透视投影、外法线定向、
 *       背面剔除、平面光照（64 档 LUT）、画家算法、逐行光栅化。
 *
 * 观测：每 10 帧打印帧号，用带时间戳的串口助手算帧率。
 *
 * 入口：video_tetra_test() —— 由 src/main.c 调用
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

/* video.c 中的官方原语 */
extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_WritemutileData16_fast(uint16_t num);

/* ==================== DMA 寄存器（与 src/video.c 一致） ==================== */
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

/* 启动一段 DMA（不等待完成） */
static void dma_start(uint16_t *pdata, int len)
{
    dw_dma_set_transfer_size(DWDMA_CH, len);
    dw_dma_set_sar(DWDMA_CH, (uint32_t)pdata);
    csi_dcache_clean_invalid_range((uint32_t *)pdata, len * 2);
    dw_dma_set_channel_en(DWDMA, 1 << CH_IDX);
}

/* 等待上一段 DMA 完成；0 成功，-1 超时 */
static int dma_wait(void)
{
    volatile uint32_t guard = 0;
    while (!DWDMA->RawBlock) {
        if (++guard > 20000000u) {          /* 约 1s 超时 */
            dma_clear_flags();
            return -1;
        }
    }
    dma_clear_flags();
    return 0;
}

/* ==================== 配色 ==================== */
#define COL_BG    0xBEFF        /* 背景浅蓝 (186,222,248) */
#define COL_BASE  0xFBC7        /* 四面体基色：橙红 (255,120,60) */
#define COL_YEL   0xFF48        /* 自检黄 (255,232,64) */
#define COL_GRN   0x27E0        /* 自检绿 (0,200,90) */

/* ==================== 屏幕与投影参数 ==================== */
#define SCR_W     480
#define SCR_H     480
#define CX        240
#define CY        240
#define PROJ_P    260            /* 透视：screen = C + X*P/(Z+D) */
#define PROJ_D    400
#define VTX_R     148            /* 顶点分量（定点 256 = 1.0） */

static const int8_t g_face[4][3] = {
    { 0, 1, 2 },
    { 0, 3, 1 },
    { 0, 2, 3 },
    { 1, 3, 2 },
};

/* 光照方向（定点 256，指向光源）：左上偏相机侧 */
#define LX (-107)
#define LY (-128)
#define LZ (195)

/* ==================== 明暗 LUT（64 档） ==================== */
#define LUT_N 64
static uint16_t g_lut[LUT_N];

static uint16_t shade565(uint16_t base, int num)
{
    int r = (base >> 11) & 0x1F;
    int g = (base >> 5) & 0x3F;
    int b = base & 0x1F;
    int r8 = (r * 255 + 15) / 31;
    int g8 = (g * 255 + 31) / 63;
    int b8 = (b * 255 + 15) / 31;

    r8 = r8 * num / 100; if (r8 > 255) r8 = 255;
    g8 = g8 * num / 100; if (g8 > 255) g8 = 255;
    b8 = b8 * num / 100; if (b8 > 255) b8 = 255;
    return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}

static void build_lut(void)
{
    int i;
    for (i = 0; i < LUT_N; i++) {
        int num = 45 + (i * 70) / (LUT_N - 1);   /* 45% .. 115% */
        g_lut[i] = shade565(COL_BASE, num);
    }
}

/* ==================== 定点 sin 表（×1024，角度 0..359） ==================== */
static const int16_t g_sin1024[360] = {
    0, 18, 36, 54, 71, 89, 107, 125, 143, 160, 178, 195, 213, 230, 248, 265,
    282, 299, 316, 333, 350, 367, 384, 400, 416, 433, 449, 465, 481, 496, 512, 527,
    543, 558, 573, 587, 602, 616, 630, 644, 658, 672, 685, 698, 711, 724, 737, 749,
    761, 773, 784, 796, 807, 818, 828, 839, 849, 859, 868, 878, 887, 896, 904, 912,
    920, 928, 935, 943, 949, 956, 962, 968, 974, 979, 984, 989, 994, 998, 1002, 1005,
    1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024, 1024, 1023, 1023, 1022, 1020,
    1018, 1016, 1014, 1011, 1008, 1005, 1002, 998, 994, 989, 984, 979, 974, 968, 962, 956,
    949, 943, 935, 928, 920, 912, 904, 896, 887, 878, 868, 859, 849, 839, 828, 818,
    807, 796, 784, 773, 761, 749, 737, 724, 711, 698, 685, 672, 658, 644, 630, 616,
    602, 587, 573, 558, 543, 527, 512, 496, 481, 465, 449, 433, 416, 400, 384, 367,
    350, 333, 316, 299, 282, 265, 248, 230, 213, 195, 178, 160, 143, 125, 107, 89,
    71, 54, 36, 18, 0, -18, -36, -54, -71, -89, -107, -125, -143, -160, -178, -195,
    -213, -230, -248, -265, -282, -299, -316, -333, -350, -367, -384, -400, -416, -433, -449, -465,
    -481, -496, -512, -527, -543, -558, -573, -587, -602, -616, -630, -644, -658, -672, -685, -698,
    -711, -724, -737, -749, -761, -773, -784, -796, -807, -818, -828, -839, -849, -859, -868, -878,
    -887, -896, -904, -912, -920, -928, -935, -943, -949, -956, -962, -968, -974, -979, -984, -989,
    -994, -998, -1002, -1005, -1008, -1011, -1014, -1016, -1018, -1020, -1022, -1023, -1023, -1024, -1024, -1024,
    -1023, -1023, -1022, -1020, -1018, -1016, -1014, -1011, -1008, -1005, -1002, -998, -994, -989, -984, -979,
    -974, -968, -962, -956, -949, -943, -935, -928, -920, -912, -904, -896, -887, -878, -868, -859,
    -849, -839, -828, -818, -807, -796, -784, -773, -761, -749, -737, -724, -711, -698, -685, -672,
    -658, -644, -630, -616, -602, -587, -573, -558, -543, -527, -512, -496, -481, -465, -449, -433,
    -416, -400, -384, -367, -350, -333, -316, -299, -282, -265, -248, -230, -213, -195, -178, -160,
    -143, -125, -107, -89, -71, -54, -36, -18
};

static inline int cos1024(int deg) { return g_sin1024[((deg + 90) % 360 + 360) % 360]; }
static inline int sin1024(int deg) { return g_sin1024[((deg % 360) + 360) % 360]; }

static int isqrt(int n)
{
    int x, y;
    if (n <= 0) return 0;
    x = n; y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

/* ==================== 顶点与面 ==================== */
typedef struct { int x, y, z; } vtx_t;
typedef struct { int x, y; } pt2_t;

static vtx_t g_world[4];
static pt2_t g_scr[4];

static int      g_vis[4];
static uint16_t g_fcol[4];
static int      g_fdepth[4];
static int      g_forder[4];
static int      g_fa[4][3], g_fb[4][3], g_fc[4][3], g_fs[4][3];
static pt2_t    g_fpt[4][3];
static int      g_fpt_minx[4], g_fpt_maxx[4], g_fpt_miny[4], g_fpt_maxy[4];

/* ==================== 行缓冲（双缓冲流水线，每段 8 行） ==================== */
#define BAND_LINES 8                            /* 8*480 = 3840 像素 < 4095 上限 */
static uint16_t g_band[2][BAND_LINES][SCR_W];   /* 2 * 7680B = 15360B */

static void row_fill(uint16_t *row, uint16_t color)
{
    int x;
    for (x = 0; x < SCR_W; x++) row[x] = color;
}

static void fill_tri_row(uint16_t *row, int y, int k)
{
    int minx = g_fpt_minx[k], maxx = g_fpt_maxx[k];
    int x;

    if (y < g_fpt_miny[k] || y > g_fpt_maxy[k]) return;

    for (x = minx; x <= maxx; x++) {
        int e0 = g_fa[k][0] * x + g_fb[k][0] * y + g_fc[k][0];
        int e1 = g_fa[k][1] * x + g_fb[k][1] * y + g_fc[k][1];
        int e2 = g_fa[k][2] * x + g_fb[k][2] * y + g_fc[k][2];
        if (e0 * g_fs[k][0] >= 0 && e1 * g_fs[k][1] >= 0 && e2 * g_fs[k][2] >= 0) {
            row[x] = g_fcol[k];
        }
    }
}

/* ==================== 一帧的 3D 准备 ==================== */
static void prepare_frame(int ax, int ay)
{
    int cx = cos1024(ax), sx = sin1024(ax);
    int cy = cos1024(ay), sy = sin1024(ay);
    static const int8_t sgn[4][3] = {
        {  1,  1,  1 },
        {  1, -1, -1 },
        { -1,  1, -1 },
        { -1, -1,  1 },
    };
    int i, k;

    int r00 = cy,  r01 = (sy * sx) >> 10, r02 = (sy * cx) >> 10;
    int r10 = 0,   r11 = cx,              r12 = -sx;
    int r20 = -sy, r21 = (cy * sx) >> 10, r22 = (cy * cx) >> 10;

    for (i = 0; i < 4; i++) {
        int px = sgn[i][0] * VTX_R;
        int py = sgn[i][1] * VTX_R;
        int pz = sgn[i][2] * VTX_R;
        int X = (r00 * px + r01 * py + r02 * pz) >> 10;
        int Y = (r10 * px + r11 * py + r12 * pz) >> 10;
        int Z = (r20 * px + r21 * py + r22 * pz) >> 10;

        g_world[i].x = X; g_world[i].y = Y; g_world[i].z = Z;
        g_scr[i].x = CX + (X * PROJ_P) / (Z + PROJ_D);
        g_scr[i].y = CY + (Y * PROJ_P) / (Z + PROJ_D);
    }

    for (k = 0; k < 4; k++) {
        int i0 = g_face[k][0], i1 = g_face[k][1], i2 = g_face[k][2];
        vtx_t *p0 = &g_world[i0], *p1, *p2;
        int e1x, e1y, e1z, e2x, e2y, e2z;
        int nx, ny, nz, gxc, gyc, gzc;
        int nl, dot, idx, t, j;

        p1 = &g_world[i1];
        p2 = &g_world[i2];
        e1x = p1->x - p0->x; e1y = p1->y - p0->y; e1z = p1->z - p0->z;
        e2x = p2->x - p0->x; e2y = p2->y - p0->y; e2z = p2->z - p0->z;
        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;
        gxc = (p0->x + p1->x + p2->x) / 3;
        gyc = (p0->y + p1->y + p2->y) / 3;
        gzc = (p0->z + p1->z + p2->z) / 3;

        /* 外法线定向（四面体中心在原点） */
        if ((nx * gxc + ny * gyc + nz * gzc) < 0) {
            nx = -nx; ny = -ny; nz = -nz;
            t = i1; i1 = i2; i2 = t;
            p1 = &g_world[i1];
            p2 = &g_world[i2];
        }

        /* 背面剔除：相机在 (0,0,-PROJ_D) */
        g_vis[k] = (nx * (-gxc) + ny * (-gyc) + nz * (-PROJ_D - gzc)) > 0;

        /* 平面光照 */
        nl = isqrt(nx * nx + ny * ny + nz * nz);
        if (nl <= 0) nl = 1;
        dot = (nx * LX + ny * LY + nz * LZ) / nl;
        idx = 32 + dot * 31 / 256;
        if (idx < 0) idx = 0;
        if (idx > LUT_N - 1) idx = LUT_N - 1;
        g_fcol[k] = g_lut[idx];
        g_fdepth[k] = gzc;

        /* 投影点、包围盒、边函数常量 */
        g_fpt[k][0] = g_scr[i0];
        g_fpt[k][1] = g_scr[i1];
        g_fpt[k][2] = g_scr[i2];
        {
            int minx = g_fpt[k][0].x, maxx = g_fpt[k][0].x;
            int miny = g_fpt[k][0].y, maxy = g_fpt[k][0].y;
            for (j = 1; j < 3; j++) {
                if (g_fpt[k][j].x < minx) minx = g_fpt[k][j].x;
                if (g_fpt[k][j].x > maxx) maxx = g_fpt[k][j].x;
                if (g_fpt[k][j].y < miny) miny = g_fpt[k][j].y;
                if (g_fpt[k][j].y > maxy) maxy = g_fpt[k][j].y;
            }
            if (minx < 0) minx = 0;
            if (maxx > SCR_W - 1) maxx = SCR_W - 1;
            if (miny < 0) miny = 0;
            if (maxy > SCR_H - 1) maxy = SCR_H - 1;
            g_fpt_minx[k] = minx; g_fpt_maxx[k] = maxx;
            g_fpt_miny[k] = miny; g_fpt_maxy[k] = maxy;

            {
                pt2_t *q0 = &g_fpt[k][0], *q1 = &g_fpt[k][1], *q2 = &g_fpt[k][2];
                int a[3], b[3], c[3], gx, gy;
                a[0] = q1->y - q0->y; b[0] = q0->x - q1->x; c[0] = -(a[0] * q0->x + b[0] * q0->y);
                a[1] = q2->y - q1->y; b[1] = q1->x - q2->x; c[1] = -(a[1] * q1->x + b[1] * q1->y);
                a[2] = q0->y - q2->y; b[2] = q2->x - q0->x; c[2] = -(a[2] * q2->x + b[2] * q2->y);
                gx = (q0->x + q1->x + q2->x) / 3;
                gy = (q0->y + q1->y + q2->y) / 3;
                for (j = 0; j < 3; j++) {
                    g_fa[k][j] = a[j];
                    g_fb[k][j] = b[j];
                    g_fc[k][j] = c[j];
                    g_fs[k][j] = (a[j] * gx + b[j] * gy + c[j]) >= 0 ? 1 : -1;
                }
            }
        }
    }

    /* 远者先画 */
    for (i = 0; i < 4; i++) g_forder[i] = i;
    for (i = 0; i < 4; i++) {
        int j;
        for (j = i + 1; j < 4; j++) {
            if (g_fdepth[g_forder[j]] > g_fdepth[g_forder[i]]) {
                int tt = g_forder[i]; g_forder[i] = g_forder[j]; g_forder[j] = tt;
            }
        }
    }
}

static void render_row(uint16_t *row, int y)
{
    int n;
    row_fill(row, COL_BG);
    for (n = 0; n < 4; n++) {
        int k = g_forder[n];
        if (!g_vis[k]) continue;
        fill_tri_row(row, y, k);
    }
}

/* 合成 y0 起的 h 行到指定缓冲（h <= BAND_LINES） */
static void render_band(uint16_t (*buf)[SCR_W], int y0, int h)
{
    int i;
    for (i = 0; i < h; i++) {
        render_row(buf[i], y0 + i);
    }
}

/* ==================== 测试入口 ==================== */
void video_tetra_test(void)
{
    int ax = 20, ay = 0;
    int frame_no = 0;

    video_init();               /* 官方初始化 + 三色自检 */
    build_lut();

    printf("[tetra] band=8 lines(3840px) + double-buffer pipeline\r\n");

    /* 自检：一次 8 行段的整帧（浅蓝底 + y=60 黄线），验证段长达标 */
    {
        int y = 0, rc = 0;
        printf("[tetra] check: band=8 lines, yellow line @ y=60\r\n");
        LCD_Set_Window(0, 0, 480, 480);
        LCD_startWriteMutileData();
        while (y < SCR_H) {
            int h = (SCR_H - y < BAND_LINES) ? (SCR_H - y) : BAND_LINES;
            int i;
            for (i = 0; i < h; i++) {
                row_fill(g_band[0][i], (y + i == 60) ? COL_YEL : COL_BG);
            }
            dma_start(&g_band[0][0][0], h * SCR_W);
            if (dma_wait() != 0) { rc = -1; break; }
            y += h;
        }
        LCD_endWriteMutileData();
        printf("[tetra]   result=%s\r\n", rc == 0 ? "OK" : "TIMEOUT");
        HAL_Delay(2500);
    }

    /* 轮询全屏绿（确认屏面可恢复，不依赖 DMA） */
    printf("[tetra] fullscreen green (poll)\r\n");
    {
        int y, x;
        LCD_Set_Window(0, 0, 480, 480);
        LCD_startWriteMutileData();
        row_fill(g_band[0][0], COL_GRN);
        for (y = 0; y < SCR_H; y++) {
            for (x = 0; x < SCR_W; x++) {
                LCD_WritemutileData16_fast(g_band[0][0][x]);
            }
        }
        LCD_endWriteMutileData();
    }
    HAL_Delay(1500);

    printf("[tetra] animation start (band=%d lines, double-buffered)\r\n", BAND_LINES);

    for (;;) {
        int y = 0;
        int cur = 0;            /* 当前 DMA 中的缓冲号 */
        int nxt = 1;            /* 正在合成的缓冲号 */
        int h;

        prepare_frame(ax, ay);

        LCD_Set_Window(0, 0, 480, 480);
        LCD_startWriteMutileData();

        /* 先合成并启动第 1 段 */
        h = (SCR_H < BAND_LINES) ? SCR_H : BAND_LINES;
        render_band(g_band[cur], 0, h);
        dma_start(&g_band[cur][0][0], h * SCR_W);
        y = h;

        /* 流水线：合成下一段（与上一段传输重叠）-> 等上一段 -> 启动 */
        while (y < SCR_H) {
            h = (SCR_H - y < BAND_LINES) ? (SCR_H - y) : BAND_LINES;
            render_band(g_band[nxt], y, h);
            dma_wait();
            dma_start(&g_band[nxt][0][0], h * SCR_W);
            y += h;
            cur ^= 1; nxt ^= 1;
        }
        dma_wait();

        LCD_endWriteMutileData();

        ax = (ax + 3) % 360;
        ay = (ay + 4) % 360;

        if ((++frame_no % 10) == 0) {
            printf("[tetra] frame %d\r\n", frame_no);
        }
    }
}
