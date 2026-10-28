/******************************************************************************
 * @file     habit_store.c
 * @brief    包子屏幕「习惯看板」实现（见 habit_store.h）
 *
 * 【放 src/ 而不是 UserFolder/ 的原因】
 *   package.yaml 的 source_file 里 `src` 目录是通配符收源码，而 UserFolder 下
 *   是逐个文件列举的。放 src/ 可以零改动进下一轮工程生成，不碰 package.yaml。
 *   （注意：已生成的 hello_world.mk 是 CDK 写死的清单，新增文件需重新生成一次）
 *
 * 【坐标空间】
 *   屏幕 480x480，逻辑坐标 = 视觉坐标（x 右 / y 下，原点左上）。
 *   卡片外框由 ui_app.c 画（60,122,360,290），本文件只填内部内容，
 *   所以下面的布局常量要和 ui_app.c 的 CARD_* 保持一致。
 *
 * 【静态字符串池（易踩坑）】
 *   ui_add_text() 存的是**字符串指针**，真正取字形发生在 ui_flush()。
 *   所以这里绝对不能用局部数组（函数返回后栈就没了）。ui_app.c 画时间是
 *   用 static tbuf 同一个道理。下面 g_strpool 就是为此存在。
 ******************************************************************************/

#include <stdint.h>
#include <string.h>
#include "habit_store.h"
#include "ui.h"

/* ---------------- 卡片几何（须与 ui_app.c 的卡片外框一致）---------------- */
#define CARD_X        60
#define CARD_Y        122
#define CARD_W        360
#define CARD_H        290

#define ROW_TOP       (CARD_Y + 15)   /* 首行顶部 = 137 */
#define ROW_H         52              /* 行高：5 行 × 52 = 260，卡片内高 290 */
#define ROW_NAME_X    76              /* 名字点阵左边界 */
#define ROW_DOT_CX    162             /* 状态圆圆心 x */
#define ROW_DOT_R     14              /* 状态圆半径 */
#define ROW_BAR_X     190             /* 进度条左边界 */
#define ROW_BAR_W     90
#define ROW_BAR_H     10
#define ROW_PCT_X     286             /* 进度文字左边界 */
#define ROW_RIGHT     404             /* 右对齐基准（卡片右边内缩 16）*/

/* ---------------- 详情页（多次型的"一圈一格"打卡界面）----------------
 * 顶部一行（高 52）= 名字 + 状态圆 + n/t + 天数，点它返回列表；
 * 下面是格子阵：5 列 × 4 行 = 最多 20 格（喝水 12 = 3 行，护眼 16 = 4 行）。
 * 半径 13→18（直径 26→36）：实机反馈 26px 的圈太小、指头按不中。
 * 20 格的硬上限和 HABIT_MAX_SLOT(32) 不一致是刻意的：屏高放不下更大的圈阵。 */
#define GRID_COLS     5
#define GRID_ROWS     4
#define GRID_R        18             /* 格子圆半径（直径 36，好按）*/
#define GRID_PITCH_X  64             /* 水平圆心间距（5 列在 360 宽内居中）*/
#define GRID_PITCH_Y  52             /* 垂直圆心间距 */
#define GRID_CX0      112            /* 第 0 列圆心 x（5 列居中：112..368）*/
#define GRID_CY0      212            /* 第 0 行圆心 y */
#define DETAIL_TOP_H  ROW_H          /* 顶部返回条高度 = 52 */
#define DETAIL_GRID_TOP (ROW_TOP + DETAIL_TOP_H)   /* = 189 */
#define DETAIL_NAME_X   102          /* 详情页名字左界（前面留了 "<" 返回箭头）*/
#define DETAIL_DOT_CX   182          /* 详情页状态圆圆心 */
#define DETAIL_PCT_X    214          /* 详情页 n/t 文字左界 */

/* ---------------- 习惯配色（RGB565，已在 ui.h 主配色体系内取色）---------- */
#define C_HAB_DONE    0xBBA2   /* 已完成：焦糖（= UI_COL_LINE）*/
#define C_HAB_NOW     0xD487   /* 该打卡：亮焦糖（= C_BTN_TOP），最醒目 */
#define C_HAB_MISS    0xD677   /* 已错过：浅灰实心 #D9CFC0 */
/* 还没到：中浅棕。原本用 C_DOT_OFF(0xE675)，在 480x480 实际屏幕上几乎
 * 和背景糊在一起，肉眼分不出"有没有这个圆"，所以加深一档。*/
#define C_HAB_FUTURE  0xC5D4

/* ---------------- 状态 ---------------- */
static habit_item_t g_items[HABIT_MAX_COUNT];
static int          g_count = 0;

/* 接收中的"最大已收序号"。BEGIN 不再立刻清空旧数据（见 habit_store_reset）*/
static int g_rx_max = -1;

/* 详情页：-1 = 列表；0..count-1 = 正在看第几个习惯的格子阵 */
static int g_detail = -1;

/* 接收中标记：BEGIN..END 之间 = 1（draw 照常用旧数据，commit 时整体替换）*/
static int g_rx_active = 0;

/* 列表滚动偏移（第 0 个可见习惯的序号）。不随 reset 清零 —— 重推数据
 * （打卡后 force 重推）会走 reset，若清零会导致用户一打卡列表就跳回顶部；
 * 越界在 draw/tap 里自动夹，安全。 */
static int g_scroll = 0;

/* 空态点一下之后的反馈窗口（毫秒）。4 秒足够 PC 回一帧；没人理就自己
 * 变回 "waiting data..."，否则一直停在 requesting 更像卡死。*/
#define HABIT_SYNC_HINT_MS  4000
static uint16_t g_sync_req_ms = 0;

/* 设备端时钟（0x10 对时前 valid=0，顶部显示 --:--）
 *
 * g_ss 是 v1.9 加的。原来只有时分，对时一到就把秒清零 —— 但 PC 是在
 * 12:30:47 发的对时，板子却认为 12:30:00，于是它比 PC 慢 47 秒，且每次
 * 对时都重新慢一次。现在秒也一起下来，板子被钉死在 PC 时间上。*/
static uint8_t  g_mon = 0, g_day = 0, g_weekday = 0, g_hh = 0, g_mm = 0, g_ss = 0;
static uint16_t g_ms_accum = 0;
static uint8_t  g_clock_valid = 0;

/* ui_add_text 需要活到 ui_flush 的字符串（见文件头说明）*/
static char g_strpool[HABIT_DRAW_ROWS][2][12];
/* 详情页自己的字符串缓冲。不能复用 g_strpool[g_detail]：列表滚动后 g_detail
 * 可达 5~7，而 g_strpool 只有 HABIT_DRAW_ROWS(5) 行，会越界写坏相邻内存。 */
static char g_dbuf[2][12];

/* ---------------- 小工具：无符号数转十进制串 ---------------- */
/* 返回写入的字符数（不含结束符），buf 至少 7 字节 */
static int u2s(uint16_t v, char *buf)
{
    char tmp[6];
    int  n = 0, i;

    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    while (v > 0 && n < 5) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = 0;
    return n;
}

/* ---------------- 数据层 ---------------- */

void habit_store_reset(void)
{
    /* ⚠️ 刻意【不清空】g_count / g_detail：
     * 整批 0x11~0x13 是 8+ 帧连续下发，只要中途夹进一次整屏刷新（100ms+），
     * 4 深帧队列就可能溢出丢帧 —— 旧实现在这里先清空，丢一帧 0x12 就
     * "个别习惯直接消失"（实机已复现）。改成原子替换：接收期间旧数据
     * 继续显示，END（commit）时一次性换成新数据。 */
    g_rx_max    = -1;
    g_rx_active = 1;
}

int habit_store_put(const uint8_t *data, uint8_t len)
{
    habit_item_t *h;
    uint8_t  idx, chars, nw, nh;
    uint16_t need;

    /* 定长头 16 字节：见 uart_protocol.h 的 CMD_HABIT_ITEM 布局
     * （v1.9 起 [15] = open_max，点阵从 [16] 开始）*/
    if (data == 0 || len < 16) {
        return -1;
    }
    idx   = data[0];
    chars = data[12];
    nw    = data[13];
    nh    = data[14];

    if (idx >= HABIT_MAX_COUNT) {
        return -2;
    }
    if (chars > HABIT_NAME_MAX_CHARS) {
        return -1;
    }
    /* 只有真要画名字时才校验点阵参数。chars == 0 表示"这条习惯没有名字，
     * 只画圆点"（PC 端没装 Pillow 时会这样发），此时 nw/nh 都是 0，
     * 不能因此把整条习惯拒掉 —— 那会导致屏幕上一个习惯都不显示。 */
    if (chars > 0 && (nw < 8 || nh == 0 || nh > 32)) {
        return -1;
    }
    /* 名字点阵长度校验：整串连续逐行扫描，每行 ceil(chars*nw/8) 字节。
     * 注意 nw 是【单字宽】，不是整串宽 —— 与 PC 端 build_habit_item 对齐。*/
    need = (uint16_t)16 + (uint16_t)(((uint16_t)chars * nw + 7) / 8) * nh;
    if (chars > 0 && (uint16_t)len < need) {
        return -1;
    }

    h = &g_items[idx];
    memset(h, 0, sizeof(*h));
    h->mode       = data[1];
    h->total      = data[2];
    h->done       = data[3];
    h->state      = data[4];
    h->cur_slot   = data[5];
    h->open_max   = data[15];
    h->streak     = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    h->mask       = (uint32_t)data[8]
                  | ((uint32_t)data[9]  << 8)
                  | ((uint32_t)data[10] << 16)
                  | ((uint32_t)data[11] << 24);
    h->name_chars = chars;
    h->name_w     = nw;
    h->name_h     = nh;

    if (h->total == 0) {
        h->total = 1;              /* 0 格会让进度条除零，兜一层 */
    }
    if (h->total > HABIT_MAX_SLOT) {
        h->total = HABIT_MAX_SLOT;
    }
    if (h->done > h->total) {
        h->done = h->total;
    }
    if (h->cur_slot != HABIT_SLOT_NONE && h->cur_slot >= h->total) {
        h->cur_slot = HABIT_SLOT_NONE;
    }
    if (h->open_max != HABIT_OPEN_NONE && h->open_max >= h->total) {
        h->open_max = HABIT_OPEN_NONE;
    }

    if (chars > 0) {
        int bytes = (int)(need - 16);
        if (bytes > (int)sizeof(h->bitmap)) {
            bytes = (int)sizeof(h->bitmap);
        }
        memcpy(h->bitmap, &data[16], (size_t)bytes);
    }

    /* 记录的 idx 可能跳号（PC 分帧丢失），以最大 idx+1 为准（commit 时生效）*/
    if ((int)idx > g_rx_max) {
        g_rx_max = (int)idx;
    }
    return 0;
}

void habit_store_commit(void)
{
    if (g_rx_max >= 0) {
        int new_count = g_rx_max + 1;
        int i;
        /* 新批比旧批短：尾部旧条目清零（chars=0 画不出名字，不会显示残影）*/
        for (i = new_count; i < g_count; i++) {
            memset(&g_items[i], 0, sizeof(habit_item_t));
        }
        g_count = new_count;
    } else if (g_rx_active) {
        g_count = 0;                    /* PC 明确发了空列表 */
    }
    /* 详情页：只有目标序号真的不存在了才退出（打卡后重推会走 reset，
     * 这里**保住详情页** —— 否则用户每点一次圈就被踢回列表，
     * 表现为"要点两遍才生效"，实机已复现。 */
    if (g_detail >= g_count) {
        g_detail = -1;
    }
    if (g_scroll > 0 && g_count > 0 && g_scroll > g_count - 1) {
        g_scroll = g_count - 1;
    }
    g_rx_max      = -1;
    g_rx_active   = 0;
    g_sync_req_ms = 0;   /* 数据已经在路上，催的反馈可以收了 */
}

int habit_store_count(void)
{
    return g_count;
}

const habit_item_t *habit_store_get(int idx)
{
    if (idx < 0 || idx >= g_count) {
        return 0;
    }
    return &g_items[idx];
}

/* ---------------- 时钟 ---------------- */

void habit_clock_set(uint8_t mon, uint8_t day, uint8_t weekday,
                     uint8_t hh, uint8_t mm, uint8_t ss)
{
    if (mon >= 1 && mon <= 12)   { g_mon = mon; }
    if (day >= 1 && day <= 31)   { g_day = day; }
    if (weekday >= 1 && weekday <= 7) { g_weekday = weekday; }
    if (hh <= 23) { g_hh = hh; }
    if (mm <= 59) { g_mm = mm; }
    /* ss == 0xFF = 旧格式（PC 没带秒）：保留当前秒，别把秒清零
     * —— 那正是"对时让时间往回跳"的成因。 */
    if (ss <= 59) { g_ss = ss; }
    g_ms_accum    = 0;
    g_clock_valid = 1;
}

void habit_clock_tick(uint32_t dt_ms)
{
    g_ms_accum = (uint16_t)(g_ms_accum + dt_ms);
    while (g_ms_accum >= 1000) {
        g_ms_accum = (uint16_t)(g_ms_accum - 1000);
        g_ss++;
        if (g_ss >= 60) {
            g_ss = 0;
            g_mm++;
            if (g_mm >= 60) {
                g_mm = 0;
                g_hh++;
                if (g_hh >= 24) {
                    g_hh = 0;
                    /* 日期跨天：板子只做粗略推进，真值等 PC 下次对时 */
                    if (g_day < 31) { g_day++; }
                    g_weekday = (uint8_t)(g_weekday % 7 + 1);
                }
            }
        }
    }
}

void habit_clock_hhmm(char *out6)
{
    if (out6 == 0) {
        return;
    }
    if (!g_clock_valid) {
        out6[0] = '-'; out6[1] = '-';
        out6[2] = ':';
        out6[3] = '-'; out6[4] = '-';
        out6[5] = 0;
        return;
    }
    out6[0] = (char)('0' + (g_hh / 10));
    out6[1] = (char)('0' + (g_hh % 10));
    out6[2] = ':';
    out6[3] = (char)('0' + (g_mm / 10));
    out6[4] = (char)('0' + (g_mm % 10));
    out6[5] = 0;
}

void habit_clock_mmdd(char *out6)
{
    if (out6 == 0) {
        return;
    }
    if (!g_clock_valid || g_mon == 0) {
        out6[0] = '-'; out6[1] = '-';
        out6[2] = '/';
        out6[3] = '-'; out6[4] = '-';
        out6[5] = 0;
        return;
    }
    out6[0] = (char)('0' + (g_mon / 10));
    out6[1] = (char)('0' + (g_mon % 10));
    out6[2] = '/';
    out6[3] = (char)('0' + (g_day / 10));
    out6[4] = (char)('0' + (g_day % 10));
    out6[5] = 0;
}

/* ---------------- 渲染 ---------------- */

static void draw_row(int i, const habit_item_t *h)
{
    int      top = ROW_TOP + i * ROW_H;
    int      cy  = top + ROW_H / 2;
    int      drawn, x, y, w, stride;
    uint16_t dot_c, name_c;

    switch (h->state) {
    case HABIT_STATE_DONE:
        dot_c  = C_HAB_DONE;
        name_c = UI_COL_SUB;
        break;
    case HABIT_STATE_PENDING:
        dot_c  = C_HAB_NOW;
        name_c = UI_COL_TEXT;
        break;
    case HABIT_STATE_MISSED:
        dot_c  = C_HAB_MISS;
        name_c = UI_COL_SUB;
        break;
    default:                       /* FUTURE */
        dot_c  = C_HAB_FUTURE;
        name_c = UI_COL_SUB;
        break;
    }

    /* 该打卡的行：左侧一条焦糖竖条，扫一眼就知道"现在该干这个" */
    if (h->state == HABIT_STATE_PENDING) {
        ui_add_round_fill(CARD_X + 8, (int16_t)(top + 8), 4, ROW_H - 16, 2, C_HAB_NOW);
    }

    /* 名字点阵：整串逐行扫描，第 i 个字占每行的第 i*nw/8 字节起 */
    drawn = h->name_chars;
    if (drawn > HABIT_DRAW_NAME_CHARS) {
        drawn = HABIT_DRAW_NAME_CHARS;   /* 屏宽放不下更多，截断显示 */
    }
    if (drawn > 0 && h->name_w >= 8 && h->name_h > 0) {
        w      = drawn * h->name_w;
        stride = drawn * (h->name_w / 8);
        x      = ROW_NAME_X;
        y      = cy - h->name_h / 2;
        ui_add_bitmap((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h->name_h,
                      h->bitmap, (int16_t)stride, name_c);
    }

    /* 状态圆 */
    if (h->state == HABIT_STATE_DONE || h->state == HABIT_STATE_MISSED) {
        ui_add_circle((int16_t)ROW_DOT_CX, (int16_t)cy, ROW_DOT_R, ROW_DOT_R, 1, dot_c);
        /* 内圈填卡片底色，做成"甜甜圈"，比实心大色块柔和 */
        ui_add_circle((int16_t)ROW_DOT_CX, (int16_t)cy, ROW_DOT_R - 5, ROW_DOT_R - 5,
                      1, C_CARD_TOP);
    } else {
        int t = (h->state == HABIT_STATE_PENDING) ? 4 : 2;
        ui_add_circle((int16_t)ROW_DOT_CX, (int16_t)cy, ROW_DOT_R, t, 0, dot_c);
        if (h->state == HABIT_STATE_PENDING) {
            ui_add_circle((int16_t)ROW_DOT_CX, (int16_t)cy, 5, 5, 1, dot_c);
        }
    }

    /* 右侧：进度（多次型）或状态字（单次型） */
    if (h->total > 1) {
        int bar_y = cy - ROW_BAR_H / 2;
        int fillw = (ROW_BAR_W * (int)h->done) / (int)h->total;

        ui_add_round_fill(ROW_BAR_X, (int16_t)bar_y, ROW_BAR_W, ROW_BAR_H,
                          ROW_BAR_H / 2, C_TRACK);
        if (fillw > 0) {
            if (fillw < ROW_BAR_H) {
                fillw = ROW_BAR_H;       /* 太短画不出圆角，给最小可见宽度 */
            }
            ui_add_round_fill(ROW_BAR_X, (int16_t)bar_y, (int16_t)fillw, ROW_BAR_H,
                              ROW_BAR_H / 2, C_HAB_DONE);
        }
        {
            char *buf = g_strpool[i][0];
            int   n   = u2s(h->done, buf);
            buf[n++]  = '/';
            u2s(h->total, buf + n);
            ui_add_text(ROW_PCT_X, (int16_t)(cy - 12), buf, 1, UI_COL_SUB);
        }
    } else {
        /* 单次型没有"几次"可言，这里放状态词。刻意用大写短词：
         * 设备端没有中文字库，只能 ASCII；短词在 12px 下更清楚。 */
        const char *st = "...";
        if (h->state == HABIT_STATE_PENDING)     { st = "NOW"; }
        else if (h->state == HABIT_STATE_DONE)   { st = "OK"; }
        else if (h->state == HABIT_STATE_MISSED) { st = "MISS"; }
        ui_add_text(ROW_BAR_X, (int16_t)(cy - 12), st, 1, dot_c);
    }

    /* 最右侧：连续天数。设备端只显示 PC 下发的值，从不自己加减。
     * clamp 到 999：再宽就会和左边的进度文字撞上（"999d" 占 51px，
     * 从 404 往左刚好到 353；进度文字最宽到 350）。 */
    {
        char    *buf = g_strpool[i][1];
        uint16_t st  = (h->streak > 999) ? 999 : h->streak;
        int      n   = u2s(st, buf);
        int      tw;
        buf[n++] = 'd';
        buf[n]   = 0;
        tw = ui_text_width(buf, 1);
        ui_add_text((int16_t)(ROW_RIGHT - tw), (int16_t)(cy - 12), buf, 1, UI_COL_SUB);
    }
}

/* ---------------- 详情页渲染（一圈一格）---------------- */

/* 格子阵：已完成=实心焦糖 / 当前该打卡=亮焦糖厚环+中心点 / 其余=浅棕细环。
 * 设备端只拿得到 mask 和 cur_slot，分不出"已错过"和"还没到"，
 * 细环统一用 FUTURE 色 —— 真值以小程序为准，屏上只做快速操作。 */
static void draw_grid(const habit_item_t *h)
{
    int k, max_k = GRID_COLS * GRID_ROWS;

    if (max_k > HABIT_MAX_SLOT) {
        max_k = HABIT_MAX_SLOT;
    }
    for (k = 0; k < (int)h->total && k < max_k; k++) {
        int gx = GRID_CX0 + (k % GRID_COLS) * GRID_PITCH_X;
        int gy = GRID_CY0 + (k / GRID_COLS) * GRID_PITCH_Y;

        if ((h->mask >> k) & 1u) {
            ui_add_circle((int16_t)gx, (int16_t)gy, GRID_R, GRID_R, 1, C_HAB_DONE);
        } else if ((int)k == (int)h->cur_slot) {
            ui_add_circle((int16_t)gx, (int16_t)gy, GRID_R, 4, 0, C_HAB_NOW);
            ui_add_circle((int16_t)gx, (int16_t)gy, 4, 4, 1, C_HAB_NOW);
        } else {
            ui_add_circle((int16_t)gx, (int16_t)gy, GRID_R, 3, 0, C_HAB_FUTURE);
        }
    }
}

static void draw_detail(const habit_item_t *h)
{
    int      top = ROW_TOP;
    int      cy  = top + DETAIL_TOP_H / 2;
    int      drawn, x, y, w, stride;
    uint16_t dot_c, name_c;
    char    *buf = g_dbuf[0];
    int      n;

    switch (h->state) {
    case HABIT_STATE_DONE:     dot_c = C_HAB_DONE; name_c = UI_COL_SUB; break;
    case HABIT_STATE_PENDING:  dot_c = C_HAB_NOW;  name_c = UI_COL_TEXT; break;
    case HABIT_STATE_MISSED:   dot_c = C_HAB_MISS; name_c = UI_COL_SUB; break;
    default:                   dot_c = C_HAB_FUTURE; name_c = UI_COL_SUB; break;
    }

    /* 顶部一行 = 返回按钮：整行可点（见 habit_card_tap）。画个 "<" 提示 */
    ui_add_text(ROW_NAME_X, (int16_t)(cy - 12), "<", 1, UI_COL_SUB);

    /* 名字点阵：详情页最多 3 个字（给右侧 n/t + 天数留地方）*/
    drawn = h->name_chars;
    if (drawn > 3) {
        drawn = 3;
    }
    if (drawn > 0 && h->name_w >= 8 && h->name_h > 0) {
        w      = drawn * h->name_w;
        stride = drawn * (h->name_w / 8);
        x      = DETAIL_NAME_X;
        y      = cy - h->name_h / 2;
        ui_add_bitmap((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h->name_h,
                      h->bitmap, (int16_t)stride, name_c);
    }

    /* 状态圆（甜甜圈画法与列表行一致）*/
    if (h->state == HABIT_STATE_DONE || h->state == HABIT_STATE_MISSED) {
        ui_add_circle((int16_t)DETAIL_DOT_CX, (int16_t)cy, ROW_DOT_R, ROW_DOT_R, 1, dot_c);
        ui_add_circle((int16_t)DETAIL_DOT_CX, (int16_t)cy, ROW_DOT_R - 5, ROW_DOT_R - 5,
                      1, C_CARD_TOP);
    } else {
        int t = (h->state == HABIT_STATE_PENDING) ? 4 : 2;
        ui_add_circle((int16_t)DETAIL_DOT_CX, (int16_t)cy, ROW_DOT_R, t, 0, dot_c);
        if (h->state == HABIT_STATE_PENDING) {
            ui_add_circle((int16_t)DETAIL_DOT_CX, (int16_t)cy, 5, 5, 1, dot_c);
        }
    }

    /* n/t（详情页专用缓冲 g_dbuf，别碰 g_strpool —— 见 g_dbuf 声明注释）*/
    n      = u2s(h->done, buf);
    buf[n++] = '/';
    u2s(h->total, buf + n);
    ui_add_text(DETAIL_PCT_X, (int16_t)(cy - 12), buf, 1, UI_COL_SUB);

    /* 连续天数（右对齐）*/
    buf = g_dbuf[1];
    {
        uint16_t st = (h->streak > 999) ? 999 : h->streak;
        int      k  = u2s(st, buf);
        buf[k++] = 'd';
        buf[k]   = 0;
        ui_add_text((int16_t)(ROW_RIGHT - ui_text_width(buf, 1)),
                    (int16_t)(cy - 12), buf, 1, UI_COL_SUB);
    }

    draw_grid(h);

    /* 格子不超过 3 行时底部有空，补一句操作提示（设备端无中文字库，只能 ASCII）*/
    if ((int)(h->total + GRID_COLS - 1) / GRID_COLS <= 3) {
        const char *hint = "TAP A CIRCLE";
        ui_add_text((int16_t)((480 - ui_text_width(hint, 1)) / 2),
                    (int16_t)(GRID_CY0 + 3 * GRID_PITCH_Y - 10), hint, 1, C_HAB_FUTURE);
    }
}

/* 右侧滚动条：轨道 + 拇指。拇指高 ∝ 可见行数/总行数，位置 ∝ g_scroll/max。
 * 放在卡片右缘内缩 12px（x=408），避开右对齐到 404 的连续天数文字。 */
static void draw_scrollbar(int n)
{
    int sb_x    = CARD_X + CARD_W - 12;   /* 408 */
    int sb_w    = 4;
    int sb_top  = ROW_TOP;                 /* 137 */
    int sb_bot  = CARD_Y + CARD_H - 14;    /* 398 */
    int sb_h    = sb_bot - sb_top;         /* 261 */
    int max     = n - HABIT_DRAW_ROWS;
    int thumb_h = (sb_h * HABIT_DRAW_ROWS) / n;
    int thumb_y;

    if (thumb_h < 22) {
        thumb_h = 22;
    }
    thumb_y = sb_top + ((sb_h - thumb_h) * g_scroll) / max;

    ui_add_round_fill((int16_t)sb_x, (int16_t)sb_top, sb_w, sb_h, 2, C_TRACK);
    ui_add_round_fill((int16_t)sb_x, (int16_t)thumb_y, sb_w, thumb_h, 2, C_HAB_DONE);
}

void habit_card_draw(void)
{
    int i, n = g_count;

    if (n <= 0) {
        /* 空态：还没收到 PC 下发的任何习惯。两行缺一不可 ——
         * 只写 "waiting data..." 时用户只会去点屏幕，点了又毫无反馈，
         * 看起来就像坏了（第一次实机测试就是这么被问到的）。所以：
         *   第一行说状态（正在等 / 已经催了）
         *   第二行说能干什么（点一下 = 让 PC 重发）
         * 颜色也要变：催过之后用焦糖色，让"点了有反应"这件事肉眼可见。*/
        const char *l1, *l2;
        uint16_t    c1;
        int         cy = CARD_Y + CARD_H / 2;

        if (g_sync_req_ms > 0) {
            l1 = "requesting...";
            l2 = "PC not answering";
            c1 = C_HAB_NOW;
        } else {
            l1 = "waiting data...";
            l2 = "TAP TO SYNC";
            c1 = UI_COL_SUB;
        }
        ui_add_text((int16_t)((480 - ui_text_width(l1, 1)) / 2), (int16_t)(cy - 26),
                    l1, 1, c1);
        ui_add_text((int16_t)((480 - ui_text_width(l2, 1)) / 2), (int16_t)(cy + 4),
                    l2, 1, UI_COL_SUB);
        return;
    }

    /* 详情页：一圈一格（对齐小程序的打卡界面）*/
    if (g_detail >= 0 && g_detail < n) {
        draw_detail(&g_items[g_detail]);
        return;
    }

    /* 列表页：从 g_scroll 开始，最多画 HABIT_DRAW_ROWS 行。偏移越界先夹回来。 */
    if (g_scroll > n - HABIT_DRAW_ROWS) {
        g_scroll = n - HABIT_DRAW_ROWS;
    }
    if (g_scroll < 0) {
        g_scroll = 0;
    }
    for (i = 0; i < HABIT_DRAW_ROWS && (g_scroll + i) < n; i++) {
        draw_row(i, &g_items[g_scroll + i]);
    }

    /* 超出 5 个：右侧画滚动条，表示下面还有 */
    if (n > HABIT_DRAW_ROWS) {
        draw_scrollbar(n);
    }
}

/* ---------------- 空态交互 ---------------- */

int habit_card_contains(int x, int y)
{
    return (x >= CARD_X && x < CARD_X + CARD_W &&
            y >= CARD_Y && y < CARD_Y + CARD_H);
}

void habit_store_mark_sync_req(void)
{
    g_sync_req_ms = HABIT_SYNC_HINT_MS;
}

int habit_store_tick(uint32_t dt_ms)
{
    if (g_sync_req_ms == 0) {
        return 0;
    }
    if (g_sync_req_ms <= dt_ms) {
        g_sync_req_ms = 0;
        return 1;              /* 提示到期：调用方重绘一次换回文字 */
    }
    g_sync_req_ms = (uint16_t)(g_sync_req_ms - dt_ms);
    return 0;
}

/* ---------------- 交互 ---------------- */

int habit_detail_active(void)
{
    return (g_detail >= 0 && g_detail < g_count) ? 1 : 0;
}

void habit_detail_close(void)
{
    g_detail = -1;
}

int habit_card_tap(int x, int y, int *out_idx, int *out_slot, int *out_action)
{
    int row;
    const habit_item_t *h;

    if (out_idx)    { *out_idx = -1; }
    if (out_slot)   { *out_slot = -1; }
    if (out_action) { *out_action = 0; }

    if (g_count <= 0) {
        return HABIT_TAP_NONE;
    }
    if (x < CARD_X || x >= CARD_X + CARD_W || y < ROW_TOP ||
        y >= CARD_Y + CARD_H) {
        return HABIT_TAP_NONE;
    }

    /* ---- 详情页：点顶部 = 返回；点圈 = 切换那一格 ---- */
    if (g_detail >= 0) {
        int col, grid_row, slot, cx, cy2, dx, dy;

        if (y < DETAIL_GRID_TOP) {
            g_detail = -1;
            return HABIT_TAP_REDRAW;      /* 点名字行 → 回列表 */
        }

        /* 找最近的圆心（四舍五入），再校验距离，指头按偏一点也能命中 */
        col      = (x - GRID_CX0 + GRID_PITCH_X / 2) / GRID_PITCH_X;
        grid_row = (y - GRID_CY0 + GRID_PITCH_Y / 2) / GRID_PITCH_Y;
        if (col < 0 || col >= GRID_COLS || grid_row < 0 || grid_row >= GRID_ROWS) {
            return HABIT_TAP_NONE;
        }
        slot = grid_row * GRID_COLS + col;
        if (slot >= (int)g_items[g_detail].total) {
            return HABIT_TAP_NONE;        /* 空位不算 */
        }
        /* 还没到时间的格子不给点（用户要的就是这条）。
         * 已完成的格子始终可点 —— 那是"撤销"，不是提前打卡。
         * 未完成的：必须 <= open_max（PC 算好的"已开窗到第几格"）。
         * 以前这里完全不判时间，实机上表现为"细空心环也能点，
         * 一点就把未来那一次记上了"。 */
        if (!((g_items[g_detail].mask >> slot) & 1u)) {
            if (g_items[g_detail].open_max == HABIT_OPEN_NONE ||
                slot > (int)g_items[g_detail].open_max) {
                return HABIT_TAP_NONE;
            }
        }
        cx = GRID_CX0 + col * GRID_PITCH_X;
        cy2 = GRID_CY0 + grid_row * GRID_PITCH_Y;
        dx = x - cx;
        dy = y - cy2;
        if (dx * dx + dy * dy > (GRID_R + 10) * (GRID_R + 10)) {
            return HABIT_TAP_NONE;        /* 离最近的圈也太远 */
        }

        if (out_idx)    { *out_idx = g_detail; }
        if (out_slot)   { *out_slot = slot; }
        if (out_action) { *out_action = ((g_items[g_detail].mask >> slot) & 1u) ? 1 : 0; }
        return HABIT_TAP_REPORT;
    }

    /* ---- 列表页：单次型直接切换；多次型进详情 ---- */
    row = (y - ROW_TOP) / ROW_H;
    if (row < 0 || row >= HABIT_DRAW_ROWS) {
        return HABIT_TAP_NONE;
    }
    row += g_scroll;                       /* 滚动了：屏幕第 row 行 = 实际第 row+g_scroll 个 */
    if (row >= g_count) {
        return HABIT_TAP_NONE;
    }
    h = &g_items[row];

    if (h->total > 1) {
        g_detail = row;
        return HABIT_TAP_REDRAW;          /* 一天多次的：进"一圈一格"详情 */
    }

    /* 单次型且还没到时间：整行不可点。
     * 这就是用户说的"细空心环也能点" —— 列表页以前完全不看 state，
     * 点一下就把今天那次记上了。多次型不在这儿拦，因为它要进详情页
     * 才能看到/撤销已完成的那几格（详情页里逐格判时间）。 */
    if (h->state == HABIT_STATE_FUTURE) {
        return HABIT_TAP_NONE;
    }

    if (out_idx)    { *out_idx = row; }
    if (out_slot)   { *out_slot = 0; }
    if (out_action) { *out_action = (h->mask & 1u) ? 1 : 0; }
    return HABIT_TAP_REPORT;
}

void habit_store_apply_local(int idx, int slot, int action)
{
    habit_item_t *h;

    if (idx < 0 || idx >= g_count) {
        return;
    }
    if (slot < 0 || slot >= HABIT_MAX_SLOT) {
        return;
    }
    h = &g_items[idx];

    if (action == 0) {
        if (!((h->mask >> slot) & 1u)) {
            h->mask |= (1u << slot);
            if (h->done < h->total) {
                h->done++;
            }
        }
    } else {
        if ((h->mask >> slot) & 1u) {
            h->mask &= ~(1u << slot);
            if (h->done > 0) {
                h->done--;
            }
        }
    }

    /* 总状态跟着格子走（配色要立刻变）。
     * 注意：**不碰 h->streak** —— 天数只能由后端产生，设备端绝不自己加。 */
    if (h->done >= h->total) {
        h->state    = HABIT_STATE_DONE;
        h->cur_slot = HABIT_SLOT_NONE;
    } else if (h->state == HABIT_STATE_DONE) {
        h->state = HABIT_STATE_PENDING;   /* 撤销了一个，退回"待确认" */
    }
}

/* ---------------- 列表滚动 ---------------- */

int habit_store_scroll_max(void)
{
    if (g_count <= HABIT_DRAW_ROWS) {
        return 0;
    }
    return g_count - HABIT_DRAW_ROWS;
}

int habit_store_scroll_offset(void)
{
    return g_scroll;
}

void habit_store_scroll(int dir)
{
    int max = habit_store_scroll_max();

    g_scroll += dir;
    if (g_scroll < 0) {
        g_scroll = 0;
    }
    if (g_scroll > max) {
        g_scroll = max;
    }
}
