/******************************************************************************
 * @file     ui_app.c
 * @brief    包子屏幕 UI 状态机实现（见 ui_app.h）
 *
 * 状态：ST_FACE（表情态，默认，眨眼） -> ST_CARDS（卡片层，三张卡横滑）
 *       -> 番茄钟流程（pomo.c 接管，运行中锁定不回落）
 * 进入：表情态长按 800ms → 卡片层；卡片层 TIMER 页点击 → 番茄钟
 * 退出：卡片层 15 秒无操作自动回落表情态；番茄钟由自身流程退出
 *       （刻意不做"卡片层长按退出"——长按易误触，用户明确要求去掉）
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include "ui_app.h"
#include "ui.h"
#include "pomo.h"
#include "habit_store.h"     /* HABIT 卡片：习惯缓存 + 渲染 + 命中 */
#include "sched_store.h"     /* TODAY 卡片：日程缓存 + 渲染 + 命中 */
#include "play_card.h"       /* 第 4 页：动作 / 情绪按钮 */
#include "qr/net_card.h"     /* 第 5 页：配网二维码（QR:<payload> / NET:<state>）*/
#include "behavior.h"        /* 动作请求 + 情绪刺激（不直接写舵机/改情绪）*/
#include "emotion/emotion_manager.h"   /* EMO_STIM_PET / EMO_STIM_TOUCH */
#include "baozi_hooks.h"      /* BAOZI_ReportHabitTap（上报打卡，不直接依赖协议层）*/

/* uart_hal 提供：串口静默计时（重绘避开数据下发窗口，见 tick 里 dirty 刷新）*/
extern void     uart_hal_tick(uint16_t dt_ms);
extern uint16_t uart_hal_rx_quiet_ms(void);

/* face_app 表情层接口（face_app.c 提供）*/
extern void face_app_init(void);
extern void face_app_set_emotion(const char *name);
extern void face_app_tick(uint32_t dt_ms);
/* 临时表情层（anim/face_app.c）：播 ms 毫秒后自动回落当前情绪。
 * 抚摸用它，是为了让表情不被 emotion_fsm 每 10ms 的情绪同步顶掉。 */
extern int  face_app_play(const char *name, uint32_t ms);
extern void face_app_hold_end(void);

/* 前置声明：remind_close（定义在前）恢复界面时要用到它 */
static void cards_show_gated(int page);
/* 前置声明：ui_app_show_net()（配网跳转）要在抚摸状态机之前调它 */
static void pet_finish(void);
/* app_core 的触摸状态：跳页前要确认手指没按着 */
extern int app_core_touch_down(void);

typedef enum {
    ST_FACE = 0,     /* 表情态 */
    ST_CARDS         /* 卡片层 */
} ui_state_t;

static ui_state_t g_state     = ST_FACE;
static int        g_card_page = 0;        /* 0..2 */
static uint8_t    g_inited    = 0;        /* ui_app_init() 是否已跑过（face_ui 桥接用）*/

/* 长按判定（仅表情态生效）*/
static uint8_t  g_pressing = 0;
static uint32_t g_press_ms = 0;
static uint8_t  g_lp_fired = 0;
#define LONGPRESS_MS   800

/* 卡片层无操作回落 */
static uint32_t g_idle_ms  = 0;
#define CARD_IDLE_BACK_MS   15000

/* 数据落板后的"脏"标记：不在 UART 帧流里直接刷屏，等串口静默后由 tick 补刷。
 * （直接在 0x13/0x16 END 里刷屏 = 整批数据还在收尾巴时就开始 100ms 的
 *  整屏 DMA，是实机花屏的高发窗口。）*/
static uint8_t g_cards_dirty = 0;
static int     g_pending_page = -1;   /* 标脏时记下待刷的页（-1 = 用 g_card_page）*/

/* ---- v1.8 提醒弹卡（PC 端 0x17 下发；时机判定在 PC，板子只展示）---- */
#define REMIND_TITLE_MAX      7
#define REMIND_BMP_MAX        (REMIND_TITLE_MAX * 2 * 16)  /* 7字×2B/行×16行 */
#define REMIND_AUTO_CLOSE_MS  60000

static uint8_t  g_remind_on      = 0;
static uint8_t  g_remind_dirty   = 0;
static uint8_t  g_remind_defer  = 0;   /* 番茄钟运行中先扣下，结束再弹 */
static uint8_t  g_remind_type    = 0;   /* 0=习惯 1=日程 */
static uint8_t  g_remind_hh      = 0;
static uint8_t  g_remind_mm      = 0;
static uint8_t  g_remind_chars   = 0;
static uint8_t  g_remind_w       = 16;
static uint8_t  g_remind_h       = 16;
static uint8_t  g_remind_bmp[REMIND_BMP_MAX];
static uint32_t g_remind_ms      = 0;
static uint8_t  g_remind_prev_cards = 0; /* 弹卡前在卡片层吗（关闭时恢复用）*/
static int      g_remind_prev_page  = 0;

void ui_app_show_remind(uint8_t type, uint8_t hh, uint8_t mm,
                        const uint8_t *bmp, uint8_t chars, uint8_t w, uint8_t h)
{
    int i, need;

    if (chars > REMIND_TITLE_MAX || w < 8 || (w & 7) || h == 0) {
        return;                        /* 协议层已挡一次，这里兜底 */
    }
    need = (int)chars * (w / 8) * h;
    if (need > REMIND_BMP_MAX) {
        return;
    }
    for (i = 0; i < need; i++) {
        g_remind_bmp[i] = bmp[i];
    }
    g_remind_type  = type;
    g_remind_hh    = hh;
    g_remind_mm    = mm;
    g_remind_chars = chars;
    g_remind_w     = w;
    g_remind_h     = h;

    /* 番茄钟计时中不打断（声音 PC 已经出了）：先扣下，pomo 结束后再弹 */
    if (pomo_active()) {
        g_remind_defer = 1;
        return;
    }
    g_remind_prev_cards = (g_state == ST_CARDS);
    g_remind_prev_page  = g_card_page;
    g_remind_defer = 0;
    g_remind_on    = 1;
    g_remind_dirty = 1;
    g_remind_ms    = 0;
}

static void remind_draw(void)
{
    ui_clear();
    ui_set_bg_gradient(C_BG_TOP, C_BG_MID, C_BG_BOT);

    ui_add_round_fill(60, 182, 360, 200, 28, C_CARD_SHAD);
    ui_add_round_fill(60, 174, 360, 200, 28, C_CARD_TOP);
    ui_add_round_rect(60, 174, 360, 200, 28, 3, C_CARD_LINE);

    /* 标题英文小字（板载只有 ASCII 字库，中文全走点阵）*/
    {
        int w = ui_text_width("REMINDER", 1);
        ui_add_text((int16_t)(240 - w / 2), 194, "REMINDER", 1, UI_COL_SUB);
    }

    /* 时间大字（与卡片页顶部时钟同款 scale 2）*/
    {
        static char tbuf[6];           /* static：ui_add_text 只存指针 */
        int w;
        tbuf[0] = (char)('0' + g_remind_hh / 10);
        tbuf[1] = (char)('0' + g_remind_hh % 10);
        tbuf[2] = ':';
        tbuf[3] = (char)('0' + g_remind_mm / 10);
        tbuf[4] = (char)('0' + g_remind_mm % 10);
        tbuf[5] = '\0';
        w = ui_text_width(tbuf, 2);
        ui_add_text((int16_t)(240 - w / 2), 220, tbuf, 2, C_TITLE);
    }

    /* 中文标题点阵，2x 放大成 32px 大字（与日程页标题同款）*/
    if (g_remind_chars > 0 && g_remind_w >= 8) {
        int w = g_remind_chars * g_remind_w;
        ui_add_bitmap2((int16_t)(240 - w), 284,
                       (int16_t)w, (int16_t)g_remind_h, g_remind_bmp,
                       (int16_t)(g_remind_chars * (g_remind_w / 8)),
                       UI_COL_TEXT, 2);
    }

    {
        int w = ui_text_width("tap to close", 1);
        ui_add_text((int16_t)(240 - w / 2), 336, "tap to close", 1, UI_COL_SUB);
    }

    ui_flush(UI_COL_BG);
}

static void remind_close(int reason)
{
    uint8_t type = g_remind_type;

    g_remind_on    = 0;
    g_remind_dirty = 0;
    g_remind_defer = 0;
    BAOZI_ReportRemindClosed(type, (uint8_t)reason);

    /* 恢复弹卡前的界面 */
    if (g_remind_prev_cards && g_state == ST_CARDS) {
        cards_show_gated(g_remind_prev_page);
    } else if (g_state == ST_CARDS) {
        cards_show_gated(g_card_page);
    } else {
        face_app_set_emotion("neutral");   /* 回表情态：重画当前脸 */
    }
}

void ui_app_cards_dirty(void)
{
    g_cards_dirty = 1;
    g_pending_page = -1;               /* 默认刷当前页（数据下发补刷场景）*/
}

/* 表情态该显示哪个表情（由 emotion_fsm 的 face_ui_* 维护）。
 * 卡片层/计时流程中只记录不显示 —— 用户正在看卡片，抢屏等于把他拽走。 */
static const char *g_face_emo = "neutral";

void ui_app_set_emotion(const char *emo)
{
    if (emo == 0) {
        return;
    }
    g_face_emo = emo;                     /* 指针必须是静态/常量串（见文件头）*/
    if (g_state == ST_FACE) {
        face_app_set_emotion(emo);
    }
}

static void back_to_face(void)
{
    g_state    = ST_FACE;
    g_pressing = 0;
    g_idle_ms  = 0;
    face_app_set_emotion(g_face_emo);      /* 回表情态时恢复"当前情绪"而非固定 neutral */
}

/* 串口/上层主动要求表情：无论当前在哪一层，都回到表情态并切换。
 * 为什么要它：如果只直接调 face_app_set_emotion()，那么在卡片层或番茄钟里
 * 收到的情绪命令会被卡片/计时界面盖住 —— 用户根本看不到变脸。
 * 所以统一走这里：先清计时流程 → 回落表情态 → 再切表情。 */
void ui_app_force_emotion(const char *emo)
{
    pomo_reset();
    g_state    = ST_FACE;
    g_pressing = 0;
    g_lp_fired = 0;
    g_idle_ms  = 0;
    if (emo != 0) {
        g_face_emo = emo;
    }
    face_app_set_emotion(g_face_emo);
}

int ui_app_ready(void)
{
    return (g_inited != 0) ? 1 : 0;
}

/* 把屏幕拉到 NET 页（配网二维码）。
 * 谁调：src/net_link.c —— 收到"热点已开 + 二维码"时自动跳过去，
 * 否则用户得自己长按进卡片层再滑三页，实机上没人猜得到。
 * 三处硬约束：
 *   · 番茄钟计时中不抢屏（那是用户主动开的、且有倒计时语义）；
 *   · 提醒弹卡显示中不抢（弹卡自己会回原界面）；
 *   · 手指正按着不抢（会在手指底下换页，体验极差）。 */
void ui_app_show_net(void)
{
    if (!g_inited) {
        return;
    }
    if (pomo_active() || g_remind_on || app_core_touch_down()) {
        return;
    }
    pet_finish();                 /* 抚摸态别带进卡片层 */
    g_state    = ST_CARDS;
    g_pressing = 0;
    g_lp_fired = 0;
    g_idle_ms  = 0;
    cards_show_gated(4);
}

/* ==================== v2.0 抚摸手势（"在脸上来回摸"）====================
 *
 * 需求（用户 2026-11）：在屏幕的特定区域连续滑动 → 进入"被抚摸"状态。
 *
 * 实现（只靠 ICNT8952BN 给的 DOWN / MOVE(7) / UP 事件，不依赖压力或多点）：
 *   · 区域：表情态下屏幕上半部（y < PET_ZONE_Y）。表情态时这块正好是脸/头顶，
 *     手指来回蹭这里 = 摸它。卡片层不做抚摸判定（那里左右滑是翻页）。
 *   · 判定：按住期间水平方向每**反转**一次计数 +1（每次有效位移 >= PET_MIN_STEP），
 *     累计 PET_REVERSALS 次 → 进入抚摸态。
 *   · 维持：抚摸态里只要还在动，每 PET_REPEAT_MS 继续给一次（较轻的）刺激，
 *     形成"越摸越开心"；同时把长按计时清零（摸不是长按，别把卡片层呼出来）。
 *   · 结束：抬手 / 手指移出区域 / 停住超过 PET_IDLE_MS → 退出，反转计数清零，
 *     表情回到"当前情绪"那张脸。
 *
 * 情绪怎么变：这里**不直接改情绪**，只发 behavior_stimulus(EMO_STIM_PET)，
 * 由 emotion_manager 的连续状态攒够愉悦度后自然漂移到 loving/happy。
 */
#define PET_ZONE_Y        300       /* 抚摸区域上界：y < 300 */
#define PET_MIN_STEP      14        /* 单次有效位移（px）*/
#define PET_REVERSALS     4         /* 需要几次方向反转才认定"在摸"*/
#define PET_IDLE_MS       600       /* 停多久算摸完（ms）*/
#define PET_REPEAT_MS     900       /* 抚摸态里的刺激间隔（ms）*/
#define PET_STIM_ONCE     45        /* 首次进入的刺激强度（1..100）*/
#define PET_STIM_REPEAT   25        /* 持续抚摸的刺激强度 */
#define PET_FACE_MS       2500      /* 抚摸表情的续期时长（每次有效移动刷新）*/

static uint8_t  g_pet_active = 0;   /* 1 = 已进入抚摸态 */
static uint8_t  g_pet_rev    = 0;   /* 方向反转计数 */
static int8_t   g_pet_dir    = 0;   /* 上一个有效水平方向：-1 左 / +1 右 */
static int16_t  g_pet_anchor = 0;   /* 当前方向的参考点 x */
static uint32_t g_pet_idle   = 0;   /* 距上一次有效移动的毫秒数 */
static uint32_t g_pet_stim   = 0;   /* 距上一次刺激的毫秒数 */

static void pet_reset(void)
{
    g_pet_active = 0;
    g_pet_rev    = 0;
    g_pet_dir    = 0;
    g_pet_idle   = 0;
    g_pet_stim   = 0;
}

/* UP / 离开区域 / 切入卡片层：结束抚摸，表情回到当前情绪 */
static void pet_finish(void)
{
    if (g_pet_active) {
        printf("[UI] PET end\r\n");
    }
    pet_reset();
    face_app_hold_end();            /* 先撤临时表情，再回当前情绪 */
    ui_app_set_emotion(g_face_emo);
}

/* 按住移动（GESTURE_MOVE）：更新反转计数，够了就进入/维持抚摸态 */
static void pet_on_move(uint16_t x, uint16_t y)
{
    int16_t dx;

    if (g_state != ST_FACE) {
        pet_reset();
        return;
    }
    if (y >= PET_ZONE_Y) {
        pet_finish();               /* 手指滑出抚摸区：这一轮摸完了 */
        return;
    }

    /* 摸的时候不是长按：把长按计时清掉，否则 800ms 后会被拽进卡片层 */
    g_pressing = 0;

    dx = (int16_t)x - g_pet_anchor;
    if ((dx >= PET_MIN_STEP) || (dx <= -PET_MIN_STEP)) {
        int8_t dir = (dx > 0) ? 1 : -1;

        if ((g_pet_dir != 0) && (dir != g_pet_dir)) {
            if (g_pet_rev < 200) {
                g_pet_rev++;
            }
        }
        g_pet_dir    = dir;
        g_pet_anchor = (int16_t)x;
        g_pet_idle   = 0;

        if (g_pet_rev >= PET_REVERSALS) {
            if (!g_pet_active) {
                g_pet_active = 1;
                g_pet_stim   = 0;
                printf("[UI] PET start (rev=%u)\r\n", (unsigned)g_pet_rev);
                ui_app_set_emotion("loving");          /* 摸到位了：先给张撒娇脸 */
                face_app_play("pet", PET_FACE_MS);     /* 临时层：2.5s 内不被情绪同步顶掉 */
                behavior_stimulus(EMO_STIM_PET, PET_STIM_ONCE);
            }
        }
    }
}

/* 每 tick 推进：判定停手超时 + 抚摸态里的周期刺激 */
static void pet_tick(uint32_t dt_ms)
{
    if (g_pet_rev == 0 && !g_pet_active) {
        return;
    }
    g_pet_idle += dt_ms;
    if (g_pet_idle >= PET_IDLE_MS) {
        pet_finish();               /* 停住了：摸完了 */
        return;
    }
    if (g_pet_active) {
        g_pet_stim += dt_ms;
        if (g_pet_stim >= PET_REPEAT_MS) {
            g_pet_stim = 0;
            face_app_play("pet", PET_FACE_MS);         /* 还在摸：续期 */
            behavior_stimulus(EMO_STIM_PET, PET_STIM_REPEAT);
        }
    }
}

/* 画指定卡片页：拟物可爱风（渐变底 + 顶部时间 + 页码圆点 + 圆角卡片）*/
void ui_cards_show(int page)
{
    int w;

    g_card_page = page;

    /* 第 2 页 = 计时功能三入口（pomo 接管渲染）*/
    if (page == 1) {
        pomo_enter_mode();
        return;
    }

    /* 第 5 页 = 配网二维码。全屏档自己铺满整屏（白底利于对焦），
     * 不走下面这套"时间 + 圆点 + 卡片框"的布局。 */
    if (page == 4 && net_card_full()) {
        ui_clear();
        net_card_draw_full();
        ui_flush_begin(UI_COL_BG);
        return;
    }

    ui_clear();
    ui_set_bg_gradient(C_BG_TOP, C_BG_MID, C_BG_BOT);

    /* 顶部当前时间：由 PC 对时下发（0x10），未对时显示 --:--。
     * tbuf 必须是 static —— ui_add_text 只存指针，取字形发生在 ui_flush。 */
    {
        static char tbuf[6];
        habit_clock_hhmm(tbuf);
        w = ui_text_width(tbuf, 2);
        ui_add_text((int16_t)((480 - w) / 2), 26, tbuf, 2, C_TITLE);
    }

    /* 页码圆点（v2.1 起 5 页：TODAY / TIMER / HABIT / PLAY / NET）*/
    {
        int i, cy = 92, cx0 = 240 - 44;
        for (i = 0; i < 5; i++) {
            ui_add_circle((int16_t)(cx0 + i * 22), (int16_t)cy, 6, 6, 1,
                          (i == page) ? C_DOT_ON : C_DOT_OFF);
        }
    }

    /* 圆角卡片（带厚度阴影）*/
    ui_add_round_fill(60, 130, 360, 290, 28, C_CARD_SHAD);
    ui_add_round_fill(60, 122, 360, 290, 28, C_CARD_TOP);
    ui_add_round_rect(60, 122, 360, 290, 28, 3, C_CARD_LINE);

    if (page == 2) {
        /* HABIT 页：习惯看板（名字点阵 + 状态圆 + 进度 + 连续天数）*/
        habit_card_draw();
    } else if (page == 3) {
        /* PLAY 页（v2.0）：动作 / 情绪按钮（只发请求与刺激，不直接写舵机）*/
        play_card_draw();
    } else if (page == 4) {
        /* NET 页（v2.1）：二维码 + 配网状态；点一下放大成全屏码 */
        net_card_draw_card();
    } else {
        /* TODAY 页：日程看板（日期组头 + 时间 + 标题点阵 + 完成圆）*/
        sched_card_draw();
    }

    /* 分片刷屏：这里只"开始"，主循环每轮刷 8 行。
     * 以前这里是 ui_flush()，一次 100~200ms 不返回，这期间触摸 IRQ 只能
     * 置个标志干等 —— 点格子"没反应 / 连点几个最后一起变"就是这么来的。 */
    ui_flush_begin(UI_COL_BG);
}

/* 数据变了要刷新卡片层：只在自己人会被打断的"卡片层"里重绘。
 * 注意排除番茄钟运行中（pomo_active）—— 那时候抢屏会毁掉计时界面。 */
void ui_app_refresh_cards(void)
{
    if (g_state != ST_CARDS) {
        return;                       /* 表情态：不打断用户 */
    }
    if (pomo_active()) {
        return;                       /* 番茄钟流程中：别插一脚 */
    }
    if (g_card_page != 0 && g_card_page != 2) {
        return;                       /* 其他页不需要习惯数据 */
    }
    ui_cards_show(g_card_page);
}

/* 交互触发的刷屏门控：串口已静默就直接刷；串口正活跃（刚点打卡/催数据，
 * PC 马上要回一长串）就只标脏，等 tick 在串口静默 150ms 后再补刷。
 * 为什么要统一走这里 —— 点击触发的 ui_cards_show 以前是直接刷屏，正好撞上
 * PC 回数据那几帧的 RX 中断，是"点一下就一条色带"的高发窗口（实机已复现）。 */
static void cards_show_gated(int page)
{
    if (uart_hal_rx_quiet_ms() >= 150) {
        ui_cards_show(page);
    } else {
        g_cards_dirty   = 1;
        g_pending_page  = page;
    }
}

/* HABIT 页点击：交给 habit_store 统一判定（列表进详情 / 详情切格子）
 * 返回"要上报"时才发 0x90；返回"只是界面变化"时直接重绘。 */
static void habit_on_tap(int x, int y)
{
    int idx = -1, slot = -1, action = 0;
    int r;

    if (habit_store_count() <= 0) {
        /* 还没收到习惯数据。以前这里是直接 return，实机上表现成
         * "点了没反应"；现在点卡片 = 催 PC 重发（上行 0x92），
         * 并且屏幕立刻换成 "requesting..." 给个反馈。 */
        if (habit_card_contains(x, y)) {
            BAOZI_RequestHabits();
            habit_store_mark_sync_req();
            ui_cards_show(2);
        }
        return;
    }
    r = habit_card_tap(x, y, &idx, &slot, &action);
    if (r == HABIT_TAP_NONE) {
        return;                       /* 没点在任何可点区域上 */
    }
    if (r == HABIT_TAP_REPORT) {
        BAOZI_ReportHabitTap((uint8_t)idx, (uint8_t)slot, (uint8_t)action);
    }

    /* 【用户的点击必须立刻画出来，不能走 cards_show_gated】
     *
     * 实机故障（用户反馈）：点格子有时不变色；连着点好几下，这几下又
     * 同时变色。根因就在这里 ——
     *   habit_store_apply_local() 已经立刻改了内存里的 mask/done/state，
     *   但 cards_show_gated 一看串口正忙就只标脏不画。于是：
     *     · 点一下 → 状态变了、屏幕没变 → 用户以为没点上；
     *     · 连点几下 → 每次状态都真的变了，攒到串口静默才一次性画出来
     *       → 看起来"同时触发"。
     * 而串口为什么忙？因为 PC 收到 0x90 后会立刻回一整批数据校正 ——
     * 那个"立刻"已经在 PC 端改成延迟 1.2 秒（见 baozi-link.py 的 0x90
     * 分支），专门给这里腾出一段干净的刷屏窗口。
     *
     * 所以：点击路径直接刷；数据落板（BAOZI_HabitEnd）那条路径继续走
     * cards_show_gated —— 那才是"整批 0x11~0x13 还在收尾巴"的高危窗口。 */
    ui_cards_show(2);
}

/* TODAY 页点击：点事件行 = 切换完成状态（上报 0x93）；空态 = 催数据 */
static void sched_on_tap(int x, int y)
{
    int idx = -1, action = 0;
    int r;

    r = sched_card_tap(x, y, &idx, &action);
    if (r == SCHED_TAP_NONE) {
        return;
    }
    if (r == SCHED_TAP_SYNC) {
        BAOZI_RequestSched();
        sched_store_mark_sync_req();
        ui_cards_show(0);                 /* 空态催数据：立刻给出 requesting 反馈 */
        return;
    }
    if (r == SCHED_TAP_REPORT) {
        BAOZI_ReportSchedTap((uint8_t)idx, (uint8_t)action);
    }
    ui_cards_show(0);                     /* 圆圈翻转：立刻画出来（理由同 habit_on_tap）*/
}

void ui_app_init(void)
{
    printf("[UI] ui_app v3 (pomodoro)\r\n");
    face_app_init();     /* 屏初始化 + 显示 neutral */
    g_inited = 1;
}

void ui_app_handle_gesture(int g, uint16_t x, uint16_t y)
{
    /* 提醒弹卡最优先：显示中吞掉所有手势，点一下 = 关闭回原界面 */
    if (g_remind_on) {
        if (g == 2) {                    /* UP = 点击 */
            remind_close(0);
        }
        return;
    }

    /* 计时深层界面（非 MODE 页）：锁定，只响应点击 */
    if (pomo_active() && !pomo_at_mode()) {
        if (g == 2) {                    /* UP = 点击 */
            pomo_on_tap((int)x, (int)y);
        }
        return;
    }

    g_idle_ms = 0;       /* 任何触摸都算"有操作" */

    switch (g) {
    case 1: /* DOWN：开始计时长按 */
        if (!g_pressing) {
            g_pressing = 1;
            g_press_ms = 0;
            g_lp_fired = 0;
        }
        break;

    case 2: /* UP（点击）*/
        g_pressing = 0;
        pet_finish();                    /* 抬手：这一轮抚摸结束（没摸过则无副作用）*/
        if (g_state == ST_CARDS) {
            if (g_card_page == 1) {
                pomo_on_tap((int)x, (int)y);      /* 计时三入口页 */
            } else if (g_card_page == 2) {
                habit_on_tap((int)x, (int)y);     /* 习惯页：点行打卡/取消 */
            } else if (g_card_page == 3) {
                if (!play_card_tap((int)x, (int)y)) {
                    g_idle_ms = 0;               /* 没点到按钮也算有操作 */
                }
            } else if (g_card_page == 4) {
                /* NET 页：点一下在"卡片内小码 ↔ 全屏大码"之间切换。
                 * 二维码要凑近扫，全屏那档模块能从 6px 放大到 12px 以上。 */
                if (net_card_tap((int)x, (int)y)) {
                    ui_cards_show(4);
                }
            } else {
                sched_on_tap((int)x, (int)y);     /* TODAY 页：点行切换完成 */
            }
        }
        break;

    /* ★ v2.0：按住中的位置更新（ICNT8952BN 的 GESTURE_MOVE）——
     *   只在表情态用：屏幕上半部来回摸 = 被抚摸（见上方 pet_* 说明）。
     *   卡片层不参与，避免与"左右滑翻页"抢手势。 */
    case 7:
        pet_on_move(x, y);
        break;

    case 3: /* 左滑：详情页先退详情，否则卡片层向后翻页 */
        if (g_state == ST_CARDS && habit_detail_active()) {
            habit_detail_close();
            cards_show_gated(2);
        } else if (g_state == ST_CARDS && g_card_page < 4) {
            cards_show_gated(g_card_page + 1);
        }
        g_pressing = 0;
        break;

    case 4: /* 右滑：详情页先退详情，否则卡片层向前翻页 */
        if (g_state == ST_CARDS && habit_detail_active()) {
            habit_detail_close();
            cards_show_gated(2);
        } else if (g_state == ST_CARDS && g_card_page > 0) {
            cards_show_gated(g_card_page - 1);
        }
        g_pressing = 0;
        break;

    case 5: /* 上滑（手指向上）：列表向下滚动（习惯页 / TODAY 页）*/
        if (g_state == ST_CARDS && !habit_detail_active()) {
            if (g_card_page == 2) {
                habit_store_scroll(1);
                cards_show_gated(2);
            } else if (g_card_page == 0) {
                sched_store_scroll(1);
                cards_show_gated(0);
            }
        }
        g_pressing = 0;
        break;

    case 6: /* 下滑（手指向下）：列表向上滚动 */
        if (g_state == ST_CARDS && !habit_detail_active()) {
            if (g_card_page == 2) {
                habit_store_scroll(-1);
                cards_show_gated(2);
            } else if (g_card_page == 0) {
                sched_store_scroll(-1);
                cards_show_gated(0);
            }
        }
        g_pressing = 0;
        break;

    default:
        break;
    }
}

void ui_app_tick(uint32_t dt_ms)
{
    /* 时钟永远先走：下面几个分支都有早返回，放在后面会漏计时
     * （典型后果：番茄钟跑 25 分钟，屏幕上的时间少走 25 分钟）。
     * dt_ms 现在是 main 循环用 csi_tick_get()（SysTick，1 tick=1ms）量的
     * 【真实】耗时，不再是写死的 20 —— 刷屏耗 100ms 的那轮，时钟也走 100ms。 */
    habit_clock_tick(dt_ms);
    uart_hal_tick((uint16_t)dt_ms);

    /* 空态"requesting..."提示到期 → 重绘一次换回待机文案
     * （提醒弹卡显示中别抢屏）*/
    if (!g_remind_on && (habit_store_tick(dt_ms) || sched_store_tick(dt_ms))) {
        ui_app_refresh_cards();
    }

    /* NET 页内容变了（新二维码 / 状态推进）：只在用户正好停在那页时补刷。
     * 其它情况下 net_link 会显式调 ui_app_show_net() 把人拉过去。 */
    if (net_card_take_dirty()) {
        if (g_state == ST_CARDS && g_card_page == 4 && !pomo_active()) {
            cards_show_gated(4);
        }
    }

    /* 提醒弹卡：番茄钟结束后补弹 → 串口静默后上屏 → 60s 自动关闭。
     * 显示期间 return：不推进卡片回落/表情/自愈刷（弹卡本身盖住一切）。 */
    if (g_remind_defer && !pomo_active()) {
        g_remind_defer      = 0;
        g_remind_on         = 1;
        g_remind_dirty      = 1;
        g_remind_ms         = 0;
        g_remind_prev_cards = (g_state == ST_CARDS);
        g_remind_prev_page  = g_card_page;
    }
    if (g_remind_on) {
        g_remind_ms += dt_ms;
        if (g_remind_ms >= REMIND_AUTO_CLOSE_MS) {
            remind_close(1);             /* 超时自动关闭 */
        } else if (g_remind_dirty && uart_hal_rx_quiet_ms() >= 150) {
            g_remind_dirty = 0;
            remind_draw();               /* 避开 UART 中发刷屏（同 dirty 门控）*/
        }
        return;
    }

    /* 计时深层界面：推进计时，锁定其他一切 */
    if (pomo_active() && !pomo_at_mode()) {
        pomo_tick(dt_ms);
        return;
    }

    /* ★ v2.0 抚摸手势推进（停手超时 → 结束；抚摸态里按间隔给刺激）。
     *   放在 pomo 早返回之后：计时界面里不做抚摸（免得和计时抢屏幕）。 */
    pet_tick(dt_ms);

    /* 【数据落板/交互后的补刷】END 帧或点击只标脏；这里等串口静默 150ms
     * （整批数据收完了）再整屏重绘。不要在 UART 中断每 87µs 一发的
     * 窗口里做 100ms 的 DMA 刷屏 —— 那是实机花屏（色带）的高发窗口。
     * g_pending_page 是点击/滑动翻页时记下的目标页；数据下发补刷则用当前页。 */
    if (g_cards_dirty) {
        if (g_state == ST_CARDS && !pomo_active() &&
            uart_hal_rx_quiet_ms() >= 150) {
            int target = (g_pending_page >= 0) ? g_pending_page : g_card_page;
            g_cards_dirty  = 0;
            g_pending_page = -1;
            ui_cards_show(target);
        } else if (g_state != ST_CARDS) {
            g_cards_dirty  = 0;        /* 在表情态：数据已落板，不用刷 */
            g_pending_page = -1;
        }
    }

    /* 【花屏自愈】卡片层每 4s 完整重刷一次。两个作用：
     * ① 顶部时钟跟着走（不然停在进入卡片那一刻）；
     * ② 实机偶发"绿紫色带"——某次刷屏途中 QSPI 数据流错了一个字节，
     *    错位会一直保留到下次完整刷屏。定时重刷让花屏几秒内自愈。
     * 同样避开数据下发窗口。 */
    if (g_state == ST_CARDS && !pomo_active() &&
        (g_card_page == 0 || g_card_page == 2 || g_card_page == 3 ||
         g_card_page == 4) &&
        uart_hal_rx_quiet_ms() >= 150) {
        static uint16_t s_repaint = 0;
        s_repaint = (uint16_t)(s_repaint + dt_ms);
        if (s_repaint >= 4000) {
            s_repaint = 0;
            ui_cards_show(g_card_page);
        }
    }

    /* 长按计时：仅表情态下，按住未滑动超阈值 → 进入卡片层 */
    if (g_pressing && g_state == ST_FACE) {
        g_press_ms += dt_ms;
        if (!g_lp_fired && g_press_ms >= LONGPRESS_MS) {
            g_lp_fired = 1;
            g_pressing = 0;
            pet_finish();            /* 抚摸态别带进卡片层 */
            g_state    = ST_CARDS;
            ui_cards_show(0);
        }
    }

    /* 卡片层（含 MODE 三入口页）：15 秒无操作自动回落表情态。
     * NET 页例外 —— 二维码是给手机扫的，用户可能举着手机对半天，
     * 这时候把屏切回笑脸等于把码收走了。 */
    if (g_state == ST_CARDS) {
        if (g_card_page != 4) {
            g_idle_ms += dt_ms;
            if (g_idle_ms >= CARD_IDLE_BACK_MS) {
                pomo_reset();            /* 清理可能残留的计时流程 */
                back_to_face();
            }
        } else {
            g_idle_ms = 0;
        }
    }

    /* 表情态：驱动眨眼 + 视线 */
    if (g_state == ST_FACE) {
        face_app_tick(dt_ms);
    }
}
