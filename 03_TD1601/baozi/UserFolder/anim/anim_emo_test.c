/******************************************************************************
 * @file     anim_emo_test.c
 * @brief    表情动画 · 串口状态机测试（情绪/意图 -> combo 序列，精准响应 + 流畅切换）
 *
 * 目的：验证「串口下发语义状态 -> 屏幕立刻换表情」这条路，与 emotion_fsm 的
 *       协议保持一致，将来可以直接把动作库那套 (mood, emotion) 映射并过来。
 *
 * ── 为什么用 combo（本次新增）─────────────────────────────────────────────
 *   单个片段循环播放会显得呆板。现在每个情绪/意图对应一组「片段序列」：
 *        happy : smile -> laugh -> snigger      （情绪强度递进，像"越来越开心"）
 *        sad   : sad -> sigh -> cry
 *        每个片段播 LOOPS_PER_CLIP 轮后自动切到序列里的下一个；
 *        序列走完一轮后，会在「变体池」里换一组（随机且不与上次相同），
 *   与动作库 movements 的"变体池 + 随机呼出"机制保持同一套设计哲学。
 *
 * 串口（115200-8N1，命令行只用 CR/LF 结束，空格只作分词 —— 见工程教训：
 *       曾把空格也当行尾导致带参数命令全部失效）：
 *     help                        帮助
 *     list                        列出所有片段（编号 + 名字）
 *     play <编号|名字前缀>        直接播某个片段（不进 combo）
 *     emo <neutral|happy|sad|angry|surprised|loving>    情绪 -> combo
 *     intent <idle|listen|speak|think|greet|sleep|wake|eat|play|proud>  意图 -> combo
 *     combo                       打印当前 combo 与进度
 *     mode <manual|auto>          手动 / 自动（auto = 随机换情绪轮播）
 *     stat                        当前状态
 *   同时兼容 PC 协议写法：EMO:HAPPY / INTENT:IDLE（大小写不敏感）
 *
 * 响应与流畅：
 *   · 主循环 5ms 一拍：轮询串口（切换延迟 <= 5ms）+ 到点渲染一帧
 *   · 切片段时从新片段第 0 帧开始（首帧是全屏，等于把旧表情整屏覆盖 -> 干净切换）
 *   · 单帧只刷变化矩形（平均 18% 屏面积），25fps 有大量余量
 *
 * 入口：anim_emo_test() —— 由 src/main.c 调用
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "anim_player.h"
#include "video.h"                  /* video_init */
#include <drv/uart.h>
#include "console/sys_console.h"   /* console.uart */
#include <drv/common.h>             /* mdelay */

/* 控制台实例（src/board_init.c 中定义并初始化）*/
extern sys_console_t console;

#define EMO_TICK_MS     5           /* 主循环步进：越小响应越灵敏 */
#define RX_MAX          64
#define LOOPS_PER_CLIP  2           /* 每个片段播几轮再走 combo 下一步 */
#define COMBO_MAX       4           /* 一组 combo 最多几个片段 */
#define VARIANT_MAX     2           /* 每个 key 最多几组变体 */

/* ---------------- combo 定义（改这里即可调风格）---------------- */
typedef struct {
    const char *clips[COMBO_MAX];   /* 片段名前缀序列，以 0 结束 */
} combo_t;

typedef struct {
    const char *key;                        /* 下发的情绪/意图名 */
    combo_t     variants[VARIANT_MAX];      /* 变体池（>=1 组） */
    uint8_t     n_variants;
} emo_entry_t;

static const emo_entry_t s_emos[] = {
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

static const emo_entry_t s_intents[] = {
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

/* ---------------- 内部状态 ---------------- */
static char        s_rx[RX_MAX];
static uint8_t     s_rxlen = 0;
static uint8_t     s_auto = 0;              /* 1 = 自动（随机换情绪） */
static int         s_clip = -1;
static uint32_t    s_frame_at = 0;          /* 下一帧的时间点 */
static uint8_t     s_play_ok = 0;
static uint32_t    s_now_ms = 0;

/* combo 播放状态 */
static const emo_entry_t *s_entry = 0;      /* 当前情绪/意图条目 */
static const combo_t     *s_combo = 0;      /* 当前选中的变体（序列） */
static uint8_t     s_seq = 0;               /* 序列内位置 */
static uint8_t     s_loops = 0;             /* 当前片段已播轮数 */
static int8_t      s_last_variant = -1;     /* 上次用过的变体（避免连续重复）*/
static uint32_t    s_rng = 0x20260926u;     /* 软件伪随机 */

/* ---------------- 小工具 ---------------- */
static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* 大小写不敏感前缀比较 */
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

/* 大小写不敏感相等 */
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

/* 十进制串转整数（不依赖 stdlib 的 strtol）*/
static int to_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (int)(*s - '0');
        s++;
    }
    return v;
}

/* 软件伪随机（LCG，够用来挑变体）*/
static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 16);
}

/* ---------------- 片段选择 ---------------- */
static int clip_by_index(int idx)
{
    int rc;
    const anim_item_t *it;

    if (idx < 0 || idx >= (int)ANIM_COUNT) {
        printf("[emo] index out of range: %d (0..%d)\r\n", idx, (int)ANIM_COUNT - 1);
        return -1;
    }
    it = &anim_items()[idx];
    rc = anim_play_open(it);
    if (rc != 0) {
        printf("[emo] cannot open #%d '%s' rc=%d size=%u\r\n",
               idx, (it->name != 0) ? it->name : "(null)", rc, (unsigned)it->size);
        return -1;
    }
    s_clip = idx;
    s_frame_at = 0;                         /* 立刻出第一帧 */
    s_play_ok = 1;
    return 0;
}

static int clip_by_prefix(const char *pat)
{
    int i;
    for (i = 0; i < (int)ANIM_COUNT; i++) {
        if (ci_prefix(anim_items()[i].name, pat)) {
            return clip_by_index(i);
        }
    }
    printf("[emo] clip '%s' not found\r\n", (pat != 0) ? pat : "(null)");
    return -1;
}

static int select_clip(const char *arg)
{
    s_combo = 0;                            /* 手动指定片段时退出 combo */
    s_entry = 0;
    if (arg[0] >= '0' && arg[0] <= '9') {
        return clip_by_index(to_int(arg));
    }
    return clip_by_prefix(arg);
}

/* 启动自检：逐个检查片段头（objcopy 库模式下最容易出错的一步）*/
static void check_resources(void)
{
    const anim_item_t *it = anim_items();
    int i;
    int bad = 0;

    printf("[emo] %u clips; first='%s' size=%u magic=%c%c%c%c\r\n",
           (unsigned)ANIM_COUNT,
           (it[0].name != 0) ? it[0].name : "(null)",
           (unsigned)it[0].size,
           (it[0].data != 0) ? (char)it[0].data[0] : '?',
           (it[0].data != 0) ? (char)it[0].data[1] : '?',
           (it[0].data != 0) ? (char)it[0].data[2] : '?',
           (it[0].data != 0) ? (char)it[0].data[3] : '?');

    for (i = 0; i < (int)ANIM_COUNT; i++) {
        if (it[i].data == 0 || it[i].size < 16 ||
            it[i].data[0] != 'A' || it[i].data[1] != 'N' ||
            it[i].data[2] != 'M' || it[i].data[3] != '3') {
            printf("[emo] BAD clip #%d '%s' size=%u\r\n", i,
                   (it[i].name != 0) ? it[i].name : "(null)", (unsigned)it[i].size);
            bad++;
        }
    }
    printf("[emo] resource check: %d/%u clips OK\r\n",
           (int)ANIM_COUNT - bad, (unsigned)ANIM_COUNT);
}

/* ---------------- combo 播放 ---------------- */
static uint8_t pick_variant(const emo_entry_t *e)
{
    uint8_t v;
    if (e->n_variants <= 1) {
        return 0;
    }
    v = (uint8_t)(rnd() % (uint32_t)e->n_variants);
    if (v == (uint8_t)s_last_variant) {             /* 避免连续两次同一组 */
        v = (uint8_t)((v + 1) % e->n_variants);
    }
    return v;
}

/* 播放某个条目的 combo（内部会选一组变体）*/
static void play_entry(const emo_entry_t *e)
{
    uint8_t v = pick_variant(e);

    s_last_variant = (int8_t)v;
    s_entry = e;
    s_combo = &e->variants[v];
    s_seq = 0;
    s_loops = 0;
    printf("[emo] %s variant %u/%u: %s", e->key,
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
    clip_by_prefix(s_combo->clips[0]);
}

/* 按名字查表并播放 */
static void apply_key(const emo_entry_t *table, const char *name)
{
    int i;
    for (i = 0; table[i].key != 0; i++) {
        if (ci_eq(table[i].key, name)) {
            play_entry(&table[i]);
            return;
        }
    }
    printf("[emo] unknown name '%s' (type help)\r\n", name);
}

/* 当前片段播完一轮：推进 combo */
static void clip_advance(void)
{
    if (s_combo == 0) {
        return;
    }
    s_loops++;
    if (s_loops < LOOPS_PER_CLIP) {
        return;                             /* 同一片段继续循环 */
    }
    s_loops = 0;
    s_seq++;
    if (s_combo->clips[s_seq] == 0) {
        /* 序列走完一轮：换一组变体再来（避免每次都一样）*/
        if (s_entry != 0 && s_entry->n_variants > 1) {
            play_entry(s_entry);
        } else {
            s_seq = 0;
            clip_by_prefix(s_combo->clips[0]);
        }
        return;
    }
    clip_by_prefix(s_combo->clips[s_seq]);
}

/* 自动模式：随机换一个情绪 */
static void auto_next(void)
{
    int n = 0;
    int pick;
    if (s_emos[0].key == 0) {
        return;
    }
    while (s_emos[n].key != 0) {
        n++;
    }
    pick = (int)(rnd() % (uint32_t)n);
    play_entry(&s_emos[pick]);
}

/* ---------------- 命令 ---------------- */
static void print_help(void)
{
    printf("[emo] commands:\r\n");
    printf("      help / list / stat / combo\r\n");
    printf("      play <n|name-prefix>\r\n");
    printf("      emo <neutral|happy|sad|angry|surprised|loving>\r\n");
    printf("      intent <idle|listen|speak|think|greet|sleep|wake|eat|play|proud>\r\n");
    printf("      mode <manual|auto>\r\n");
    printf("      (PC protocol also accepted: EMO:HAPPY / INTENT:IDLE)\r\n");
}

static void print_list(void)
{
    int i;
    printf("[emo] %u clips:\r\n", (unsigned)ANIM_COUNT);
    for (i = 0; i < (int)ANIM_COUNT; i++) {
        printf("      %2d  %-28s %u frames\r\n", i,
               (anim_items()[i].name != 0) ? anim_items()[i].name : "(null)",
               (unsigned)anim_items()[i].frames);
    }
}

static void print_combo(void)
{
    int len = 0;

    if (s_combo == 0) {
        printf("[emo] no combo active (single clip mode)\r\n");
        return;
    }
    while (len < COMBO_MAX && s_combo->clips[len] != 0) {
        len++;
    }
    printf("[emo] combo '%s' step %u/%d loops %u/%u\r\n",
           (s_entry != 0) ? s_entry->key : "?",
           (unsigned)(s_seq + 1), len,
           (unsigned)s_loops, (unsigned)LOOPS_PER_CLIP);
}

static void print_stat(void)
{
    if (s_clip >= 0) {
        printf("[emo] clip=#%d %s  mode=%s\r\n", s_clip,
               (anim_items()[s_clip].name != 0) ? anim_items()[s_clip].name : "(null)",
               s_auto ? "auto" : "manual");
    } else {
        printf("[emo] clip=none  mode=%s\r\n", s_auto ? "auto" : "manual");
    }
}

static void handle_line(char *line)
{
    char *argv[6];
    int argc = split_args(line, argv, 6);

    if (argc == 0) {
        return;
    }

    /* 兼容 PC 协议：EMO:xxx / INTENT:xxx */
    if (ci_prefix(argv[0], "EMO:")) {
        apply_key(s_emos, argv[0] + 4);
        return;
    }
    if (ci_prefix(argv[0], "INTENT:")) {
        apply_key(s_intents, argv[0] + 7);
        return;
    }

    if (ci_eq(argv[0], "help")) {
        print_help();
    } else if (ci_eq(argv[0], "list")) {
        print_list();
    } else if (ci_eq(argv[0], "stat")) {
        print_stat();
    } else if (ci_eq(argv[0], "combo")) {
        print_combo();
    } else if (ci_eq(argv[0], "play") && argc >= 2) {
        if (select_clip(argv[1]) != 0) {
            printf("[emo] cannot play: %s\r\n", argv[1]);
        }
    } else if (ci_eq(argv[0], "emo") && argc >= 2) {
        apply_key(s_emos, argv[1]);
    } else if (ci_eq(argv[0], "intent") && argc >= 2) {
        apply_key(s_intents, argv[1]);
    } else if (ci_eq(argv[0], "mode") && argc >= 2) {
        if (ci_prefix(argv[1], "auto")) {
            s_auto = 1;
            printf("[emo] mode=auto (random emotion combo)\r\n");
        } else {
            s_auto = 0;
            printf("[emo] mode=manual\r\n");
        }
    } else {
        printf("[emo] unknown command: %s (type help)\r\n", argv[0]);
    }
}

static void poll_uart(void)
{
    char c;
    while (csi_uart_receive(console.uart, &c, 1, 0) == 1) {
        if (c == '\r' || c == '\n') {       /* 只用 CR/LF 结束一行 */
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
static void emo_tick(void)
{
    poll_uart();
    s_now_ms += EMO_TICK_MS;

    if (!s_play_ok) {
        return;
    }

    if (s_auto) {
        /* 自动模式：每个片段播 LOOPS_PER_CLIP 轮 -> 换下一个情绪 */
        if (s_now_ms >= s_frame_at) {
            if (anim_play_step() == 1) {
                s_loops++;
                if (s_loops >= LOOPS_PER_CLIP) {
                    s_loops = 0;
                    auto_next();
                }
            }
            s_frame_at = s_now_ms + (uint32_t)ANIM_FRAME_DELAY_MS;
        }
        return;
    }

    /* 手动模式：到点渲染一帧；一轮播完 -> combo 走下一步 */
    if (s_now_ms >= s_frame_at) {
        if (anim_play_step() == 1) {
            clip_advance();
        }
        s_frame_at = s_now_ms + (uint32_t)ANIM_FRAME_DELAY_MS;
    }
}

void anim_emo_test(void)
{
    anim_init();                        /* 官方 LCD 初始化 + 三色自检 */
    printf("[emo] expression combo test (serial driven)\r\n");
    printf("[emo] %u clips, src %ux%u x%u, frame %u ms, tick %u ms, %u loops/clip\r\n",
           (unsigned)ANIM_COUNT, (unsigned)ANIM_SRC_SIZE, (unsigned)ANIM_SRC_SIZE,
           (unsigned)ANIM_SCALE, (unsigned)ANIM_FRAME_DELAY_MS, (unsigned)EMO_TICK_MS,
           (unsigned)LOOPS_PER_CLIP);
    check_resources();                  /* 资源自检：先确认 23 个头都在 */
    print_help();

    /* 默认：手动模式，先播 neutral 的 combo */
    apply_key(s_emos, "neutral");

    for (;;) {
        emo_tick();
        mdelay(EMO_TICK_MS);
    }
}
