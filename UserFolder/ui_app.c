/******************************************************************************
 * @file     ui_app.c
 * @brief    包子屏幕 UI 状态机实现（见 ui_app.h）
 *
 * 状态：ST_FACE（表情态，默认，眨眼） -> ST_CARDS（卡片层，七页横滑）
 *       -> 番茄钟流程（pomo.c 接管，运行中锁定不回落）
 * 进入：表情态【同时按住两只眼睛约 0.55s】→ 卡片层。
 *       以前是"任意位置长按 800ms"，但脸上现在有脸颊滑动/额头连点两套手势，
 *       长按落点几乎总在别人的地盘上 —— 误触进 UI 就成了常态。
 *       眼睛是唯一"点上去什么也不发生"的区域，两只手同时压住更是明确意图。
 * 退出（两条，任何一条都会立刻回表情态）：
 *   ① 右上角圆形 X —— 七页统一，位置固定（页码圆点那一行的右端）
 *   ② 屏幕顶部往下拖 —— 起点 y < 110 才算，卡片内下滑仍然是列表滚动
 *   （2026-10-31 起删掉了"15 秒无操作自动回落"：看列表会被自己收走，而且回落
 *     那一瞬手指若还压在眼睛上，550ms 后又被双指长按拉回来 —— 屏幕来回闪。）
 * 计时深层页（PSET/RUN/REST/QUIT/DONE/UP/CDSET/CD）不吃 ①②：
 *   那里手势被整体锁死，只响应点击，退出走自己的 EXIT / GIVE UP。
 *
 * 页面骨架统一由 ui_cards_chrome() 画（背景 / 顶部时钟 / 5 个页码圆点 /
 * 关闭按钮 / 圆角卡片框），第 2 页的 pomo MODE 页也走它 —— 否则会出现
 * "同一层里两套版式"，用户第一反应是"这一页坏了"。
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ui_app.h"
#include "ui.h"
#include "pomo.h"
#include "habit_store.h"     /* HABIT 卡片：习惯缓存 + 渲染 + 命中 */
#include "sched_store.h"     /* TODAY 卡片：日程缓存 + 渲染 + 命中 */
#include "play_card.h"       /* 第 4 页：动作 / 情绪按钮 */
#include "face_picker.h"     /* 第 5 页：表情选择（浏览 + 播放全部表情）*/
#include "anim/face_model.h" /* face_model_get：FACES 页点名字取姿势 */
#include "qr/net_card.h"     /* 第 6 页：配网二维码（QR:<payload> / NET:<state>）*/
#include "behavior.h"        /* 动作请求 + 情绪刺激（不直接写舵机/改情绪）*/
#include "movements/moves.h" /* move_id_t：FACES 页点表情联动身体动作 */
#include "emotion/emotion_manager.h"   /* EMO_STIM_PET / EMO_STIM_TOUCH */
#include "baozi_hooks.h"      /* BAOZI_ReportHabitTap（上报打卡，不直接依赖协议层）*/
#include "voice_link.h"      /* voice_say：手势进情绪后发一行 VOICE: 给 PC → ESP32 播语音 */
#include "demo/demo_show.h"  /* 第 7 页 DEMO：一键演示（屏幕 + 舵机整套跑一遍）*/

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
/* 强制整屏重画当前脸：卡片层/弹卡退出时必须调（apply_pose 对同名姿势不重绘，
 * 不调的话屏幕会一直停在卡片那一屏，直到下一次眨眼才闪回脸）。 */
extern void face_app_repaint(void);
/* 当前这一帧在视觉坐标 (x,y) 处有没有画东西。脸部手势用它区分
 * "脸颊/额头的空白区" 与 "眼睛/嘴"（见下方 face_zone）。 */
extern int  face_app_hit_test(int x, int y);

/* 前置声明：remind_close（定义在前）恢复界面时要用到它 */
static void cards_show_gated(int page);
/* 前置声明：ui_app_show_net()（配网跳转）要在抚摸状态机之前调它 */
static void face_gesture_reset(void);
/* 前置声明：back_to_face()/cards_close() 在定义之前就要清双指长按状态 */
static void eye_hold_reset(void);
/* app_core 的触摸状态：跳页前要确认手指没按着 */
extern int app_core_touch_down(void);
/* 触摸驱动的触点缓存（src/ICNT8952BN.c）：返回最近一帧的触点数（0/1/2）。
 * 双指长按两只眼睛必须知道"有几根手指"，单点手势码给不了这个信息。 */
extern int tp_get_points(uint16_t *xs, uint16_t *ys);

typedef enum {
    ST_FACE = 0,     /* 表情态 */
    ST_CARDS         /* 卡片层 */
} ui_state_t;

static ui_state_t g_state     = ST_FACE;
static int        g_card_page = 0;        /* 0..2 */
static uint8_t    g_inited    = 0;        /* ui_app_init() 是否已跑过（face_ui 桥接用）*/

/* 表情态该显示哪个表情（由 emotion_fsm 的 face_ui_* 维护）。
 * 卡片层/计时流程中只记录不显示 —— 用户正在看卡片，抢屏等于把他拽走。
 * 定义放在这里而不是 ui_app_set_emotion() 旁边：remind_close() 定义在前面，
 * 它恢复表情态时也要用到这个"当前情绪"。 */
static const char *g_face_emo = "neutral";

/* ---- 进入卡片层的触发：双指长按两只眼睛（仅表情态生效）----
 * 计时/判定都在 eye_hold_tick() 里，这里只放状态。 */
static uint16_t g_eye_ms     = 0;   /* 已经按住多久（ms）*/
static uint16_t g_eye_lost   = 0;   /* 触点丢失累计（抗 IRQ 合并）*/
static uint8_t  g_eye_fired  = 0;   /* 这一轮已经进过卡片层 */
static uint8_t  g_eye_mt_ok  = 0;   /* 见过 2 个触点 = 面板确实支持双指 */
static uint8_t  g_eye_latch  = 0;   /* 1 = 必须先把手指抬起来，才允许再进卡片层 */
static uint16_t g_swallow_ms = 0;   /* >0 时把 UP 当"抬手指"而不是"点击" */
#define EYE_HOLD_MS     550         /* 按住多久算长按（< TP_IDLE_RESET_MS 600）*/
#define EYE_LOST_MS     180         /* 触点短暂丢失的容忍窗口 */
#define SWALLOW_UP_MS   500         /* 进卡片层后这段时间里的 UP 不当作点击 */

/* 卡片层空闲时长（毫秒）。
 * 以前 >=15000 就自动回落表情态，2026-10-31 用户决定取消那条路径 —— 现在只累加，
 * 没有消费方（留给以后做屏幕保护/休眠）。出口只有右上角 X 与顶部下滑。 */
static uint32_t g_idle_ms  = 0;

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

    /* 时间大字（与卡片页顶部时钟同款，圆角数码）*/
    {
        static char tbuf[6];           /* static：ui_add_clock 直接取字形，但仍沿用静态缓冲 */
        int w;
        tbuf[0] = (char)('0' + g_remind_hh / 10);
        tbuf[1] = (char)('0' + g_remind_hh % 10);
        tbuf[2] = ':';
        tbuf[3] = (char)('0' + g_remind_mm / 10);
        tbuf[4] = (char)('0' + g_remind_mm % 10);
        tbuf[5] = '\0';
        w = ui_clock_width(tbuf, 48);
        ui_add_clock((int16_t)(240 - w / 2), 216, tbuf, 48, C_TITLE);
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
        /* 回表情态：同样要"取消残留刷屏 + 强制整屏重画"。
         * remind_draw() 是同步 ui_flush() 刷的整屏，关掉之后如果只调
         * face_app_set_emotion("neutral")，情绪本来就是 neutral 时一行都不画，
         * 屏上会一直留着那张 REMINDER 卡片。 */
        ui_flush_abort();
        face_app_set_emotion(g_face_emo);
        face_app_repaint();
    }
}

void ui_app_cards_dirty(void)
{
    g_cards_dirty = 1;
    g_pending_page = -1;               /* 默认刷当前页（数据下发补刷场景）*/
}

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
    eye_hold_reset();
    g_eye_latch = 1;      /* 手指还按着的话，得先抬手才允许再进卡片层（防来回闪） */
    g_idle_ms  = 0;

    /* ★ 两件事必须都做，否则"退出 UI"在屏上是看不出来的：
     *   ① ui_flush_abort()：卡片分片刷屏可能只画了上半屏，不取消的话剩下的行
     *      会继续刷上去（用户已经退出了却看到整屏卡片），而且紧接着的整屏
     *      脸渲染会和它交错 —— 实机花屏。
     *   ② face_app_repaint()：apply_pose() 对同名姿势不重绘，光调
     *      face_app_set_emotion() 在情绪没变时一行都不画。 */
    ui_flush_abort();
    face_app_set_emotion(g_face_emo);      /* 回表情态时恢复"当前情绪"而非固定 neutral */
    face_app_repaint();
}

/* FACES 页点表情时，把"表情名 → 身体动作"的联动发出去。
 *
 * 为什么需要它：face_app_play() 只改脸，不碰舵机。若用户点的是 angry 这张脸，
 * 脸变成怒眼、脖子却纹丝不动 —— "表情"和"身体"就割裂了。
 * 只有六个情绪有对应的身体动作（MOVE_HAPPY/ANGRY/...），其余纯表情
 * （star_struck/dizzy/idea 等）不联动 —— 它们本来就没有"身体语言"。 */
static void cards_show_face_move(const char *pose)
{
    static const struct {
        const char *name;
        move_id_t   move;
    } map[] = {
        { "neutral",    MOVE_IDLE      },
        { "happy",      MOVE_HAPPY     },
        { "laugh",      MOVE_HAPPY     },   /* 大笑也是开心动作 */
        { "sad",        MOVE_SAD       },
        { "sad_anguish", MOVE_SAD      },
        { "cry",        MOVE_SAD       },
        { "angry_low",  MOVE_ANGRY     },
        { "angry_high", MOVE_ANGRY     },
        { "surprised",  MOVE_SURPRISED },
        { "shock",      MOVE_SURPRISED },
        { "loving",     MOVE_LOVING    },
        { "shy",        MOVE_LOVING    },
        { 0, MOVE_IDLE }
    };
    int i;

    if (pose == 0) {
        return;
    }
    for (i = 0; map[i].name != 0; i++) {
        if (strcmp(pose, map[i].name) == 0) {
            behavior_request_move((uint8_t)map[i].move);
            return;
        }
    }
}

/* 主动关闭卡片层（右上角 X / 顶部下滑）。
 *
 * 必须 pomo_reset()：MODE 页在 pomo 里也是一个状态（st = PS_MODE != PS_OFF），
 * 不清掉的话 pomo_active() 一直是 1，而 ui_app_refresh_cards() 里有
 * "计时中别插一脚"的守卫 —— 从那以后卡片页的数据就再也不刷新了。 */
static void cards_close(void)
{
    pomo_reset();
    back_to_face();          /* 里面会 eye_hold_reset() */
}

/* 串口/上层主动要求表情。
 *
 * ★ 2026-10-31 用户决定：**卡片层/计时流程里不抢屏**，只把情绪记下来。
 *   以前这里无条件 pomo_reset() + 回表情态，于是 ESP32 语音链路每次推一个
 *   状态、PC 每次发 0x01，都会把正在看卡片的用户直接拽回表情态 —— 用户看到
 *   的就是"进了 UI 莫名其妙就退出来"。退出卡片层时 back_to_face() 会把这张
 *   脸画出来，晚一点看到，但不打断操作。
 *
 * ★ 表情态下也只在**情绪真的变了**时才整屏重绘：ESP32 会持续重发同一个状态，
 *   每次都 render_all() 一屏（100~200ms 阻塞）等于让屏幕一直半刷不刷。
 */
void ui_app_force_emotion(const char *emo)
{
    const char *want = (emo != 0) ? emo : g_face_emo;
    int same = (g_face_emo == want) ? 1 : (strcmp(g_face_emo, want) == 0);

    if (g_state != ST_FACE) {
        g_face_emo = want;        /* 卡片层：只记住，退出时再显示 */
        return;
    }
    g_face_emo = want;
    if (same) {
        return;                   /* 同情绪：不动屏（这条路径可能每秒被调很多次）*/
    }

    pomo_reset();
    eye_hold_reset();
    g_idle_ms  = 0;
    ui_flush_abort();
    face_app_set_emotion(g_face_emo);
    face_app_repaint();
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
    face_gesture_reset();           /* 手势态别带进卡片层 */
    g_state    = ST_CARDS;
    eye_hold_reset();
    g_idle_ms  = 0;
    cards_show_gated(5);          /* NET 页现在是第 6 页（index 5）*/
}

/* ==================== v3 脸部手势（脸颊滑动 / 额头连点）====================
 *
 * 需求（用户 2026-10-31）：
 *   · 左右脸颊区域**滑动** -> 情绪进入 loving
 *   · 额头区域**连点三下** -> 情绪进入 angry
 *   · 30s 内反复触发 -> 强度递增（angry 从半怒到怒眼；loving 动作更足）
 *
 * 区域定义（用户原话）：眼睛两圆心的水平连线 l1；l1 以下、**未被绘制**的
 * 区域是脸颊；l1 以上是额头。
 *   · l1 = y 240：v9 基准里眼睛中心 cy = 0（屏幕中心 240），所以两圆心连线
 *     就是屏幕水平中线。
 *   · "未被绘制"用 face_app_hit_test() 判：它按**当前这一帧**的部件包围盒
 *     回答"这里画了东西没有"。点在眼睛/嘴/腮红/泪上都不算脸颊或额头 ——
 *     否则点眼睛也会被当成戳脸颊。
 *
 * 情绪怎么变：这是用户明确的手势指令，直接调 emo_force() 设定情绪 + 强度，
 * 不走 behavior_stimulus 的渐进漂移。理由：连点三下额头是"命令"，
 * 靠刺激攒要好几下才漂到位，用户会觉得没反应。
 * 屏幕与舵机不用在这里同步：emotion_fsm 下一拍会读 emo_current() 切动作、
 * 并调 face_ui_emotion() 同步表情。
 */
#define FACE_LINE_Y       240    /* 眼圆心连线 l1 */
#define FACE_EDGE_MARGIN  18     /* 屏幕边缘内缩，避免边框/圆角误触 */

#define ZONE_NONE         0
#define ZONE_CHEEK        1
#define ZONE_FOREHEAD     2

#define CHEEK_MIN_STEP    12     /* 判定方向的最小水平位移（px）*/
#define CHEEK_REVERSALS   2      /* 几次方向反转算"在脸颊上滑" */
#define CHEEK_IDLE_MS     520    /* 停多久算这一轮滑完 */

#define FORE_TAPS         3      /* 连点几次触发 */
#define FORE_GAP_MS       800    /* 相邻两次点击的最大间隔 */

#define GEST_REPEAT_MS    30000  /* 30s 内重复触发 -> 升级 */
#define GEST_MAX_LEVEL    3
#define GEST_FACE_MS      2500   /* 表情临时层时长 */

static uint32_t g_ui_ms = 0;     /* 软件时钟：tick 累计，供手势窗口使用 */

/* 脸颊滑动状态 */
static uint8_t  g_cheek_on   = 0;   /* 1 = 本轮已触发过（避免一次滑动连发）*/
static uint8_t  g_cheek_rev  = 0;
static int8_t   g_cheek_dir  = 0;
static int16_t  g_cheek_x    = 0;
static uint32_t g_cheek_idle = 0;

/* 额头连点状态 */
static uint8_t  g_fore_taps  = 0;
static uint32_t g_fore_gap   = 0;   /* 距上一次点击的毫秒数 */

/* 强度等级（脸颊 / 额头各自独立窗口）*/
static uint8_t  g_cheek_level = 0;
static uint32_t g_cheek_at    = 0;
static uint8_t  g_fore_level  = 0;
static uint32_t g_fore_at     = 0;

/* 视觉坐标属于哪个区域（NONE / CHEEK / FOREHEAD）*/
static int face_zone(int x, int y)
{
    if ((x < FACE_EDGE_MARGIN) || (x > (480 - FACE_EDGE_MARGIN)) ||
        (y < FACE_EDGE_MARGIN) || (y > (480 - FACE_EDGE_MARGIN))) {
        return ZONE_NONE;
    }
    if (face_app_hit_test(x, y)) {
        return ZONE_NONE;        /* 点在眼睛/嘴/装饰上：既不是脸颊也不是额头 */
    }
    return (y < FACE_LINE_Y) ? ZONE_FOREHEAD : ZONE_CHEEK;
}

/* 30s 窗口内的强度等级：首次 -> 1；窗口内再触发 -> +1（封顶）；
 * 超过窗口 -> 重新从 1 开始。用无符号减法比较，49 天溢出也安全。 */
static uint8_t gest_level(uint8_t *level, uint32_t *at)
{
    if ((*level == 0) || ((uint32_t)(g_ui_ms - *at) > GEST_REPEAT_MS)) {
        *level = 1;
    } else if (*level < GEST_MAX_LEVEL) {
        (*level)++;
    }
    *at = g_ui_ms;
    return *level;
}

/* 清空这一轮的手势状态（抬手 / 离开区域 / 进卡片层）。
 * 刻意【不】动表情：临时层会让脸自己保持 GEST_FACE_MS，到期回落当前情绪，
 * 抬手瞬间就把它撤掉会闪一下。 */
static void face_gesture_reset(void)
{
    g_cheek_on   = 0;
    g_cheek_rev  = 0;
    g_cheek_dir  = 0;
    g_cheek_idle = 0;
    /* 额头连点计数【不】在这里清：它是跨"按下-抬起"累计的（点一下就抬一次），
     * 由 FORE_GAP_MS 超时自己归零。在这里清会导致永远凑不满三下。 */
}

/* 脸颊滑动达标：进入 loving，等级越高 energy 越大（舵机动作更足）*/
static void cheek_fire(void)
{
    uint8_t lvl = gest_level(&g_cheek_level, &g_cheek_at);
    /* loving 的 energy 低于 480 会被 g_picks 选成 shy（害羞）；
     * 这里保持 560+ 让它一直是"正经撒娇"，并用等级推高动作幅度。 */
    int16_t energy = (int16_t)(560 + (int32_t)lvl * 120);

    g_cheek_on = 1;
    printf("[UI] cheek slide -> loving L%u (e=%d)\r\n", (unsigned)lvl, (int)energy);
    emo_force(EMO_LOVING, 780, energy, 700);
    face_app_play("loving", GEST_FACE_MS);
    voice_say(VOICE_LOVING, lvl);       /* 通知 PC → ESP32 播一句（等级决定语气）*/
}

/* 额头连点达标：进入 angry。等级 1 = 半怒（angry_low），2+ = 怒眼（angry_high）*/
static void fore_fire(void)
{
    uint8_t lvl = gest_level(&g_fore_level, &g_fore_at);
    /* angry 的 energy：550~779 = angry_low，>=780 = angry_high */
    int16_t energy = (int16_t)(620 + (int32_t)lvl * 150);

    printf("[UI] forehead x3 -> angry L%u (e=%d)\r\n", (unsigned)lvl, (int)energy);
    emo_force(EMO_ANGRY, 200, energy, 300);
    face_app_play((lvl >= 2) ? "angry_high" : "angry_low", GEST_FACE_MS);
    voice_say(VOICE_ANGRY, lvl);        /* 等级越高，ESP32 那边选的火气越大 */
}

/* 按住移动（GESTURE_MOVE）：脸颊区内来回滑 -> 累计方向反转 */
static void cheek_on_move(uint16_t x, uint16_t y)
{
    int16_t dx;

    if (g_state != ST_FACE) {
        face_gesture_reset();
        return;
    }
    if (face_zone((int)x, (int)y) != ZONE_CHEEK) {
        face_gesture_reset();        /* 滑出脸颊：这一轮结束 */
        return;
    }

    /* 脸颊滑动和"进卡片层"是两套手势，互不相干：
     * 进卡片层靠双指长按眼睛（eye_hold_tick），这里不用再清长按计时。 */

    dx = (int16_t)x - g_cheek_x;
    if ((dx >= CHEEK_MIN_STEP) || (dx <= -CHEEK_MIN_STEP)) {
        int8_t dir = (dx > 0) ? 1 : -1;

        if ((g_cheek_dir != 0) && (dir != g_cheek_dir) && (g_cheek_rev < 100)) {
            g_cheek_rev++;
        }
        g_cheek_dir  = dir;
        g_cheek_x    = (int16_t)x;
        g_cheek_idle = 0;

        if ((g_cheek_rev >= CHEEK_REVERSALS) && (g_cheek_on == 0)) {
            cheek_fire();
        }
    }
}

/* 点击（GESTURE_UP）：额头区域内连点 -> 计数，够 FORE_TAPS 触发 */
static void fore_on_tap(uint16_t x, uint16_t y)
{
    if (face_zone((int)x, (int)y) != ZONE_FOREHEAD) {
        g_fore_taps = 0;
        return;
    }
    if (g_fore_gap > FORE_GAP_MS) {
        g_fore_taps = 0;             /* 间隔太久：重新数 */
    }
    g_fore_taps++;
    g_fore_gap = 0;

    if (g_fore_taps >= FORE_TAPS) {
        g_fore_taps = 0;
        fore_fire();
    }
}

/* 每 tick：推进软件时钟 + 两个手势的超时判定 */
static void face_gesture_tick(uint32_t dt_ms)
{
    g_ui_ms += dt_ms;

    if (g_fore_gap < 60000U) {
        g_fore_gap += dt_ms;
    }
    if ((g_cheek_rev != 0) || (g_cheek_on != 0)) {
        g_cheek_idle += dt_ms;
        if (g_cheek_idle >= CHEEK_IDLE_MS) {
            face_gesture_reset();    /* 停手：这一轮滑完 */
        }
    }
}

/* ==================== 进卡片层：双指长按两只眼睛 ====================
 *
 * 用户 2026-10-31 要求把"进 UI"改成同时长按两只眼睛，"这样就不容易出乱子"。
 * 背景：脸上已经有两套手势（脸颊滑动 → loving，额头连点 → angry），而旧规则是
 * "任意位置按住 800ms 进卡片层" —— 抚摸和连点的落点几乎都在别人的地盘上，
 * 手上稍微停一下就被拽进 UI。眼睛是脸上唯一"点上去什么也不发生"的区域
 * （face_zone 已经把绘制区排除），两只手同时压住两只眼睛，意图明确、误触近零。
 *
 * 三个必须处理的现实问题：
 *  ① 要拿到两个触点。单点触摸驱动给不出第二根手指，所以 ICNT8952BN.c 补了
 *     双点读取（tp_get_points）。万一面板只上报单点，g_eye_mt_ok 永远为 0，
 *     判定退化成"单指长按任一只眼睛"—— 仍然只认眼睛、仍然比原来安全，
 *     也不会出现"怎么按都进不去 UI"的死局。
 *  ② 抬手会被当成点击。进卡片层是系统自己跳的，用户手指还在屏上；抬手那一下
 *     会被当成点击落到卡片页上（比如把日程某行勾掉）。所以跳转后 500ms 内的
 *     UP 一律吞掉（g_swallow_ms）。
 *  ③ 整屏刷屏期间 IRQ 会被合并，偶尔读到空帧。给 180ms 容忍窗口，
 *     否则长按会被自己的刷屏打断。
 */

/* 眼睛判定区：v9 基准眼心 cx = ±99.5（半像素坐标 -199/2），cy = 0
 * → 视觉坐标 (140,240) / (340,240)。
 *
 * 用【椭圆】而不是圆：眼睛本身横宽竖扁（rx 最大 88、ry 最大 88，但多数姿势
 * 竖向只有 20~50），圆形要么框不住大眼、要么把嘴的上沿一起圈进来。
 * 96×88 是拿 _probe/eye_zone_check.c 扫出来的：30 个姿势里眼睛实心像素
 * 最少覆盖 95%，且没有任何"非眼睛部件"的质心落在区内（戳嘴不会误判）。
 * 两个椭圆之间还留了 8px 死带（236~244）—— 指纹正好落在屏幕中线时
 * 既不算左眼也不算右眼。改这里的数就要重跑那个工具。 */
#define EYE_L_CX    140
#define EYE_R_CX    340
#define EYE_CY      240
#define EYE_RX      96
#define EYE_RY      88
#define EYE_MIN_SEP 120          /* 两触点至少差这么多才像"两只手"*/

static int in_eye(uint16_t x, uint16_t y, int cx)
{
    int dx;
    int dy;

    if ((x >= 480) || (y >= 480)) {
        return 0;                /* 坐标越界（驱动解析到脏数据）：直接否掉 */
    }
    dx = (int)x - cx;
    dy = (int)y - EYE_CY;
    /* 二次型 dx²/rx² + dy²/ry² <= 1 的整数形式，最坏量级
     * 340²×88² + 240²×96² ≈ 1.4e9，int32 装得下，不会溢出。 */
    return ((dx * dx) * (EYE_RY * EYE_RY) + (dy * dy) * (EYE_RX * EYE_RX))
           <= (EYE_RX * EYE_RX) * (EYE_RY * EYE_RY);
}

/* 这一帧的触点是"两只眼睛都按住了"吗 */
static int eye_hold_match(int n, const uint16_t *xs, const uint16_t *ys)
{
    if (n >= 2) {
        int sep = (int)xs[0] - (int)xs[1];

        if (sep < 0) {
            sep = -sep;
        }
        if (sep < EYE_MIN_SEP) {
            return 0;                /* 两指挤在一起：不是"左右各一只" */
        }
        /* 不假设点位顺序：两个方向都试 */
        if ((in_eye(xs[0], ys[0], EYE_L_CX) && in_eye(xs[1], ys[1], EYE_R_CX)) ||
            (in_eye(xs[1], ys[1], EYE_L_CX) && in_eye(xs[0], ys[0], EYE_R_CX))) {
            /* 真的凑成"左右各一只眼"了，才认定面板支持双指（此后不再降级）。
             * 阈值放在成功分支里：万一芯片偶发上报一个假的第二点，
             * 也不会把单点降级通道永久关掉。 */
            g_eye_mt_ok = 1;
            return 1;
        }
        return 0;
    }
    if ((n == 1) && !g_eye_mt_ok) {
        /* 单点面板降级：按住任一只眼睛也算（见上面 ①）*/
        return in_eye(xs[0], ys[0], EYE_L_CX) || in_eye(xs[0], ys[0], EYE_R_CX);
    }
    return 0;
}

static void eye_hold_reset(void)
{
    g_eye_ms    = 0;
    g_eye_lost  = 0;
    g_eye_fired = 0;
}

/* 每 tick：推进"双指按住"的时长，够了就进卡片层 */
static void eye_hold_tick(uint32_t dt_ms)
{
    uint16_t xs[2], ys[2];
    int n;

    if (g_eye_fired) {
        return;                     /* 这一轮已经进过卡片层，等抬手复位 */
    }
    n = tp_get_points(xs, ys);

    /* 刚从卡片层回落：手指要是还压在眼睛上，不许立刻再进去。
     * 不加这道闸的话，"按住两只眼睛看卡片 → 15s 无操作自动回落 → 550ms 后
     * 又被判成双指长按 → 又进卡片层"会变成一个死循环，屏幕上就是来回闪。
     * 必须先真的看到触点清空（抬手）才解锁。 */
    if (g_eye_latch) {
        if (n == 0) {
            g_eye_latch = 0;
        }
        return;
    }

    if (eye_hold_match(n, xs, ys)) {
        g_eye_lost = 0;
        if (g_eye_ms < EYE_HOLD_MS) {
            g_eye_ms = (uint16_t)(g_eye_ms + dt_ms);
        }
        if (g_eye_ms >= EYE_HOLD_MS) {
            printf("[UI] both eyes held -> cards\r\n");
            g_eye_fired  = 1;
            g_eye_ms     = 0;
            g_swallow_ms = SWALLOW_UP_MS;
            face_gesture_reset();   /* 手势态别带进卡片层 */
            g_state   = ST_CARDS;
            g_idle_ms = 0;
            ui_cards_show(0);
        }
        return;
    }
    /* 丢一帧就清零太脆：刷屏那几十毫秒里读到空帧是常事，给容忍窗口 */
    if (g_eye_ms != 0) {
        g_eye_lost = (uint16_t)(g_eye_lost + dt_ms);
        if (g_eye_lost >= EYE_LOST_MS) {
            eye_hold_reset();
        }
    }
}

/* 画指定卡片页：拟物可爱风（渐变底 + 顶部时间 + 页码圆点 + 圆角卡片）*/

/* ---------------- 页面骨架的固定参数 ----------------
 * 六页共用，改一处六页一起变。pomo 的 MODE 页（第 2 页）也走这里，
 * 所以这些数字不要在别处再抄一份。
 *
 * 页码：0 TODAY / 1 TIMER / 2 HABIT / 3 PLAY / 4 FACES / 5 NET / 6 DEMO
 * （v2.3 起加 FACES 表情选择页，NET 从 4 迁到 5；v3.1 末尾加 DEMO 一键演示页）*/
#define UI_CARD_PAGES    7
#define UI_DOT_CY        92      /* 页码圆点那一行的 y */
/* 圆点整行居中：n 个点跨度 (n-1)×STEP，起点 = 240 - 跨度/2。
 * 写死数字的话加一页就得手改，漏改就是整行偏一边。 */
#define UI_DOT_CX0       (240 - ((UI_CARD_PAGES - 1) * UI_DOT_STEP / 2))
#define UI_DOT_STEP      22
#define UI_CARD_X        60
#define UI_CARD_Y        122
#define UI_CARD_W        360
#define UI_CARD_H        290

/* 关闭按钮：跟页码圆点同一行，放在右端。
 *
 * 为什么是"可见按钮"而不是长按退出：
 *   v2.0 曾经做过"卡片层长按退出"，实机上用户按住看内容就会被弹回表情层，
 *   所以删掉了；但删掉之后卡片层就只剩 15 秒无操作自动回落，NET 页还是永远
 *   不回落（怕把二维码收走）—— 用户根本没有"我现在就想关掉"的手段。
 *   固定位置的小按钮既不会误触，也不依赖用户去猜手势。
 *
 * 触摸判定比视觉大一圈：480 屏上 1px ≈ 0.125mm，视觉 40px 只有 5mm，
 * 手指点不中；判定给到 ±34（约 8.5mm）。 */
#define UI_CLOSE_CX      408
#define UI_CLOSE_CY      UI_DOT_CY
#define UI_CLOSE_R       20
#define UI_CLOSE_HIT     34
/* 顶部下滑退出：判定用【起点】y（滑动事件回传的是 start 坐标）。
 * 卡片内容从 122 开始，所以 110 这条线不会和列表滚动打架。 */
#define UI_CLOSE_SWIPE_Y 110

static int close_hit(int x, int y)
{
    int dx = x - UI_CLOSE_CX;
    int dy = y - UI_CLOSE_CY;

    return (dx * dx + dy * dy) <= (UI_CLOSE_HIT * UI_CLOSE_HIT);
}

void ui_cards_chrome(int page);

/* 顶部公共头：当前时间 + 六个页码圆点（可选关闭按钮）。
 *
 * 为什么单独抽出来：番茄钟的深层子页（POMODORO 设置 / 专注 / 正计时 / 倒计时…）
 * 内容是整屏铺开的，塞不进 60,122,360,290 的圆角卡片框，所以它们不能直接用
 * ui_cards_chrome()。但它们仍然在"第 2 页"里面，顶栏必须和其余页一模一样 ——
 * 否则同一个 App 里出现"3 个圆点 / 6 个圆点"两套页码，用户第一反应是这页坏了
 * （用户 2026-10-31 反馈的正是这个）。
 *
 * 调用约定：本函数只往绘制列表里追加元素，不清屏、不设背景 —— 谁调谁负责
 * 先 ui_clear() + ui_set_bg_gradient()。 */
void ui_cards_header(int page, int with_close)
{
    int i, w;

    /* 顶部当前时间：由 PC 对时下发（0x10），未对时显示 --:--。
     * 用圆角数码而不是 ui_add_text(scale 2)：12×24 点阵放大就是 2×2 硬方块，
     * 屏上每个台阶都看得见（用户 2026-10-28 反馈）。v2.2 起改成本文件的
     * ui_add_clock()，笔画是圆角矩形，边缘是圆的。 */
    {
        static char tbuf[6];
        habit_clock_hhmm(tbuf);
        w = ui_clock_width(tbuf, 48);
        ui_add_clock((int16_t)((480 - w) / 2), 24, tbuf, 48, C_TITLE);
    }

    /* 页码圆点 */
    for (i = 0; i < UI_CARD_PAGES; i++) {
        ui_add_circle((int16_t)(UI_DOT_CX0 + i * UI_DOT_STEP), UI_DOT_CY,
                      6, 6, 1, (i == page) ? C_DOT_ON : C_DOT_OFF);
    }

    /* 关闭按钮：细圆环 + 一个 X。不用两条斜线是因为绘制列表只有轴对齐矩形，
     * 加一个任意角度的线段图元要动 ui.c 的整条绘制链，代价不值。
     * 字库 12×24，scale 1 放在 40px 的环里比例正好。
     * 深层子页传 with_close=0：那里的手势是锁死的（退出走各自的 EXIT / GIVE UP），
     * 画一个按不动的 X 等于骗人。 */
    if (with_close) {
        ui_add_circle(UI_CLOSE_CX, UI_CLOSE_CY, UI_CLOSE_R, 3, 0, C_HILITE);
        {
            int cw = ui_text_width("X", 1);
            ui_add_text((int16_t)(UI_CLOSE_CX - cw / 2),
                        (int16_t)(UI_CLOSE_CY - 12), "X", 1, C_TITLE);
        }
    }
}

void ui_cards_chrome(int page)
{
    ui_clear();
    ui_set_bg_gradient(C_BG_TOP, C_BG_MID, C_BG_BOT);

    ui_cards_header(page, 1);

    /* 圆角卡片（带厚度阴影）*/
    ui_add_round_fill(UI_CARD_X, UI_CARD_Y + 8, UI_CARD_W, UI_CARD_H,
                      28, C_CARD_SHAD);
    ui_add_round_fill(UI_CARD_X, UI_CARD_Y, UI_CARD_W, UI_CARD_H,
                      28, C_CARD_TOP);
    ui_add_round_rect(UI_CARD_X, UI_CARD_Y, UI_CARD_W, UI_CARD_H,
                      28, 3, C_CARD_LINE);
}

void ui_cards_show(int page)
{
    g_card_page = page;

    /* 第 2 页 = 计时功能三入口（pomo 接管渲染，内部同样调 ui_cards_chrome）*/
    if (page == 1) {
        pomo_enter_mode();
        return;
    }

    /* 不是第 2 页却还停在 MODE 状态（用户从第 2 页滑走了）：必须清掉。
     * pomo 的 PS_MODE 也算"计时流程中"，留着会让 ui_app_refresh_cards() 和
     * 花屏自愈的 !pomo_active() 守卫一直不成立 —— 表现就是"滑走的这几秒里
     * 卡片数据不更新、花屏不自愈"。深层计时页不会走到这里（手势被锁定）。 */
    if (pomo_at_mode()) {
        pomo_reset();
    }

    /* 第 6 页 = 配网二维码。全屏档自己铺满整屏（白底利于对焦），
     * 不走下面这套"时间 + 圆点 + 卡片框"的布局。 */
    if (page == 5 && net_card_full()) {
        ui_clear();
        net_card_draw_full();
        ui_flush_begin(UI_COL_BG);
        return;
    }

    ui_cards_chrome(page);

    if (page == 2) {
        /* HABIT 页：习惯看板（名字点阵 + 状态圆 + 进度 + 连续天数）*/
        habit_card_draw();
    } else if (page == 3) {
        /* PLAY 页（v2.0）：动作 / 情绪按钮（只发请求与刺激，不直接写舵机）*/
        play_card_draw();
    } else if (page == 4) {
        /* FACES 页（v2.3）：表情选择，点名字回表情态播放 */
        face_picker_draw();
    } else if (page == 5) {
        /* NET 页（v2.1）：二维码 + 配网状态；点一下放大成全屏码 */
        net_card_draw_card();
    } else if (page == 6) {
        /* DEMO 页（v3.1）：一键演示，把屏幕与舵机的能力整套跑一遍 */
        demo_card_draw();
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
    /* 演示进行中最优先：整套演示会自己翻表情、自己要动作，这时候任何手势
     * （翻页 / 双击进眼 / 抚摸）都是添乱 —— 唯一的例外是"点一下 = 停下"，
     * 演示要能随时叫停，否则用户只能干等 90 秒。 */
    if (demo_show_running()) {
        if (g == 2) {                    /* UP = 点击 */
            demo_show_stop();
            if (g_state == ST_CARDS) {
                ui_cards_show(g_card_page);   /* 停在 DEMO 页：按钮变回 START */
            }
        }
        return;
    }

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

    /* ★ 刚因双指长按跳进卡片层：接下来这一小段时间里的手指动作，都是"进 UI"
     *   那个手势的尾巴 —— 两指分离的位移、抬手帧的坐标跳变。当成点击会误触
     *   卡片内容（比如勾掉一行日程），当成滑动就是"刚进 UI 就自己翻了一页"。
     *   以前只吞 UP，实机表现正是"进卡片层瞬间闪一下/跳页"。
     *   DOWN(1) 和 MOVE(7) 放行：前者是下一轮新触摸的起点，后者在卡片层本来
     *   就没有消费方（cheek_on_move 会自己按 g_state 直接返回）。 */
    if ((g_swallow_ms != 0) && (g != 1) && (g != 7)) {
        return;
    }

    switch (g) {
    case 1: /* DOWN：这里不再需要记长按起点。
             * "进卡片层"改由 eye_hold_tick() 轮询触点缓存判断（双指按眼睛），
             * 手势码只负责点击/滑动，两套逻辑互不干扰。 */
        break;

    case 2: /* UP（点击）*/
        face_gesture_reset();            /* 抬手：这一轮脸颊滑动结束 */
        eye_hold_reset();                /* 抬手：双指长按这一轮结束 */
        /* "进卡片层后的抬手不是点击"已由上面的 g_swallow_ms 统一拦掉 */
        if (g_state == ST_FACE) {
            /* 表情态：额头连点三下 -> angry。放这里是因为它只认"点击"，
             * 而表情态本来没有任何点击逻辑，不会与别的手势抢。 */
            fore_on_tap(x, y);
            break;
        }
        if (g_state == ST_CARDS) {
            /* 关闭按钮最先判：它在页码圆点那一行，与任何卡片内容都不重叠，
             * 放在这里可以保证五页行为一致（包括 pomo 的 MODE 页）。 */
            if (close_hit((int)x, (int)y)) {
                cards_close();
                break;
            }
            if (g_card_page == 1) {
                pomo_on_tap((int)x, (int)y);      /* 计时三入口页 */
            } else if (g_card_page == 2) {
                habit_on_tap((int)x, (int)y);     /* 习惯页：点行打卡/取消 */
            } else if (g_card_page == 3) {
                /* PLAY 页：点按钮 -> 请求动作 / 情绪刺激，然后【回表情态】。
                 * 为什么要收卡片：这一页的反馈全在机器人身上（脖子在动、脸在变），
                 * 而卡片层正好把脸盖住 —— 用户按完盯着屏幕看，屏幕一个像素都不变，
                 * 结论就是"按了没反应"。与 FACES 页点名字后 cards_close() 同一套做法。
                 * PRAISE / SCOLD 的瞬时表情（smug / nervous，2s）也只有在表情态才看得见。
                 *
                 * ★ TRACK 开关是唯一的例外（PLAY_TAP_KEEP）：那一行自己要显示
                 *   ON/OFF，关掉卡片层就看不出按没按上，想切回来还得重新长按+滑页。
                 *   所以它留在本页，只标脏让下一拍重绘 —— 重绘时情绪状态机那一拍
                 *   已经把模式切完，画出来的就是新状态。 */
                {
                    int r = play_card_tap((int)x, (int)y);

                    if (r == PLAY_TAP_CLOSE) {
                        cards_close();
                    } else if (r == PLAY_TAP_KEEP) {
                        ui_app_cards_dirty();
                    } else {
                        g_idle_ms = 0;           /* 没点到按钮也算有操作 */
                    }
                }
            } else if (g_card_page == 4) {
                /* FACES 页：点名字 -> 立刻回表情态播放该表情 3s，并且
                 * 如果这个表情对应某个【情绪】，同时请求脖子做对应动作，
                 * 否则"脸在生气、身子却不动"——用户看到的是割裂的。 */
                {
                    int idx = face_picker_tap((int)x, (int)y);
                    if (idx >= 0) {
                        const char *pose = face_model_get(idx)->name;

                        face_app_play(pose, 3000);
                        /* 表情名 -> 身体动作（只给有身体动作的情绪配；
                         * 纯表情如 star_struck/dizzy/idea 不联动舵机）*/
                        cards_show_face_move(pose);
                        cards_close();            /* 收卡片，回表情态看脸 */
                    }
                }
            } else if (g_card_page == 5) {
                /* NET 页：点一下在"卡片内小码 ↔ 全屏大码"之间切换。
                 * 二维码要凑近扫，全屏那档模块能从 6px 放大到 12px 以上。 */
                if (net_card_tap((int)x, (int)y)) {
                    ui_cards_show(5);
                }
            } else if (g_card_page == 6) {
                /* DEMO 页：点 START → 收卡片回表情态，再开演。
                 *
                 * ★ 顺序不能反：cards_close() 内部会把脸设回"当前情绪"，
                 *   它会清掉临时表情层 —— 先 start 的话刚摆好的 listen 脸
                 *   当场被抹掉，开场那一下是闪的。
                 *   收卡片是必须的：演示的反馈全在脸和脖子上，
                 *   留着卡片盖着脸就什么都看不到（与 PLAY / FACES 页同一套做法）。 */
                if (demo_card_tap((int)x, (int)y)) {
                    if (demo_show_running()) {
                        demo_show_stop();
                        ui_cards_show(6);        /* 按钮从 STOP 变回 START */
                    } else {
                        cards_close();
                        demo_show_start();
                    }
                } else {
                    g_idle_ms = 0;
                }
            } else {
                sched_on_tap((int)x, (int)y);     /* TODAY 页：点行切换完成 */
            }
        }
        break;

    /* ★ 按住中的位置更新（ICNT8952BN 的 GESTURE_MOVE）——
     *   只在表情态用：脸颊区域来回滑 = 进入 loving（见上方脸部手势说明）。
     *   卡片层不参与，避免与"左右滑翻页"抢手势。 */
    case 7:
        cheek_on_move(x, y);
        break;

    case 3: /* 左滑：详情页先退详情，否则卡片层向后翻页 */
        if (g_state == ST_CARDS && habit_detail_active()) {
            habit_detail_close();
            cards_show_gated(2);
        } else if (g_state == ST_CARDS && g_card_page < (UI_CARD_PAGES - 1)) {
            cards_show_gated(g_card_page + 1);
        }
        break;

    case 4: /* 右滑：详情页先退详情，否则卡片层向前翻页 */
        if (g_state == ST_CARDS && habit_detail_active()) {
            habit_detail_close();
            cards_show_gated(2);
        } else if (g_state == ST_CARDS && g_card_page > 0) {
            cards_show_gated(g_card_page - 1);
        }
        break;

    case 5: /* 上滑（手指向上）：列表向下滚动（习惯页 / TODAY 页 / 表情页）*/
        if (g_state == ST_CARDS && !habit_detail_active()) {
            if (g_card_page == 2) {
                habit_store_scroll(1);
                cards_show_gated(2);
            } else if (g_card_page == 0) {
                sched_store_scroll(1);
                cards_show_gated(0);
            } else if (g_card_page == 4) {
                face_picker_scroll(1);
                cards_show_gated(4);
            }
        }
        break;

    case 6: /* 下滑（手指向下）：列表向上滚动 */
        /* 从屏幕顶部往下拖 = 收起卡片层（下拉关闭）。
         * 判定用【起点】y：tp_get_gesture 的滑动分支回传的是 start 坐标，
         * 所以"在列表里往下滚"（起点在卡片内 y>=122）不会误触。 */
        if (g_state == ST_CARDS && y < UI_CLOSE_SWIPE_Y) {
            cards_close();
            break;
        }
        if (g_state == ST_CARDS && !habit_detail_active()) {
            if (g_card_page == 2) {
                habit_store_scroll(-1);
                cards_show_gated(2);
            } else if (g_card_page == 0) {
                sched_store_scroll(-1);
                cards_show_gated(0);
            } else if (g_card_page == 4) {
                face_picker_scroll(-1);
                cards_show_gated(4);
            }
        }
        break;

    default:
        break;
    }
}

void ui_app_tick(uint32_t dt_ms)
{
    /* 抬手吞掉窗口：必须放在所有早返回之前 —— 进卡片层以后就不再走表情态
     * 那一段了，写在那儿会让 g_swallow_ms 永远减不到 0。 */
    if (g_swallow_ms != 0) {
        g_swallow_ms = (g_swallow_ms > dt_ms) ? (uint16_t)(g_swallow_ms - dt_ms) : 0;
    }

    /* ★ 一键演示：必须放在所有早返回之前（提醒弹卡 / 番茄钟 / 卡片层都会
     *   提前 return，写在后面演示就停在那儿不动了）。
     *   动作播放是阻塞的，但 moves 的 20ms 钩子会把 app_core_step() 拉起来，
     *   所以这里在"脖子正在演"的期间照样能推进计时。 */
    demo_show_tick(dt_ms);

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
        if (g_state == ST_CARDS && g_card_page == 5 && !pomo_active()) {
            cards_show_gated(5);
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
    face_gesture_tick(dt_ms);

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
         g_card_page == 4 || g_card_page == 5 || g_card_page == 6) &&
        uart_hal_rx_quiet_ms() >= 150) {
        static uint16_t s_repaint = 0;
        s_repaint = (uint16_t)(s_repaint + dt_ms);
        if (s_repaint >= 4000) {
            s_repaint = 0;
            ui_cards_show(g_card_page);
        }
    }

    /* 卡片层空闲计时。
     *
     * ★ 2026-10-31 用户决定：**取消 15 秒无操作自动回落**。
     *   看日程/习惯列表本来就要看一会儿，被自己收走很烦；更糟的是以前回落
     *   之后如果手指还压在眼睛上，550ms 后又被判成双指长按拉回卡片层 ——
     *   屏幕上就是来回闪。现在出口只有两个：右上角 X、从屏幕顶部往下拖。
     *   （NET 页本来就不回落，二维码要给手机对半天。）
     *   g_idle_ms 仍然累加，留给以后做屏幕保护/休眠用。 */
    if (g_state == ST_CARDS) {
        g_idle_ms += dt_ms;
    }

    /* 表情态：先判"双指长按两只眼睛"（可能把状态切到卡片层），再驱动脸 */
    if (g_state == ST_FACE) {
        eye_hold_tick(dt_ms);
    }
    if (g_state == ST_FACE) {
        face_app_tick(dt_ms);
    }
}
