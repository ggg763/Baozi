/******************************************************************************
 * @file     face_app.c
 * @brief    表情状态机：切姿势 + 过渡 + 动作叠加 + 眨眼 + 节流重绘
 *
 * 一条 tick 的流程：
 *
 *   时间 ──► face_action（正弦叠加）──► 目标参数
 *                                        │
 *                        微扫视 / 眨眼 ──┤（直接写入，不过平滑）
 *                                        ▼
 *                                  一阶平滑（τ≈70ms）
 *                                        │
 *   姿势A ─┐                            ▼
 *          ├─► face_model_mix ──► face_model_apply ──► face_show_parts ──► 屏
 *   姿势B ─┘   （过渡插值）        （眨眼/视线/张嘴）
 *
 * 三条设计约束（都是本工程踩过的坑）：
 *   ① 【非阻塞】本函数由 ui_app_tick() 每轮调用，不能 mdelay 走完过渡 ——
 *      否则这段时间触摸/串口全卡住。过渡做成状态机，每 tick 推一步。
 *   ② 【节流重绘】face_show_parts() 是整屏 60 段 DMA（≈20ms）。动作层每 tick
 *      都可能有新参数，跟着刷会把 SPI 跑满（撕裂/白块）。这里限制 50ms 一帧。
 *   ③ 【一 tick 只重绘一次】所有参数改完统一 render()，不在中途画。
 *
 * 对上层暴露的接口（ui_app.c / face_ui_card.c 调用）：
 *   face_app_init()                  屏初始化 + 显示 neutral（带 idle 动作）
 *   face_app_set_emotion(name)       切姿势（兼容旧签名 void，ui_app 用）
 *   face_app_try_emotion(name)       同上，但返回 0/-1（face_ui_card 判"有没有"）
 *   face_app_has_pose(name)          1 = 有该姿势
 *   face_app_play(name, ms)          临时层：播 ms 毫秒后自动回落当前情绪
 *   face_app_tick(dt_ms)             推进
 ******************************************************************************/

#include <stdint.h>

#include "face_render.h"    /* face_init / face_show_parts / FACE_MAX_PARTS */
#include "face_model.h"     /* 姿势表 + 几何叠加 */
#include "face_action.h"    /* 时间 -> 参数 */

/* 过渡：7 × 40ms ≈ 280ms。步长必须 > 单帧渲染时间（≈20ms），
 * 否则一步没画完就要画下一步，SPI 会被拉满。 */
#define MORPH_STEPS         7
#define MORPH_STEP_MS       40

/* 最小重绘间隔（ms）：整屏一帧约 20ms，20ms 刷一次会让 SPI 长期跑满。 */
#define FACE_REDRAW_MIN_MS  50

/* dt 上限：主循环若被长动作/断点卡住，别把几秒的 dt 一次灌进来
 * （会让平滑直接跳到终点，看着像"闪一下"）。*/
#define FACE_DT_MAX_MS      200

/* 平滑时间常数：视线要跟得紧（扫视感），眼睛大小/嘴要慢（呼吸感）*/
#define TAU_GAZE_MS         60
#define TAU_SHAPE_MS        130

/* 眨眼时序（ms）*/
#define BLINK_CLOSE_MS      60
#define BLINK_HOLD_MS       50
#define BLINK_OPEN_MS       110
#define BLINK_TOTAL_MS      (BLINK_CLOSE_MS + BLINK_HOLD_MS + BLINK_OPEN_MS)

/* 微扫视：每 1.2~2.8s 抖一下，持续 240ms。
 * 这是"活物感"最便宜的一项 —— 静止时眼珠极轻微地动，一眼就区别于贴图。*/
#define SAC_MIN_MS          1200
#define SAC_VAR_MS          1600
#define SAC_HOLD_MS         240

/* ---------------------------------------------------------------- 状态 */
static int              g_pose_idx = -1;
static const fd_pose_t *g_pose = 0;

static fd_part_t g_to_parts[FACE_MAX_PARTS];
static int       g_to_n = 0;
static fd_part_t g_from_parts[FACE_MAX_PARTS];
static int       g_from_n = 0;

/* 当前真正画在屏上的部件（过渡的起点、以及"没变化就不重绘"的判据）*/
static fd_part_t g_cur_parts[FACE_MAX_PARTS];
static int       g_cur_n = 0;
static uint16_t  g_col = 0xFFFF;

static int      g_morph_on = 0;
static int      g_morph_step = 0;
static uint32_t g_morph_acc = 0;
static uint16_t g_morph_col_from = 0xFFFF;
static uint16_t g_morph_col_to = 0xFFFF;

static const face_act_t *g_act = 0;
static face_expr_t g_target;        /* 动作层输出 + 微扫视 */
static face_expr_t g_smooth;        /* 平滑后，实际画出去 */
static face_expr_t g_last;          /* 上次画出去的（判"要不要重绘"）*/

static uint32_t g_t = 0;            /* 全局时间（动作相位、随机数都用它）*/
static uint32_t g_redraw_acc = 0;
static uint32_t g_rng = 0x20261027u;

/* 眨眼 */
static uint32_t g_blink_wait = 2400;
static uint32_t g_blink_t = 0;
static uint8_t  g_blink_on = 0;
static uint8_t  g_blink_anim = 0;

/* 微扫视 */
static uint32_t g_sac_next = 800;
static uint32_t g_sac_until = 0;
static int16_t  g_sac_x = 0, g_sac_y = 0;

/* 无聊时偶尔打个哈欠：这是"状态驱动 + 小动作"的组合 ——
 * 心情 bored 时脸是 bored，但每 9~18s 冒一个 yawn（1.8s）再自己回落，
 * 否则一张静止的"无聊脸"看久了像死机。*/
#define GAG_MIN_MS   9000
#define GAG_VAR_MS   9000
static int      g_bored_idx = -1;
static uint32_t g_gag_at = 0;

/* 临时层（face_app_play）：播完自动回落 */
static uint32_t g_hold_ms = 0;
static const char *g_emo = "neutral";   /* 当前情绪名（回落目标）*/

/* ---------------------------------------------------------------- 强度 → 表情 */
/* 【核心表】情绪 + 强度 → 具体表情。
 *
 * 为什么要这张表：状态机对表情层只暴露 6 个情绪名，而姿势表有 30 个。若只按名字
 * 一对一映射，23 个表情永远显示不出来。但其实 emotion_manager 已经算好了
 * happiness / energy / curiosity 三个连续量（千分制），只是以前只喂给舵机增益。
 * 同一情绪在不同强度下换一张脸，是最省改动、语义也最自然的做法：
 *   happy 攒到 880 以上 -> 大笑；sad 掉到 300 以下 -> 委屈；再掉 -> 哭；
 *   angry 的 energy 高 -> 怒眼，低 -> 撇嘴赌气。
 *
 * 规则表按顺序扫描，第一条全部条件满足者胜出；每个情绪最后一条是兜底（全区间）。
 * 数值来自 emotion_manager.c 的 s_emo_states[]（neutral 500/500/500、happy 900/800/700、
 * sad 200/300/400、angry 200/900/300、surprised 600/1000/900、loving 800/600/700），
 * 阈值就是围绕这几个点定的。想调手感只改这张表。 */
/* 大小写不敏感比较（实现在下面"小工具"一节）*/
static int ci_same(const char *a, const char *b);

enum { FP_NEUTRAL = 0, FP_HAPPY, FP_SAD, FP_ANGRY, FP_SURPRISED, FP_LOVING };

typedef struct {
    uint8_t     emo;                    /* FP_* */
    int16_t     h_lo, h_hi;             /* happiness 区间（含端点）*/
    int16_t     e_lo, e_hi;             /* energy 区间 */
    int16_t     c_lo, c_hi;             /* curiosity 区间 */
    const char *pose;
} fpick_t;

static const fpick_t g_picks[] = {
    /* neutral：精力见底 -> 犯困 */
    { FP_NEUTRAL,     0, 1000,   0,  320,    0, 1000, "sleepy" },
    { FP_NEUTRAL,     0, 1000,   0, 1000,    0, 1000, "neutral" },

    /* happy：极度好奇 -> 星星眼；开心到顶 -> 大笑；否则普通笑。
     * 阈值刻意放在"标称值之上"：emo_apply(HAPPY) 给的是 900/800/700，
     * 这时应显示标准笑脸；只有被夸等刺激把它顶到 960 以上才变成大笑。 */
    { FP_HAPPY,       0, 1000,   0, 1000,  950, 1000, "star_struck" },
    { FP_HAPPY,     960, 1000, 700, 1000,    0, 1000, "laugh" },
    { FP_HAPPY,       0, 1000,   0, 1000,    0, 1000, "happy" },

    /* sad：标称 200 显示普通难过；被凶一次（80）直接哭；半路是委屈 */
    { FP_SAD,         0,  120,   0, 1000,    0, 1000, "cry" },
    { FP_SAD,         0,  180,   0, 1000,    0, 1000, "sad_anguish" },
    { FP_SAD,         0, 1000,   0, 1000,    0, 1000, "sad" },

    /* angry：energy 高 = 真怒，低了就变成撇嘴赌气 */
    { FP_ANGRY,       0, 1000, 780, 1000,    0, 1000, "angry_high" },
    { FP_ANGRY,       0, 1000, 550, 1000,    0, 1000, "angry_low" },
    { FP_ANGRY,       0, 1000,   0, 1000,    0, 1000, "pout" },

    /* surprised：惊吓（愉悦度低）-> 缩瞳；否则标准惊讶。
     * 星星眼留给"开心 + 极度好奇"，不抢惊讶的默认脸。 */
    { FP_SURPRISED,   0,  400,   0, 1000,    0, 1000, "shock" },
    { FP_SURPRISED,   0, 1000,   0, 1000,    0, 1000, "surprised" },

    /* loving：没什么力气 -> 害羞 */
    { FP_LOVING,      0, 1000,   0,  480,    0, 1000, "shy" },
    { FP_LOVING,      0, 1000,   0, 1000,    0, 1000, "loving" }
};

#define PICK_COUNT ((int)(sizeof(g_picks) / sizeof(g_picks[0])))

/* 连续状态（千分制）。默认中性：不在固件里跑时（PC 测试）也不会选到"犯困"。 */
static int16_t g_st_h = 500;
static int16_t g_st_e = 500;
static int16_t g_st_c = 500;

/* 由上层（face_ui_card.c）在每次情绪/心情变化前推入。
 * 放在这里而不是直接 include emotion_manager.h：anim 层不该依赖 emotion 层，
 * 这样 face_app.c 仍然能在 PC 上单独编译做回归。 */
void face_app_set_state(uint16_t happiness, uint16_t energy, uint16_t curiosity)
{
    g_st_h = (int16_t)((happiness > 1000) ? 1000 : happiness);
    g_st_e = (int16_t)((energy    > 1000) ? 1000 : energy);
    g_st_c = (int16_t)((curiosity > 1000) ? 1000 : curiosity);
}

static int emotion_index(const char *name)
{
    static const char *const names[6] = {
        "neutral", "happy", "sad", "angry", "surprised", "loving"
    };
    int i;

    if (name == 0) {
        return -1;
    }
    for (i = 0; i < 6; i++) {
        if (ci_same(name, names[i]) != 0) {
            return i;
        }
    }
    return -1;
}

static const char *pick_emotion_pose(uint8_t emo)
{
    int i;

    for (i = 0; i < PICK_COUNT; i++) {
        const fpick_t *r = &g_picks[i];

        if (r->emo != emo) {
            continue;
        }
        if ((g_st_h < r->h_lo) || (g_st_h > r->h_hi)) {
            continue;
        }
        if ((g_st_e < r->e_lo) || (g_st_e > r->e_hi)) {
            continue;
        }
        if ((g_st_c < r->c_lo) || (g_st_c > r->c_hi)) {
            continue;
        }
        return r->pose;
    }
    return "neutral";
}

/* ---------------------------------------------------------------- 名字映射 */
/* 上层（emotion_fsm / face_ui_card）用的是【情绪词表】，与 v9 的姿势名不完全重合：
 *   angry  -> angry_high（情绪"生气"用强档；angry_low 留给"有点不爽"的细微差别）
 *   greet  -> happy
 * 以前这些名字直接对应旧模型，v9 表情表是照参考素材命名的，所以在这里做一次映射。
 * 放在这一层而不是改数据表：姿势表是美工资产（模拟器导出），不该掺进接口词汇。 */
static const struct {
    const char *from;
    const char *to;
} g_alias[] = {
    { "angry",  "angry_high" },
    { "greet",  "happy" },
    { "idle",   "neutral" },
    { "sleeping", "asleep" },
    { 0, 0 }
};

static const char *resolve_name(const char *name)
{
    int e, i;

    if (name == 0) {
        return 0;
    }
    /* 情绪名 -> 按当前强度挑一张脸（这是 30 个表情能被触发的关键）*/
    e = emotion_index(name);
    if (e >= 0) {
        return pick_emotion_pose((uint8_t)e);
    }
    for (i = 0; g_alias[i].from != 0; i++) {
        const char *a = g_alias[i].from;
        const char *b = name;
        while ((*a != '\0') && (*b != '\0')) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
            if (ca != cb) {
                break;
            }
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return g_alias[i].to;
        }
    }
    return name;
}

static int ci_same(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    while ((*a != '\0') && (*b != '\0')) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return ((*a == '\0') && (*b == '\0')) ? 1 : 0;
}

/* ---------------------------------------------------------------- 小工具 */
static uint32_t rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16);
}

static int16_t smooth_i16(int16_t cur, int16_t tgt, uint32_t dt, uint16_t tau)
{
    int32_t d = (int32_t)tgt - cur;
    int32_t k;

    if (tau == 0) {
        return tgt;
    }
    k = (int32_t)((dt * 256u) / tau);
    if (k > 255) {
        k = 255;
    }
    return (int16_t)(cur + d * k / 256);
}

static int expr_same(const face_expr_t *a, const face_expr_t *b)
{
    return (a->gaze_dx == b->gaze_dx) && (a->gaze_dy == b->gaze_dy) &&
           (a->blink == b->blink) && (a->eye_scale == b->eye_scale) &&
           (a->mouth_open == b->mouth_open);
}

/* ---------------------------------------------------------------- 渲染 */
/* 把 g_cur_parts 交给屏幕。三处调用共用，保证"一帧只发一次 DMA"。 */
static void show_current(int n, uint16_t col)
{
    g_cur_n = n;
    g_col = col;
    face_model_apply(g_cur_parts, n, &g_smooth);
    face_show_parts(g_cur_parts, n, col);
}

/* 过渡中的一帧：几何与颜色同时在两头之间插值 */
static void show_morph(uint8_t t)
{
    int n = face_model_mix(g_from_parts, g_from_n, g_to_parts, g_to_n,
                           t, g_cur_parts, FACE_MAX_PARTS);
    show_current(n, face_model_lerp_color(g_morph_col_from, g_morph_col_to, t));
}

/* 稳态：直接画目标姿势 */
static void show_pose(void)
{
    int i;
    for (i = 0; i < g_to_n; i++) {
        g_cur_parts[i] = g_to_parts[i];
    }
    show_current(g_to_n, g_morph_col_to);
}

/* ---------------------------------------------------------------- 换姿势 */
/* 返回 0 = 切了/本来就是它，-1 = 没这个姿势 */
static int apply_pose(const char *name)
{
    int idx = face_model_find(resolve_name(name));
    const fd_pose_t *ps;
    int i;

    if (idx < 0) {
        return -1;
    }
    /* 同一个姿势直接返回 —— 过渡【进行中】也要返回。
     * 这个条件曾经写成 `idx == g_pose_idx && g_morph_on == 0`，于是过渡还没走完时
     * 情绪层再推一次同名姿势，就会把 morph_step 清零重来；而 emotion_fsm 是每 10ms
     * 推一次，结果过渡永远走不完，表情卡在中间不动。 */
    if (idx == g_pose_idx) {
        return 0;
    }

    ps = face_model_get(idx);

    /* 起点 = 屏上正在显示的那一帧（不是上个姿势的目标值）。
     * 用目标值当起点的话，动作切到一半再切姿势会跳一下。 */
    g_from_n = g_cur_n;
    for (i = 0; i < g_cur_n; i++) {
        g_from_parts[i] = g_cur_parts[i];
    }
    g_morph_col_from = g_col;

    g_to_n = face_model_build(ps, g_to_parts, FACE_MAX_PARTS);
    g_morph_col_to = ps->col;

    g_pose_idx = idx;
    g_pose = ps;
    g_act = face_act_find(ps->name);

    if (g_from_n == 0) {            /* 开机第一帧：直接到位，不做过渡 */
        g_morph_on = 0;
        show_pose();
    } else {
        g_morph_on = 1;
        g_morph_step = 0;
        g_morph_acc = 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- 对外 */
void face_app_init(void)
{
    face_init();

    face_expr_reset(&g_target);
    face_expr_reset(&g_smooth);
    face_expr_reset(&g_last);
    g_last.eye_scale = 0;           /* 强制第一帧判为"有变化" */

    g_pose_idx = -1;
    g_bored_idx = face_model_find("bored");
    g_gag_at = 0;
    g_cur_n = 0;
    g_morph_on = 0;
    g_hold_ms = 0;
    g_blink_on = 0;
    g_blink_anim = 0;
    g_blink_wait = 2400;
    g_t = 0;
    g_redraw_acc = 0;

    (void)apply_pose("neutral");
}

int face_app_has_pose(const char *name)
{
    return (face_model_find(resolve_name(name)) >= 0) ? 1 : 0;
}

int face_app_try_emotion(const char *name)
{
    if (name == 0 || name[0] == '\0') {
        return -1;
    }
    /* 临时表情期间，情绪层每 10ms 都会把"当前情绪"重新推过来一次。
     * 若这个名字和进入临时表情前的情绪相同，就当作没变化 —— 否则临时表情活不过一拍
     * （这正是 HANDOVER §7-1 那个"抚摸表情被情绪同步抢走"的老问题）。 */
    if (g_hold_ms != 0) {
        if (ci_same(name, g_emo)) {
            return 0;
        }
        g_hold_ms = 0;              /* 真的换情绪了：临时表情让位 */
    }
    g_emo = name;
    return apply_pose(name);
}

/* 结束临时表情，立刻回落到当前情绪（抚摸结束时用，不必等它自然到期） */
void face_app_hold_end(void)
{
    if (g_hold_ms != 0) {
        g_hold_ms = 0;
        (void)apply_pose(g_emo);
    }
}

/* 兼容 ui_app.c 既有的 void 调用约定 */
void face_app_set_emotion(const char *name)
{
    (void)face_app_try_emotion(name);
}

/* 临时表情：播 ms 毫秒后自动回落到当前情绪。
 * 抚摸 / 聆听 / 思考这类"事件性"表情都走这里，不需要各自一套状态。 */
int face_app_play(const char *name, uint32_t ms)
{
    if (face_model_find(resolve_name(name)) < 0) {
        return -1;
    }
    (void)apply_pose(name);
    g_hold_ms = ms;
    return 0;
}

void face_app_tick(uint32_t dt_ms)
{
    uint8_t need = 0;

    if (dt_ms == 0) {
        dt_ms = 1;
    }
    if (dt_ms > FACE_DT_MAX_MS) {
        dt_ms = FACE_DT_MAX_MS;
    }
    g_t += dt_ms;

    /* ⓪ 临时表情到期 -> 回落情绪 */
    if (g_hold_ms != 0) {
        if (g_hold_ms > dt_ms) {
            g_hold_ms -= dt_ms;
        } else {
            g_hold_ms = 0;
            (void)apply_pose(g_emo);
        }
    }

    /* ⓪-b 无聊时偶尔打个哈欠（只在稳态时触发，不打断过渡/临时表情）*/
    if ((g_bored_idx >= 0) && (g_hold_ms == 0) && (g_morph_on == 0) &&
        (g_pose_idx == g_bored_idx)) {
        if (g_gag_at == 0) {
            g_gag_at = g_t + GAG_MIN_MS + (rnd() % GAG_VAR_MS);
        } else if (g_t >= g_gag_at) {
            g_gag_at = 0;
            (void)face_app_play("yawn", 1800);
            return;                 /* 下一拍再画哈欠的第一帧 */
        }
    } else {
        g_gag_at = 0;
    }

    /* ① 过渡：每 MORPH_STEP_MS 推一步，期间不推进动作 */
    if (g_morph_on) {
        g_morph_acc += dt_ms;
        if (g_morph_acc >= MORPH_STEP_MS) {
            g_morph_acc -= MORPH_STEP_MS;
            g_morph_step++;
            if (g_morph_step >= MORPH_STEPS) {
                g_morph_on = 0;
                show_pose();
            } else {
                show_morph((uint8_t)(g_morph_step * 255 / MORPH_STEPS));
            }
            g_last = g_smooth;
        }
        return;
    }

    /* ② 动作层：时间 -> 目标参数 */
    face_act_sample(g_act, g_t, &g_target);

    /* ③ 微扫视：叠在动作的视线上 */
    if (g_t >= g_sac_next) {
        g_sac_next = g_t + SAC_MIN_MS + (rnd() % SAC_VAR_MS);
        g_sac_until = g_t + SAC_HOLD_MS;
        g_sac_x = (int16_t)((int32_t)(rnd() % 25) - 12);
        g_sac_y = (int16_t)((int32_t)(rnd() % 9) - 4);
    }
    if (g_t < g_sac_until) {
        g_target.gaze_dx = (int16_t)(g_target.gaze_dx + g_sac_x);
        g_target.gaze_dy = (int16_t)(g_target.gaze_dy + g_sac_y);
    }

    /* ④ 眨眼状态机（按时间算，不按帧数 —— 帧率变了速度也不变）*/
    if (g_blink_on == 0) {
        if (g_blink_wait > dt_ms) {
            g_blink_wait -= dt_ms;
        } else {
            g_blink_on = 1;
            g_blink_t = 0;
            /* 10% 概率连眨两下：真人很少均匀地眨 */
            g_blink_wait = ((rnd() % 100) < 10)
                           ? (260 + rnd() % 160)
                           : (2200 + rnd() % 2800);
        }
    } else {
        g_blink_t += dt_ms;
        if (g_blink_t < BLINK_CLOSE_MS) {
            g_blink_anim = (uint8_t)(255u * g_blink_t / BLINK_CLOSE_MS);
        } else if (g_blink_t < BLINK_CLOSE_MS + BLINK_HOLD_MS) {
            g_blink_anim = 255;
        } else if (g_blink_t < BLINK_TOTAL_MS) {
            uint32_t k = g_blink_t - BLINK_CLOSE_MS - BLINK_HOLD_MS;
            g_blink_anim = (uint8_t)(255u - 255u * k / BLINK_OPEN_MS);
        } else {
            g_blink_on = 0;
            g_blink_anim = 0;
        }
    }

    /* ⑤ 平滑：视线快、形状慢；眨眼不过平滑（要的就是"啪"一下）*/
    g_smooth.gaze_dx = smooth_i16(g_smooth.gaze_dx, g_target.gaze_dx, dt_ms, TAU_GAZE_MS);
    g_smooth.gaze_dy = smooth_i16(g_smooth.gaze_dy, g_target.gaze_dy, dt_ms, TAU_GAZE_MS);
    g_smooth.eye_scale = smooth_i16(g_smooth.eye_scale, g_target.eye_scale, dt_ms, TAU_SHAPE_MS);
    {
        int16_t m = smooth_i16(g_smooth.mouth_open, g_target.mouth_open, dt_ms, TAU_SHAPE_MS);
        g_smooth.mouth_open = (uint8_t)((m < 0) ? 0 : m);
    }
    {
        int16_t b = g_target.blink;
        if ((int16_t)g_blink_anim > b) {
            b = (int16_t)g_blink_anim;
        }
        g_smooth.blink = (uint8_t)((b < 0) ? 0 : b);
    }

    /* ⑥ 变化了才重绘，且不高于 20fps */
    if (expr_same(&g_smooth, &g_last) == 0) {
        need = 1;
    }
    if (need != 0) {
        g_redraw_acc += dt_ms;
        if (g_redraw_acc >= FACE_REDRAW_MIN_MS) {
            /* 减去而不是清零：20ms 的 tick 下清零会让实际间隔变成 60ms（16fps），
             * 减去才稳定在 50ms（20fps）。 */
            g_redraw_acc -= FACE_REDRAW_MIN_MS;
            g_last = g_smooth;
            show_pose();
        }
    }
}
