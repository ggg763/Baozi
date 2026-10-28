/******************************************************************************
 * @file     video_baozi_test.c
 * @brief    3D 包子：带光照的球面包子，绕竖直轴自转 + 上下小幅浮动
 *
 * 底座：官方 video.c + 每帧一次全屏 RAMWR 事务，像素由 CPU 软件合成
 *       （已验证稳定，无 DMA、无局部窗口、无帧缓冲，仅 480x2B 行缓冲）。
 *
 * 立体感来源（纯整数软件 3D，无浮点/无 libm）：
 *   1. 包子 = 球面，像素 (x,y) -> 法线 (u, v, w)，w = isqrt(R^2-u^2-v^2)
 *   2. 固定平行光（屏幕左上偏前）做 Lambert 明暗：l = u*Lx+v*Ly+w*Lz
 *      明暗查 64 档 LUT（整数生成），边缘自然变暗、左上方高亮
 *   3. 高光斑：法线与半程向量 H 的点积超阈值 -> 白色反光点
 *   4. 球面纹理：12 条经线（含 1 条更宽更深的"合口缝"）随 theta 转动；
 *      经线用 atan 查表得到方位角，按 30 度取模判断
 *   5. 顶部收口区（球顶深色环 + 放射小褶），上下浮动时地面阴影固定
 *
 * 入口：video_baozi_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "video.h"              /* video_init / HAL_Delay */

/* video.h 未声明的官方原语（video.c 中定义） */
extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_WritemutileData16_fast(uint16_t num);

/* ==================== 配色（RGB565 基色，最终由光照 LUT 调制） ==================== */
#define COL_BG      0xBEFF      /* 背景浅蓝 (186,222,248) */
#define COL_DOUGH   0xFF9A      /* 包子皮 (255,240,211) */
#define COL_FOLD    0xAC2B      /* 普通褶影 (174,133,95) */
#define COL_DEEP    0x82E7      /* 合口深褶 (133,92,60) */
#define COL_COLLAR  0x6A67      /* 顶部收口 (110,78,56) */
#define COL_SHADOW  0x9D59      /* 地面阴影 (152,170,202) */
#define COL_HILITE  0xFFFF      /* 高光白 */

/* ==================== 布局与旋转参数 ==================== */
#define CX          240
#define CY_BASE     238                 /* 球心 y 基准 */
#define R_SPH       150                 /* 横向半径 */
#define RY_SPH      138                 /* 纵向半径（略扁，更像包子） */
#define FLOAT_A     11                  /* 上下浮动幅度 px */
#define SHADOW_CY   430
#define SHADOW_RX   118
#define SHADOW_RY   12

#define FOLD_N      12                  /* 经线数（30 度一条） */
#define W_FOLD      2                   /* 普通经线半宽（度） */
#define W_DEEP      4                   /* 合口深褶半宽（度） */
#define FOLD_TOP    (-15)               /* 经线只出现在上半球（vy < 此值） */
#define POLAR_MIN2  900                 /* 接近球极时跳过经线判断（u^2+w^2 阈值） */

/* ==================== 光照（定点 1024） ==================== */
#define LX  (-430)                      /* 光源方向 L = (-0.42, -0.50, 0.76) */
#define LY  (-512)
#define LZ  (778)
#define HX  (-230)                      /* 半程向量 H = normalize(L + V) */
#define HY  (-273)
#define HZ  (964)
#define THR_HL  (150528)                /* 0.98 * R_SPH * 1024（高光阈值） */

/* 明暗索引：idx = 32 + (l * INV) >> SHIFT，l ∈ [-R*1024, R*1024] */
#define IDX_INV     436
#define IDX_SHIFT   21

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

/* ==================== atan 表：atan(q/256) 的角度（0..45 度），q = 0..256 ==================== */
static const uint8_t g_atan_tab[257] = {
    0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7, 7,
    7, 7, 8, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10,
    11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 13, 14, 14,
    14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 16, 17, 17, 17, 17,
    17, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 20, 20, 20, 20, 20,
    21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23, 23,
    24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26,
    27, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29,
    29, 30, 30, 30, 30, 30, 30, 31, 31, 31, 31, 31, 31, 32, 32, 32,
    32, 32, 32, 32, 33, 33, 33, 33, 33, 33, 34, 34, 34, 34, 34, 34,
    35, 35, 35, 35, 35, 35, 35, 36, 36, 36, 36, 36, 36, 36, 37, 37,
    37, 37, 37, 37, 37, 38, 38, 38, 38, 38, 38, 38, 39, 39, 39, 39,
    39, 39, 39, 39, 40, 40, 40, 40, 40, 40, 40, 41, 41, 41, 41, 41,
    41, 41, 41, 42, 42, 42, 42, 42, 42, 42, 42, 43, 43, 43, 43, 43,
    43, 43, 43, 44, 44, 44, 44, 44, 44, 44, 44, 44, 45, 45, 45, 45,
    45
};

/* 整数平方根（牛顿迭代） */
static int isqrt(int n)
{
    int x, y;
    if (n <= 0) return 0;
    x = n;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

/* 方位角：返回向量 (u, w) 的角度（度，0..359，0 度 = +w 方向） */
static int angle_deg(int u, int w)
{
    int au = (u < 0) ? -u : u;
    int aw = (w < 0) ? -w : w;
    int q, al;

    if (au >= aw) {
        /* 以 u 轴为基准：al = atan(|w| / |u|) */
        q = au ? (aw * 256) / au : 0;
        al = g_atan_tab[q];
        if (u > 0) return (w >= 0) ? (90 - al) : (90 + al);
        else       return (w >= 0) ? (270 + al) : (270 - al);
    } else {
        /* 以 w 轴为基准：al = atan(|u| / |w|) */
        q = aw ? (au * 256) / aw : 0;
        al = g_atan_tab[q];
        if (w > 0) return (u >= 0) ? al : (360 - al);
        else       return (u >= 0) ? (180 - al) : (180 + al);
    }
}

/* ==================== 明暗 LUT（64 档，整数生成） ==================== */
#define LUT_N 64

static uint16_t g_lut_dough[LUT_N];
static uint16_t g_lut_fold[LUT_N];
static uint16_t g_lut_deep[LUT_N];
static uint16_t g_lut_collar[LUT_N];

/* RGB565 -> 8bit 分量后再按亮度百分比 num 调制，返回 RGB565 */
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

static void build_luts(void)
{
    int i;
    for (i = 0; i < LUT_N; i++) {
        int num = 55 + (i * 55) / (LUT_N - 1);   /* 55% .. 110% 亮度（不过曝） */
        g_lut_dough[i]  = shade565(COL_DOUGH, num);
        g_lut_fold[i]   = shade565(COL_FOLD, num);
        g_lut_deep[i]   = shade565(COL_DEEP, num);
        g_lut_collar[i] = shade565(COL_COLLAR, num);
    }
}

/* ==================== 行缓冲 ==================== */
#define ROW_W 480
static uint16_t g_row[ROW_W];

static void row_fill(uint16_t *row, uint16_t color)
{
    int x;
    for (x = 0; x < ROW_W; x++) row[x] = color;
}

static void row_span(uint16_t *row, int x0, int x1, uint16_t color)
{
    int x;
    if (x0 < 0) x0 = 0;
    if (x1 > ROW_W - 1) x1 = ROW_W - 1;
    for (x = x0; x <= x1; x++) row[x] = color;
}

static void row_ellipse(uint16_t *row, int y, int cx, int cy, int rx, int ry, uint16_t color)
{
    int d = y - cy;
    int h;
    if (d < 0) d = -d;
    if (d > ry) return;
    h = (rx * isqrt(ry * ry - d * d)) / ry;
    row_span(row, cx - h, cx + h, color);
}

/* ==================== 单行合成（3D 椭球包子） ==================== */
static void render_row(uint16_t *row, int y, int cy, int theta)
{
    int dy = y - cy;
    int vy, u, w;

    row_fill(row, COL_BG);
    row_ellipse(row, y, CX, SHADOW_CY, SHADOW_RX, SHADOW_RY, COL_SHADOW);

    if (dy > RY_SPH || dy < -RY_SPH) return;

    /* 椭球折算：把纵向坐标映射回球面，得到统一的法线参数 (u, vy, w) */
    vy = dy * R_SPH / RY_SPH;

    {
        int half = isqrt(R_SPH * R_SPH - vy * vy);      /* 该行横向覆盖半宽 */

        for (u = -half; u <= half; u++) {
            int l, idx, hl;
            uint16_t col;

            w = isqrt(R_SPH * R_SPH - u * u - vy * vy); /* 法线 z 分量（未归一化） */

            /* --- 光照 --- */
            l = u * LX + vy * LY + w * LZ;              /* 量纲 R*1024 */
            idx = 32 + ((l * IDX_INV) >> IDX_SHIFT);
            if (idx < 0) idx = 0;
            if (idx > LUT_N - 1) idx = LUT_N - 1;

            /* --- 高光斑 --- */
            hl = u * HX + vy * HY + w * HZ;
            if (hl > THR_HL) {
                col = COL_HILITE;
            } else if (vy < -129) {
                col = g_lut_collar[idx];               /* 顶部收口（球顶深色环） */
            } else if (vy < -117) {
                col = g_lut_fold[idx];                 /* 收口过渡环 */
            } else if (vy < FOLD_TOP && (u * u + w * w) > POLAR_MIN2) {
                /* --- 上半球球面经线纹理（随 theta 转动，像包子顶部放射褶） --- */
                int phi = angle_deg(u, w);
                int d = phi - theta;
                int k, m, fw;

                if (d < 0) d += 360;
                d %= 360;
                k = (d / 30) % FOLD_N;
                m = d % 30;
                fw = (k == 0) ? W_DEEP : W_FOLD;

                if (m <= fw || m >= 30 - fw) {
                    col = (k == 0) ? g_lut_deep[idx] : g_lut_fold[idx];
                } else {
                    col = g_lut_dough[idx];
                }
            } else {
                col = g_lut_dough[idx];                /* 平滑皮面 / 球极附近 */
            }

            row[CX + u] = col;
        }
    }
}

/* ==================== 测试入口 ==================== */
void video_baozi_test(void)
{
    int theta = 0;      /* 自转角（度） */
    int phase = 0;      /* 浮动相位（度） */

    video_init();       /* 官方初始化 + 三色自检 */
    build_luts();

    printf("[baozi3d] lit sphere baozi: spin + float\r\n");

    for (;;) {
        int dy = (FLOAT_A * sin1024(phase) + 512) >> 10;
        int cy = CY_BASE + dy;
        int y, x;

        LCD_Set_Window(0, 0, 480, 480);
        LCD_startWriteMutileData();
        for (y = 0; y < 480; y++) {
            render_row(g_row, y, cy, theta);
            for (x = 0; x < 480; x++) {
                LCD_WritemutileData16_fast(g_row[x]);
            }
        }
        LCD_endWriteMutileData();

        theta = (theta + 5) % 360;      /* 自转 5°/帧 */
        phase = (phase + 6) % 360;      /* 浮动相位 6°/帧 */
        HAL_Delay(5);
    }
}
