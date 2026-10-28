/******************************************************************************
 * @file     video_smile_dma_test.c
 * @brief    灵动笑脸（DMA 发送版）：验证"全屏单事务 + 逐行 DMA"能否大幅提速
 *
 * 背景：笑脸动画每帧要发 480x480x2B = 460KB。轮询发送（每像素读两次状态
 *       寄存器）CPU 开销是带宽的 4~5 倍 → 只有约 8~12fps。若改用 DMA 把
 *       行缓冲灌进 OSPI（官方 LCD_DmaSend），理论上可达 50MHz 四线 QSPI 的
 *       带宽上限（约 25MB/s → 460KB ≈ 18ms/帧 ≈ 55fps）。
 *
 * 关键前提（吸取之前教训）：
 *   - 仍然只用**全屏窗口 (0,0,480,480) 的一次 RAMWR 事务**，绝不使用局部窗口；
 *   - DMA 只负责"喂数据"，窗口/CS/模式切换仍由主流程控制；
 *   - 若 DMA 不可用，把下面的 TX_USE_DMA 改为 0 即可回退到已验证的轮询发送。
 *
 * 帧序（串口打印进度）：
 *   ① 轮询全屏浅蓝 2s            —— 基线（已知正常）
 *   ② DMA 逐行发送整帧：仅第 100 行为黄色，其余浅蓝，停 4s
 *      → 期望：屏幕只有一条水平黄色细线，位于约 1/5 高度处
 *      → 若整屏花/线位置不对/整屏黄 → DMA 通路不可用，改用 TX_USE_DMA=0
 *   ③ 轮询全屏绿 2s              —— 自愈检查
 *   ④ DMA 版笑脸动画（持续，每 10 帧打印一次帧号）
 *
 * 看帧率：用带时间戳的串口助手，比较两次 "[fast] frame N" 的时间差，
 *         10 帧的时间差 / 10 就是单帧耗时（例如 10 帧用 180ms → 55fps）。
 *
 * 入口：video_smile_dma_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "video.h"              /* video_init / HAL_Delay */

/* video.c 中的官方原语 */
extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_WritemutileData16_fast(uint16_t num);
extern void LCD_DmaSend(uint16_t *pdata, int len);

/* 1 = 逐行 DMA 发送；0 = 回退到轮询发送（稳定但慢） */
#define TX_USE_DMA 1

/* ==================== 配色 ==================== */
#define COL_BG    0xBEFF        /* 背景浅蓝 (186,222,248) */
#define COL_INK   0x49A5        /* 眼睛/嘴巴深棕 (74,52,40) */
#define COL_WHT   0xFFFF        /* 高光白 */
#define COL_YEL   0xFF48        /* 自检黄 (255,232,64) */
#define COL_GRN   0x27E0        /* 自检绿 (0,200,90) */

/* ==================== 布局 ==================== */
#define EYE_LX    165
#define EYE_RX    315
#define EYE_CY    190
#define EYE_R      42
#define EYE_BLINK_RY 9
#define EYE_SMILE_DY 18
#define HL_R      13
#define HL_OFF_X  (-12)
#define HL_OFF_Y  (-12)

#define MOUTH_CX  240
#define MOUTH_CY  318
#define SMILE_OUT_RX 78
#define SMILE_OUT_RY 50
#define SMILE_IN_RX  64
#define SMILE_IN_RY  38
#define BIG_RX    72
#define BIG_RY    48
#define OMOUTH_OUT_RX 45
#define OMOUTH_OUT_RY 32
#define OMOUTH_IN_RX  27
#define OMOUTH_IN_RY  17

/* ==================== 表情状态 ==================== */
#define EYE_OPEN   0
#define EYE_BLINK  1
#define EYE_SMILE  2
#define MOUTH_SMILE 0
#define MOUTH_BIG   1
#define MOUTH_O     2

typedef struct {
    uint8_t eye;
    uint8_t mouth;
    int8_t  gaze;
    uint8_t frames;
} frame_t;

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

/* ==================== 行缓冲与发送 ==================== */
#define ROW_W 480
static uint16_t g_row[ROW_W];

static void row_fill(uint16_t *row, uint16_t color)
{
    int x;
    for (x = 0; x < ROW_W; x++) row[x] = color;
}

/* 发一行：DMA 或轮询 */
static void tx_row(uint16_t *row)
{
#if TX_USE_DMA
    LCD_DmaSend(row, ROW_W);            /* 官方 DMA：len = 像素个数 */
#else
    int x;
    for (x = 0; x < ROW_W; x++) {
        LCD_WritemutileData16_fast(row[x]);
    }
#endif
}

/* ==================== 图元（整数几何） ==================== */
static inline int in_ellipse(int dx, int dy, int rx, int ry)
{
    return (dx * dx * ry * ry + dy * dy * rx * rx) <= (rx * rx * ry * ry);
}

static void draw_eye(uint16_t *row, int y, int ex, int ey, int mode, int gaze)
{
    int ddy = y - ey;
    int x0, x1, x;

    if (mode == EYE_BLINK) {
        if (ddy < -EYE_BLINK_RY || ddy > EYE_BLINK_RY) return;
    } else {
        if (ddy < -EYE_R || ddy > EYE_R) return;
    }
    x0 = ex - EYE_R; x1 = ex + EYE_R;
    if (x0 < 0) x0 = 0;
    if (x1 > ROW_W - 1) x1 = ROW_W - 1;

    for (x = x0; x <= x1; x++) {
        int dx = x - ex;

        if (mode == EYE_BLINK) {
            if (in_ellipse(dx, ddy, EYE_R, EYE_BLINK_RY)) {
                row[x] = COL_INK;
            }
        } else if (mode == EYE_SMILE) {
            if ((dx * dx + ddy * ddy) <= (EYE_R * EYE_R) &&
                (dx * dx + (ddy + EYE_SMILE_DY) * (ddy + EYE_SMILE_DY)) > (EYE_R * EYE_R)) {
                row[x] = COL_INK;
            }
        } else {
            if ((dx * dx + ddy * ddy) <= (EYE_R * EYE_R)) {
                row[x] = COL_INK;
            }
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

static void draw_mouth(uint16_t *row, int y, int mode)
{
    int dy = y - MOUTH_CY;
    int x, x0, x1;

    if (mode == MOUTH_SMILE) {
        if (dy < 0 || dy > SMILE_OUT_RY) return;
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
            if (in_ellipse(dx, dy, SMILE_OUT_RX, SMILE_OUT_RY) &&
                !in_ellipse(dx, dy, SMILE_IN_RX, SMILE_IN_RY)) {
                row[x] = COL_INK;
            }
        } else if (mode == MOUTH_BIG) {
            if (in_ellipse(dx, dy, BIG_RX, BIG_RY)) {
                row[x] = COL_INK;
            }
        } else {
            if (in_ellipse(dx, dy, OMOUTH_OUT_RX, OMOUTH_OUT_RY) &&
                !in_ellipse(dx, dy, OMOUTH_IN_RX, OMOUTH_IN_RY)) {
                row[x] = COL_INK;
            }
        }
    }
}

static void render_row(uint16_t *row, int y, int eye_mode, int mouth_mode, int gaze)
{
    row_fill(row, COL_BG);
    draw_eye(row, y, EYE_LX + gaze, EYE_CY, eye_mode, gaze);
    draw_eye(row, y, EYE_RX + gaze, EYE_CY, eye_mode, gaze);
    draw_mouth(row, y, mouth_mode);
}

/* 全屏单事务：逐行合成 + 发送整帧 */
static void flush_frame(int eye_mode, int mouth_mode, int gaze)
{
    int y;
    LCD_Set_Window(0, 0, 480, 480);
    LCD_startWriteMutileData();
    for (y = 0; y < 480; y++) {
        render_row(g_row, y, eye_mode, mouth_mode, gaze);
        tx_row(g_row);
    }
    LCD_endWriteMutileData();
}

/* 自检帧：DMA 逐行发送整帧，仅第 mark_line 行是黄色 */
static void self_check_frame(int mark_line)
{
    int y;
    LCD_Set_Window(0, 0, 480, 480);
    LCD_startWriteMutileData();
    for (y = 0; y < 480; y++) {
        row_fill(g_row, (y == mark_line) ? COL_YEL : COL_BG);
        tx_row(g_row);
    }
    LCD_endWriteMutileData();
}

/* 轮询全屏单色（基线/自愈，不用 DMA） */
static void poll_fill(uint16_t color)
{
    int y, x;
    LCD_Set_Window(0, 0, 480, 480);
    LCD_startWriteMutileData();
    for (y = 0; y < 480; y++) {
        for (x = 0; x < 480; x++) {
            LCD_WritemutileData16_fast(color);
        }
    }
    LCD_endWriteMutileData();
}

/* ==================== 测试入口 ==================== */
void video_smile_dma_test(void)
{
    int idx = 0;
    int left;
    int frame_no = 0;

    video_init();               /* 官方初始化 + 三色自检 */

    printf("[fast] DMA row-send test, TX_USE_DMA=%d\r\n", TX_USE_DMA);

    /* ① 基线：轮询全屏浅蓝 */
    printf("[fast] 1 poll fullscreen BG\r\n");
    poll_fill(COL_BG);
    HAL_Delay(2000);

    /* ② 自检：DMA 整帧，仅第 100 行黄 */
    printf("[fast] 2 DMA frame, line100 yellow\r\n");
    self_check_frame(100);
    HAL_Delay(4000);

    /* ③ 自愈检查：轮询全屏绿 */
    printf("[fast] 3 poll fullscreen GREEN\r\n");
    poll_fill(COL_GRN);
    HAL_Delay(2000);

    /* ④ DMA 版笑脸动画，每 10 帧打印帧号（用串口时间戳算帧率） */
    printf("[fast] 4 DMA smile animation\r\n");
    left = g_anim[0].frames;

    for (;;) {
        const frame_t *f = &g_anim[idx];

        flush_frame(f->eye, f->mouth, f->gaze);

        if (--left <= 0) {
            idx = (idx + 1) % ANIM_N;
            left = g_anim[idx].frames;
        }
        if ((++frame_no % 10) == 0) {
            printf("[fast] frame %d\r\n", frame_no);
        }
    }
}
