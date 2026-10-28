/******************************************************************************
 * @file     gfx.c
 * @brief    屏幕图形库实现（TD1601 + 480x480 QSPI 屏）
 *
 * 内部机制（都是实测验证过的，请勿随意改动）：
 *   1. 一次绘制 = 一次"局部/全屏窗口 + RAMWR 事务"，数据长度与窗口像素数
 *      **严格相等**（否则面板会错位污染）；
 *   2. 像素数据用 DMA 分段发送，**每段最多 8 行**（8*480 = 3840 像素，
 *      受 DW DMA 块传输上限 4095 限制，超限 DMA 不会完成）；
 *   3. 每个事务之间留 60us 间隔，避免背靠背窗口事务被面板漏处理；
 *   4. 不做像素回读（面板不支持），所以局部图元的 bg 参数必须传入
 *      "窗口内其余像素"的颜色。
 ******************************************************************************/

#include "gfx.h"
#include "video.h"              /* video_init */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

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

/* ==================== DMA（与 src/video.c 一致） ==================== */
#define DWDMA     ((dw_dma_regs_t *)DW_DMA_BASE)
#define CH_IDX    2
#define DWDMA_CH  ((dw_dma_ch_regs_t *)(DW_DMA_BASE + (CH_IDX * 0x58)))

#define GFX_ROWS  8                     /* 每段行数：8*480 = 3840 < 4095 */

static uint16_t g_seg[GFX_ROWS][GFX_W];

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

/* ==================== 事务封装 ==================== */
static void seg_begin(int x, int y, int w, int h)
{
    LCD_Set_Window((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    LCD_startWriteMutileData();
}

/* 发送段缓冲的前 rows 行（每行 w 个像素，连续存放） */
static void seg_send(int w, int rows)
{
    dma_start(&g_seg[0][0], rows * w);
    dma_wait();
}

static void seg_end(void)
{
    LCD_endWriteMutileData();
    udelay(60);                         /* 事务间隔 */
}

/* 整数平方根 */
static int isqrt_i(int n)
{
    int x, y;
    if (n <= 0) return 0;
    x = n; y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

/* 把矩形区域裁剪到屏幕内；返回 1 表示有效 */
static int clip_rect(int *x, int *y, int *w, int *h)
{
    int x0 = *x, y0 = *y, x1 = *x + *w - 1, y1 = *y + *h - 1;
    if (x1 < 0 || y1 < 0 || x0 > GFX_W - 1 || y0 > GFX_H - 1) return 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GFX_W - 1) x1 = GFX_W - 1;
    if (y1 > GFX_H - 1) y1 = GFX_H - 1;
    *x = x0; *y = y0; *w = x1 - x0 + 1; *h = y1 - y0 + 1;
    return (*w > 0 && *h > 0);
}

/* ==================== 多边形填充（3~4 顶点，内部复用） ==================== */
static void fill_poly(const int *px, const int *py, int n, uint16_t color, uint16_t bg)
{
    int minx = px[0], maxx = px[0], miny = py[0], maxy = py[0];
    int x0, y0, w, h, i, j, done;
    int a[4], b[4], c[4], s[4], gx = 0, gy = 0;
    uint16_t *flat = &g_seg[0][0];

    for (i = 1; i < n; i++) {
        if (px[i] < minx) minx = px[i];
        if (px[i] > maxx) maxx = px[i];
        if (py[i] < miny) miny = py[i];
        if (py[i] > maxy) maxy = py[i];
    }

    x0 = minx; y0 = miny; w = maxx - minx + 1; h = maxy - miny + 1;
    if (!clip_rect(&x0, &y0, &w, &h)) return;

    /* 边函数 + 重心符号（重心必在凸多边形内部） */
    {
        int sumx = 0, sumy = 0;
        for (i = 0; i < n; i++) {
            int i2 = (i + 1) % n;
            a[i] = py[i2] - py[i];
            b[i] = px[i] - px[i2];
            c[i] = -(a[i] * px[i] + b[i] * py[i]);
            sumx += px[i];
            sumy += py[i];
        }
        gx = sumx / n;
        gy = sumy / n;
    }
    for (i = 0; i < n; i++) {
        s[i] = (a[i] * gx + b[i] * gy + c[i]) >= 0 ? 1 : -1;
    }

    seg_begin(x0, y0, w, h);
    done = 0;
    while (done < h) {
        int rows = (h - done < GFX_ROWS) ? (h - done) : GFX_ROWS;
        for (i = 0; i < rows; i++) {
            int sy = y0 + done + i;
            uint16_t *row = flat + i * w;
            for (j = 0; j < w; j++) {
                int sx = x0 + j;
                int inside = 1;
                int k;
                for (k = 0; k < n; k++) {
                    int e = a[k] * sx + b[k] * sy + c[k];
                    if (e * s[k] < 0) { inside = 0; break; }
                }
                row[j] = inside ? color : bg;
            }
        }
        seg_send(w, rows);
        done += rows;
    }
    seg_end();
}

/* ==================== 对外接口 ==================== */

void gfx_init(void)
{
    video_init();                       /* 官方 LCD 初始化 + 三色自检 */
}

void gfx_clear(uint16_t color)
{
    int y = 0, i;
    uint16_t *flat = &g_seg[0][0];

    seg_begin(0, 0, GFX_W, GFX_H);
    while (y < GFX_H) {
        int rows = (GFX_H - y < GFX_ROWS) ? (GFX_H - y) : GFX_ROWS;
        for (i = 0; i < rows * GFX_W; i++) flat[i] = color;
        seg_send(GFX_W, rows);
        y += rows;
    }
    seg_end();
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int i, done;
    uint16_t *flat = &g_seg[0][0];

    if (!clip_rect(&x, &y, &w, &h)) return;

    seg_begin(x, y, w, h);
    done = 0;
    while (done < h) {
        int rows = (h - done < GFX_ROWS) ? (h - done) : GFX_ROWS;
        for (i = 0; i < rows * w; i++) flat[i] = color;
        seg_send(w, rows);
        done += rows;
    }
    seg_end();
}

void gfx_fill_circle(int cx, int cy, int r, uint16_t color, uint16_t bg)
{
    int x0, y0, w, h, i, j, done;
    uint16_t *flat = &g_seg[0][0];

    if (r <= 0) return;
    x0 = cx - r; y0 = cy - r; w = 2 * r + 1; h = 2 * r + 1;
    if (!clip_rect(&x0, &y0, &w, &h)) return;

    seg_begin(x0, y0, w, h);
    done = 0;
    while (done < h) {
        int rows = (h - done < GFX_ROWS) ? (h - done) : GFX_ROWS;
        for (i = 0; i < rows; i++) {
            int sy = y0 + done + i;
            int dy = sy - cy;
            int half = -1;
            uint16_t *row = flat + i * w;

            if (dy <= r && dy >= -r) half = isqrt_i(r * r - dy * dy);

            for (j = 0; j < w; j++) {
                int dx = (x0 + j) - cx;
                row[j] = (half >= 0 && dx >= -half && dx <= half) ? color : bg;
            }
        }
        seg_send(w, rows);
        done += rows;
    }
    seg_end();
}

void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                       uint16_t color, uint16_t bg)
{
    int px[3], py[3];
    px[0] = x0; py[0] = y0;
    px[1] = x1; py[1] = y1;
    px[2] = x2; py[2] = y2;
    fill_poly(px, py, 3, color, bg);
}

void gfx_line(int x0, int y0, int x1, int y1, int thick, uint16_t color, uint16_t bg)
{
    int abx = x1 - x0, aby = y1 - y0;
    int len, hw, nx, ny;
    int px[4], py[4];

    if (thick < 1) thick = 1;
    len = isqrt_i(abx * abx + aby * aby);
    if (len == 0) {                     /* 退化为点 */
        len = 1;
        abx = 1;
    }
    hw = (thick + 1) / 2;
    nx = (-aby) * hw / len;             /* 法向偏移 */
    ny = (abx) * hw / len;

    px[0] = x0 + nx; py[0] = y0 + ny;
    px[1] = x0 - nx; py[1] = y0 - ny;
    px[2] = x1 - nx; py[2] = y1 - ny;
    px[3] = x1 + nx; py[3] = y1 + ny;
    fill_poly(px, py, 4, color, bg);
}

void gfx_blit(int x, int y, int w, int h, const uint16_t *pix)
{
    int sx = 0, sy = 0, cw = w, ch = h, done;

    /* 裁剪：同时记录源偏移 */
    if (x < 0) { sx = -x; x = 0; cw = w - sx; }
    if (y < 0) { sy = -y; y = 0; ch = h - sy; }
    if (x + cw > GFX_W) cw = GFX_W - x;
    if (y + ch > GFX_H) ch = GFX_H - y;
    if (cw <= 0 || ch <= 0) return;

    seg_begin(x, y, cw, ch);
    done = 0;
    while (done < ch) {
        int rows = (ch - done < GFX_ROWS) ? (ch - done) : GFX_ROWS;
        const uint16_t *src = pix + (sy + done) * w + sx;
        /* 未水平裁剪且地址 4 字节对齐时，可直接 DMA 发送（省一次拷贝） */
        if (cw == w && (((uint32_t)src & 3) == 0)) {
            dma_start((uint16_t *)src, rows * cw);
            dma_wait();
        } else {
            int i;
            for (i = 0; i < rows; i++) {
                memcpy(&g_seg[i][0], pix + (sy + done + i) * w + sx, (size_t)cw * 2);
            }
            seg_send(cw, rows);
        }
        done += rows;
    }
    seg_end();
}

void gfx_blit_key(int x, int y, int w, int h, const uint16_t *pix,
                  uint16_t key, uint16_t bg)
{
    int sx = 0, sy = 0, cw = w, ch = h, done;

    if (x < 0) { sx = -x; x = 0; cw = w - sx; }
    if (y < 0) { sy = -y; y = 0; ch = h - sy; }
    if (x + cw > GFX_W) cw = GFX_W - x;
    if (y + ch > GFX_H) ch = GFX_H - y;
    if (cw <= 0 || ch <= 0) return;

    seg_begin(x, y, cw, ch);
    done = 0;
    while (done < ch) {
        int rows = (ch - done < GFX_ROWS) ? (ch - done) : GFX_ROWS;
        int i, j;
        for (i = 0; i < rows; i++) {
            const uint16_t *src = pix + (sy + done + i) * w + sx;
            uint16_t *dst = &g_seg[i][0];
            for (j = 0; j < cw; j++) {
                dst[j] = (src[j] == key) ? bg : src[j];
            }
        }
        seg_send(cw, rows);
        done += rows;
    }
    seg_end();
}
