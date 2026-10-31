/******************************************************************************
 * @file     pomo.c
 * @brief    计时功能模块实现（见 pomo.h）：番茄钟 / 正计时 / 倒计时
 *
 * 页面结构（480x480，视觉=逻辑坐标，排线朝下）：
 *   MODE   三入口：POMODORO / COUNT UP / COUNTDOWN（卡片层第 2 页）
 *   PSET   番茄钟：选本轮个数（1-10 圆圈）+ EXIT / START
 *   RUN    专注倒计时 MM:SS + 轮数 + GIVE UP（→ QUIT 确认）
 *   REST   休息倒计时（同 RUN 布局）
 *   QUIT   GIVE UP? + YES / NO
 *   DONE   ALL DONE!（点击返回）
 *   UP     正计时：左上角 EXIT + START/PAUSE + RESET（大数字 MM:SS）
 *   CDSET  倒计时设置：时/分/秒 三列 + / - + EXIT / START
 *   CD     倒计时运行 HH:MM:SS + PAUSE/RESUME + GIVE UP
 *   CDDONE TIME UP!（点击返回）
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include "pomo.h"
#include "ui.h"

/* 回卡片层：ui_app.c 提供（重绘指定卡片页，page=1 会转回 MODE 页）*/
extern void ui_cards_show(int page);
/* 卡片页公共骨架（背景 / 顶部时钟 / 页码圆点 / 关闭按钮 / 卡片框）。
 * MODE 页就是卡片层的第 2 页，必须跟其余四页共用同一套骨架 —— 以前这里
 * 自己画 3 个圆点、不画时钟、不画卡片框，整页像另一个 App。 */
extern void ui_cards_chrome(int page);
/* 只要顶栏（时钟 + 六个页码圆点，不画关闭按钮、不画卡片框）。
 * 深层子页用它：内容是整屏铺开的，套不进卡片框，但页码必须是同一套 6 个。*/
extern void ui_cards_header(int page, int with_close);

/* 深层子页的统一开场：清屏 + 背景渐变 + 第 2 页顶栏。
 * 之前每个 render_xxx 各写一遍，且其中两个还在用旧的 3 圆点 page_dots()，
 * 于是同一个番茄钟流程里出现了两种页码样式。 */
static void page_begin(void)
{
    ui_clear();
    ui_set_bg_gradient(C_BG_TOP, C_BG_MID, C_BG_BOT);
    ui_cards_header(1, 0);
}

typedef enum {
    PS_OFF = 0,
    PS_MODE,         /* 三入口页（卡片层第 2 页）*/
    PS_PSET,         /* 番茄钟个数选择 */
    PS_RUN,          /* 番茄钟专注 */
    PS_REST,         /* 番茄钟休息 */
    PS_QUIT,         /* 放弃确认（RUN/REST/CD 共用）*/
    PS_DONE,         /* 番茄钟全部完成 */
    PS_UP,           /* 正计时 */
    PS_CDSET,        /* 倒计时时长设置 */
    PS_CD,           /* 倒计时运行 */
    PS_CDDONE        /* 倒计时到点 */
} pomo_state_t;

static pomo_state_t st = PS_OFF;
static pomo_state_t st_before_quit = PS_RUN;

static int total_rounds = 4;            /* 本轮番茄钟个数（用户选定）*/
static int done_rounds = 0;             /* 已完成的专注数 */
static int remain_sec = 0;              /* 倒计时剩余秒 */
static int elapsed_sec = 0;             /* 正计时已过秒 */
static int cd_hour = 0;                 /* 倒计时设定：时 */
static int cd_min = 5;                  /* 倒计时设定：分 */
static int cd_sec = 0;                  /* 倒计时设定：秒 */
static int up_running = 0;              /* 正计时是否在走 */
static int cd_paused = 0;               /* 倒计时是否暂停 */
static uint32_t ms_acc = 0;             /* 毫秒累积 */

#define FOCUS_SEC   (25 * 60)
#define REST_SEC    (5 * 60)

/* ---------------- 布局常量 ---------------- */
/* 番茄钟 10 圆圈：两行各 5。
 * 起点从 186 下移到 204：顶栏加了时钟（24~72）与页码圆点（86~99）之后，
 * 标题落在 100、副标题落在 140（字节底 161），第一行圆再从 186 起只剩 21px，
 * 现在留 15px，两行间距 128。 */
#define CIRC_R      28
#define CIRC_T      4
static const int circ_x[5] = { 96, 168, 240, 312, 384 };
static const int circ_y[2] = { 204, 332 };
#define CIRC_SHAD_DY  3      /* 圆点厚度层下移量 */

/* MODE 三入口按钮 {x,y,w,h}
 * 卡片框是 (60,122,360,290)，即 y 122..412。三个 70 高的按钮正好占 210，
 * 余下 80 均分四段（上 / 间 / 间 / 下 = 20），所以起点是 142 / 232 / 322。
 * 改版前是 120/220/320，第一个按钮的顶边压出卡片框 2px。 */
static const int mode_btn[3][4] = {
    { 90, 142, 300, 70 },
    { 90, 232, 300, 70 },
    { 90, 322, 300, 70 }
};
/* 通用 EXIT / START（设置页底部）*/
static const int exit_btn[4]  = { 70, 385, 130, 60 };
static const int start_btn[4] = { 280, 385, 130, 60 };
/* 番茄钟运行：GIVE UP（居中）*/
static const int giveup_btn[4] = { 160, 385, 160, 60 };
/* 正计时：底部一行 EXIT / START(PAUSE) / RESET。
 * EXIT 原来在左上角 (16,16)，顶栏加了时钟后就撞上了，挪到底部更好点。 */
static const int up_exit_btn[4] = { 16, 385, 120, 60 };
static const int up_go_btn[4]   = { 156, 385, 148, 60 };
static const int up_rst_btn[4]  = { 316, 385, 120, 60 };
/* 倒计时设置：时/分/秒 三列 + / -（列中心 120/240/360）
 * 顶栏占掉 24~98 之后整体下移 16：上排 + 顶边 160，
 * 大数字占 218..290，下排 - 顶边 302，互不重叠。 */
static const int cs_h_up[4] = { 80, 160, 80, 56 };
static const int cs_h_dn[4] = { 80, 302, 80, 56 };
static const int cs_m_up[4] = { 200, 160, 80, 56 };
static const int cs_m_dn[4] = { 200, 302, 80, 56 };
static const int cs_s_up[4] = { 320, 160, 80, 56 };
static const int cs_s_dn[4] = { 320, 302, 80, 56 };
/* 倒计时运行：PAUSE/RESUME + GIVE UP */
static const int cd_pause_btn[4]  = { 80, 385, 150, 60 };
static const int cd_giveup_btn[4] = { 250, 385, 150, 60 };
/* 放弃确认 */
static const int yes_btn[4] = { 80, 300, 140, 60 };
static const int no_btn[4]  = { 260, 300, 140, 60 };

/* 数字字符串（静态存储：渲染列表只存指针）*/
static char num_str[10][3];
static char hh_str[3];
static char mm_str[3];
static char ss_str[3];

/* ---------------- 渲染辅助（拟物可爱风皮肤）---------------- */

/* 立体按钮：圆角 + 上亮下暗 + 下方厚度阴影 */
static void btn(const int b[4], const char *label, int scale)
{
    int tw = ui_text_width(label, scale);
    int r  = b[3] / 2;                      /* 胶囊圆角 */

    ui_add_btn3d((int16_t)b[0], (int16_t)b[1], (int16_t)b[2], (int16_t)b[3],
                 (int16_t)r, C_CARD_TOP, C_CARD_BOT, C_CARD_SHAD);
    ui_add_round_rect((int16_t)b[0], (int16_t)b[1], (int16_t)b[2], (int16_t)b[3],
                      (int16_t)r, 2, C_CARD_LINE);
    ui_add_text((int16_t)(b[0] + (b[2] - tw) / 2),
                (int16_t)(b[1] + (b[3] - 24 * scale) / 2),
                label, scale, C_TITLE);
}

/* 主按钮（实心焦糖 + 厚度）：用于 START / 确认类 */
static void btn_primary(const int b[4], const char *label, int scale)
{
    int tw = ui_text_width(label, scale);
    int r  = b[3] / 2;

    ui_add_btn3d((int16_t)b[0], (int16_t)b[1], (int16_t)b[2], (int16_t)b[3],
                 (int16_t)r, C_BTN_TOP, C_BTN_BOT, C_BTN_SHAD);
    ui_add_text((int16_t)(b[0] + (b[2] - tw) / 2),
                (int16_t)(b[1] + (b[3] - 24 * scale) / 2),
                label, scale, C_CARD_TOP);
}

/* 带图标的入口按钮（左侧彩色圆点 + 文字）*/
static void btn_icon(const int b[4], const char *label, int scale, uint16_t dot)
{
    int r  = b[3] / 2;
    int ix = b[0] + 30;                     /* 图标圆心 x */
    int iy = b[1] + b[3] / 2;

    ui_add_btn3d((int16_t)b[0], (int16_t)b[1], (int16_t)b[2], (int16_t)b[3],
                 (int16_t)r, C_CARD_TOP, C_CARD_BOT, C_CARD_SHAD);
    ui_add_round_rect((int16_t)b[0], (int16_t)b[1], (int16_t)b[2], (int16_t)b[3],
                      (int16_t)r, 2, C_CARD_LINE);
    ui_add_circle((int16_t)ix, (int16_t)iy, 13, 3, 1, dot);
    ui_add_text((int16_t)(b[0] + 58),
                (int16_t)(b[1] + (b[3] - 24 * scale) / 2),
                label, scale, C_TITLE);
}

static int pt_in(int x, int y, const int b[4])
{
    return (x >= b[0] && x <= b[0] + b[2] && y >= b[1] && y <= b[1] + b[3]);
}

/* 大数字 + 可选环形进度（拟物风）。sec=剩余秒，total=总秒（0=不画进度环）
 * 说明：默认不画环（用户要求去掉），仅保留纯文字大时钟。*/
static void big_clock_ring(int sec, int total, int cy, int scale)
{
    static char buf[10];
    int mm, ss, w;

    if (sec > 99 * 60 + 59) {
        sec = 99 * 60 + 59;
    }
    mm = sec / 60;
    ss = sec % 60;
    buf[0] = (char)('0' + (mm / 10) % 10);
    buf[1] = (char)('0' + mm % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + ss / 10);
    buf[4] = (char)('0' + ss % 10);
    buf[5] = 0;

    if (total > 0) {
        int sweep = (sec * 360) / total;    /* 剩余比例 -> 弧长 */
        ui_add_circle(240, (int16_t)cy, 66, 8, 0, C_TRACK);      /* 底槽 */
        ui_add_arc(240, (int16_t)cy, 66, 8, sweep, C_HILITE);    /* 进度弧 */
    }
    /* v2.2：圆角数码。23 是旧点阵的半高（24*scale/2），用来保持纵向居中不变。 */
    w = ui_clock_width(buf, (int16_t)(24 * scale));
    ui_add_clock((int16_t)((480 - w) / 2), (int16_t)(cy - 12 * scale),
                 buf, (int16_t)(24 * scale), UI_COL_TEXT);
}

/* 大数字 MM:SS（秒数），scale 5，纯文字无圆环。buf 必须 static。*/
static void big_clock(int sec)
{
    big_clock_ring(sec, 0, 240, 5);
}

/* 大数字 HH:MM:SS（倒计时，支持超 1 小时），scale 4 */
static void big_clock_hms(int sec)
{
    static char buf[9];
    int h, m, s, w;
    if (sec > 99 * 3600 + 59 * 60 + 59) {
        sec = 99 * 3600 + 59 * 60 + 59;
    }
    h = sec / 3600;
    m = (sec % 3600) / 60;
    s = sec % 60;
    buf[0] = (char)('0' + (h / 10) % 10);
    buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + m / 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + s / 10);
    buf[7] = (char)('0' + s % 10);
    buf[8] = 0;
    /* v2.2：圆角数码，h = 96（原 scale 4 × 24）。8 个字符 + 两个冒号 */
    w = ui_clock_width(buf, 96);
    ui_add_clock((int16_t)((480 - w) / 2), 216, buf, 96, UI_COL_TEXT);
}

/* 两位数字字符串（时/分/秒显示）*/
static void two_digit(char out[3], int v)
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

/* ---------------- 各页渲染（拟物可爱风）---------------- */
static void render_mode(void)
{
    ui_cards_chrome(1);                 /* 第 2 页 = 索引 1，与其余四页同骨架 */
    btn_icon(mode_btn[0], "POMODORO",  2, C_ICON_RED);
    btn_icon(mode_btn[1], "COUNT UP",  2, C_ICON_GRN);
    btn_icon(mode_btn[2], "COUNTDOWN", 2, C_ICON_BLU);
    /* 分片刷，与 ui_cards_show 的其余页一致：同步 ui_flush() 会一次卡
     * 100~200ms，这期间触摸 IRQ 只能置标志，点了像没反应。 */
    ui_flush_begin(UI_COL_BG);
}

static void render_pset(void)
{
    const char *title = "POMODORO";
    int i, w;

    page_begin();                       /* 顶栏与其余卡片页一致：时钟 + 6 个圆点 */

    /* 标题下移到 y=100：顶栏的页码圆点在 y=92（占 86~98），
     * 原来放在 76 会正好压在圆点上。 */
    w = ui_text_width(title, 2);
    ui_add_text((int16_t)((480 - w) / 2), 100, title, 2, C_TITLE);

    w = ui_text_width("HOW MANY?", 1);
    ui_add_text((int16_t)((480 - w) / 2), 140, "HOW MANY?", 1, UI_COL_SUB);

    for (i = 0; i < 10; i++) {
        int row = i / 5;
        int col = i % 5;
        int sel = (i + 1) == total_rounds;
        int cx = circ_x[col];
        int cy = circ_y[row];

        num_str[i][0] = (char)('0' + (i + 1) / 10);
        num_str[i][1] = (char)('0' + (i + 1) % 10);
        num_str[i][2] = 0;
        if (num_str[i][0] == '0') {
            num_str[i][0] = num_str[i][1];
            num_str[i][1] = 0;
        }

        if (sel) {
            /* 选中：实心焦糖圆 + 白字 */
            ui_add_circle((int16_t)cx, (int16_t)(cy + CIRC_SHAD_DY), CIRC_R, CIRC_R,
                          1, C_CARD_SHAD);                    /* 厚度 */
            ui_add_circle((int16_t)cx, (int16_t)cy, CIRC_R, CIRC_R,
                          1, C_HILITE);
            w = ui_text_width(num_str[i], 1);
            ui_add_text((int16_t)(cx - w / 2), (int16_t)(cy - 12), num_str[i], 1,
                        C_CARD_TOP);
        } else {
            /* 未选：米色填充 + 焦糖描边 */
            ui_add_circle((int16_t)cx, (int16_t)(cy + CIRC_SHAD_DY), CIRC_R, CIRC_R,
                          1, C_CARD_SHAD);
            ui_add_circle((int16_t)cx, (int16_t)cy, CIRC_R, CIRC_R,
                          1, C_CARD_TOP);
            ui_add_circle((int16_t)cx, (int16_t)cy, CIRC_R, 3,
                          0, C_CARD_LINE);
            w = ui_text_width(num_str[i], 1);
            ui_add_text((int16_t)(cx - w / 2), (int16_t)(cy - 12), num_str[i], 1,
                        C_TITLE);
        }
    }

    btn(exit_btn, "EXIT", 2);
    btn_primary(start_btn, "START", 2);
    ui_flush(UI_COL_BG);
}

/* 番茄钟/倒计时共用：大数字 + 标签 + 轮数 + 按钮 */
static void render_timer(const char *label)
{
    char round[5];
    int w;

    page_begin();

    /* 顶部状态标签 + 轮数（y=108：让开顶栏的页码圆点，那排在 86~99）*/
    if (st == PS_RUN || st == PS_REST) {
        round[0] = (char)('0' + ((done_rounds + 1 > total_rounds)
                                     ? total_rounds : done_rounds + 1));
        round[1] = '/';
        round[2] = (char)('0' + total_rounds);
        round[3] = 0;
        /* 组合标签："FOCUS  2/4" */
        {
            static char head[20];
            int n = 0, k = 0;
            for (k = 0; label[k] != 0 && n < 12; k++) {
                head[n++] = label[k];
            }
            head[n++] = ' ';
            head[n++] = ' ';
            head[n++] = round[0];
            head[n++] = round[1];
            head[n++] = round[2];
            head[n] = 0;
            w = ui_text_width(head, 1);
            ui_add_text((int16_t)((480 - w) / 2), 108, head, 1, UI_COL_SUB);
        }
    } else {
        w = ui_text_width(label, 1);
        ui_add_text((int16_t)((480 - w) / 2), 108, label, 1, UI_COL_SUB);
    }

    /* 大数字（纯文字，无进度环）*/
    if (st == PS_CD) {
        big_clock_hms(remain_sec);
    } else {
        big_clock(remain_sec);
    }

    if (st == PS_RUN || st == PS_REST) {
        btn(giveup_btn, "GIVE UP", 1);
    } else {
        /* 倒计时运行页：PAUSE/RESUME 在左、GIVE UP 在右（两个独立按钮位）*/
        btn_primary(cd_pause_btn, cd_paused ? "RESUME" : "PAUSE", 1);
        btn(cd_giveup_btn, "GIVE UP", 1);
    }

    ui_flush(UI_COL_BG);
}

/* 正计时：大数字 + 底部 EXIT / START。 */
static void render_up(void)
{
    int w;
    const char *label = "COUNT UP";

    page_begin();

    w = ui_text_width(label, 1);
    ui_add_text((int16_t)((480 - w) / 2), 108, label, 1, UI_COL_SUB);

    big_clock(elapsed_sec);

    /* EXIT 从左上角挪到底部一行：左上角现在是顶栏时钟的地盘，
     * 三个按钮并排反而更好点（与番茄钟设置页的 EXIT / START 同款位置）。 */
    btn(up_exit_btn, "EXIT", 1);
    btn_primary(up_go_btn, up_running ? "PAUSE" : "START", 1);
    btn(up_rst_btn, "RESET", 1);
    ui_flush(UI_COL_BG);
}

/* 倒计时设置：时/分/秒 三列 + / -，大数字显示当前设定 */
static void render_cdset(void)
{
    const char *title = "COUNTDOWN";
    int w;

    page_begin();

    /* 只留标题：顶栏加了时钟之后，160 起就是 +/- 那一行，
     * 再塞一行 "SET TIME" 会和 + 号圆按钮贴到一起。 */
    w = ui_text_width(title, 2);
    ui_add_text((int16_t)((480 - w) / 2), 100, title, 2, C_TITLE);

    /* 时列 */
    btn(cs_h_up, "+", 2);
    two_digit(hh_str, cd_hour);
    w = ui_clock_width(hh_str, 72);
    ui_add_clock((int16_t)(120 - w / 2), 218, hh_str, 72, C_TITLE);
    btn(cs_h_dn, "-", 2);

    w = ui_clock_width(":", 72);
    ui_add_clock((int16_t)(180 - w / 2), 218, ":", 72, UI_COL_SUB);

    /* 分列 */
    btn(cs_m_up, "+", 2);
    two_digit(mm_str, cd_min);
    w = ui_clock_width(mm_str, 72);
    ui_add_clock((int16_t)(240 - w / 2), 218, mm_str, 72, C_TITLE);
    btn(cs_m_dn, "-", 2);

    w = ui_clock_width(":", 72);
    ui_add_clock((int16_t)(300 - w / 2), 218, ":", 72, UI_COL_SUB);

    /* 秒列 */
    btn(cs_s_up, "+", 2);
    two_digit(ss_str, cd_sec);
    w = ui_clock_width(ss_str, 72);
    ui_add_clock((int16_t)(360 - w / 2), 218, ss_str, 72, C_TITLE);
    btn(cs_s_dn, "-", 2);

    btn(exit_btn, "EXIT", 2);
    btn_primary(start_btn, "START", 2);
    ui_flush(UI_COL_BG);
}

static void render_quit(void)
{
    int w;
    const char *msg = "GIVE UP?";

    page_begin();
    w = ui_text_width(msg, 3);
    ui_add_text((int16_t)((480 - w) / 2), 180, msg, 3, C_TITLE);

    btn(yes_btn, "YES", 2);
    btn_primary(no_btn, "NO", 2);
    ui_flush(UI_COL_BG);
}

static void render_done(const char *msg)
{
    int w;
    page_begin();
    w = ui_text_width(msg, 3);
    ui_add_text((int16_t)((480 - w) / 2), 220, msg, 3, C_HILITE);
    ui_flush(UI_COL_BG);
}

/* ---------------- 阶段切换 ---------------- */
static void begin_focus(void)
{
    remain_sec = FOCUS_SEC;
    ms_acc = 0;
    st = PS_RUN;
    render_timer("FOCUS");
    printf("[EVENT] FOCUS_START\r\n");
}

static void begin_rest(void)
{
    remain_sec = REST_SEC;
    ms_acc = 0;
    st = PS_REST;
    render_timer("REST");
    printf("[EVENT] REST_START\r\n");
}

static void begin_cd(void)
{
    remain_sec = cd_hour * 3600 + cd_min * 60 + cd_sec;
    if (remain_sec < 1) {
        remain_sec = 1;
    }
    cd_paused = 0;
    ms_acc = 0;
    st = PS_CD;
    render_timer("COUNTDOWN");
}

/* ---------------- 对外接口 ---------------- */
void pomo_reset(void)
{
    st = PS_OFF;
    done_rounds = 0;
    remain_sec = 0;
    elapsed_sec = 0;
    up_running = 0;
    cd_paused = 0;
    ms_acc = 0;
}

int pomo_active(void)
{
    return st != PS_OFF;
}

int pomo_at_mode(void)
{
    return st == PS_MODE;
}

/* 卡片层第 2 页：渲染三入口 */
void pomo_enter_mode(void)
{
    st = PS_MODE;
    render_mode();
}

void pomo_on_tap(int x, int y)
{
    int i;

    switch (st) {
    case PS_MODE:
        if (pt_in(x, y, mode_btn[0])) {
            total_rounds = 4;
            done_rounds = 0;
            st = PS_PSET;
            render_pset();
        } else if (pt_in(x, y, mode_btn[1])) {
            elapsed_sec = 0;
            up_running = 0;
            st = PS_UP;
            render_up();
        } else if (pt_in(x, y, mode_btn[2])) {
            cd_hour = 0;
            cd_min = 5;
            cd_sec = 0;
            st = PS_CDSET;
            render_cdset();
        }
        break;

    case PS_PSET:
        for (i = 0; i < 10; i++) {
            int row = i / 5;
            int col = i % 5;
            int dx = x - circ_x[col];
            int dy = y - circ_y[row];
            if (dx * dx + dy * dy <= CIRC_R * CIRC_R) {
                total_rounds = i + 1;
                render_pset();
                return;
            }
        }
        if (pt_in(x, y, exit_btn)) {
            st = PS_MODE;
            render_mode();
        } else if (pt_in(x, y, start_btn)) {
            done_rounds = 0;
            begin_focus();
        }
        break;

    case PS_RUN:
    case PS_REST:
        if (pt_in(x, y, giveup_btn)) {
            st_before_quit = st;
            st = PS_QUIT;
            render_quit();
        }
        break;

    case PS_CD:
        if (pt_in(x, y, cd_pause_btn)) {
            cd_paused = !cd_paused;         /* PAUSE <-> RESUME */
            render_timer("COUNTDOWN");
        } else if (pt_in(x, y, cd_giveup_btn)) {
            st_before_quit = st;
            st = PS_QUIT;
            render_quit();
        }
        break;

    case PS_UP:
        if (pt_in(x, y, up_exit_btn)) {
            st = PS_MODE;                   /* 回三入口页 */
            render_mode();
        } else if (pt_in(x, y, up_go_btn)) {
            up_running = !up_running;       /* START <-> PAUSE */
            render_up();
        } else if (pt_in(x, y, up_rst_btn)) {
            elapsed_sec = 0;
            up_running = 0;
            render_up();
        }
        break;

    case PS_CDSET:
        if (pt_in(x, y, cs_h_up)) {
            cd_hour = (cd_hour + 1) % 100;
            render_cdset();
        } else if (pt_in(x, y, cs_h_dn)) {
            cd_hour = (cd_hour + 99) % 100;
            render_cdset();
        } else if (pt_in(x, y, cs_m_up)) {
            cd_min = (cd_min + 1) % 60;
            render_cdset();
        } else if (pt_in(x, y, cs_m_dn)) {
            cd_min = (cd_min + 59) % 60;
            render_cdset();
        } else if (pt_in(x, y, cs_s_up)) {
            cd_sec = (cd_sec + 1) % 60;
            render_cdset();
        } else if (pt_in(x, y, cs_s_dn)) {
            cd_sec = (cd_sec + 59) % 60;
            render_cdset();
        } else if (pt_in(x, y, exit_btn)) {
            st = PS_MODE;
            render_mode();
        } else if (pt_in(x, y, start_btn)) {
            begin_cd();
        }
        break;

    case PS_QUIT:
        if (pt_in(x, y, yes_btn)) {
            pomo_reset();
            st = PS_MODE;
            render_mode();
        } else if (pt_in(x, y, no_btn)) {
            st = st_before_quit;
            if (st == PS_UP) {
                render_up();
            } else {
                render_timer(st == PS_REST ? "REST"
                             : (st == PS_CD ? "COUNTDOWN" : "FOCUS"));
            }
        }
        break;

    case PS_DONE:
    case PS_CDDONE:
        st = PS_MODE;
        render_mode();
        break;

    default:
        break;
    }
}

void pomo_tick(uint32_t dt_ms)
{
    int redraw = 0;

    if (st != PS_RUN && st != PS_REST && st != PS_CD && st != PS_UP) {
        return;
    }
    if (st == PS_UP && !up_running) {
        return;                              /* 正计时暂停/未开始 */
    }
    if (st == PS_CD && cd_paused) {
        return;                              /* 倒计时暂停 */
    }
    ms_acc += dt_ms;
    while (ms_acc >= 1000) {
        ms_acc -= 1000;
        if (st == PS_UP) {
            elapsed_sec++;
        } else {
            remain_sec--;
        }
        redraw = 1;

        if (st != PS_UP && remain_sec <= 0) {
            if (st == PS_RUN) {
                done_rounds++;
                if (done_rounds >= total_rounds) {
                    st = PS_DONE;
                    render_done("ALL DONE!");
                    printf("[EVENT] POMO_DONE\r\n");
                    return;
                }
                begin_rest();
            } else if (st == PS_REST) {
                begin_focus();
            } else {
                st = PS_CDDONE;
                render_done("TIME UP!");
                printf("[EVENT] CD_DONE\r\n");
            }
            return;
        }
    }
    /* 每秒重绘一次（整屏刷新，1Hz 下 QSPI 带宽完全够）*/
    if (redraw) {
        if (st == PS_UP) {
            render_up();
        } else {
            render_timer(st == PS_REST ? "REST"
                         : (st == PS_CD ? "COUNTDOWN" : "FOCUS"));
        }
    }
}
