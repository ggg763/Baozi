/******************************************************************************
 * @file     face_render.c
 * @brief    屏幕传输层实现（见 face_render.h）
 *
 * 【必须遵守】LCD_DmaSend 的起点要对齐：
 *   video.c 里 LCD_DmaSend() 会对 pdata 调 csi_dcache_clean_invalid_range()，
 *   而 DCache 的 invalidate 是按 32 字节 cache 行整行作废的。pdata 若落在
 *   cache 行中间，会波及同一行内的其它数据，重读拿到旧值 -> 白色块。
 *   从行缓冲【开头】发（每段同一地址）天然满足。
 *
 * 每行流程：
 *   1. 清覆盖度（480 B）
 *   2. 对每个【纵向范围覆盖本行】的部件求区间、累加覆盖度（2 条子扫描线）
 *   3. 覆盖度调色写进行缓冲（0 -> 背景，255 -> 前景，中间按比例混合）
 *
 * 相比旧版（每行先刷背景再画实心区间）多了覆盖度这一层，但换来两件事：
 *   · 纵向抗锯齿：圆顶不再是台阶（旧版只有横向端点 AA）
 *   · 部件可以重叠混合（腮红/气泡半透明）而不会互相擦掉
 * 代价是每行多一次 480 像素的调色；实测整帧仍在 20ms DMA 预算内。
 ******************************************************************************/

#include <stdint.h>

#include "face_render.h"
#include "video.h"                  /* video_init */
#include <drv/common.h>             /* mdelay */

/* src/video.c 提供的官方原语 */
extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_DmaSend(uint16_t *pdata, int len);
/* ★ 关键的一行：必须显式声明。
 * 缺声明的后果不是"链接失败"，而是编译器按【隐式声明】处理成 `int LCD_WaitIdle()`，
 * 与 video.c 里的 `void` 定义不一致，"每段 DMA 后等它真正发完"这一步会失效
 * —— 屏幕底部的白色矩形块就是这么来的。 */
extern void LCD_WaitIdle(void);

/* 每段最多 SEG_MAX_ROWS 行（8*480 = 3840 < 4095，DMA 单次上限）*/
#define SEG_MAX_ROWS   8

/* 行段缓冲（双缓冲）。整屏刷新时每段从缓冲【开头】发（起点固定对齐）；
 * 两块交替：写 B 时 A 正在被 SPI 移位。
 * 为什么必须这样：LCD_DmaSend() 只等 DMA 搬完（RawBlock），不等 SPI 把最后
 * 一个 bit 移出去；若不等就直接覆写同一块缓冲并发起下一次 DMA，正在移位的
 * 数据会与新数据交错 -> 屏幕上出现错位的白块/白条。 */
static uint16_t g_segA[SEG_MAX_ROWS][FD_W];
static uint16_t g_segB[SEG_MAX_ROWS][FD_W];

/* 实际每段行数（1..SEG_MAX_ROWS）。默认 8。 */
static int g_seg_rows = SEG_MAX_ROWS;

/* 本帧的部件表 + 每个部件的纵向覆盖范围（行号，已含 1 行余量）。
 * 预存范围的意义：480 行里绝大多数行与某个部件毫无交集，
 * 不必为它做两次扫描线求解。眼睛类部件只占 ~160 行。 */
static fd_part_t g_parts[FACE_MAX_PARTS];
static int       g_parts_n = 0;
static int16_t   g_row0[FACE_MAX_PARTS];
static int16_t   g_row1[FACE_MAX_PARTS];
static uint16_t  g_fg = 0xFFFF;

static uint8_t   g_cov[FD_W];

static face_bus_cb_t s_bus_cb = 0;

void face_set_bus_callback(face_bus_cb_t cb)
{
    s_bus_cb = cb;
}

/* ---------------------------------------------------------------- 纵向范围 */
/* 保守估计：宁可多算几行，不能漏。带旋转/星形的横向半径可能大于纵向，
 * 所以取两者较大值 —— 多算只是慢一点，漏算会直接缺一块。 */
static void part_rows(const fd_part_t *p, int16_t *y0, int16_t *y1)
{
    int cy = FD_CY + p->y / 2;

    if (p->kind == FD_CAP) {
        /* 胶囊：两端点纵向范围 + 半径（rx 存的是线宽半径） */
        int ya = FD_CY + p->y / 2;
        int yb = FD_CY + p->b / 2;
        int r  = p->rx / 2;
        int lo = (ya < yb) ? ya : yb;
        int hi = (ya > yb) ? ya : yb;
        *y0 = (int16_t)(lo - r - 1);
        *y1 = (int16_t)(hi + r + 1);
        return;
    }
    {
        int half;
        if ((p->kind == FD_SUP && p->rot != 0) || p->kind == FD_STAR) {
            half = ((p->rx > p->ry) ? p->rx : p->ry) / 2;
        } else {
            half = p->ry / 2;
        }
        *y0 = (int16_t)(cy - half - 1);
        *y1 = (int16_t)(cy + half + 1);
    }
}

/* ---------------------------------------------------------------- 单行绘制 */
static void render_row(int y, uint16_t *line)
{
    int i;

    fd_cov_clear(g_cov);
    for (i = 0; i < g_parts_n; i++) {
        const fd_part_t *p = &g_parts[i];
        uint8_t alpha = 255;

        if (y < g_row0[i] || y > g_row1[i]) {
            continue;
        }
        fd_cov_part(g_cov, p, y, 0, alpha);
        if (p->mirror) {
            fd_cov_part(g_cov, p, y, 1, alpha);
        }
    }
    fd_blend_row(line, g_cov, g_fg, FACE_COL_BG);
}

/* ---------------------------------------------------------------- 整屏 */
/* 窗口固定 480x480，每段从行缓冲【开头】发；双缓冲交替 + 每段后 LCD_WaitIdle()。 */
static void render_all(void)
{
    int row, k, flip = 0;

    LCD_Set_Window(0, 0, FD_W, FD_H);
    LCD_startWriteMutileData();

    for (row = 0; row < FD_H; row += g_seg_rows) {
        int rows = (FD_H - row < g_seg_rows) ? (FD_H - row) : g_seg_rows;
        uint16_t (*seg)[FD_W] = flip ? g_segB : g_segA;

        flip = !flip;
        for (k = 0; k < rows; k++) {
            render_row(row + k, seg[k]);
        }

        if (s_bus_cb != 0) {
            s_bus_cb(1);            /* 即将 DMA：外部挂起会走 AHB 的寄存器写 */
        }
        LCD_DmaSend(&seg[0][0], rows * FD_W);
        LCD_WaitIdle();             /* 等本段真正发完，再写下一块 */
        if (s_bus_cb != 0) {
            s_bus_cb(0);            /* 段间空隙：外部补写（≈1ms，足够）*/
        }
    }

    LCD_endWriteMutileData();
}

/* ---------------------------------------------------------------- 对外 */
void face_init(void)
{
    int i, r;

    video_init();                   /* 官方 LCD 初始化 + 三色自检 */

    /* 开机先整屏刷一次背景色。
     * video_init() 的三色自检最后刷的是蓝色，先清一遍，避免开机瞬间看到残留。 */
    for (i = 0; i < SEG_MAX_ROWS; i++) {
        int j;
        for (j = 0; j < FD_W; j++) {
            g_segA[i][j] = FACE_COL_BG;
        }
    }
    LCD_Set_Window(0, 0, FD_W, FD_H);
    LCD_startWriteMutileData();
    for (r = 0; r < FD_H / SEG_MAX_ROWS; r++) {
        if (s_bus_cb != 0) { s_bus_cb(1); }
        LCD_DmaSend(&g_segA[0][0], SEG_MAX_ROWS * FD_W);
        LCD_WaitIdle();
        if (s_bus_cb != 0) { s_bus_cb(0); }
    }
    LCD_endWriteMutileData();

    g_parts_n = 0;
}

void face_show_parts(const fd_part_t *parts, int n, uint16_t fg)
{
    int i;

    if (n > FACE_MAX_PARTS) {
        n = FACE_MAX_PARTS;
    }
    for (i = 0; i < n; i++) {
        g_parts[i] = parts[i];
        part_rows(&g_parts[i], &g_row0[i], &g_row1[i]);
    }
    g_parts_n = n;
    g_fg = fg;

    render_all();
}

void face_set_segrows(int n)
{
    if (n < 1) {
        n = 1;
    }
    if (n > SEG_MAX_ROWS) {
        n = SEG_MAX_ROWS;
    }
    g_seg_rows = n;
    if (g_parts_n > 0) {
        render_all();
    }
}

int face_get_segrows(void)
{
    return g_seg_rows;
}
