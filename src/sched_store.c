/******************************************************************************
 * @file     sched_store.c
 * @brief    TODAY 页日程缓存 + 渲染 + 点击（见 sched_store.h）
 *
 * 【渲染模仿小程序首页】
 *   组头「9/27 SUN · N 件」→ 事件行「09:00 标题 ○/●」。
 *   设备端没有中文字库，标题由 PC 用 Pillow 渲成 1bpp 点阵下发；
 *   组头里的星期用 ASCII 缩写（SUN/MON/...），日期用数字。
 *
 * 【滚动】与习惯列表同一套手势（上滑=向下看 / 下滑=向上看），
 *   g_scroll = 第一条可见事件的序号；组头在事件行前面跟着走。
 *
 * 【点击】绘制时把每个事件行的 y 区间记进 g_rows，点击时反查。
 * ==========================================================================*/

#include <stdint.h>
#include <string.h>
#include "sched_store.h"
#include "ui.h"

/* ---------------- 卡片几何（与 habit_store.c / ui_app.c 一致）------------- */
#define CARD_X        60
#define CARD_Y        122
#define CARD_W        360
#define CARD_H        290

#define CONTENT_TOP   (CARD_Y + 15)    /* 137 */
#define CONTENT_BOT   (CARD_Y + CARD_H - 14)   /* 398 */

#define HDR_H         30               /* 日期组头高（保持原尺寸）*/
#define ROW_H         56               /* 事件行高（时间 48px 大字，行距相应加大）*/
#define TIME_X        66               /* 时间文本左界（scale2：5 字 × 24 = 120，到 186）*/
/* 标题左界：原来是 190 —— 时间实际画到 186，中间只剩 4px，实机上看就是
 * "10:00噢噢" 糊在一起。右移到 210 给时间留出 24px（正好是时间字号的一个
 * 字宽）当间隙。
 * 代价：右侧空间从 192px 缩到 174px，6 字标题（192px）放不下了 —— 所以
 * draw_item_row 里按实际可容纳字数截断【显示】。注意别去改协议里的
 * SCHED_TITLE_MAX_CHARS：那是 PC 端组帧和板子收帧共用的，单边改会让
 * 6 字日程整条被拒收（直接不显示），比截掉一个字严重得多。 */
#define TITLE_X       210              /* 标题点阵左界（2x 后最多 5 字 × 32 = 160，到 370）*/
#define DOT_CX        400              /* 完成圆圆心 x（R16：384..416，卡片内）*/
#define DOT_R         16
#define LINE_X        76               /* 分隔线左界 */
#define LINE_W        312

/* ---------------- 配色（与 habit_store.c 同一套）---------------- */
#define C_SCH_DONE    0xBBA2   /* 焦糖 */
#define C_SCH_FUTURE  0xC5D4   /* 中浅棕 */
#define C_SCH_MISS    0xD677   /* 浅灰 */

/* ---------------- 状态 ---------------- */
static sched_item_t g_items[SCHED_MAX_ITEMS];
static int          g_count = 0;

/* 接收中的"最大已收序号"，END 时整体替换（见 commit）*/
static int g_rx_max = -1;

/* 列表滚动（第一条可见事件）。同 habit 的 g_scroll：不随重推清零。 */
static int g_scroll = 0;

/* 空态反馈窗 */
#define SCHED_SYNC_HINT_MS  4000
static uint16_t g_sync_req_ms = 0;

/* 绘制时记录的事件行（供点击反查）*/
static struct { int16_t y0, y1; uint8_t idx; } g_rows[SCHED_MAX_ITEMS + 2];
static int g_nrows = 0;

/* static 字符串池（ui_add_text 只存指针，见 habit_store.c 文件头说明）*/
static char g_hdrbuf[3][20];     /* 组头 "9/27 SUN x3" */
static char g_timebuf[8][6];     /* 事件时间 "09:00" */

/* ---------------- 小工具 ---------------- */
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

static const char *weekday_name(uint8_t w)
{
    static const char *names[8] = {
        0, "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"
    };
    return (w >= 1 && w <= 7) ? names[w] : "";
}

/* ---------------- 数据层 ---------------- */

void sched_store_reset(void)
{
    /* BEGIN：只标记"接收中"，不清旧数据。
     * 整批 0x15 有 12+ 帧，万一队列溢出丢了中间几帧，
     * 旧行为（BEGIN 即清空）会让屏幕直接缺一截甚至空页；
     * 现在等 END 一起替换 —— 丢帧时旧数据还在，配合 PC 的
     * 0x91 确认重发，最多晚 2 秒，不会"个别日程消失"。 */
    g_rx_max = -1;
}

int sched_store_put(const uint8_t *data, uint8_t len)
{
    sched_item_t *h;
    uint8_t  idx, chars, nw, nh;
    uint16_t need;

    /* 定长头 10 字节：见 uart_protocol.h 的 CMD_SCHED_ITEM 布局 */
    if (data == 0 || len < 10) {
        return -1;
    }
    idx   = data[0];
    chars = data[7];
    nw    = data[8];
    nh    = data[9];

    if (idx >= SCHED_MAX_ITEMS) {
        return -2;
    }
    if (chars > SCHED_TITLE_MAX_CHARS) {
        return -1;
    }
    /* chars == 0 = 没有点阵（PC 没装 Pillow），只画时间+圆，合法 */
    if (chars > 0 && (nw < 8 || nh == 0 || nh > 24)) {
        return -1;
    }
    need = (uint16_t)10 + (uint16_t)(((uint16_t)chars * nw + 7) / 8) * nh;
    if (chars > 0 && (uint16_t)len < need) {
        return -1;
    }

    h = &g_items[idx];
    memset(h, 0, sizeof(*h));
    h->mon     = data[1];
    h->day     = data[2];
    h->weekday = data[3];
    h->hh      = data[4];
    h->mm      = data[5];
    h->status  = data[6];
    h->chars   = chars;
    h->nw      = nw;
    h->nh      = nh;

    if (h->mon < 1 || h->mon > 12)  { h->mon = 1; }
    if (h->day < 1 || h->day > 31)  { h->day = 1; }
    if (h->hh > 23)                 { h->hh = 0; }
    if (h->mm > 59)                 { h->mm = 0; }
    if (h->status > SCHED_STATUS_DONE) { h->status = SCHED_STATUS_PENDING; }

    if (chars > 0) {
        int bytes = (int)(need - 10);
        if (bytes > (int)sizeof(h->bitmap)) {
            bytes = (int)sizeof(h->bitmap);
        }
        memcpy(h->bitmap, &data[10], (size_t)bytes);
    }

    if ((int)idx > g_rx_max) {
        g_rx_max = (int)idx;
    }
    return 0;
}

void sched_store_commit(void)
{
    if (g_rx_max >= 0) {
        int new_count = g_rx_max + 1;
        int i;
        /* 新批比旧批短：把尾部旧条目清掉（chars=0 的条目画不出标题）*/
        for (i = new_count; i < g_count; i++) {
            memset(&g_items[i], 0, sizeof(sched_item_t));
        }
        g_count = new_count;
    } else {
        g_count = 0;
    }
    if (g_scroll >= g_count) {
        g_scroll = 0;
    }
    g_rx_max      = -1;
    g_sync_req_ms = 0;
}

int sched_store_count(void)
{
    return g_count;
}

const sched_item_t *sched_store_get(int idx)
{
    if (idx < 0 || idx >= g_count) {
        return 0;
    }
    return &g_items[idx];
}

/* ---------------- 渲染 ---------------- */

static void draw_group_header(int y, const sched_item_t *h, int items_left)
{
    /* 组头：● 9/27 SUN x3 */
    static int slot = 0;
    char *buf;
    int   n;

    slot = (slot + 1) % 3;
    buf  = g_hdrbuf[slot];

    n  = u2s(h->mon, buf);
    buf[n++] = '/';
    n += u2s(h->day, buf + n);
    buf[n++] = ' ';
    {
        const char *w = weekday_name(h->weekday);
        while (*w) { buf[n++] = *w++; }
    }
    buf[n++] = ' ';
    buf[n++] = 'x';
    n += u2s((uint16_t)items_left, buf + n);
    buf[n] = 0;

    ui_add_circle(80, (int16_t)(y + HDR_H / 2), 4, 4, 1, C_SCH_DONE);
    ui_add_text(92, (int16_t)(y + (HDR_H - 24) / 2 + 1), buf, 1, C_SCH_DONE);
}

static void draw_item_row(int y, int idx, const sched_item_t *h)
{
    int  cy = y + ROW_H / 2;
    char *buf;
    int  n, slot;

    /* 时间 */
    slot = (idx < 8) ? idx : (idx % 8);
    buf  = g_timebuf[slot];
    n    = u2s(h->hh, buf);
    buf[n++] = ':';
    if (h->mm < 10) { buf[n++] = '0'; }
    u2s(h->mm, buf + n);
    /* 时间 2 倍字号（24px 字模 → 48px 高 24px 宽），与顶部时钟同大 */
    ui_add_text(TIME_X, (int16_t)(cy - 24), buf, 2, UI_COL_SUB);

    /* 标题点阵：线上仍是 16px 点阵，画的时候 2x 放大成 32px 大字 */
    if (h->chars > 0 && h->nw >= 8 && h->nh > 0) {
        /* TITLE_X 右移后右侧变窄，按「到完成圆左边缘还能放几个字」截断显示。
         * 只截断绘制宽度（stride 相应取前 chars 个字），数据本身不动。 */
        int maxc  = (DOT_CX - DOT_R - TITLE_X) / h->nw;
        int chars = (h->chars > maxc) ? maxc : (int)h->chars;
        if (chars > 0) {
            int w = chars * h->nw;
            int stride = chars * (h->nw / 8);
            ui_add_bitmap2((int16_t)TITLE_X, (int16_t)(cy - h->nh),
                           (int16_t)w, (int16_t)h->nh, h->bitmap,
                           (int16_t)stride,
                           h->status == SCHED_STATUS_DONE ? UI_COL_SUB : UI_COL_TEXT,
                           2);
        }
    }

    /* 完成圆：空心=待办，实心甜甜圈=完成（同小程序的 ○ / ●）*/
    if (h->status == SCHED_STATUS_DONE) {
        ui_add_circle((int16_t)DOT_CX, (int16_t)cy, DOT_R, DOT_R, 1, C_SCH_DONE);
        ui_add_circle((int16_t)DOT_CX, (int16_t)cy, DOT_R - 6, DOT_R - 6, 1, C_CARD_TOP);
    } else {
        ui_add_circle((int16_t)DOT_CX, (int16_t)cy, DOT_R, 2, 0, C_SCH_FUTURE);
    }

    /* 行分隔线（最后一行下面那条画不画无所谓，这里统一画）*/
    ui_add_rect((int16_t)LINE_X, (int16_t)(y + ROW_H - 1), LINE_W, 1, 1, C_TRACK);
}

void sched_card_draw(void)
{
    int i, n = g_count;
    int y, shown, first_i;

    if (n <= 0) {
        /* 空态两行提示（同习惯页：说明状态 + 告诉用户能点）*/
        const char *l1, *l2;
        uint16_t    c1;
        int         cy = CARD_Y + CARD_H / 2;

        if (g_sync_req_ms > 0) {
            l1 = "requesting...";
            l2 = "PC not answering";
            c1 = C_SCH_DONE;
        } else {
            l1 = "no plans";
            l2 = "TAP TO SYNC";
            c1 = UI_COL_SUB;
        }
        ui_add_text((int16_t)((480 - ui_text_width(l1, 1)) / 2), (int16_t)(cy - 26),
                    l1, 1, c1);
        ui_add_text((int16_t)((480 - ui_text_width(l2, 1)) / 2), (int16_t)(cy + 4),
                    l2, 1, UI_COL_SUB);
        return;
    }

    if (g_scroll > n - 1) { g_scroll = n - 1; }
    if (g_scroll < 0)     { g_scroll = 0; }

    g_nrows = 0;
    y       = CONTENT_TOP;
    first_i = -1;
    shown   = 0;

    for (i = g_scroll; i < n; i++) {
        const sched_item_t *h  = &g_items[i];
        const sched_item_t *ph = (i > 0) ? &g_items[i - 1] : 0;
        int hdr = (i == g_scroll) ||
                  (ph != 0 && (ph->mon != h->mon || ph->day != h->day));
        int need = (hdr ? HDR_H : 0) + ROW_H;

        if (y + need > CONTENT_BOT) {
            break;                       /* 放不下了：滚动看 */
        }
        if (hdr) {
            /* 这一组还有几条在屏上能看见：往下数到放不下为止 */
            int left = 1, j;
            int yy   = y + need;
            for (j = i + 1; j < n && g_items[j].mon == h->mon &&
                            g_items[j].day == h->day; j++) {
                int nh2 = ((j + 1 < n && (g_items[j].mon != g_items[j + 1].mon ||
                                          g_items[j].day != g_items[j + 1].day))
                           ? HDR_H : 0) + ROW_H;
                if (yy + nh2 > CONTENT_BOT) { break; }
                yy += nh2;
                left++;
            }
            draw_group_header(y, h, left);
            y += HDR_H;
        }
        if (first_i < 0) { first_i = i; }
        draw_item_row(y, i, h);
        if (g_nrows < SCHED_MAX_ITEMS + 2) {
            g_rows[g_nrows].y0  = (int16_t)y;
            g_rows[g_nrows].y1  = (int16_t)(y + ROW_H - 1);
            g_rows[g_nrows].idx = (uint8_t)i;
            g_nrows++;
        }
        y += ROW_H;
        shown++;
    }

    if (shown == 0) {
        g_scroll--;                       /* 夹紧后一屏都放不下：回退一格重画 */
        if (g_scroll < 0) { g_scroll = 0; }
        sched_card_draw();
        return;
    }

    /* 滚动条：还有看不见的条目才画 */
    if (g_scroll + shown < n) {
        int sb_x   = CARD_X + CARD_W - 12;     /* 408 */
        int sb_top = CONTENT_TOP;
        int sb_h   = CONTENT_BOT - CONTENT_TOP;
        int thumb_h = (sb_h * shown) / n;
        int thumb_y;
        if (thumb_h < 22) { thumb_h = 22; }
        thumb_y = sb_top + ((sb_h - thumb_h) * g_scroll) / (n - 1 > 0 ? n - 1 : 1);
        ui_add_round_fill((int16_t)sb_x, (int16_t)sb_top, 4, sb_h, 2, C_TRACK);
        ui_add_round_fill((int16_t)sb_x, (int16_t)thumb_y, 4, thumb_h, 2, C_SCH_DONE);
    }
    (void)first_i;
}

/* ---------------- 空态交互 ---------------- */

int sched_card_contains(int x, int y)
{
    return (x >= CARD_X && x < CARD_X + CARD_W &&
            y >= CARD_Y && y < CARD_Y + CARD_H);
}

void sched_store_mark_sync_req(void)
{
    g_sync_req_ms = SCHED_SYNC_HINT_MS;
}

int sched_store_tick(uint32_t dt_ms)
{
    if (g_sync_req_ms == 0) {
        return 0;
    }
    if (g_sync_req_ms <= dt_ms) {
        g_sync_req_ms = 0;
        return 1;
    }
    g_sync_req_ms = (uint16_t)(g_sync_req_ms - dt_ms);
    return 0;
}

/* ---------------- 交互 ---------------- */

int sched_card_tap(int x, int y, int *out_idx, int *out_action)
{
    int i;

    if (out_idx)    { *out_idx = -1; }
    if (out_action) { *out_action = 0; }

    if (g_count <= 0) {
        return sched_card_contains(x, y) ? SCHED_TAP_SYNC : SCHED_TAP_NONE;
    }
    if (x < CARD_X || x >= CARD_X + CARD_W || y < CONTENT_TOP ||
        y >= CONTENT_BOT) {
        return SCHED_TAP_NONE;
    }
    for (i = 0; i < g_nrows; i++) {
        if (y >= g_rows[i].y0 && y <= g_rows[i].y1) {
            int idx = g_rows[i].idx;
            if (x > DOT_CX + DOT_R + 4) {
                return SCHED_TAP_NONE;    /* 点到滚动条上：不算 */
            }
            if (out_idx)    { *out_idx = idx; }
            if (out_action) { *out_action =
                (g_items[idx].status == SCHED_STATUS_DONE) ? 1 : 0; }
            return SCHED_TAP_REPORT;
        }
    }
    return SCHED_TAP_NONE;
}

void sched_store_apply_local(int idx, int action)
{
    if (idx < 0 || idx >= g_count) {
        return;
    }
    if (action == 0) {
        g_items[idx].status = SCHED_STATUS_DONE;
    } else {
        g_items[idx].status = SCHED_STATUS_PENDING;
    }
    /* 真值以后端为准：PC 回写成功后重推 0x14~0x16 校正 */
}

/* ---------------- 滚动 ---------------- */

void sched_store_scroll(int dir)
{
    g_scroll += dir;
    if (g_scroll < 0)           { g_scroll = 0; }
    if (g_scroll > g_count - 1) { g_scroll = g_count - 1; }
    if (g_scroll < 0)           { g_scroll = 0; }
}
