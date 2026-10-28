/******************************************************************************
 * @file     ui.c
 * @brief    包子屏幕 UI 绘图层实现（见 ui.h）
 *
 * 渲染方式：整屏 480x480，每 8 行一段，双缓冲交替 + 每段后 LCD_WaitIdle()，
 *          与 face_render.c 的 render_current() 同构（已在上板验证的形态）。
 *
 * 【屏幕扫描方向结论（2026-09-27 三轮实机标定）】
 *   发送缓冲 (c=行内索引/快速维, k=行号/慢速维) 在"排线朝左"姿态下
 *   显示于物理 (479-k, c)；而最终安装方向为【排线朝下】（屏相对朝左
 *   姿态逆时针转 90°观看），联立得：视觉坐标 = 缓冲坐标 (c, k) 直接相等。
 *   因此：行号 k = 纵向 y、行内索引 c = 横向 x，按"逐逻辑行"绘制即可，
 *   face_render.c（表情）同为逐行画法，在排线朝下方向下天然正确，无需修改。
 *
 * 文字：用 FONT.h 的 asc2_2412（12 宽 x 24 高，列行式，每列 3 字节，MSB=顶）。
 * 坐标：逻辑坐标 = 用户视觉坐标（x 右 / y 下为正，原点左上角）。
 ******************************************************************************/

#include <stdint.h>
#include "ui.h"
#include "FONT.h"                 /* asc2_2412 ASCII 字库 */
#include "video.h"                /* video_init */

/* src/video.c 提供的官方原语（与 face_render.c 一致）*/
extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_DmaSend(uint16_t *pdata, int len);
extern void LCD_WaitIdle(void);
extern void LCD_WaitReady(void);

#define UI_SZ         480
#define UI_SEG_ROWS   8
#define UI_CHAR_W     12   /* asc2_2412：12 宽（列数）*/
#define UI_CHAR_H     24   /* asc2_2412：24 高（每列 3 字节，MSB=顶部）*/
/* 字库格式（依旧 LCD.c ShowChar 的官方解析确认）：
 * asc2_2412 每字符 36 字节 = 12 列 × 3 字节/列；
 * 每列 24bit 纵向排列，MSB 对应该列最上面的像素。 */

/* 双缓冲（写 B 时 A 正在被 DMA 发送；每段从缓冲开头地址发，天然 32 字节对齐）*/
static uint16_t g_segA[UI_SEG_ROWS][UI_SZ];
static uint16_t g_segB[UI_SEG_ROWS][UI_SZ];

/* ---- 绘制列表容量 ----------------------------------------------------
 * 注意：圆（circle）槽位最容易被撑爆，因为一个"带厚度+描边的数字圆点"
 * 要占 3 个槽（阴影层/填充层/描边层），一屏 10 个数字 = 30 个，
 * 再加选中态与页面圆点。历史上 CIRCLE 上限设为 24 导致 8/9/10
 * 的圆底被静默丢弃（文字在 TEXT 列表里、容量足够，所以只剩数字）。
 * 宁可给足余量，也不要在渲染期静默丢元素。                       */
#define UI_MAX_RECT   24
#define UI_MAX_TEXT   48     /* 文字：数字 + 标签 + 按钮，留足余量 */
#define UI_MAX_CIRCLE 64     /* 圆：见上方说明，必须 >= 40 */
#define UI_MAX_ROUND  96     /* 圆角块：v2.2 起时钟用圆角数码画，一个 8 字符的
                              * "00:00:00" 就要 6×7 = 42 个块，再加卡片/按钮就顶到
                              * 原来 64 的上限 —— 超了会**静默丢弃**后面的元素
                              * （实测：番茄钟大数字最后两位直接不显示）。*/
#define UI_MAX_ARC    4      /* 圆弧（进度环）*/
#define UI_MAX_BITMAP 12     /* 1bpp 点阵块（汉字名，HABIT 卡片每行一个）*/

typedef struct { int16_t x, y, w, h, t; uint16_t color; } ui_rect_t;
typedef struct { int16_t x, y; int scale; uint16_t color; const char *s; } ui_text_t;
typedef struct { int16_t cx, cy, r, t; uint8_t filled; uint16_t color; } ui_circle_t;
/* 圆角块：r>0 圆角；t>0 表示边框模式（t 为线宽），t<=0 表示实心 */
typedef struct { int16_t x, y, w, h, r, t; uint16_t color; } ui_round_t;
/* 圆弧：从 12 点方向顺时针 sweep 度 */
typedef struct { int16_t cx, cy, r, t; int sweep; uint16_t color; } ui_arc_t;
/* 1bpp 点阵块：逐行扫描，每行 stride 字节，MSB = 该行最左像素 */
typedef struct {
    int16_t x, y, w, h, stride;
    int16_t scale;                  /* 放大倍数：1=原尺寸；2=整块 2x（日程标题用） */
    const uint8_t *data;
    uint16_t color;
} ui_bitmap_t;

static ui_rect_t  g_rects[UI_MAX_RECT];
static int        g_nrect = 0;
static ui_text_t  g_texts[UI_MAX_TEXT];
static int        g_ntext = 0;
static ui_circle_t g_circles[UI_MAX_CIRCLE];
static int        g_ncircle = 0;
static ui_round_t g_rounds[UI_MAX_ROUND];
static int        g_nround = 0;
static ui_arc_t   g_arcs[UI_MAX_ARC];
static int        g_narc = 0;
static ui_bitmap_t g_bitmaps[UI_MAX_BITMAP];
static int        g_nbitmap = 0;

/* 渐变背景（三段、逐行线性插值）。g_grad_on=0 时用纯色 bg */
static uint16_t   g_bg_top = 0, g_bg_mid = 0, g_bg_bot = 0;
static int        g_grad_on = 0;

/* RGB565 逐通道线性插值：t = 0..256（256 表示取 b） */
static uint16_t ui_lerp565(uint16_t a, uint16_t b, int t)
{
    int ar, ag, ab, br, bg, bb, r, g, bl;

    ar = (a >> 11) & 0x1F;  ag = (a >> 5) & 0x3F;  ab = a & 0x1F;
    br = (b >> 11) & 0x1F;  bg = (b >> 5) & 0x3F;  bb = b & 0x1F;

    r  = ar + (((br - ar) * t) >> 8);
    g  = ag + (((bg - ag) * t) >> 8);
    bl = ab + (((bb - ab) * t) >> 8);

    if (r  < 0) { r  = 0; }  if (r  > 31) { r  = 31; }
    if (g  < 0) { g  = 0; }  if (g  > 63) { g  = 63; }
    if (bl < 0) { bl = 0; }  if (bl > 31) { bl = 31; }

    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* 取第 y 行的背景色：0..239 在 top->mid 之间插值，240..479 在 mid->bot 之间 */
static uint16_t grad_row_color(int y)
{
    if (y < 240) {
        return ui_lerp565(g_bg_top, g_bg_mid, (y * 256) / 240);
    }
    return ui_lerp565(g_bg_mid, g_bg_bot, ((y - 240) * 256) / 240);
}

/* 行内闭区间填色（自动夹到屏幕内）*/
static void fill_span(uint16_t *line, int xa, int xb, uint16_t color)
{
    int i;
    if (xa > xb) {
        int tmp = xa;
        xa = xb;
        xb = tmp;
    }
    if (xa < 0) {
        xa = 0;
    }
    if (xb >= UI_SZ) {
        xb = UI_SZ - 1;
    }
    for (i = xa; i <= xb; i++) {
        line[i] = color;
    }
}

/* 整数开方（二进制逐位，无浮点），用于圆截面 */
static int ui_isqrt(int n)
{
    int x = n, y = 0, bit = 1 << 30;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= y + bit) {
            x -= y + bit;
            y = (y >> 1) + bit;
        } else {
            y >>= 1;
        }
        bit >>= 2;
    }
    return y;
}

void ui_clear(void)
{
    g_nrect = 0;
    g_ntext = 0;
    g_ncircle = 0;
    g_nround = 0;
    g_narc = 0;
    g_nbitmap = 0;
    g_grad_on = 0;
}

/* 圆角实心块 */
void ui_add_round_fill(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, uint16_t color)
{
    if (g_nround >= UI_MAX_ROUND) {
        return;
    }
    if (r < 0) {
        r = 0;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    g_rounds[g_nround].x = x;
    g_rounds[g_nround].y = y;
    g_rounds[g_nround].w = w;
    g_rounds[g_nround].h = h;
    g_rounds[g_nround].r = r;
    g_rounds[g_nround].t = 0;          /* 0 = 实心 */
    g_rounds[g_nround].color = color;
    g_nround++;
}

/* 圆角边框 */
void ui_add_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, int16_t t, uint16_t color)
{
    if (g_nround >= UI_MAX_ROUND) {
        return;
    }
    if (t < 1) {
        t = 1;
    }
    if (r < 0) {
        r = 0;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    g_rounds[g_nround].x = x;
    g_rounds[g_nround].y = y;
    g_rounds[g_nround].w = w;
    g_rounds[g_nround].h = h;
    g_rounds[g_nround].r = r;
    g_rounds[g_nround].t = t;          /* >0 = 边框 */
    g_rounds[g_nround].color = color;
    g_nround++;
}

/* 立体按钮：先画厚度色（整体下移 shade_dy），再画主体 */
void ui_add_btn3d(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                  uint16_t top, uint16_t bot, uint16_t shade)
{
    /* 1) 厚度层：同尺寸、下移 4px，用阴影色 */
    ui_add_round_fill(x, (int16_t)(y + 4), w, h, r, shade);
    /* 2) 主体下半（深色）*/
    ui_add_round_fill(x, y, w, h, r, bot);
    /* 3) 主体上半（亮色）—— 只覆盖上面一半，形成上亮下暗的渐变感 */
    ui_add_round_fill(x, y, w, (int16_t)(h / 2), r, top);
}

/* 进度圆弧（12 点起顺时针）*/
void ui_add_arc(int16_t cx, int16_t cy, int16_t r, int16_t t,
                int sweep_deg, uint16_t color)
{
    if (g_narc >= UI_MAX_ARC) {
        return;
    }
    if (r < 2) {
        r = 2;
    }
    if (t < 1) {
        t = 1;
    }
    if (t > r) {
        t = r;
    }
    if (sweep_deg < 0) {
        sweep_deg = 0;
    }
    if (sweep_deg > 360) {
        sweep_deg = 360;
    }
    g_arcs[g_narc].cx = cx;
    g_arcs[g_narc].cy = cy;
    g_arcs[g_narc].r = r;
    g_arcs[g_narc].t = t;
    g_arcs[g_narc].sweep = sweep_deg;
    g_arcs[g_narc].color = color;
    g_narc++;
}

void ui_set_bg_gradient(uint16_t top, uint16_t mid, uint16_t bot)
{
    g_bg_top = top;
    g_bg_mid = mid;
    g_bg_bot = bot;
    g_grad_on = 1;
}

void ui_add_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                 int16_t t, uint16_t color)
{
    if (g_nrect >= UI_MAX_RECT) {
        return;
    }
    if (t < 1) {
        t = 1;
    }
    if (h > 0 && t > h / 2) {
        t = h / 2;
    }
    g_rects[g_nrect].x = x;
    g_rects[g_nrect].y = y;
    g_rects[g_nrect].w = w;
    g_rects[g_nrect].h = h;
    g_rects[g_nrect].t = t;
    g_rects[g_nrect].color = color;
    g_nrect++;
}

void ui_add_text(int16_t x, int16_t y, const char *s, int scale, uint16_t color)
{
    if (g_ntext >= UI_MAX_TEXT) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    g_texts[g_ntext].x = x;
    g_texts[g_ntext].y = y;
    g_texts[g_ntext].scale = scale;
    g_texts[g_ntext].color = color;
    g_texts[g_ntext].s = s;
    g_ntext++;
}

void ui_add_circle(int16_t cx, int16_t cy, int16_t r, int16_t t,
                   int filled, uint16_t color)
{
    if (g_ncircle >= UI_MAX_CIRCLE) {
        return;
    }
    if (r < 2) {
        r = 2;
    }
    if (t < 1) {
        t = 1;
    }
    if (t > r) {
        t = r;
    }
    g_circles[g_ncircle].cx = cx;
    g_circles[g_ncircle].cy = cy;
    g_circles[g_ncircle].r = r;
    g_circles[g_ncircle].t = t;
    g_circles[g_ncircle].filled = (uint8_t)(filled ? 1 : 0);
    g_circles[g_ncircle].color = color;
    g_ncircle++;
}

/* 1bpp 点阵块（汉字名）：data 逐行扫描，每行 stride 字节，MSB=最左像素。
 * 硬件层不关心字体，PC 端渲染好点阵传下来即可显示任意中文。 */
void ui_add_bitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                   const uint8_t *data, int16_t stride, uint16_t color)
{
    int16_t need;

    if (g_nbitmap >= UI_MAX_BITMAP) {
        return;
    }
    if (data == 0 || w <= 0 || h <= 0) {
        return;                     /* 无名字的习惯：只显示圆点，合法 */
    }
    need = (int16_t)((w + 7) / 8);
    if (stride < need) {
        stride = need;              /* 行距不足会读串行，兜一层 */
    }
    g_bitmaps[g_nbitmap].x      = x;
    g_bitmaps[g_nbitmap].y      = y;
    g_bitmaps[g_nbitmap].w      = w;
    g_bitmaps[g_nbitmap].h      = h;
    g_bitmaps[g_nbitmap].stride = stride;
    g_bitmaps[g_nbitmap].scale  = 1;
    g_bitmaps[g_nbitmap].data   = data;
    g_bitmaps[g_nbitmap].color  = color;
    g_nbitmap++;
}

/* 同 ui_add_bitmap，但整块放大 scale 倍（日程标题 16px 点阵 2x=32px 大字） */
void ui_add_bitmap2(int16_t x, int16_t y, int16_t w, int16_t h,
                    const uint8_t *data, int16_t stride, uint16_t color,
                    int16_t scale)
{
    if (scale <= 1) {
        ui_add_bitmap(x, y, w, h, data, stride, color);
        return;
    }
    if (g_nbitmap >= UI_MAX_BITMAP || data == 0 || w <= 0 || h <= 0) {
        return;
    }
    ui_add_bitmap(x, y, (int16_t)(w * scale), (int16_t)(h * scale), data, stride, color);
    /* ui_add_bitmap 里 scale 被写成 1，这里改回真实倍数（w/h 保持源尺寸！）*/
    g_nbitmap--;
    g_bitmaps[g_nbitmap].x      = x;
    g_bitmaps[g_nbitmap].y      = y;
    g_bitmaps[g_nbitmap].w      = w;
    g_bitmaps[g_nbitmap].h      = h;
    g_bitmaps[g_nbitmap].stride = stride;
    g_bitmaps[g_nbitmap].scale  = scale;
    g_bitmaps[g_nbitmap].data   = data;
    g_bitmaps[g_nbitmap].color  = color;
    g_nbitmap++;
}

int ui_text_width(const char *s, int scale)
{
    int n = 0;
    if (s != 0) {
        while (s[n] != 0) {
            n++;
        }
    }
    if (n <= 0) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    /* 字符宽 + 字符间 1 个 scale 间距 */
    return n * UI_CHAR_W * scale + (n - 1) * scale;
}

/* ==================== 圆角数码（七段笔画） ====================
 *
 * 七段编号（与 k_clk_seg 位序一致）：
 *      aaaa
 *     f    b
 *     f    b
 *      gggg
 *     e    c
 *     e    c
 *      dddd
 *
 * 几何：每个笔画都是一个圆角矩形，横笔高 = 竖笔宽 = t，圆角半径 = t/2
 * → 两端的圆头是半圆，所以 48px 也好 120px 也好都没有台阶。
 * 竖笔高度取 h/2 并且**跨过中间那条横笔**，相邻笔画的圆头互相咬合，
 * 拐角不会出现缺口（这是七段数码最容易做难看的地方）。
 */
static const uint8_t k_clk_seg[10] = {
    /*     abcdefg  */
    0x3F,  /* 0: a b c d e f   */
    0x06,  /* 1:   b c         */
    0x5B,  /* 2: a b   d e   g */
    0x4F,  /* 3: a b c d     g */
    0x66,  /* 4:   b c   f   g */
    0x6D,  /* 5: a   c d f   g */
    0x7D,  /* 6: a   c d e f g */
    0x07,  /* 7: a b c         */
    0x7F,  /* 8: 全部          */
    0x6F,  /* 9: a b c d   f g */
};

/* 比例（相对数字高度 h）：笔画厚度 / 数字宽 / 字距 / 冒号宽 / '1' 的格子宽 */
#define CLK_THICK(h)   ((h) * 15 / 100)
#define CLK_DIGIT_W(h) ((h) * 52 / 100)
#define CLK_GAP(h)     ((h) * 11 / 100)
#define CLK_COLON_W(h) ((h) * 24 / 100)
/* '1' 只有两条竖笔，占满整格会在左右各留一大块空白（"09:41" 看着像
 * "09:4   1"）。给它一个窄格，并且**宽度计算要用同一个值**，否则居中会偏。 */
#define CLK_ONE_W(h)   ((h) * 38 / 100)

static void clock_digit(int16_t x, int16_t y, int16_t w, int16_t h,
                        int16_t t, uint8_t seg, uint16_t color)
{
    int16_t r    = (int16_t)(t / 2);
    int16_t hw   = (int16_t)(w - t);        /* 横笔长度（两端各收 t/2） */
    int16_t half = (int16_t)(h / 2);
    int16_t mid  = (int16_t)((h - t) / 2);
    /* 竖笔要比半高多伸 t/2：两个胶囊若是**尖端对尖端**相接，交界处会收成
     * 一个"腰"（'0' 的左右两边中间会有个缺口，'2' 的拐角会断开）。
     * 多伸半个笔画让它们真正重叠，视觉上才是一根连续的边。 */
    int16_t vlen = (int16_t)(half + t / 2);

    if (seg & 0x01) {                       /* a 上 */
        ui_add_round_fill((int16_t)(x + t / 2), y, hw, t, r, color);
    }
    if (seg & 0x40) {                       /* g 中 */
        ui_add_round_fill((int16_t)(x + t / 2), (int16_t)(y + mid), hw, t, r, color);
    }
    if (seg & 0x08) {                       /* d 下 */
        ui_add_round_fill((int16_t)(x + t / 2), (int16_t)(y + h - t), hw, t, r, color);
    }
    if (seg & 0x20) {                       /* f 左上 */
        ui_add_round_fill(x, y, t, vlen, r, color);
    }
    if (seg & 0x02) {                       /* b 右上 */
        ui_add_round_fill((int16_t)(x + w - t), y, t, vlen, r, color);
    }
    if (seg & 0x10) {                       /* e 左下 */
        ui_add_round_fill(x, (int16_t)(y + half - t / 2), t, vlen, r, color);
    }
    if (seg & 0x04) {                       /* c 右下 */
        ui_add_round_fill((int16_t)(x + w - t), (int16_t)(y + half - t / 2),
                          t, vlen, r, color);
    }
}

void ui_add_clock(int16_t x, int16_t y, const char *s, int16_t h, uint16_t color)
{
    int16_t t     = (int16_t)CLK_THICK(h);
    int16_t w     = (int16_t)CLK_DIGIT_W(h);
    int16_t gap   = (int16_t)CLK_GAP(h);
    int16_t cw    = (int16_t)CLK_COLON_W(h);
    int16_t cx    = x;
    int     first = 1;

    if (s == 0 || h <= 0) {
        return;
    }
    if (t < 2) {
        t = 2;
    }
    if (w < t * 2) {
        w = (int16_t)(t * 2);
    }

    for (; *s != '\0'; s++) {
        if (!first) {
            cx = (int16_t)(cx + gap);
        }
        first = 0;

        if (*s == '1') {
            /* 窄格 + 居中：七段的 1 只有右侧两条竖笔，占满整格会左右各留
             * 一大块空白，与前后字符脱开。 */
            int16_t ow = (int16_t)CLK_ONE_W(h);
            if (ow < t) {
                ow = t;
            }
            clock_digit((int16_t)(cx + (ow - t) / 2), y, ow, h, t,
                        k_clk_seg[1], color);
            cx = (int16_t)(cx + ow);
        } else if (*s >= '0' && *s <= '9') {
            clock_digit(cx, y, w, h, t, k_clk_seg[*s - '0'], color);
            cx = (int16_t)(cx + w);
        } else if (*s == ':') {
            int16_t r = (int16_t)(t / 2);
            ui_add_circle((int16_t)(cx + cw / 2), (int16_t)(y + h * 3 / 10),
                          r, r, 1, color);
            ui_add_circle((int16_t)(cx + cw / 2), (int16_t)(y + h * 7 / 10),
                          r, r, 1, color);
            cx = (int16_t)(cx + cw);
        } else if (*s == '-') {
            /* 未对时时显示的 "--:--"：只画中间那横 */
            ui_add_round_fill((int16_t)(cx + t / 2), (int16_t)(y + (h - t) / 2),
                              (int16_t)(w - t), t, (int16_t)(t / 2), color);
            cx = (int16_t)(cx + w);
        } else {
            cx = (int16_t)(cx + w / 2);     /* 空格/未知字符：占位不画 */
        }
    }
}

int ui_clock_width(const char *s, int16_t h)
{
    int16_t w   = (int16_t)CLK_DIGIT_W(h);
    int16_t gap = (int16_t)CLK_GAP(h);
    int16_t cw  = (int16_t)CLK_COLON_W(h);
    int t_min   = CLK_THICK(h);
    int total   = 0;
    int n       = 0;

    if (s == 0 || h <= 0) {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (n > 0) {
            total += gap;
        }
        if (*s == '1') {
            int ow = CLK_ONE_W(h);
            total += (ow < t_min) ? t_min : ow;
        } else if (*s >= '0' && *s <= '9') {
            total += w;
        } else if (*s == ':' || *s == '-') {
            total += (*s == ':') ? cw : w;
        } else {
            total += w / 2;
        }
        n++;
    }
    return total;
}

/* ---------------------------------------------------------------- 矩形 */
/* 矩形边框在屏幕第 y 行的落色（逐逻辑行，行内索引 = 横向 x）*/
static void paint_rect_row(const ui_rect_t *r, int y, uint16_t *line)
{
    int x0 = r->x;
    int y0 = r->y;
    int x1 = r->x + r->w - 1;
    int y1 = r->y + r->h - 1;
    int t  = r->t;

    if (y < y0 || y > y1) {
        return;
    }
    if (y < y0 + t || y > y1 - t) {
        fill_span(line, x0, x1, r->color);              /* 上/下边：整条横线 */
    } else {
        fill_span(line, x0, x0 + t - 1, r->color);      /* 左边：竖线段 */
        fill_span(line, x1 - t + 1, x1, r->color);      /* 右边：竖线段 */
    }
}

/* ---------------------------------------------------------------- 圆 */
/* 圆/圆环在屏幕第 y 行的落色 */
static void paint_circle_row(const ui_circle_t *c, int y, uint16_t *line)
{
    int dy = y - c->cy;
    int ay = (dy < 0) ? -dy : dy;
    int oh, ih, iw;

    if (ay > c->r) {
        return;
    }
    oh = ui_isqrt(c->r * c->r - ay * ay);               /* 该行外半宽 */

    if (c->filled) {
        fill_span(line, c->cx - oh, c->cx + oh, c->color);
        return;
    }
    ih = c->r - c->t;                                   /* 内半径 */
    if (ih < 1 || ay > ih) {
        fill_span(line, c->cx - oh, c->cx + oh, c->color);  /* 顶/底边：实心段 */
        return;
    }
    iw = ui_isqrt(ih * ih - ay * ay);
    fill_span(line, c->cx - oh, c->cx - iw, c->color);
    fill_span(line, c->cx + iw, c->cx + oh, c->color);
}

/* ---------------------------------------------------------------- 圆角块 */
/* 第 y 行上，圆角块的水平覆盖区间。返回 0 表示本行无覆盖。 */
static int round_span(const ui_round_t *q, int y, int *xa, int *xb)
{
    int x0 = q->x, y0 = q->y;
    int x1 = q->x + q->w - 1;
    int y1 = q->y + q->h - 1;
    int r  = q->r;
    int dy, ins;

    if (y < y0 || y > y1) {
        return 0;
    }
    ins = 0;
    if (r > 0) {
        /* 上圆角区：y 距顶 < r */
        if (y < y0 + r) {
            dy = (y0 + r) - y;                 /* 1..r */
            ins = r - ui_isqrt(r * r - dy * dy);
        } else if (y > y1 - r) {
            dy = y - (y1 - r);                 /* 1..r */
            ins = r - ui_isqrt(r * r - dy * dy);
        }
    }
    if (ins < 0) {
        ins = 0;
    }
    if (ins * 2 >= q->w) {
        return 0;
    }
    *xa = x0 + ins;
    *xb = x1 - ins;
    return 1;
}

static void paint_round_row(const ui_round_t *q, int y, uint16_t *line)
{
    int xa, xb;

    if (!round_span(q, y, &xa, &xb)) {
        return;
    }
    if (q->t <= 0) {
        fill_span(line, xa, xb, q->color);                 /* 实心 */
        return;
    }
    /* 边框模式：顶/底两行整条，其余只左右两条 */
    if (y < q->y + q->t || y > q->y + q->h - 1 - q->t) {
        fill_span(line, xa, xb, q->color);
    } else {
        fill_span(line, xa, xa + q->t - 1, q->color);
        fill_span(line, xb - q->t + 1, xb, q->color);
    }
}

/* ---------------------------------------------------------------- 圆弧 */
/* 判断点相对圆心的角度（12 点方向起、顺时针 0..359），整数运算。
 * 屏幕 y 向下：设 u = dx（向右正），v = -dy（向上正）。
 * 12 点 = (u=0,v>0) => 0°；3 点 = (u>0,v=0) => 90°；其余类推。*/
static int arc_angle_of(int dx, int dy)
{
    long long u = (long long)dx;
    long long v = (long long)(-dy);
    long long num, den;
    int q;

    if (u == 0 && v == 0) {
        return 0;
    }
    if (v >= 0 && u >= 0) {
        q = 0; num = u; den = v;          /* 0..90   12点->3点 */
    } else if (u > 0) {
        q = 1; num = -v; den = u;         /* 90..180 3点->6点 */
    } else if (v <= 0) {
        q = 2; num = -u; den = -v;        /* 180..270 6点->9点 */
    } else {
        q = 3; num = v; den = -u;         /* 270..360 9点->12点 */
    }
    if (den + num == 0) {
        return q * 90;
    }
    return q * 90 + (int)(num * 90 / (num + den));
}

/* 圆弧在屏幕第 y 行的落色 */
static void paint_arc_row(const ui_arc_t *a, int y, uint16_t *line)
{
    int dy = y - a->cy;
    int ay = (dy < 0) ? -dy : dy;
    int oh, ih, x;

    if (ay > a->r) {
        return;
    }
    oh = ui_isqrt(a->r * a->r - ay * ay);
    ih = a->r - a->t;
    if (ih < 0) {
        ih = 0;
    }

    for (x = a->cx - oh; x <= a->cx + oh; x++) {
        int dx  = x - a->cx;
        int d2  = dx * dx + dy * dy;
        if (x < 0 || x >= UI_SZ) {
            continue;                    /* 越界写 line[] 会踩坏相邻内存 → 花屏 */
        }
        if (d2 > a->r * a->r || d2 < ih * ih) {
            continue;
        }
        if (arc_angle_of(dx, dy) < a->sweep) {
            line[x] = a->color;
        }
    }
}


/* 1bpp 点阵在屏幕第 y 行的落色（逐逻辑行，与文字同构）。
 * scale>1 时每个源像素放大成 scale×scale 块（日程标题 2x 大字用）：
 * 屏幕行 dy 对应源行 dy/scale，一个源像素横向铺 scale 个屏幕像素。 */
static void paint_bitmap_row(const ui_bitmap_t *b, int y, uint16_t *line)
{
    const uint8_t *row;
    int dy = y - b->y;
    int s  = (b->scale > 1) ? b->scale : 1;
    int x, k;

    if (dy < 0 || dy >= b->h * s) {
        return;
    }
    row = b->data + (dy / s) * b->stride;
    for (x = 0; x < b->w; x++) {
        int xx = b->x + x * s;
        if (xx >= UI_SZ) {
            break;
        }
        /* MSB = 该行最左像素 */
        if ((row[x >> 3] >> (7 - (x & 7))) & 1) {
            for (k = 0; k < s && xx + k < UI_SZ; k++) {
                line[xx + k] = b->color;
            }
        }
    }
}

/* 文字在屏幕第 y 行的落色（逐逻辑行）。
 * 字符位图 12 列 × 24 行：列 lc 的 24bit 存于 glyph[lc*3 .. lc*3+2]，
 * 字节 j 覆盖行 j*8..j*8+7，MSB = 该组最上面的行。 */
static void paint_text_row(const ui_text_t *tm, int y, uint16_t *line)
{
    int idx;
    int s    = tm->scale;
    int step = UI_CHAR_W * s + s;
    int dy   = y - tm->y;

    if (dy < 0 || dy >= UI_CHAR_H * s) {
        return;
    }
    int crow = dy / s;                          /* 字符内的行 0..23 */
    int j    = crow >> 3;                       /* 第几个字节 0..2 */
    int bit  = 7 - (crow & 7);                  /* MSB = 顶部 */

    for (idx = 0; tm->s[idx] != 0; idx++) {
        unsigned char c = (unsigned char)tm->s[idx];
        int cx, lc, xx, x;

        if (c < 32 || c > 126) {
            continue;
        }
        cx = tm->x + idx * step;
        {
            const unsigned char *glyph = asc2_2412[c - 32];
            for (lc = 0; lc < UI_CHAR_W; lc++) {
                if ((glyph[lc * 3 + j] >> bit) & 1) {
                    for (xx = 0; xx < s; xx++) {
                        x = cx + lc * s + xx;
                        if (x >= 0 && x < UI_SZ) {
                            line[x] = tm->color;
                        }
                    }
                }
            }
        }
    }
}

/* 整行：背景 + 圆角块 + 矩形 + 点阵 + 圆 + 圆弧 + 文字 */
static void paint_row(int y, uint16_t *line, uint16_t bg)
{
    int i;
    uint16_t rowbg = bg;

    if (g_grad_on) {
        rowbg = grad_row_color(y);       /* 逐行线性插值，无肉眼可见色带 */
    }
    for (i = 0; i < UI_SZ; i++) {
        line[i] = rowbg;
    }
    for (i = 0; i < g_nround; i++) {
        paint_round_row(&g_rounds[i], y, line);
    }
    for (i = 0; i < g_nrect; i++) {
        paint_rect_row(&g_rects[i], y, line);
    }
    for (i = 0; i < g_nbitmap; i++) {
        paint_bitmap_row(&g_bitmaps[i], y, line);
    }
    for (i = 0; i < g_ncircle; i++) {
        paint_circle_row(&g_circles[i], y, line);
    }
    for (i = 0; i < g_narc; i++) {
        paint_arc_row(&g_arcs[i], y, line);
    }
    for (i = 0; i < g_ntext; i++) {
        paint_text_row(&g_texts[i], y, line);
    }
}

/* ---------------- 分片刷新（v1.9） ----------------
 * 整屏 480×480 一次刷完要 100~200ms。这期间主循环一步都走不动，触摸 IRQ
 * 只能把标志置上干等 —— 用户快速连点时，几次按下/抬起被合并成一次，
 * 实机表现就是"点一下没反应"和"连点几个最后一起变色"。
 *
 * 拆成每片 8 行（≈3~8ms）后，片与片之间主循环能回到触摸轮询，
 * 点击延迟从 200ms 级降到 10ms 级。画面仍是自顶向下一整屏，只是分几帧。
 */
static struct {
    uint8_t  active;
    uint8_t  started;
    uint8_t  flip;
    int      row;
    uint16_t bg;
} g_flush;

/* ★ 总线仲裁（见 ui.h 的 ui_set_bus_callback 说明）。
 * 用法：ui_set_bus_callback(servo_bus_event);
 * 段前 cb(1) 让舵机把 PWM 写挂起，段后 cb(0) 让它补写（空隙 ≈1ms）。 */
static ui_bus_cb_t g_bus_cb = 0;

void ui_set_bus_callback(ui_bus_cb_t cb)
{
    g_bus_cb = cb;
}

int ui_flush_busy(void)
{
    return g_flush.active;
}

void ui_flush_begin(uint16_t bg)
{
    /* 上一次还没刷完就开始新的：先把 SPI 批量写关掉，避免状态机脏掉。
     * 屏幕上会短暂停在上一次的中间，但下一片立刻从头覆盖。 */
    if (g_flush.active && g_flush.started) {
        LCD_endWriteMutileData();
    }
    g_flush.active  = 1;
    g_flush.started = 0;
    g_flush.flip    = 0;
    g_flush.row     = 0;
    g_flush.bg      = bg;
}

int ui_flush_step(void)
{
    int k, rows;
    uint16_t (*seg)[UI_SZ];

    if (!g_flush.active) {
        return 1;
    }
    if (!g_flush.started) {
        LCD_Set_Window(0, 0, UI_SZ, UI_SZ);
        LCD_startWriteMutileData();
        g_flush.started = 1;
    }

    seg           = g_flush.flip ? g_segB : g_segA;
    g_flush.flip  = !g_flush.flip;
    rows = (UI_SZ - g_flush.row < UI_SEG_ROWS) ? (UI_SZ - g_flush.row) : UI_SEG_ROWS;

    for (k = 0; k < rows; k++) {
        paint_row(g_flush.row + k, seg[k], g_flush.bg);
    }
    if (g_bus_cb != 0) {
        g_bus_cb(1);                 /* 即将 DMA：外部挂起 AHB 寄存器写 */
    }
    LCD_DmaSend(&seg[0][0], rows * UI_SZ);
    LCD_WaitIdle();
    if (g_bus_cb != 0) {
        g_bus_cb(0);                 /* 段间空隙：外部补写 */
    }

    g_flush.row += rows;
    if (g_flush.row >= UI_SZ) {
        LCD_endWriteMutileData();
        g_flush.active  = 0;
        g_flush.started = 0;
        g_flush.row     = 0;
        return 1;
    }
    return 0;
}

void ui_flush(uint16_t bg)
{
    ui_flush_begin(bg);
    while (!ui_flush_step()) {
        /* 同步语义：调用方要它刷完就刷完（表情切换等不能半张脸）*/
    }
}
