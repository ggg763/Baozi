/******************************************************************************
 * @file     demo_show.c
 * @brief    一键演示实现（见 demo_show.h）
 *
 * 每步两张牌：
 *   ① 等播报（DS_WAIT）：打印 DEMO:<NAME>:<IDX>，脸上摆 "listen"（在听喇叭说话），
 *      等 PC 回 DEMORUN；PC 不在就按 DEMO_WAIT_DEFAULT_MS 自己往下走。
 *   ② 演（DS_PLAY）：临时表情层播 N 毫秒 + 请求一个动作，时间到进下一步。
 *
 * ★ 两条规矩，改这个文件前先看：
 *   1. 不直接写舵机：动作一律 behavior_request_move() → emotion_fsm，
 *      和 PLAY 页那些按钮走同一条路（限位/增益/总线仲裁全都照旧）。
 *   2. 演示期间要不停 mood_fsm_touch()：心情状态机 60s 没人理就进 BORED、
 *      180s 进 SLEEPING，届时脸和动作都会被它接管 —— 整套演示约 90 秒，
 *      不自报互动的话演到一半会自己"睡着"。
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "demo_show.h"
#include "ui.h"                     /* 第 7 页绘制 */
#include "behavior.h"               /* behavior_request_move（不直接写舵机）*/
#include "movements/moves.h"        /* move_id_t */
#include "emotion/mood_fsm.h"       /* mood_fsm_touch */

/* face_app 临时表情层（anim/face_app.c）：
 * face_app_play  = 播 ms 毫秒后自动回落到当前情绪
 * face_app_hold_end = 立刻结束临时表情（停止演示时用） */
extern int  face_app_play(const char *name, uint32_t ms);
extern void face_app_hold_end(void);

/* ==================== 演示脚本 ====================
 * name : 上行 DEMO:<name> 的名字，PC 按它找 assets/demo/<name>.wav
 * pose : 表情姿势名（anim/face_poses_v9.h 里的 30 个之一）
 * move : 动作 ID（movements/moves.h）；-1 = 这一步只演屏幕、不动脖子
 * hold : 这一步演多久（ms）
 *
 * ⚠️ 改 name 必须同步改 PC 侧 demo_voice.py 的 DEMO_SCRIPTS，否则 PC 找不到音频。
 *
 * 顺序是有讲究的：先待机（让人看清"没事的时候它在干嘛"），再按情绪强弱排
 * （开心→难过→生气→惊讶→撒娇），然后是两个"纯机械能力"（8 字 / 睡觉），
 * 最后两步只演屏幕（眨眼 / 视线）——把"屏幕自己的功能"单独亮出来。 */
typedef struct {
    const char *name;
    const char *pose;
    int         move;
    uint16_t    hold_ms;
} demo_step_t;

static const demo_step_t k_steps[DEMO_STEPS] = {
    { "IDLE",      "neutral",   MOVE_IDLE,      9000 },  /* 待机巡视（变体本身 18~23s，只看前 9s）*/
    { "HAPPY",     "happy",     MOVE_HAPPY,     5000 },
    { "SAD",       "sad",       MOVE_SAD,       5000 },
    { "ANGRY",     "angry_high", MOVE_ANGRY,    4500 },
    { "SURPRISED", "surprised", MOVE_SURPRISED, 4200 },
    { "LOVING",    "loving",    MOVE_LOVING,    5500 },
    { "EIGHT",     "laugh",     MOVE_EIGHT,     6500 },  /* 脖子画横 8 字 */
    { "SLEEP",     "asleep",    MOVE_SLEEP,     5500 },
    { "WINK",      "wink",      -1,             3500 },  /* 只演屏幕：眨眼 */
    { "GLANCE",    "glance",    -1,             3500 },  /* 只演屏幕：视线 */
};

/* 等播报时摆的表情：喇叭在说话，屏幕上却已经在演别的，观感是割裂的 */
#define DEMO_LISTEN_POSE    "listen"

typedef enum {
    DS_OFF = 0,     /* 没在演示 */
    DS_WAIT,        /* 已请求播报，等 PC 的 DEMORUN */
    DS_PLAY         /* 正在演 */
} demo_state_t;

static demo_state_t s_state   = DS_OFF;
static int          s_idx     = 0;
static uint32_t     s_ms      = 0;          /* 当前阶段已用时间 */
static uint32_t     s_wait_ms = DEMO_WAIT_DEFAULT_MS;   /* 这一步最多等多久 */

/* ---------------------------------------------------------------- 内部 */

/* 行比较：大小写不敏感（串口助手里敲小写也要认） */
static int line_eq(const char *line, const char *kw)
{
    int i;

    for (i = 0; kw[i] != '\0'; i++) {
        char c = line[i];

        if ((c >= 'a') && (c <= 'z')) {
            c = (char)(c - 'a' + 'A');
        }
        if (c != kw[i]) {
            return 0;
        }
    }
    return (line[i] == '\0') ? 1 : 0;
}

/* "DEMOWAIT:4200" → 4200（非数字返回 -1）*/
static int parse_wait_ms(const char *line)
{
    const char *p = line + 9;           /* 跳过 "DEMOWAIT:" */
    int v = 0;
    int any = 0;

    while (*p != '\0') {
        if ((*p < '0') || (*p > '9')) {
            return -1;
        }
        v = v * 10 + (*p - '0');
        if (v > 1000000) {
            return -1;
        }
        any = 1;
        p++;
    }
    return any ? v : -1;
}

/* 开始演当前这一步（等播报结束 / 等超时 都汇到这里）*/
static void step_go(void)
{
    const demo_step_t *st = &k_steps[s_idx];

    (void)face_app_play(st->pose, st->hold_ms);
    if (st->move >= 0) {
        behavior_request_move((uint8_t)st->move);
    }
    s_state = DS_PLAY;
    s_ms    = 0;

    printf("[demo] go %d/%d %s pose=%s hold=%ums\r\n",
           s_idx + 1, DEMO_STEPS, st->name, st->pose, (unsigned)st->hold_ms);
}

/* 进入第 idx 步：先请 PC 播报 */
static void step_begin(int idx)
{
    if ((idx < 0) || (idx >= DEMO_STEPS)) {
        return;
    }
    s_idx     = idx;
    s_ms      = 0;
    s_wait_ms = DEMO_WAIT_DEFAULT_MS;

    /* 演示期间自报"有人在互动"：否则 60s 后心情降级到 BORED、
     * 180s 后 SLEEPING，脸和动作会被心情接管，演示就走样了。 */
    mood_fsm_touch();

    (void)face_app_play(DEMO_LISTEN_POSE, DEMO_WAIT_MAX_MS);

    /* 这一行是给 PC 的播报请求（PC 按前缀抓，混在 printf 里不影响串口助手看）*/
    printf("DEMO:%s:%d\r\n", k_steps[idx].name, idx);

    s_state = DS_WAIT;
}

static void step_next(void)
{
    if (s_idx + 1 >= DEMO_STEPS) {
        s_state = DS_OFF;
        s_idx   = 0;
        s_ms    = 0;
        face_app_hold_end();            /* 立刻回落到当前情绪，不等临时层自然到期 */
        printf("[demo] finished\r\n");
        printf("DEMOEND\r\n");
        return;
    }
    step_begin(s_idx + 1);
}

/* ---------------------------------------------------------------- 对外 */

void demo_show_start(void)
{
    if (s_state != DS_OFF) {
        return;                         /* 已经在演：连点不重头再来 */
    }
    face_app_hold_end();                /* 清掉抚摸等残留的临时表情 */
    printf("[demo] start: %d steps\r\n", DEMO_STEPS);
    step_begin(0);
}

void demo_show_stop(void)
{
    if (s_state == DS_OFF) {
        return;
    }
    s_state = DS_OFF;
    s_idx   = 0;
    s_ms    = 0;
    face_app_hold_end();
    printf("[demo] stopped\r\n");
    printf("DEMOABORT\r\n");
}

int demo_show_running(void)
{
    return (s_state != DS_OFF) ? 1 : 0;
}

int demo_show_index(void)
{
    return (s_state != DS_OFF) ? s_idx : -1;
}

const char *demo_show_name(void)
{
    return (s_state != DS_OFF) ? k_steps[s_idx].name : 0;
}

void demo_show_tick(uint32_t dt_ms)
{
    if (s_state == DS_OFF) {
        return;
    }
    if (dt_ms == 0u) {
        dt_ms = 1u;
    }
    s_ms += dt_ms;

    if (s_state == DS_WAIT) {
        /* 等播报：PC 回 DEMORUN 会直接切到 DS_PLAY；
         * 等不到就按窗口自己往下走 —— 没接 PC 时整套演示照样能演完。 */
        if (s_ms >= s_wait_ms) {
            step_go();
        }
        return;
    }

    /* DS_PLAY：演满这一步的时间就进下一步 */
    if (s_ms >= (uint32_t)k_steps[s_idx].hold_ms) {
        step_next();
    }
}

int demo_show_handle_line(const char *line)
{
    if (line == 0) {
        return 0;
    }
    if (line_eq(line, "DEMOGO")) {
        demo_show_start();
        return 1;
    }
    if (line_eq(line, "DEMOQUIT")) {
        demo_show_stop();
        return 1;
    }
    if (line_eq(line, "DEMORUN")) {
        if (s_state == DS_WAIT) {
            step_go();
        } else {
            printf("[demo] DEMORUN ignored (not waiting)\r\n");
        }
        return 1;
    }
    if ((line[0] == 'D' || line[0] == 'd') &&
        (line[1] == 'E' || line[1] == 'e') &&
        (line[2] == 'M' || line[2] == 'm') &&
        (line[3] == 'O' || line[3] == 'o') &&
        (line[4] == 'W' || line[4] == 'w') &&
        (line[5] == 'A' || line[5] == 'a') &&
        (line[6] == 'I' || line[6] == 'i') &&
        (line[7] == 'T' || line[7] == 't') &&
        (line[8] == ':')) {
        int v = parse_wait_ms(line);

        if ((v > 0) && (s_state == DS_WAIT)) {
            s_wait_ms = ((uint32_t)v > DEMO_WAIT_MAX_MS)
                        ? (uint32_t)DEMO_WAIT_MAX_MS : (uint32_t)v;
            printf("[demo] announce window -> %ums\r\n", (unsigned)s_wait_ms);
        }
        return 1;
    }
    return 0;
}

/* ==================== 卡片层第 7 页：DEMO ====================
 * 与 play_card 同款皮肤（ui_add_btn3d 立体胶囊），只有一个大按钮。
 * 演示跑起来之后卡片会被收掉（要看脸），所以"正在演第几步"这行
 * 只有用户自己滑回这一页时才看得到 —— 但那正好是他想看进度的时候。 */
#define DEMO_BTN_X      (240 - 130)
#define DEMO_BTN_Y      190
#define DEMO_BTN_W      260
#define DEMO_BTN_H      84

void demo_card_draw(void)
{
    /* ⚠️ ui_add_text() 只把【指针】存进绘制列表，真正取字是在 ui_flush 那一步 ——
     *   局部数组出门就悬空了（实机表现：这一行永远画不出来，还不报错）。
     *   位图那条链上踩过同一个坑（qr 的 net_card 也是这么注释的），这里用
     *   static 缓冲：内容每帧重画覆盖，不存在并发读。 */
    static char s_progress[24];
    int w;
    const char *title = "DEMO";
    const char *btn;
    const char *hint;

    w = ui_text_width(title, 1);
    ui_add_text((int16_t)(240 - w / 2), 126, title, 1, C_TITLE);

    /* 时长是实测算出来的：10 步动作 47s + PC 播报 49s ≈ 100s
     * （PC 没接时只有动作那 47s）。写"about 90s"会让人以为卡住了。 */
    w = ui_text_width("10 steps / about 100s", 1);
    ui_add_text((int16_t)(240 - w / 2), 146, "10 steps / about 100s", 1, UI_COL_SUB);

    btn  = demo_show_running() ? "STOP" : "START";
    hint = demo_show_running() ? "running..." : "screen + servo tour";

    ui_add_btn3d(DEMO_BTN_X, DEMO_BTN_Y, DEMO_BTN_W, DEMO_BTN_H,
                 (int16_t)(DEMO_BTN_H / 2), C_BTN_TOP, C_BTN_BOT, C_BTN_SHAD);
    ui_add_round_rect(DEMO_BTN_X, DEMO_BTN_Y, DEMO_BTN_W, DEMO_BTN_H,
                      (int16_t)(DEMO_BTN_H / 2), 2, C_CARD_LINE);

    w = ui_text_width(btn, 2);
    ui_add_text((int16_t)(240 - w / 2), DEMO_BTN_Y + 24, btn, 2, C_CARD_TOP);

    w = ui_text_width(hint, 1);
    ui_add_text((int16_t)(240 - w / 2), 300, hint, 1, UI_COL_SUB);

    if (demo_show_running()) {
        const char *nm = demo_show_name();

        if (nm == 0) {
            nm = "-";
        }
        /* "3 / 10  HAPPY" —— 名字本来就是大写英文，直接用 ASCII 拼 */
        (void)snprintf(s_progress, (size_t)sizeof(s_progress), "%d / %d  %s",
                       demo_show_index() + 1, DEMO_STEPS, nm);
        w = ui_text_width(s_progress, 1);
        ui_add_text((int16_t)(240 - w / 2), 330, s_progress, 1, C_TITLE);
    } else {
        w = ui_text_width("tap START to run", 1);
        ui_add_text((int16_t)(240 - w / 2), 330, "tap START to run", 1, UI_COL_SUB);
    }
}

int demo_card_tap(int x, int y)
{
    /* 只判"点没点上"，【不在这里】真的开始/停止 ——
     * 开始的顺序有讲究（先收卡片、再 demo_show_start，否则刚摆好的 listen 脸
     * 会被 cards_close() 里的 face_app_set_emotion 抹掉），这个顺序只有
     * ui_app.c 知道，所以动作交给它做。 */
    if ((x >= DEMO_BTN_X) && (x < DEMO_BTN_X + DEMO_BTN_W) &&
        (y >= DEMO_BTN_Y) && (y < DEMO_BTN_Y + DEMO_BTN_H)) {
        return 1;
    }
    return 0;
}
