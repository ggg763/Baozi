/******************************************************************************
 * @file     anim_player.c
 * @brief    ANM3 动画播放器实现（240x240 资源 + 2x 放大到 480x480 面板）
 *
 * 数据通路（每帧、每 8 资源行一块）：
 *   XIP flash 压缩块 --puff--> g_idx(索引) --查表+水平放大--> g_rgb(480 宽)
 *   --(垂直放大：同一段发 scale 次)--> LCD_DmaSend --> 面板局部窗口
 *
 * 面板纪律（沿用 gfx / video_tetra_test 的实测结论）：
 *   1. 一次事务里发往窗口的像素数必须严格等于窗口像素数，否则画面错位污染；
 *      因此即使某块解压失败，也用索引 0 的颜色补齐该块再发，保证长度一致。
 *   2. DW DMA 单次 ≤ 4095 像素 → 每段固定 8 资源行（8*240*2 = 3840）。
 *
 * RAM：g_idx 3840B + g_rgb 7680B + g_pal 512B ≈ 12 KB，无整帧缓冲。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>

#include "anim_player.h"
#include "video.h"                  /* video_init() */
#include <drv/common.h>             /* mdelay() */

#include "third_party/puff.h"       /* puff() */

/* 资源接入方式：
 *   0 = anim_res.c 里的 C 数组（默认，最省事）
 *   1 = objcopy 生成的 libanimres.a（配合 run_gif2res_lib.bat；改代码不重烧资源）
 */
#ifndef ANIM_USE_BIN
#define ANIM_USE_BIN 0
#endif

#if ANIM_USE_BIN
#include "anim_offsets.h"           /* g_anim_bin[]：名字/偏移/大小/帧数 */
extern const unsigned char _binary_anim_all_bin_start[];    /* objcopy 生成 */
static anim_item_t g_bin_items[ANIM_COUNT];
static void anim_items_init(void)
{
    unsigned int i;
    for (i = 0; i < ANIM_COUNT; i++) {
        g_bin_items[i].name = g_anim_bin[i].name;
        g_bin_items[i].data = (const unsigned char *)(_binary_anim_all_bin_start + g_anim_bin[i].off);
        g_bin_items[i].size = g_anim_bin[i].size;
        g_bin_items[i].frames = g_anim_bin[i].frames;
    }
}
#define ANIM_ITEMS g_bin_items
#else
static void anim_items_init(void)
{
}
#define ANIM_ITEMS g_anim_items
#endif

/* src/video.c 提供的官方原语 */
extern void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);
extern void LCD_startWriteMutileData(void);
extern void LCD_endWriteMutileData(void);
extern void LCD_DmaSend(uint16_t *pdata, int len);

#define ANM_MAGIC0      'A'
#define ANM_MAGIC1      'N'
#define ANM_MAGIC2      'M'
#define ANM_MAGIC3      '3'

#define ANM_HDR_SIZE    14          /* magic4 + srcW2 + srcH2 + scale2 + frames2 + delay2 */
#define ANM_PAL_SIZE    512         /* 256 x RGB565（前 16 项有效） */
#define ANM_DATA_OFF    (ANM_HDR_SIZE + ANM_PAL_SIZE)

#define ANM_PANEL       480
#define ANM_BLOCK_ROWS  8           /* 与 PC 端 gif2res.py 的 BLOCK_ROWS 一致 */
#define ANM_MAX_IDX     (ANM_BLOCK_ROWS * ANM_PANEL)    /* puff 输出：索引 */
#define ANM_MAX_RGB     (ANM_BLOCK_ROWS * ANM_PANEL)    /* 放大后的 RGB565 段 */

static uint8_t  g_idx[ANM_MAX_IDX];
static uint16_t g_rgb[ANM_MAX_RGB];
static uint16_t g_pal[256];

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* 两个 RGB565 的加权平均（凸组合，结果必落在调色板区间内）*/
static uint16_t mix565(uint16_t a, uint16_t b, int wa, int wb)
{
    int r  = (((a >> 11) & 0x1F) * wa + ((b >> 11) & 0x1F) * wb) / (wa + wb);
    int g  = (((a >> 5) & 0x3F) * wa + ((b >> 5) & 0x3F) * wb) / (wa + wb);
    int bl = ((a & 0x1F) * wa + (b & 0x1F) * wb) / (wa + wb);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* ---------------------------------------------------------------- 渲染 */
static void render_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint16_t nblk, const uint8_t *lens, const uint8_t *src,
                         uint16_t scale)
{
    unsigned int done = 0;
    uint16_t b;

    LCD_Set_Window((uint16_t)(x * scale), (uint16_t)(y * scale),
                   (uint16_t)(w * scale), (uint16_t)(h * scale));
    LCD_startWriteMutileData();

    for (b = 0; b < nblk; b++) {
        unsigned int clen = rd16(lens + 2u * b);
        unsigned int rows = (unsigned int)(h - done);
        unsigned int npx, r, c, k;
        unsigned long dlen, slen;
        int rc;

        if (rows > ANM_BLOCK_ROWS) {
            rows = ANM_BLOCK_ROWS;
        }
        npx = rows * w;
        if (npx == 0 || npx > ANM_MAX_IDX) {
            break;
        }

        dlen = npx;
        slen = clen;
        rc = puff(g_idx, &dlen, src, &slen);

        if (rc == 0 && dlen == npx) {
            /* 正常路径：索引查表 + 水平放大。
             * scale==2 时用线性插值（0.75/0.25 权重）—— 比像素复制圆润得多，
             * 且插值结果介于两像素之间，必落在调色板灰阶区间内。 */
            for (r = 0; r < rows; r++) {
                const uint8_t *s = g_idx + r * w;
                uint16_t *d = g_rgb + r * w * scale;
                if (scale == 2) {
                    for (c = 0; c < w; c++) {
                        uint16_t a = g_pal[s[c]];
                        uint16_t b = g_pal[s[(c + 1 < (int)w) ? (c + 1) : c]];
                        d[2 * c]     = mix565(a, b, 1, 3);   /* 0.75a + 0.25b */
                        d[2 * c + 1] = mix565(a, b, 3, 1);   /* 0.25a + 0.75b */
                    }
                } else {
                    for (c = 0; c < w; c++) {
                        uint16_t v = g_pal[s[c]];
                        for (k = 0; k < scale; k++) {
                            d[c * scale + k] = v;
                        }
                    }
                }
            }
        } else {
            /* 异常：用索引 0 的颜色补齐，保证本事务像素数与窗口一致 */
            printf("[anim] inflate err rc=%d blk=%u (padded)\r\n", rc, (unsigned)b);
            for (r = 0; r < rows; r++) {
                uint16_t *d = g_rgb + r * w * scale;
                for (c = 0; c < (unsigned int)(w * scale); c++) {
                    d[c] = g_pal[0];
                }
            }
        }

        /* 垂直放大：同一段重复发送 scale 次 */
        for (k = 0; k < scale; k++) {
            LCD_DmaSend(g_rgb, (int)(rows * w * scale));
        }

        src += clen;
        done += rows;
    }

    LCD_endWriteMutileData();
}

/* ---------------------------------------------------------------- 对外 */
void anim_init(void)
{
    video_init();                       /* 官方 LCD 初始化 + 三色自检 */
    anim_items_init();                  /* objcopy 库模式下构造条目表 */
}

/* 资源条目表访问（屏蔽“C 数组 / objcopy 库”两种接入模式）*/
const anim_item_t *anim_items(void)
{
#if ANIM_USE_BIN
    return g_bin_items;
#else
    return g_anim_items;
#endif
}

/* ---------------- 单帧游标播放（非阻塞，供状态机 / 串口测试使用） ---------------- */
typedef struct {
    const anim_item_t *item;
    const uint8_t *rec0;        /* 帧表首 */
    const uint8_t *dat0;        /* 数据区首 */
    const uint8_t *rec;         /* 当前帧记录 */
    const uint8_t *dat;         /* 当前帧数据 */
    uint16_t frames;
    uint16_t idx;
    uint16_t scale;
} anim_pb_t;

static anim_pb_t s_pb;
static uint8_t   s_pb_valid = 0;

/* 选片段并复位游标：返回 0 成功（失败时调用方可以保留原有画面）*/
int anim_play_open(const anim_item_t *item)
{
    const uint8_t *buf, *rec, *rec_end;
    uint16_t src_w, scale, frames, delay_ms, i;

    s_pb_valid = 0;
    if (item == 0 || item->data == 0 || item->size < ANM_DATA_OFF) {
        return -1;
    }
    buf = item->data;
    if (buf[0] != ANM_MAGIC0 || buf[1] != ANM_MAGIC1 ||
        buf[2] != ANM_MAGIC2 || buf[3] != ANM_MAGIC3) {
        return -2;
    }
    src_w = rd16(buf + 4);
    scale = rd16(buf + 8);
    frames = rd16(buf + 10);
    delay_ms = rd16(buf + 12);
    if (src_w == 0 || scale == 0 || (uint16_t)(src_w * scale) != ANM_PANEL) {
        return -3;
    }
    if (frames == 0 || delay_ms == 0) {
        return -4;
    }

    for (i = 0; i < 256; i++) {
        g_pal[i] = rd16(buf + ANM_HDR_SIZE + 2u * i);
    }

    /* 帧记录区紧随调色板；数据区在全部记录之后 */
    rec = buf + ANM_DATA_OFF;
    rec_end = rec;
    for (i = 0; i < frames; i++) {
        uint16_t nblk = rd16(rec_end + 8);
        rec_end += 10u + 2u * nblk;
        if (rec_end > buf + item->size) {
            return -5;
        }
    }

    s_pb.item = item;
    s_pb.rec0 = rec;
    s_pb.rec = rec;
    s_pb.dat0 = rec_end;
    s_pb.dat = rec_end;
    s_pb.frames = frames;
    s_pb.idx = 0;
    s_pb.scale = scale;
    s_pb_valid = 1;
    return 0;
}

/* 渲染下一帧并推进游标；返回 1 表示“刚刚回卷到开头”（上一轮播完）*/
int anim_play_step(void)
{
    uint16_t x, y, w, h, nblk, b;
    const uint8_t *lens;
    unsigned int total = 0;
    int wrapped = 0;

    if (!s_pb_valid) {
        return -1;
    }
    if (s_pb.idx >= s_pb.frames) {
        s_pb.rec = s_pb.rec0;
        s_pb.dat = s_pb.dat0;
        s_pb.idx = 0;
        wrapped = 1;
    }

    x = rd16(s_pb.rec);
    y = rd16(s_pb.rec + 2);
    w = rd16(s_pb.rec + 4);
    h = rd16(s_pb.rec + 6);
    nblk = rd16(s_pb.rec + 8);
    lens = s_pb.rec + 10;
    for (b = 0; b < nblk; b++) {
        total += rd16(lens + 2u * b);
    }

    render_frame(x, y, w, h, nblk, lens, s_pb.dat, s_pb.scale);

    s_pb.rec += 10u + 2u * nblk;
    s_pb.dat += total;
    s_pb.idx++;
    return wrapped;
}

int anim_play(const anim_item_t *item, int loops)
{
    const uint8_t *buf, *rec, *rec_end, *dat;
    uint16_t src_w, scale, frames, delay_ms, i, b;
    int loop;

    if (item == 0 || item->data == 0 || item->size < ANM_DATA_OFF) {
        printf("[anim] bad item\r\n");
        return -1;
    }
    buf = item->data;
    if (buf[0] != ANM_MAGIC0 || buf[1] != ANM_MAGIC1 ||
        buf[2] != ANM_MAGIC2 || buf[3] != ANM_MAGIC3) {
        printf("[anim] bad magic in %s (need ANM3)\r\n", (item->name != 0) ? item->name : "?");
        return -2;
    }

    src_w = rd16(buf + 4);
    scale = rd16(buf + 8);
    frames = rd16(buf + 10);
    delay_ms = rd16(buf + 12);

    if (src_w == 0 || scale == 0 || (uint16_t)(src_w * scale) != ANM_PANEL) {
        printf("[anim] bad geometry src=%u scale=%u\r\n", (unsigned)src_w, (unsigned)scale);
        return -3;
    }
    if (frames == 0 || delay_ms == 0) {
        printf("[anim] empty clip\r\n");
        return -4;
    }

    for (i = 0; i < 256; i++) {
        g_pal[i] = rd16(buf + ANM_HDR_SIZE + 2u * i);
    }

    /* 帧记录区紧随调色板；数据区在全部记录之后 */
    rec = buf + ANM_DATA_OFF;
    rec_end = rec;
    for (i = 0; i < frames; i++) {
        uint16_t nblk = rd16(rec_end + 8);
        rec_end += 10u + 2u * nblk;
        if (rec_end > buf + item->size) {
            printf("[anim] truncated frame table\r\n");
            return -5;
        }
    }

    for (loop = 0; loop < loops; loop++) {
        rec = buf + ANM_DATA_OFF;
        dat = rec_end;

        for (i = 0; i < frames; i++) {
            uint16_t x = rd16(rec);
            uint16_t y = rd16(rec + 2);
            uint16_t w = rd16(rec + 4);
            uint16_t h = rd16(rec + 6);
            uint16_t nblk = rd16(rec + 8);
            const uint8_t *lens = rec + 10;
            unsigned int total = 0;

            for (b = 0; b < nblk; b++) {
                total += rd16(lens + 2u * b);
            }
            render_frame(x, y, w, h, nblk, lens, dat, scale);
            mdelay(delay_ms);

            rec += 10u + 2u * nblk;
            dat += total;
        }
    }
    return 0;
}

void anim_demo(void)
{
    unsigned int i;

    anim_init();
    anim_items_init();
    printf("[anim] %u clips, src %ux%u x%u, delay %u ms\r\n",
           (unsigned)ANIM_COUNT, (unsigned)ANIM_SRC_SIZE, (unsigned)ANIM_SRC_SIZE,
           (unsigned)ANIM_SCALE, (unsigned)ANIM_FRAME_DELAY_MS);

    for (;;) {
        for (i = 0; i < ANIM_COUNT; i++) {
            printf("[anim] play %s (%u frames)\r\n",
                   ANIM_ITEMS[i].name, (unsigned)ANIM_ITEMS[i].frames);
            anim_play(&ANIM_ITEMS[i], 2);
        }
    }
}
