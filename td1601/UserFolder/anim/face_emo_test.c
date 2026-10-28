/******************************************************************************
 * @file     face_emo_test.c
 * @brief    参数化脸 · 串口状态机测试（双轨方案的"参数化"一侧）
 *
 * 与 anim_emo_test.c（GIF 位图版）的区别：
 *   · 表情由几何参数【实时绘制】（face_render），无位图、无解压、零传输
 *   · 切换情绪 = 【参数插值过渡】（约 300ms），而不是硬切 -> 这才是"像一个人"
 *   · 可随时叠加【眨眼】与【视线偏移】（GIF 版做不到这两项）
 *
 * 串口（115200-8N1，行尾只认 CR/LF，空格只作分词）：
 *     help / list / stat / combo
 *     play <n|name-prefix>                            直接播一个 face clip
 *     emo <neutral|happy|sad|angry|surprised|loving>   情绪 -> combo（带过渡）
 *     intent <idle|listen|speak|think|greet|sleep|wake|eat|play|proud>
 *     blink [0..255]                                  不带参数 = 眨一下
 *     gaze <dx> <dy>                                  视线偏移（像素，-8..8）
 *     mode <manual|auto>
 *   兼容 PC 协议：EMO:HAPPY / INTENT:IDLE（大小写不敏感）
 *
 * 入口：face_emo_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "face_render.h"
#include "face_model.h"
#include "face_action.h"            /* 行为层：把"表情"变成"会动的动作" */
#include "face_ui.h"                /* 对外接口：给情感状态机（emotion_fsm）驱动 */

/* 位图管线（ANM3）对照实验。默认关：它需要 package.yaml 里的
 * libanimres.a（~1MB），烧录会非常慢。要做“gif vs face 谁在闪”
 * 对比时，把这里改成 1，并在 package.yaml 打开那 3 个位图源文件 + animres。 */
#define FACE_USE_GIF   0
#if FACE_USE_GIF
#include "anim_player.h"
#endif
#include <drv/uart.h>
#include "console/sys_console.h"   /* console.uart */
#include <drv/common.h>             /* mdelay */

/* 控制台实例（src/board_init.c 中定义并初始化）*/
extern sys_console_t console;

#define FACE_TICK_MS    20          /* 主循环步进：命令响应 <= 20ms */
#define FACE_CLIP_MS    40          /* 画面推进间隔 = 25fps，与素材帧率一致。
                                     * 每帧都要走整段 DMA，20ms 一拍会让 SPI 长期
                                     * 跑满 -> 画面撕裂/边缘冒白块。 */
#define RX_MAX          64
#define LOOPS_PER_CLIP  1           /* 每个 clip 播几轮再走 combo 下一步
                                     * 1 = 做一次就换下一个，像“自然反应”；
                                     * 调成 2 会明显变机械（重复同一动作）*/
#define COMBO_MAX       4
#define VARIANT_MAX     2
#define MORPH_STEPS     10          /* 过渡步数 */
#define MORPH_STEP_MS   30          /* 每步时长 -> 过渡约 300ms */

/* 模型模式的最小重绘间隔（ms）。
 * 为什么需要节流：整屏刷新是 60 段 DMA，一帧 ≈20ms。动作层每 20ms 都可能产出
 * 新参数，若跟着 20ms 刷一次，SPI 就长期跑满 —— 这是"画面撕裂 / 边缘冒白块"
 * 的老坑（见 FACE_CLIP_MS 的注释）。视线动画本身很慢，50ms 一帧（20fps）足够
 * 顺滑，还留一半总线带宽给舵机 DMA 让路。 */
#define MODEL_REDRAW_MIN_MS 50

/* 眨眼动画步数（每步一次重绘，约 20ms）：闭合 2 步 + 睁开 3 步 ≈ 100ms。
 * 真实眨眼约 100~150ms，且几乎不在闭眼状态停留 —— 原来"压扁后停 80ms"
 * 的做法就是“迟钝感”的来源。 */
#define BLINK_CLOSE_STEPS  2
#define BLINK_OPEN_STEPS   3

/* ---------------- combo（片段名前缀序列 + 变体池）---------------- */
typedef struct {
    const char *clips[COMBO_MAX];
} fcombo_t;

typedef struct {
    const char *key;
    fcombo_t    variants[VARIANT_MAX];
    uint8_t     n_variants;
} fentry_t;

static const fentry_t s_emos[] = {
    { "neutral",   { { { "smile_static", "ponder", "confident", 0 } },
                     { { "ponder", "smile_static", 0 } } }, 2 },
    { "happy",     { { { "smile", "laugh", "snigger", 0 } },
                     { { "dance", "smile", "laugh", 0 } } }, 2 },
    { "sad",       { { { "sad", "sigh", "cry", 0 } },
                     { { "sigh", "sad", "ponder", 0 } } }, 2 },
    { "angry",     { { { "angry", "mock", "mute", 0 } },
                     { { "angry", "mute", 0 } } }, 2 },
    { "surprised", { { { "shocked", "panic", "question", 0 } },
                     { { "question", "shocked", 0 } } }, 2 },
    { "loving",    { { { "shy", "yummy", "smile", 0 } },
                     { { "shy", "smile", 0 } } }, 2 },
    { 0, { { { 0 } } }, 0 }
};

static const fentry_t s_intents[] = {
    { "idle",   { { { "ponder", "smile_static", 0 } } }, 1 },
    { "listen", { { { "ponder", "question", 0 } } }, 1 },
    { "speak",  { { { "laugh", "smile", "snigger", 0 } } }, 1 },
    { "think",  { { { "investigate", "ponder", 0 } } }, 1 },
    { "greet",  { { { "dance", "smile", 0 } } }, 1 },
    { "sleep",  { { { "asleep", "yawn", 0 } } }, 1 },
    { "wake",   { { { "yawn", "shocked", "smile", 0 } } }, 1 },
    { "eat",    { { { "yummy", "smile", 0 } } }, 1 },
    { "play",   { { { "badminton", "laugh", 0 } } }, 1 },
    { "proud",  { { { "confident", "smile", 0 } } }, 1 },
    { 0, { { { 0 } } }, 0 }
};

/* ---------------- pose（姿势）= 【模型】+【动作】----------------
 * 这是本次扩展的核心抽象：
 *   模型（face_model.c）  只管"这张脸长什么样"，是静态的；
 *   动作（face_action.c）只管"参数随时间怎么变"，不关心长相；
 *   pose 把两者绑在一起，于是"加一个新表情"= 加一条模型 + 可能加一个动作 + 这里一行。
 *
 * 上面两张表（s_emos / s_intents）是【逐帧素材轨道】的 combo（GIF 逆向的 23 个 clip），
 * 现在仍然保留：`emo` / `intent` 命令会先查 pose 表，命不中才回落到 combo。
 *
 * act 填 0 = 只显示静态模型（例如 asleep 本来就是"不动的睡脸"）。 */
typedef struct {
    const char *name;           /* 姿势名（串口 / 情绪名 / 意图名，映射到这里）*/
    const char *model;          /* 用哪个模型 */
    const char *action;         /* 配哪个动作（0 = 静态）*/
} fpose_t;

static const fpose_t s_poses[] = {
    /* ---- 情绪（与 emotion_manager 的 6 个同名）+ 心情 ---- */
    { "neutral",   "neutral",   "idle" },
    { "happy",     "happy",     "idle" },
    { "sad",       "sad",       "idle" },
    { "angry",     "angry",     "idle" },
    { "surprised", "surprised", "idle" },
    { "loving",    "loving",    "pet"  },      /* 爱意 = 眯眼蹭（复用被抚摸的动作）*/
    { "asleep",    "asleep",     0     },      /* 睡脸不动，留给动作层的只有呼吸（未做）*/

    /* ---- 意图（INTENT:xxx）专用表情：PC 下发 think 就真的"在想" ---- */
    { "think",     "think",     "think" },
    { "listen",    "neutral",   "wait" },
    { "question",  "surprised", "idle"  },     /* 抬眉 + 瞪眼 = 疑问 */
    { "greet",     "happy",     "idle"  },

    /* ---- 交互事件（触发源后续接入；先用串口 pose pet / pose look 验证观感）---- */
    { "pet",       "pet",       "pet"  },
    { "look",      "look",      "look_around" },
    { 0, 0, 0 }
};

/* ---------------- 状态 ---------------- */
static char        s_rx[RX_MAX];
static uint8_t     s_rxlen = 0;
static uint32_t    s_now = 0;
static uint8_t     s_mode = 0;          /* 0=manual 1=auto 2=cycle（纯轮播全部素材）*/
static uint16_t    s_clip_acc = 0;      /* 画面推进累加器（限帧用）*/
static uint8_t     s_hold = 0;          /* 1 = 冻结画面（排查闪烁用：静止时还闪就是面板/时钟侧）*/
static int         s_clip = -1;
static uint8_t     s_playing = 0;

static const fentry_t *s_entry = 0;
static const fcombo_t *s_combo = 0;
static uint8_t     s_seq = 0;
static uint8_t     s_loops = 0;
static int8_t      s_last_variant = -1;
static uint32_t    s_rng = 0x20260926u;

/* 眨眼动画：0=等诗 1=闭合 2=睁开。像真眼一样“快闭快开”，中间不停留。 */
static uint16_t    s_blink_wait = 2000;     /* 下次眨眼倒计时（ms）*/
static uint8_t     s_blink_ph = 0;
static uint8_t     s_blink_st = 0;
static int8_t      s_gaze_x = 0;
static int8_t      s_gaze_y = 0;

/* ---------------- 参数化模型模式（face_model）----------------
 * s_use_model = 1 时不用逐帧素材，直接用“左眼/右眼/嘴 三个对象 + 控制参数”算出来画。
 * 与逐帧路线共存，用 mode model / mode cycle 切换。 */
static uint8_t     s_use_model = 0;
static uint8_t     s_ui_mode = 0;               /* 1 = 由情感状态机驱动（屏幕侧不再自己读串口）*/
static int         s_model_idx = 0;             /* face_model_find() 的下标 */
static face_expr_t s_expr;                      /* 最终交给 face_model_build 的参数 */
static face_part_t s_parts[FACE_MAX_PARTS];     /* 模型算出的几何部件 */

/* ---- 参数合成（动作层 + 眨眼动画 + 用户覆盖，见 model_compose）----
 * s_act_expr  : 动作层（face_action）的输出，纯粹的"此刻该这样"
 * s_anim_blink: 眨眼动画的当前值（快闭快开那套）
 * s_user_*    : 串口命令的静态覆盖（blink <v> / escal / mopen）
 * 三者合成后才进 s_expr，所以"眨眼动画"和"眯眼动作"可以叠在一起（取较大值）。*/
static face_expr_t s_act_expr;
static face_expr_t s_last_expr;                 /* 上次真正画出去的参数（判断要不要重绘）*/
static uint8_t     s_anim_blink = 0;
static uint16_t    s_redraw_acc = 0;            /* 重绘节流累加 */
static int16_t     s_user_eye_scale = 0;        /* 0 = 不覆盖模型/动作的值 */
static uint8_t     s_user_mouth_open = 0;
static uint8_t     s_user_blink = 0;
static const char *s_pose = "neutral";          /* 当前姿势名（打印用）*/

#if FACE_USE_GIF
/* ---------------- 对照实验：位图管线 ----------------
 * 敲 gif 切到 anim_player（ANM3 解压 + 同样的 LCD 原语），face 切回。
 * 用途：把“闪烁到底在渲染侧还是驱动/面板侧”一刀切开。
 * 注意：默认关闭，因为 libanimres.a 有 ~1MB，烧录很慢（见 FACE_USE_GIF）。 */
static uint8_t     s_use_gif = 0;
static uint8_t     s_gif_inited = 0;
static int         s_gif_idx = 0;
static uint16_t    s_gif_acc = 0;
#endif

/* ---------------- 小工具 ---------------- */
static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_prefix(const char *s, const char *key)
{
    if (s == 0 || key == 0) {
        return 0;
    }
    while (*key) {
        if (lc(*s) != lc(*key)) {
            return 0;
        }
        s++;
        key++;
    }
    return 1;
}

static int ci_eq(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    while (*a && *b) {
        if (lc(*a) != lc(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

static int split_args(char *line, char *argv[], int maxn)
{
    int n = 0;
    char *p = line;
    while (*p && n < maxn) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    return n;
}

static int to_int(const char *s)
{
    int v = 0;
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (int)(*s - '0');
        s++;
    }
    return neg ? -v : v;
}

static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 16);
}

/* ---------------- clip 查找 ---------------- */
static const face_clip_t *find_clip(const char *pat)
{
    int i;
    for (i = 0; i < (int)FACE_COUNT; i++) {
        if (ci_prefix(g_face_clips[i].name, pat)) {
            return &g_face_clips[i];
        }
    }
    return 0;
}

static void check_resources(void)
{
    int i;
    int bad = 0;
    for (i = 0; i < (int)FACE_COUNT; i++) {
        if (g_face_clips[i].name == 0 || g_face_clips[i].n_frames < 2 ||
            g_face_clips[i].frames == 0 || g_face_clips[i].total_ms == 0) {
            printf("[femo] BAD clip #%d\r\n", i);
            bad++;
        }
    }
    printf("[femo] resource check: %d/%u clips OK\r\n",
           (int)FACE_COUNT - bad, (unsigned)FACE_COUNT);
}

/* ---------------- 过渡（丝滑切换的核心）---------------- */
static face_part_t  s_from[FACE_MAX_PARTS];
static face_frame_t s_from_f;

static void morph_to(const face_clip_t *clip)
{
    const face_part_t *cur;
    int n, i, step;

    if (clip == 0 || clip->n_frames == 0) {
        return;
    }
    /* 抓当前【基础姿态】快照：从"现在这样"过渡到目标片段的第 0 关键帧。
     * 必须用 face_get_base() 而不是 face_current()：后者已叠加 amp/眨眼/视线，
     * 若拿它当起点，apply_extras 会再叠加一次（眼睛被压两次、视线偏移两倍），
     * 切换瞬间眼睛就会跳。 */
    cur = face_get_base(&n);
    if (n > FACE_MAX_PARTS) {
        n = FACE_MAX_PARTS;
    }
    for (i = 0; i < n; i++) {
        s_from[i] = cur[i];
    }
    s_from_f.n_parts = (uint8_t)n;
    s_from_f.p = s_from;

    for (step = 0; step <= MORPH_STEPS; step++) {
        face_show_mix(&s_from_f, &clip->frames[0], (uint8_t)(step * 255 / MORPH_STEPS));
        mdelay(MORPH_STEP_MS);
    }
}

/* ---------------- combo 播放 ---------------- */
static uint8_t pick_variant(const fentry_t *e)
{
    uint8_t v;
    if (e->n_variants <= 1) {
        return 0;
    }
    v = (uint8_t)(rnd() % (uint32_t)e->n_variants);
    if (v == (uint8_t)s_last_variant) {
        v = (uint8_t)((v + 1) % e->n_variants);
    }
    return v;
}

static void start_clip(const face_clip_t *c, int do_morph)
{
    if (c == 0) {
        return;
    }
    if (do_morph) {
        morph_to(c);                    /* 平滑过渡到目标 */
    }
    face_clip_open(c);
    s_playing = 1;
    s_clip = (int)(c - g_face_clips);
    printf("[femo] play #%d %s (%u key frames)\r\n", s_clip,
           (c->name != 0) ? c->name : "?", (unsigned)c->n_frames);
}

static void play_entry(const fentry_t *e)
{
    uint8_t v = pick_variant(e);
    s_mode = 0;                         /* 手动/emo 命令一律切出 cycle 模式 */
    s_use_model = 0;                    /* 也切出参数化模型模式，否则 tick 不推进素材 */
    s_last_variant = (int8_t)v;
    s_entry = e;
    s_combo = &e->variants[v];
    s_seq = 0;
    s_loops = 0;
    printf("[femo] %s variant %u/%u: %s", e->key,
           (unsigned)(v + 1), (unsigned)e->n_variants,
           (s_combo->clips[0] != 0) ? s_combo->clips[0] : "?");
    {
        int k = 1;
        while (s_combo->clips[k] != 0) {
            printf(" -> %s", s_combo->clips[k]);
            k++;
        }
    }
    printf("\r\n");
    start_clip(find_clip(s_combo->clips[0]), 1);
}

static void clip_advance(void)
{
    const face_clip_t *c;

    if (s_combo == 0) {
        return;
    }
    s_loops++;
    if (s_loops < LOOPS_PER_CLIP) {
        return;
    }
    s_loops = 0;
    s_seq++;
    if (s_combo->clips[s_seq] == 0) {
        if (s_entry != 0 && s_entry->n_variants > 1) {
            play_entry(s_entry);                /* 换成另一组变体 */
        } else {
            s_seq = 0;
            start_clip(find_clip(s_combo->clips[0]), 0);   /* 同组重播，不再过渡 */
        }
        return;
    }
    c = find_clip(s_combo->clips[s_seq]);
    start_clip(c, 0);                           /* combo 内部切换不做过渡（本来就连贯）*/
}

static void auto_next(void)
{
    int n = 0;
    if (s_emos[0].key == 0) {
        return;
    }
    while (s_emos[n].key != 0) {
        n++;
    }
    play_entry(&s_emos[rnd() % (uint32_t)n]);
}

/* ---------------- 纯轮播：把全部素材按顺序完整播一遍 ---------------- */
static int s_cycle_i = 0;

static void cycle_next(int via_morph)
{
    const face_clip_t *c = &g_face_clips[s_cycle_i];
    printf("[femo] cycle %d/%u '%s'\r\n", s_cycle_i + 1, (unsigned)FACE_COUNT,
           (c->name != 0) ? c->name : "?");
    start_clip(c, via_morph);
    s_cycle_i = (s_cycle_i + 1) % (int)FACE_COUNT;
}

static void cycle_start(void)
{
    s_mode = 2;
    s_combo = 0;
    s_entry = 0;
    s_loops = 0;
    s_cycle_i = 0;
    printf("[femo] mode=cycle: play all %u clips in order\r\n", (unsigned)FACE_COUNT);
    cycle_next(1);
}

/* 用模型 + 参数算一遍并画出来 */
static void model_redraw(void)
{
    const face_model_t *m = face_model_get(s_model_idx);
    int n = face_model_build(m, &s_expr, s_parts, FACE_MAX_PARTS);
    face_show_parts(s_parts, n);
}

/* 用户用 gaze 命令设的“视线基准”。动作层是在它上面【叠加】偏移，
 * 而不是直接覆写 gaze_dx/dy —— 否则动作一动，敲过的 gaze 就没用了。 */
static int16_t s_gaze_base_dx = 0;
static int16_t s_gaze_base_dy = 0;

/* 两份参数是否相同（决定要不要重绘；避免"参数没变也刷一遍整屏"）*/
static int expr_same(const face_expr_t *a, const face_expr_t *b)
{
    return ((a->gaze_dx == b->gaze_dx) && (a->gaze_dy == b->gaze_dy) &&
            (a->blink == b->blink) && (a->eye_scale == b->eye_scale) &&
            (a->mouth_open == b->mouth_open) &&
            (a->mouth_scale == b->mouth_scale)) ? 1 : 0;
}

/* ---------------- 参数合成：动作层 + 眨眼动画 + 用户覆盖 ----------------
 * 三层优先级不同，合并规则也就不同：
 *   · 视线：动作层的值 + 用户 gaze 命令设的基准（叠加，基准是"看向哪边"的大方向）
 *   · 闭眼：三者取【较大值】—— 动作层要"眯眼享受"（pet 的 blink 80~140），
 *           眨眼动画要"瞬间闭上"（255），静态命令 blink <v> 是调试用，取大者
 *           才能让"眯着眼 + 随机眨眼"这两件事同时成立。
 *   · 眼睛大小 / 张嘴：用户命令优先（调试时的临时放大不能被动作顶掉）。
 * 返回 1 = 参数有变化、值得重绘。 */
static int model_compose(void)
{
    face_expr_t e;
    int changed;

    e.gaze_dx = (int16_t)(s_act_expr.gaze_dx + s_gaze_base_dx);
    e.gaze_dy = (int16_t)(s_act_expr.gaze_dy + s_gaze_base_dy);

    e.blink = s_act_expr.blink;
    if (s_anim_blink > e.blink) {
        e.blink = s_anim_blink;
    }
    if (s_user_blink > e.blink) {
        e.blink = s_user_blink;
    }

    e.eye_scale = (s_user_eye_scale > 0) ? s_user_eye_scale : s_act_expr.eye_scale;
    if (e.eye_scale <= 0) {
        e.eye_scale = 100;
    }

    e.mouth_open = (s_user_mouth_open > s_act_expr.mouth_open)
                       ? s_user_mouth_open : s_act_expr.mouth_open;
    e.mouth_scale = 100;

    changed = expr_same(&s_last_expr, &e) ? 0 : 1;
    if (changed != 0) {
        s_last_expr = e;
    }
    s_expr = e;                 /* s_expr 始终是最新值（重绘时直接用）*/
    return changed;
}

/* ---------------- 模型之间的过渡（切表情不再"硬切"）----------------
 * 逐帧轨道早就有 morph（morph_to），但模型模式一直是"参数一改就换一张脸"，
 * 观感上是硬跳。这里把【旧模型的基础姿态】插值到【新模型的基础姿态】，
 * 复用渲染层的 face_show_mix()（它内部按面积分组配对，左眼配左眼、嘴配嘴）。
 *
 * 起点必须用 face_get_base()：face_current() 已经叠过眨眼/视线/增益，
 * 拿它当起点会被再叠一次（切换瞬间眼睛跳、视线偏移两倍）。 */
static face_part_t  s_morph_from[FACE_MAX_PARTS];
static face_part_t  s_morph_to[FACE_MAX_PARTS];
static face_frame_t s_morph_from_f;
static face_frame_t s_morph_to_f;

static void model_morph_to(int new_idx)
{
    const face_model_t *nm = face_model_get(new_idx);
    const face_part_t *cur;
    face_expr_t zero;
    int i, n, m, step;

    if (nm == 0) {
        return;
    }

    cur = face_get_base(&n);
    if (n > FACE_MAX_PARTS) {
        n = FACE_MAX_PARTS;
    }
    for (i = 0; i < n; i++) {
        s_morph_from[i] = cur[i];
    }
    s_morph_from_f.n_parts = (uint8_t)n;
    s_morph_from_f.p = s_morph_from;

    face_expr_reset(&zero);
    m = face_model_build(nm, &zero, s_morph_to, FACE_MAX_PARTS);
    s_morph_to_f.n_parts = (uint8_t)m;
    s_morph_to_f.p = s_morph_to;

    for (step = 0; step <= MORPH_STEPS; step++) {
        face_show_mix(&s_morph_from_f, &s_morph_to_f,
                      (uint8_t)(step * 255 / MORPH_STEPS));
        mdelay(MORPH_STEP_MS);
    }
}

/* ---------------- pose 查找 / 切换 ---------------- */
/* 心情名 -> 姿势名的翻译（定义在文件末尾的 face_ui 部分，handle_line 里要用）*/
static const char *ui_model_name(const char *name);

static const fpose_t *pose_find(const char *name)
{
    int i;

    if (name == 0) {
        return 0;
    }
    for (i = 0; s_poses[i].name != 0; i++) {
        if (ci_eq(s_poses[i].name, name) != 0) {
            return &s_poses[i];
        }
    }
    return 0;
}

/* 切到一个姿势（模型 + 动作）。返回 0 = 成功，-1 = 姿势/模型不存在。 */
static int model_set_pose(const fpose_t *p)
{
    const face_act_t *act;
    face_expr_t from;
    int idx;

    if (p == 0) {
        return -1;
    }
    idx = face_model_find(p->model);
    if (idx < 0) {
        printf("[femo] pose '%s' -> no model '%s'\r\n", p->name, p->model);
        return -1;
    }

    /* 已经是这个姿势就直接返回：状态机每拍都会调 face_ui_emotion()，
     * 不短路的话会每 10ms 重绘一次并刷一行日志。 */
    if ((s_use_model != 0) && (idx == s_model_idx) && (ci_eq(s_pose, p->name) != 0)) {
        return 0;
    }

    /* 动作从"现在这样"接着滑（而不是从模型的中间位出发），切换才不跳 */
    from = s_act_expr;

    if ((s_use_model == 0) || (idx != s_model_idx)) {
        model_morph_to(idx);            /* 约 10×30ms 的参数插值过渡 */
        s_model_idx = idx;
    }
    s_use_model = 1;

    /* 换姿势时清掉上一次的静态覆盖，免得 escal / mopen 一直粘在新脸上 */
    s_user_eye_scale = 0;
    s_user_mouth_open = 0;
    s_user_blink = 0;
    s_anim_blink = 0;
    s_redraw_acc = 0;

    act = (p->action != 0) ? face_act_find(p->action) : 0;
    if (act != 0) {
        face_act_start(act, &from);
    } else {
        face_act_stop();
        face_expr_reset(&s_act_expr);   /* 静态表情：参数归零，只留模型本身 */
    }

    s_pose = p->name;
    (void)model_compose();
    model_redraw();
    printf("[femo] pose=%s model=%s act=%s\r\n",
           p->name, p->model, (act != 0) ? act->name : "-");
    return 0;
}

static void apply_key(const fentry_t *table, const char *name)
{
    int i;
    for (i = 0; table[i].key != 0; i++) {
        if (ci_eq(table[i].key, name)) {
            play_entry(&table[i]);
            return;
        }
    }
    printf("[femo] unknown name '%s' (type help)\r\n", name);
}

/* ---------------- 命令 ---------------- */
static void print_help(void)
{
    printf("[femo] commands:\r\n");
    printf("      help / list / stat / combo\r\n");
    printf("      play <n|name-prefix>\r\n");
    printf("      emo <neutral|happy|sad|angry|surprised|loving>\r\n");
    printf("      intent <idle|listen|speak|think|greet|sleep|wake|eat|play|proud>\r\n");
    printf("      blink [0..255]   gaze <dx> <dy>   mode <manual|auto>\r\n");
    printf("      amp <50..300>    size <50..300>    speed <25..400>   (live)\r\n");
    printf("        amp = motion only, size = feature size, both default to fit screen\r\n");
    printf("      mode <manual|auto|cycle>   cycle = play all %u clips in order\r\n",
           (unsigned)FACE_COUNT);
    printf("      hold [0|1]   freeze the picture (debug flicker: if it still\r\n");
    printf("                   flickers while frozen, it is panel/clock side)\r\n");
    printf("      gif [name]      **COMPARE**: switch to bitmap pipeline (ANM3)\r\n");
    printf("      face            switch back to the parametric model\r\n");
    printf("      mode model      switch to the 3-object parametric model\r\n");
    printf("      model <name>    pick a pose/model (type 'models' / 'poses')\r\n");
    printf("      models          list parametric models\r\n");
    printf("      pose <name>     model + action, e.g. think / pet / look\r\n");
    printf("      poses           list poses (model + action)\r\n");
    printf("      act <name>      change action only (type 'acts' to list)\r\n");
    printf("      acts            list actions (idle/look_around/think/pet/wait)\r\n");
    printf("      mopen <0..255>  escal <50..200>   reset   (model params)\r\n");
    printf("      segrows <1..8>   DMA rows per segment (debug flicker: if the\r\n");
    printf("                   number of flashing blocks changes with it, it is\r\n");
    printf("                   the segmentation/DMA side)\r\n");
    printf("      (PC protocol: EMO:HAPPY / INTENT:IDLE)\r\n");
}

static void print_list(void)
{
    int i;
    printf("[femo] %u face clips:\r\n", (unsigned)FACE_COUNT);
    for (i = 0; i < (int)FACE_COUNT; i++) {
        printf("      %2d  %-26s %u keys  %u ms\r\n", i,
               (g_face_clips[i].name != 0) ? g_face_clips[i].name : "(null)",
               (unsigned)g_face_clips[i].n_frames, (unsigned)g_face_clips[i].total_ms);
    }
}

static void print_stat(void)
{
    printf("[femo] clip=%d '%s' combo=%s seq=%u loops=%u auto=%u gaze=%d,%d\r\n",
           s_clip,
           (s_clip >= 0 && g_face_clips[s_clip].name != 0) ? g_face_clips[s_clip].name : "-",
           (s_entry != 0) ? s_entry->key : "-",
           (unsigned)s_seq, (unsigned)s_loops, (unsigned)s_mode,
           (int)s_gaze_x, (int)s_gaze_y);
    printf("[femo] amp=%u%% size=%u%% speed=%u%% segrows=%d\r\n",
           (unsigned)face_get_amp(), (unsigned)face_get_size(),
           (unsigned)face_get_speed(), face_get_segrows());
}

static void handle_line(char *line)
{
    char *argv[6];
    int argc = split_args(line, argv, 6);

    if (argc == 0) {
        return;
    }
    /* PC 协议：先按 pose（参数化轨道，会动）解释，命中就切过去；没命中再回落到
     * 逐帧素材 combo。这样 EMO:SLEEPING 也能被翻译成 asleep（见 ui_model_name）。*/
    if (ci_prefix(argv[0], "EMO:")) {
        const fpose_t *p = pose_find(ui_model_name(argv[0] + 4));
        if (p != 0) {
            (void)model_set_pose(p);
        } else {
            apply_key(s_emos, argv[0] + 4);
        }
        return;
    }
    if (ci_prefix(argv[0], "INTENT:")) {
        const fpose_t *p = pose_find(argv[0] + 7);
        if (p != 0) {
            (void)model_set_pose(p);
        } else {
            apply_key(s_intents, argv[0] + 7);
        }
        return;
    }

    if (ci_eq(argv[0], "help")) {
        print_help();
    } else if (ci_eq(argv[0], "list")) {
        print_list();
    } else if (ci_eq(argv[0], "stat")) {
        print_stat();
    } else if (ci_eq(argv[0], "combo")) {
        print_stat();
    } else if (ci_eq(argv[0], "play") && argc >= 2) {
        const face_clip_t *c;
        if (argv[1][0] >= '0' && argv[1][0] <= '9') {
            int idx = to_int(argv[1]);
            c = (idx >= 0 && idx < (int)FACE_COUNT) ? &g_face_clips[idx] : 0;
        } else {
            c = find_clip(argv[1]);
        }
        if (c == 0) {
            printf("[femo] no such clip: %s\r\n", argv[1]);
        } else {
            s_combo = 0;
            s_entry = 0;
            s_mode = 0;                 /* 手动播放切出 cycle */
            s_use_model = 0;            /* 也切出参数化模型模式 */
            start_clip(c, 1);
        }
    } else if (ci_eq(argv[0], "emo") && argc >= 2) {
        const fpose_t *p = pose_find(argv[1]);
        if (p != 0) {
            (void)model_set_pose(p);            /* 参数化轨道优先（会动）*/
        } else {
            apply_key(s_emos, argv[1]);         /* 素材名 -> 逐帧 combo */
        }
    } else if (ci_eq(argv[0], "intent") && argc >= 2) {
        const fpose_t *p = pose_find(argv[1]);
        if (p != 0) {
            (void)model_set_pose(p);
        } else {
            apply_key(s_intents, argv[1]);
        }
    } else if (ci_eq(argv[0], "blink")) {
        if (argc >= 2) {
            uint8_t v = (uint8_t)to_int(argv[1]);
            if (s_use_model) {
                s_user_blink = v;                   /* 静态覆盖（与动作/动画取较大值）*/
                (void)model_compose();
                model_redraw();
            } else {
                face_blink(v);                      /* 指定值 = 静态设定（0..255）*/
            }
            printf("[femo] blink=%d\r\n", (int)v);
        } else {
            s_blink_ph = 1;                         /* 不带参数 = 播一次眨眼动画 */
            s_blink_st = 0;
            printf("[femo] blink\r\n");
        }
    } else if (ci_eq(argv[0], "gaze") && argc >= 3) {
        s_gaze_x = (int8_t)to_int(argv[1]);
        s_gaze_y = (int8_t)to_int(argv[2]);
        if (s_use_model) {
            /* 模型模式下：设“基准”，动作层会在它上面叠偏移（两者相加）*/
            s_gaze_base_dx = (int16_t)(s_gaze_x * 2);   /* 像素 -> 1/2 像素 */
            s_gaze_base_dy = (int16_t)(s_gaze_y * 2);
            (void)model_compose();
            model_redraw();
        } else {
            face_gaze(s_gaze_x, s_gaze_y);
        }
        printf("[femo] gaze %d %d\r\n", (int)s_gaze_x, (int)s_gaze_y);
    } else if (ci_eq(argv[0], "amp") && argc >= 2) {
        face_set_amp((uint16_t)to_int(argv[1]));
        printf("[femo] amp=%u%%\r\n", (unsigned)face_get_amp());
    } else if (ci_eq(argv[0], "size") && argc >= 2) {
        face_set_size((uint16_t)to_int(argv[1]));
        printf("[femo] size=%u%%\r\n", (unsigned)face_get_size());
    } else if (ci_eq(argv[0], "speed") && argc >= 2) {
        face_set_speed((uint16_t)to_int(argv[1]));
        printf("[femo] speed=%u%%\r\n", (unsigned)face_get_speed());
    } else if (ci_eq(argv[0], "mode") && argc >= 2) {
        if (ci_prefix(argv[1], "model")) {
            const fpose_t *p = pose_find(s_pose);
            s_hold = 0;
            if (p == 0) {
                p = pose_find("neutral");
            }
            (void)model_set_pose(p);            /* 回到参数化模型 + 继续跑动作 */
        } else if (ci_prefix(argv[1], "cycle")) {
            s_use_model = 0;
            cycle_start();
        } else {
            s_use_model = 0;
            s_mode = ci_prefix(argv[1], "auto") ? 1 : 0;
            printf("[femo] mode=%s\r\n", (s_mode == 1) ? "auto" : "manual");
        }
    } else if (ci_eq(argv[0], "pose") && argc >= 2) {
        const fpose_t *p = pose_find(argv[1]);
        if (p == 0) {
            printf("[femo] no such pose: %s (type poses)\r\n", argv[1]);
        } else {
            (void)model_set_pose(p);
        }
    } else if (ci_eq(argv[0], "poses")) {
        int i;
        printf("[femo] %d poses (model + action):\r\n", (int)(sizeof(s_poses) / sizeof(s_poses[0]) - 1));
        for (i = 0; s_poses[i].name != 0; i++) {
            printf("      %-10s model=%-10s act=%s%s\r\n", s_poses[i].name,
                   s_poses[i].model,
                   (s_poses[i].action != 0) ? s_poses[i].action : "-",
                   (ci_eq(s_poses[i].name, s_pose) && s_use_model) ? "   <- current" : "");
        }
    } else if (ci_eq(argv[0], "act") && argc >= 2) {
        /* 只换动作、不换模型：调动作幅度/节奏时最方便 */
        const face_act_t *a = face_act_find(argv[1]);
        if (a == 0) {
            printf("[femo] no such action: %s (type acts)\r\n", argv[1]);
        } else {
            s_use_model = 1;
            face_act_start(a, &s_act_expr);
            printf("[femo] action=%s (%u keys, loop=%u)\r\n",
                   a->name, (unsigned)a->n_keys, (unsigned)a->loop);
        }
    } else if (ci_eq(argv[0], "acts")) {
        int i;
        printf("[femo] %d actions:\r\n", face_act_count());
        for (i = 0; i < face_act_count(); i++) {
            const face_act_t *a = face_act_get(i);
            printf("      %-12s %u keys  %s\r\n", a->name, (unsigned)a->n_keys, a->desc);
        }
    } else if (ci_eq(argv[0], "model") && argc >= 2) {
        /* 名字既可能是 pose 也可能是纯模型；先按 pose 找（会带动作），
         * 找不到再当模型名处理（配 idle 动作，免得一张死脸）。*/
        const fpose_t *p = pose_find(argv[1]);
        if (p != 0) {
            (void)model_set_pose(p);
        } else {
            int idx = face_model_find(argv[1]);
            fpose_t tmp;
            if (idx < 0) {
                printf("[femo] no such model/pose: %s (type models / poses)\r\n", argv[1]);
            } else {
                tmp.name = face_model_get(idx)->name;
                tmp.model = face_model_get(idx)->name;
                tmp.action = "idle";
                (void)model_set_pose(&tmp);
            }
        }
    } else if (ci_eq(argv[0], "mopen") && argc >= 2) {
        s_user_mouth_open = (uint8_t)to_int(argv[1]);
        (void)model_compose();
        model_redraw();
        printf("[femo] mouth_open=%d\r\n", (int)s_user_mouth_open);
    } else if (ci_eq(argv[0], "escal") && argc >= 2) {
        s_user_eye_scale = (int16_t)to_int(argv[1]);
        (void)model_compose();
        model_redraw();
        printf("[femo] eye_scale=%d%%\r\n", (int)s_user_eye_scale);
    } else if (ci_eq(argv[0], "reset")) {
        s_user_eye_scale = 0;
        s_user_mouth_open = 0;
        s_user_blink = 0;
        s_gaze_base_dx = 0;
        s_gaze_base_dy = 0;
        (void)model_compose();
        model_redraw();
        printf("[femo] expr reset\r\n");
    } else if (ci_eq(argv[0], "models")) {
        int i;
        printf("[femo] %d parametric models:\r\n", face_model_count());
        for (i = 0; i < face_model_count(); i++) {
            printf("      %2d  %s%s\r\n", i, face_model_get(i)->name,
                   (i == s_model_idx) ? "   <- current" : "");
        }
    } else if (ci_eq(argv[0], "gif")) {
#if FACE_USE_GIF
        if (!s_gif_inited) {
            anim_init();                    /* 内部做 video_init + 条目表 */
            s_gif_inited = 1;
        }
        s_use_gif = 1;
        s_use_model = 0;
        s_hold = 0;
        if (argc >= 2) {
            int i;
            for (i = 0; i < ANIM_COUNT; i++) {
                if (ci_prefix(anim_items()[i].name, argv[1])) {
                    s_gif_idx = i;
                    break;
                }
            }
        }
        anim_play_open(&anim_items()[s_gif_idx]);
        s_gif_acc = 0;
        printf("[femo] GIF pipeline: clip %d/%d '%s'  (compare flicker!)\r\n",
               s_gif_idx + 1, ANIM_COUNT, anim_items()[s_gif_idx].name);
#else
        printf("[femo] gif pipeline disabled (build flag FACE_USE_GIF=0)\r\n");
        printf("[femo]   it needs libanimres.a ~1MB -> slow flashing; enable to compare\r\n");
#endif
    } else if (ci_eq(argv[0], "face")) {
#if FACE_USE_GIF
        s_use_gif = 0;
#endif
        (void)model_set_pose(pose_find((pose_find(s_pose) != 0) ? s_pose : "neutral"));
        printf("[femo] back to parametric model (pose=%s)\r\n", s_pose);
    } else if (ci_eq(argv[0], "hold")) {
        s_hold = (argc >= 2) ? (uint8_t)(to_int(argv[1]) ? 1 : 0)
                             : (uint8_t)(s_hold ^ 1);
        printf("[femo] hold=%u (1 = freeze picture)\r\n", (unsigned)s_hold);
    } else if (ci_eq(argv[0], "segrows") && argc >= 2) {
        face_set_segrows(to_int(argv[1]));      /* 每段行数 1..8，排查闪烁用 */
        printf("[femo] segrows=%d\r\n", face_get_segrows());
    } else if (ci_eq(argv[0], "cycle")) {
        cycle_start();                      /* 等价于 mode cycle */
    } else {
        printf("[femo] unknown command: %s (type help)\r\n", argv[0]);
    }
}

static void poll_uart(void)
{
    char c;
    while (csi_uart_receive(console.uart, &c, 1, 0) == 1) {
        if (c == '\r' || c == '\n') {
            if (s_rxlen > 0) {
                s_rx[s_rxlen] = 0;
                handle_line(s_rx);
                s_rxlen = 0;
            }
        } else if (s_rxlen < (RX_MAX - 1)) {
            s_rx[s_rxlen++] = (char)c;
        }
    }
}

/* ---------------- 主循环 ---------------- */
static void face_emo_tick(void)
{
    if (!s_ui_mode) {
        poll_uart();        /* 独立入口时自己收命令；被情感状态机接管后，串口归它独占 */
    }
    s_now += FACE_TICK_MS;

    if (s_hold) {
        return;                         /* 冻结：不推进画面、不眨眼，屏幕停在当前帧 */
    }

    /* 眨眼状态机（全局唯一一套）：只算出 s_anim_blink，两种轨道各自取用。
     * 以前模型分支里又写了一遍同样的推进 → 一个 tick 推两步，模型模式的眨眼
     * 实际只有约 50ms，看着像"抽了一下"。现在统一在这里推进。
     * 快闭快开、中间不停留：真人眨眼 100~150ms，且几乎不在闭眼态停留。 */
    if (s_blink_ph == 0) {
        if (s_blink_wait > FACE_TICK_MS) {
            s_blink_wait -= FACE_TICK_MS;
        } else {
            s_blink_ph = 1;
            s_blink_st = 0;
        }
    } else if (s_blink_ph == 1) {
        s_blink_st++;
        s_anim_blink = (uint8_t)((255u * s_blink_st) / BLINK_CLOSE_STEPS);
        if (s_blink_st >= BLINK_CLOSE_STEPS) {
            s_blink_ph = 2;
            s_blink_st = 0;
        }
    } else {
        s_blink_st++;
        s_anim_blink = (uint8_t)(255u - (255u * s_blink_st) / BLINK_OPEN_STEPS);
        if (s_blink_st >= BLINK_OPEN_STEPS) {
            s_blink_ph = 0;
            s_anim_blink = 0;
            s_blink_wait = (uint16_t)(1500 + (rnd() % 2500));
        }
    }

    /* 逐帧（素材）轨道：眨眼/视线是渲染层直接叠加的，必须每步刷一次；
     * 模型轨道走 model_compose()，不在这里刷屏。 */
    if ((s_use_model == 0) && ((s_blink_ph != 0) || (s_anim_blink != 0))) {
        face_blink(s_anim_blink);
    }

    /* combo 推进（眨眼动画期间不推进：一次 tick 只重绘一次，否则动画会被拖慢一倍）*/
    /* 位图管线（对照实验）：按 ANIM_FRAME_DELAY_MS 的节奏推进即可 */
#if FACE_USE_GIF
    if (s_use_gif) {
        s_gif_acc += FACE_TICK_MS;
        if (s_gif_acc >= ANIM_FRAME_DELAY_MS) {
            s_gif_acc = 0;
            if (anim_play_step()) {
                s_gif_idx = (s_gif_idx + 1) % ANIM_COUNT;
                anim_play_open(&anim_items()[s_gif_idx]);
                printf("[femo] gif clip %d/%d '%s'\r\n",
                       s_gif_idx + 1, ANIM_COUNT, anim_items()[s_gif_idx].name);
            }
        }
        return;
    }
#endif

    /* 模型模式：动作层推进 + 参数合成，【合并成一次重绘】。
     * 为什么必须合并：render_current() 是“整屏 60 段 DMA”的长事务；
     * 一 tick 里若重绘两次，第二次的 LCD_Set_Window() 会在第一次还没发完时
     * 就 CLR_CS1 把它打断 —— 面板收到半个事务，屏幕上就是固定位置的白色块
     * （先写的行没事，最后写的那几行错乱，所以看着在底部）。
     * 所以这里只改参数，最后统一重绘一次（并做 50ms 节流，见 MODEL_REDRAW_MIN_MS）。*/
    if (s_use_model) {
        uint8_t flags = face_act_tick(FACE_TICK_MS, &s_act_expr);
        int need;

        /* 动作要求“到位眨一下”（k_look / k_think 的 blink_once）*/
        if (((flags & FACE_ACT_BLINK_ONCE) != 0) && (s_blink_ph == 0)) {
            s_blink_ph = 1;
            s_blink_st = 0;
        }

        need = model_compose();
        if ((flags & FACE_ACT_REDRAW) != 0) {
            need = 1;
        }
        if (need != 0) {
            s_redraw_acc = (uint16_t)(s_redraw_acc + FACE_TICK_MS);
            if (s_redraw_acc >= MODEL_REDRAW_MIN_MS) {
                s_redraw_acc = 0;
                model_redraw();
            }
        }
        return;
    }

    if (s_playing && s_blink_ph == 0) {
        s_clip_acc += FACE_TICK_MS;
        if (s_clip_acc >= FACE_CLIP_MS) {
            s_clip_acc = 0;
            if (face_clip_step(FACE_CLIP_MS)) {
                if (s_mode == 2) {
                    cycle_next(1);          /* 纯轮播：播完一个就换下一个（带过渡）*/
                } else if (s_mode == 1) {
                    s_loops++;
                    if (s_loops >= LOOPS_PER_CLIP) {
                        s_loops = 0;
                        auto_next();
                    }
                } else {
                    clip_advance();
                }
            }
        }
    }
}

void face_emo_test(void)
{
    face_init();
    printf("[femo] parameterised face state machine (serial driven)\r\n");
    printf("[femo] %u clips, tick %u ms, morph %u x %u ms\r\n",
           (unsigned)FACE_COUNT, (unsigned)FACE_TICK_MS,
           (unsigned)MORPH_STEPS, (unsigned)MORPH_STEP_MS);
    check_resources();
    print_help();

    face_expr_reset(&s_expr);
    face_expr_reset(&s_act_expr);
    face_expr_reset(&s_last_expr);
    s_use_model = 0;            /* 先置 0，让下面的 model_set_pose 走完整流程（含启动动作）*/
    s_model_idx = face_model_find("neutral");
    if (s_model_idx < 0) {
        s_model_idx = 0;
    }
    {
        const fpose_t *p = pose_find("neutral");

        if (p != 0) {
            (void)model_set_pose(p);    /* 模型 + idle 动作：开机就在动 */
        } else {
            s_use_model = 1;
            model_redraw();
        }
    }

    for (;;) {
        face_emo_tick();
        mdelay(FACE_TICK_MS);
    }
}

/* ==================== 对外接口：供情感状态机（emotion_fsm）驱动 ====================
 * 见 anim/face_ui.h 的说明。要点：
 *   ① 调用 face_ui_init() 之后，本文件不再 poll_uart()（串口归 emotion_fsm 独占）；
 *   ② face_ui_tick() 就是 face_emo_tick()，非阻塞，调用方按 ~20ms 调；
 *   ③ 情绪 / 心情 / 意图 → 【模型 + 动作】的映射都在这里，舵机侧不重复；
 *   ④ 三个 setter 都返回 0 = 切换成功、-1 = 没有对应姿势 —— 调用方据此决定是否回落
 *      （emotion_fsm 就是靠这个让 INTENT:THINK 优先于情绪表情的）。 */

/* 心情 / 情绪名 → 姿势名的翻译（同名的不动）*/
static const char *ui_model_name(const char *name)
{
    if (name == 0) {
        return "neutral";
    }
    if (ci_eq(name, "sleeping") || ci_eq(name, "sleep")) {
        return "asleep";               /* 心情 SLEEPING → asleep（闭眼 + 泡泡）*/
    }
    if (ci_eq(name, "bored") || ci_eq(name, "active") || ci_eq(name, "idle")) {
        return "neutral";              /* 没有“无聊/活跃”专用模型，降级为 neutral */
    }
    return name;
}

/* 按名字切姿势。translate=1 时先做心情翻译（情绪/心情用），0 时原样查（意图用）。
 * 注意：这里不打印失败 —— emotion_fsm 每拍都会调，打印会刷屏；
 * 成功切换的那一行由 model_set_pose() 打（并且同姿势会短路）。*/
static int ui_show(const char *name, int translate)
{
    const char *key = (translate != 0) ? ui_model_name(name) : name;
    const fpose_t *p = pose_find(key);

    if (p == 0) {
        return -1;
    }
    return model_set_pose(p);
}

void face_ui_init(void)
{
    const fpose_t *p;

    face_init();
    face_expr_reset(&s_expr);
    face_expr_reset(&s_act_expr);
    face_expr_reset(&s_last_expr);
    s_anim_blink = 0;

    s_model_idx = face_model_find("neutral");
    if (s_model_idx < 0) {
        s_model_idx = 0;
    }
    s_use_model = 0;                    /* 先置 0：让 model_set_pose 走完整流程（启动动作）*/

    p = pose_find("neutral");
    if (p != 0) {
        (void)model_set_pose(p);        /* 模型 + idle 动作（待机也会动）*/
    } else {
        s_use_model = 1;
        model_redraw();
    }
    s_ui_mode = 1;                      /* 【关键】交出串口所有权 */
}

void face_ui_tick(void)
{
    face_emo_tick();
}

int face_ui_emotion(const char *name)
{
    return ui_show(name, 1);
}

int face_ui_mood(const char *name)
{
    return ui_show(name, 1);
}

int face_ui_intent(const char *name)
{
    return ui_show(name, 0);
}
