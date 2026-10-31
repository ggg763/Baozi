/******************************************************************************
 * @file     emotion_test.c
 * @brief    情感状态机 / 动作库 · 串口交互测试（逐个巡览各状态的动作）
 *
 * 目的：上板时不改代码、不重新烧录，就能把【9 个动作】和【情绪/心情 → 动作】
 * 全部走一遍，并用串口看着内部状态（模式/动作/增益/情绪/心情/空闲时长）。
 *
 * ==================== 两种模式 ====================
 *   manual（默认）：手动指定播哪个动作（next / play），增益也可手调（gain）。
 *                   用途：单个动作看效果、调幅度/节奏。
 *   fsm          ：按 (心情, 情绪) 自动选动作 + 自动调制增益，等价于正式运行。
 *                   用途：验证"情绪→动作"映射与"连续状态→幅度/速度"算法。
 *   任何 EMO:/INTENT:/情绪名 命令会自动切到 fsm 模式（因为那正是正式路径）。
 *
 * ==================== 命令表（115200-8N1，回车结束）====================
 *   help                       命令表
 *   list                       9 个动作的编号 + 名字（+ 当前是哪个）
 *   next                       切到下一个动作并播放            [manual]
 *   play <编号|名字> [变体号]   播放指定动作，如 play 3 / play 0 2 [manual]
 *                              （不给变体号 = 随机挑，与正式运行一致）
 *   repeat                     重播当前动作（从 pause 恢复也算）
 *   pause                      暂停（保持当前姿态，便于看静止位置）
 *   center                     回中位并暂停
 *   gain [amp spd]             手动调制增益（千分比，1000=原样）；不带参数复位  [manual]
 *   mode [manual|fsm]          切模式（不带参数只打印当前模式）
 *   thr <bored_s> <sleep_s>    临时改空闲降级阈值（秒）；0 0 恢复 20/30；
 *                              并重新开始计时 —— 几秒就能看完 ACTIVE→BORED→SLEEPING
 *   state                      打印当前状态（含 fsm 此刻会选哪个动作）
 *   --- 以下是正式状态机的协议命令（自动切 fsm 模式）---
 *   EMO:HAPPY|SAD|ANGRY|SURPRISED|LOVING|NEUTRAL
 *   EMO:IDLE|BORED|SLEEPING
 *   INTENT:CHAT|GREETING|QUESTION|COMMAND|NONE
 *   裸名：happy / sad / angry / surprised / loving / neutral / idle / bored /
 *         sleeping / sleep / wake
 *
 * ==================== 典型测试流程 ====================
 *   list                      → 看动作清单（含每个状态有几个变体）
 *   next  next  next ...      → 逐个动作看一遍（每个动作会循环播，直到你发下一条）
 *   play 0                    → 单独看待机（缓慢巡视；每次随机换一个变体）
 *   play 0 2                  → 固定看待机变体2（调它的幅度/节奏时用）
 *   play 4                    → 单独看生气（快、幅度大）
 *   gain 700 700              → 同一动作、幅度 0.7 倍、速度 0.7 倍（对比看调制效果）
 *   gain 1200 1400            → 幅度 1.2 倍、速度 1.4 倍
 *   thr 3 6                   → 降级阈值缩到 3s/6s
 *   EMO:HAPPY                 → 进 fsm：开心（应切 happy，amp/spd 由公式算出）
 *   （等 4 秒不输入）           → mood 自动降级 bored（动作切 bored，速度 ×0.7）
 *   （再等 3 秒）              → mood 自动降级 sleeping（动作切 sleep：垂头 + 呼吸）
 *   EMO:SAD                   → 立刻从睡眠唤醒 + 切 sad（验证"睡眠中可被命令叫醒"）
 *   state                     → 核对内部状态
 *
 * ==================== 改动验证清单（串口逐条过）====================
 * 烧录后不必改代码，按下面逐条确认"这版改动真的生效"。每条都给了【期望现象】，
 * 对不上通常意味着：烧的还是旧版本，或新文件没登记进 package.yaml（那就不会被编译）。
 *
 * ① 这版编进去了吗
 *      help        → 必须看得到 " variants :" 那两行；看不到 = 烧的是旧版本
 *      list        → 每个状态后面带 "(N variants)"；idle 应为 3、bored 应为 2
 *
 * ② 待机三条变体（2026-09 由"V 字摇摆点头"改为"缓慢巡视四周"）
 *      play 0 1    → 变体1 缓慢环视：慢左看 → 停 → 极慢扫到右 → 停 → 回中（≈20s）
 *      play 0 2    → 变体2 远望俯视：抬头看远 → 左看 → 低头看桌面 → 右看（≈23s）
 *      play 0 3    → 变体3 扁 8 字巡游：慢慢画两圈 ∞（≈18s）
 *      期望：三条"性格"明显不同、每段都是秒级慢动作、到两侧会停住张望；
 *            若还看到原来那种 ±80us 的快速点头摇摆 = 动作没换过来
 *
 * ③ 随机呼出、不与上次重复
 *      play 0      → 每播完一轮会打 "[test] variant #n/3"
 *      期望：相邻两轮的 n 不相同；三次里能看到 2~3 个不同变体
 *
 * ④ 无聊两个变体 + 心情降级
 *      play 7 1    → 歪头发呆（歪到一侧、发很久的呆、换边、回正）
 *      play 7 2    → 垂头发呆（头更低、几乎不动、只在低位慢慢挪）
 *      thr 3 6     → 等 4 秒自动降级到 bored（日志 [test] mood: bored）
 *      EMO:SAD     → 立刻从降级状态醒来并切 sad（验证状态机路径与"可被打断"）
 *
 * ⑤ 幅度整体 +20%（MOVE_AMP_BASE = 1200，2026-09）
 *      play 0 1    → 看左右极限位置
 *      gain 833 1000 → 833 × 1.2 ≈ 1000，即"上一版的幅度"，来回对比同一变体的摆幅
 *      期望：默认（gain 1000）比 gain 833 大约大 20%；
 *            state 里的 actual(amp) 仍是 1000 —— 因为 +20% 是动作库的基准，不在调制增益里体现
 *
 * ⑥ 播放期间可打断（20ms 粒度）
 *      play 0 2    → 动作播到一半时立刻发 play 4（生气）
 *      期望：最多 ~20ms 就切过去（moves_set_tick_hook 每 20ms 轮询一次串口）
 *
 * ⑦ 正式状态机（fsm）路径
 *      EMO:HAPPY / EMO:SAD / EMO:LOVING → 各切到对应动作，日志打 "[fsm] <move> variant #n/m"
 *      （配合 thr 缩短）                → 自动降级 bored → sleeping
 *
 * ==================== 实现说明 ====================
 * - 与正式状态机共用：协议解析 emotion_fsm_feed_line()、动作映射
 *   emotion_fsm_move_for()、增益调制 emotion_fsm_apply_gain()；
 * - 播放期间靠动作库的 tick 钩子（moves_set_tick_hook）每 20ms 轮询串口，
 *   所以动作播到一半也能立刻收命令、立刻打断；
 * - 钩子里【只】做"收字节 + 置标志"，真正的命令执行放在主循环（避免在播放
 *   过程中重入动作库）；
 * - 命令是单槽缓冲：动作播放的瞬间连发多条命令，只有最后一条会被执行
 *   （发完看回显再发下一条即可）。
 *
 * 舵机分工（实测）：YAW=PA29/ch3 左右转、PITCH=PA27/ch2 抬/低头（YAW 2026-09 由 PA0 换脚）；
 * 脉宽增大 → YAW 右转 / PITCH 抬头；限位 YAW 300~1700、PITCH 800~1600。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/uart.h>
#include "console/sys_console.h"        /* console.uart */
#include "drv/common.h"                 /* mdelay */
#include "board_config.h"
#include "movements/moves.h"            /* 动作库 + 调制增益 + tick 钩子 */
#include "emotion/emotion_manager.h"    /* 情绪 + 连续状态 */
#include "emotion/mood_fsm.h"           /* 心情 + 阈值 */
#include "emotion/emotion_fsm.h"        /* 复用：协议解析 / 动作映射 / 增益调制 */
#include "test/emotion_test.h"

/* 控制台实例（src/board_init.c 中定义）*/
extern sys_console_t console;

/* ==================== 模式与内部状态 ==================== */

#define ET_MODE_MANUAL      0       /* 手动指定动作 */
#define ET_MODE_FSM         1       /* 按情绪/心情自动选（= 正式运行）*/

static uint32_t s_now_ms = 0;       /* 软件毫秒时基 */
static int      s_mode = ET_MODE_MANUAL;
static int      s_paused = 0;       /* 1 = 暂停（不播放，保持姿态）*/
static int      s_cur_move = MOVE_IDLE;
static int      s_manual_move = MOVE_IDLE;
static int      s_fixed_var = -1;   /* >=0 = 固定播该变体（调参用）；-1 = 随机（= 正式行为）*/
static uint16_t s_manual_amp = MOVE_GAIN_UNIT;
static uint16_t s_manual_spd = MOVE_GAIN_UNIT;
static uint8_t  s_dirty = 1;        /* 1 = 下一拍强制重设增益并打印一次 */
static mood_id_t s_last_mood = MOOD_MAX;

static char     s_cmd[40];                  /* 命令缓冲（钩子写入，主循环消费）*/
static volatile uint8_t s_cmd_ready = 0;

/* ==================== 小工具 ==================== */

/* 大小写不敏感：line 是否以 prefix 开头 */
static int et_prefix_eq(const char *line, const char *prefix)
{
    int i = 0;

    while (prefix[i] != '\0') {
        char a = line[i];
        char b = prefix[i];

        if (a == '\0') {
            return 0;
        }
        if ((a >= 'A') && (a <= 'Z')) { a = (char)(a - 'A' + 'a'); }
        if ((b >= 'A') && (b <= 'Z')) { b = (char)(b - 'A' + 'a'); }
        if (a != b) {
            return 0;
        }
        i++;
    }
    return 1;
}

/* 大小写不敏感：整串相等 */
static int et_text_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0')) {
        char ca = *a;
        char cb = *b;

        if ((ca >= 'A') && (ca <= 'Z')) { ca = (char)(ca - 'A' + 'a'); }
        if ((cb >= 'A') && (cb <= 'Z')) { cb = (char)(cb - 'A' + 'a'); }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return ((*a == '\0') && (*b == '\0')) ? 1 : 0;
}

/* 读十进制（成功返回 0）*/
static int et_atoi(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int any = 0;

    while ((*s >= '0') && (*s <= '9')) {
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
        any = 1;
    }

    if (any == 0) {
        return -1;
    }
    *out = v;
    return 0;
}

/* 按空格把一行切成最多 max 个 token（就地改写，返回个数）*/
static int et_split(char *line, char *tok[], int max)
{
    int n = 0;
    char *p = line;

    while ((*p != '\0') && (n < max)) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        tok[n++] = p;
        while ((*p != '\0') && (*p != ' ')) {
            p++;
        }
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }
    return n;
}

/* 编号或名字 → 动作 ID；不认识返回 -1 */
static int et_find_move(const char *s)
{
    uint32_t n;
    int i;

    if (et_atoi(s, &n) == 0) {
        return (n < (uint32_t)MOVE_MAX) ? (int)n : -1;
    }
    for (i = 0; i < (int)MOVE_MAX; i++) {
        if (et_text_eq(s, moves_name((move_id_t)i)) != 0) {
            return i;
        }
    }
    return -1;
}

/* 这一行是不是"正式状态机的协议命令"（EMO:/INTENT:/情绪名/心情名）*/
static int et_is_protocol_line(const char *line)
{
    static const char *const words[] = {
        "happy", "sad", "angry", "surprised", "loving", "neutral",
        "idle", "bored", "sleeping", "sleep", "wake"
    };
    int i;

    if ((et_prefix_eq(line, "EMO:") != 0) || (et_prefix_eq(line, "INTENT:") != 0)) {
        return 1;
    }
    for (i = 0; i < (int)(sizeof(words) / sizeof(words[0])); i++) {
        if (et_text_eq(line, words[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

/* ==================== 串口（钩子与主循环共用）==================== */

/* 非阻塞收字节，按行（CR/LF/空格）切分。
 * 关键：这里【只置标志 + 请求打断】，不执行命令 —— 因为它可能在动作播放中途被调用。 */
static void et_poll_uart(void)
{
    static char buf[40];
    static int len = 0;
    char c;

    while (csi_uart_receive(console.uart, &c, 1, 0) == 1) {
        /* ⚠️ 只能用 CR/LF 结束一行，【不能把空格当行尾】——
         * 本测试的命令都是"命令 + 参数"（gain 1200 1200 / play 0 2 / thr 3 6 / mode fsm），
         * 空格是参数分隔符。早期把它当行尾，导致参数被切成独立命令、带参数的命令全部失效。 */
        if ((c == '\r') || (c == '\n')) {
            buf[len] = '\0';
            if ((len > 0) && (s_cmd_ready == 0)) {
                int i;
                for (i = 0; i <= len; i++) {        /* 连结尾 '\0' 一起拷 */
                    s_cmd[i] = buf[i];
                }
                s_cmd_ready = 1;
                moves_interrupt();                  /* 让正在播的动作尽快让出 */
            }
            len = 0;
        } else if (len < (int)sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

/* 动作库每 20ms 回调一次：推进时基 + 收串口 */
static void et_tick_hook(void)
{
    s_now_ms += (uint32_t)MOVE_TICK_MS;
    et_poll_uart();
}

/* ==================== 打印 ==================== */

static void et_print_help(void)
{
    printf("\r\n===== Emotion FSM - Serial Move Test (115200-8N1, end with CR) =====\r\n");
    printf(" moves    : list | next | play <id|name> [variant] | repeat | pause | center\r\n");
    printf(" variants : a state with several variants plays them at random (never twice\r\n");
    printf("            in a row); 'play <id> <variant 1..N>' pins one for tuning\r\n");
    printf(" gain     : gain [amp spd]   permille, 1000 = unchanged (e.g. gain 700 700)\r\n");
    printf(" state    : mode [manual|fsm] | thr <bored_s> <sleep_s> | state\r\n");
    printf(" protocol : EMO:HAPPY|SAD|ANGRY|SURPRISED|LOVING|NEUTRAL  (auto -> fsm)\r\n");
    printf("            EMO:IDLE|BORED|SLEEPING   INTENT:CHAT|GREETING|...\r\n");
    printf("            bare: happy/sad/angry/surprised/loving/neutral/idle/bored/\r\n");
    printf("                  sleeping/sleep/wake\r\n");
    printf(" misc     : help | state\r\n\r\n");
}

static void et_print_list(void)
{
    int i;

    printf("[test] moves (id = name, variants):\r\n");
    for (i = 0; i < (int)MOVE_MAX; i++) {
        uint8_t nv = moves_variant_count((move_id_t)i);

        printf("   %d = %s  (%u variant%s)%s\r\n", i, moves_name((move_id_t)i),
               (unsigned)nv, (nv > 1U) ? "s" : "",
               (i == s_cur_move) ? "   <== current" : "");
    }
    printf("   (0 idle slow look-around x3 / 1 happy wiggle / 2 eight figure-8 /\r\n");
    printf("    3 sad downcast / 4 angry shake / 5 surprised look-up / 6 loving tilt /\r\n");
    printf("    7 bored x2 / 8 sleep)\r\n");
}

static void et_print_state(void)
{
    emo_state_t st = emo_state();

    printf("[test] mode=%s  paused=%d  move=%s  variant %u/%u (%s)\r\n",
           (s_mode == ET_MODE_FSM) ? "fsm" : "manual", s_paused,
           moves_name((move_id_t)s_cur_move),
           (unsigned)(moves_last_variant((move_id_t)s_cur_move) + 1U),
           (unsigned)moves_variant_count((move_id_t)s_cur_move),
           (s_fixed_var < 0) ? "random" : "fixed");
    printf("       manual(amp=%u spd=%u)  actual(amp=%u spd=%u)\r\n",
           s_manual_amp, s_manual_spd, moves_amp_gain(), moves_spd_gain());
    printf("       emo=%s (h=%d e=%d c=%d)\r\n",
           emo_current_name(), st.happiness, st.energy, st.curiosity);
    printf("       mood=%s  idle=%ums  thr(bored>%ums sleep>%ums)\r\n",
           mood_name(mood_fsm_mood()), mood_fsm_idle_ms(),
           mood_fsm_bored_ms(), mood_fsm_sleep_ms());
    printf("       fsm would pick: %s\r\n",
           moves_name(emotion_fsm_move_for(mood_fsm_mood(), emo_current())));
}

/* ==================== 模式切换 ==================== */

static void et_to_manual(void)
{
    if (s_mode != ET_MODE_MANUAL) {
        s_mode = ET_MODE_MANUAL;
        printf("[test] mode -> manual (you pick the move; gain is manual)\r\n");
    }
}

static void et_to_fsm(void)
{
    if (s_mode != ET_MODE_FSM) {
        s_mode = ET_MODE_FSM;
        s_paused = 0;
        printf("[test] mode -> fsm (move + gain follow emo/mood)\r\n");
    }
    s_dirty = 1;
}

/* ==================== 命令处理（主循环里执行，可安全调动作库）==================== */

static void et_handle_cmd(char *line)
{
    char *tok[4];
    int n;

    if (line[0] == '\0') {
        return;
    }

    /* ① 协议行：原样交给正式状态机的解析（用原始行，避免被下面的分词破坏）*/
    if (et_is_protocol_line(line) != 0) {
        et_to_fsm();
        s_paused = 0;
        emotion_fsm_feed_line(line);
        return;
    }

    /* ② 测试自有命令 */
    n = et_split(line, tok, 4);
    if (n == 0) {
        return;
    }

    if ((et_text_eq(tok[0], "help") != 0) || (et_text_eq(tok[0], "?") != 0)) {
        et_print_help();
        return;
    }
    if (et_text_eq(tok[0], "state") != 0) {
        et_print_state();
        return;
    }
    if (et_text_eq(tok[0], "list") != 0) {
        et_print_list();
        return;
    }

    if (et_text_eq(tok[0], "next") != 0) {
        et_to_manual();
        s_manual_move = (s_manual_move + 1) % (int)MOVE_MAX;
        s_paused = 0;
        s_dirty = 1;
        printf("[test] next move: %d = %s\r\n", s_manual_move,
               moves_name((move_id_t)s_manual_move));
        return;
    }

    if (et_text_eq(tok[0], "play") != 0) {
        int id;

        if (n < 2) {
            printf("[test] usage: play <id|name> [variant]   (send list to see the table)\r\n");
            return;
        }
        id = et_find_move(tok[1]);
        if (id < 0) {
            printf("[test] unknown move '%s'   (send list to see the table)\r\n", tok[1]);
            return;
        }
        et_to_manual();
        s_manual_move = id;
        s_paused = 0;
        s_dirty = 1;

        if (n >= 3) {                       /* play <id> <variant 1..N>：固定播一个变体 */
            uint32_t vi;
            uint32_t nv = (uint32_t)moves_variant_count((move_id_t)id);

            if ((et_atoi(tok[2], &vi) != 0) || (vi < 1U) || (vi > nv)) {
                printf("[test] variant must be 1..%u   (e.g. play %d 1)\r\n",
                       (unsigned)nv, id);
                return;
            }
            s_fixed_var = (int)(vi - 1U);   /* 输入与显示都用 1-based，内部 0-based */
            printf("[test] play: %d = %s  fixed variant #%u/%u\r\n",
                   id, moves_name((move_id_t)id), (unsigned)vi, (unsigned)nv);
        } else {                            /* 随机播（= 正式状态机行为）*/
            s_fixed_var = -1;
            printf("[test] play: %d = %s  (random variant)\r\n",
                   id, moves_name((move_id_t)id));
        }
        return;
    }

    if (et_text_eq(tok[0], "repeat") != 0) {
        s_paused = 0;
        s_dirty = 1;
        printf("[test] repeat %s\r\n", moves_name((move_id_t)s_cur_move));
        return;
    }

    if (et_text_eq(tok[0], "pause") != 0) {
        s_paused = 1;
        printf("[test] paused (pose holds; send repeat to resume)\r\n");
        return;
    }

    if (et_text_eq(tok[0], "center") != 0) {
        s_paused = 1;
        move_center(300U);              /* 此时不在播放中，可安全直接调轨迹原语 */
        printf("[test] centered and paused\r\n");
        return;
    }

    if (et_text_eq(tok[0], "gain") != 0) {
        if (n < 3) {
            s_manual_amp = MOVE_GAIN_UNIT;
            s_manual_spd = MOVE_GAIN_UNIT;
        } else {
            uint32_t a;
            uint32_t b;

            if ((et_atoi(tok[1], &a) != 0) || (et_atoi(tok[2], &b) != 0)) {
                printf("[test] usage: gain <amp> <spd>   e.g. gain 700 700 (permille)\r\n");
                return;
            }
            s_manual_amp = (uint16_t)a;
            s_manual_spd = (uint16_t)b;
        }
        et_to_manual();
        s_paused = 0;
        s_dirty = 1;
        printf("[test] manual gain amp=%u spd=%u (clamped to 100~4000)\r\n",
               s_manual_amp, s_manual_spd);
        return;
    }

    if (et_text_eq(tok[0], "mode") != 0) {
        if (n >= 2) {
            if (et_text_eq(tok[1], "fsm") != 0) {
                et_to_fsm();
            } else if (et_text_eq(tok[1], "manual") != 0) {
                et_to_manual();
                s_dirty = 1;
            } else {
                printf("[test] usage: mode [manual|fsm]\r\n");
                return;
            }
        }
        printf("[test] mode = %s\r\n", (s_mode == ET_MODE_FSM) ? "fsm" : "manual");
        return;
    }

    if (et_text_eq(tok[0], "thr") != 0) {
        uint32_t b;
        uint32_t sl;

        if ((n < 3) || (et_atoi(tok[1], &b) != 0) || (et_atoi(tok[2], &sl) != 0)) {
            printf("[test] usage: thr <bored_s> <sleep_s>;  thr 0 0 -> back to 60/180\r\n");
            return;
        }
        if ((b > 3600U) || (sl > 3600U)) {
            printf("[test] too large (max 3600 s)\r\n");
            return;
        }

        if ((b == 0U) && (sl == 0U)) {
            mood_fsm_reset_thresholds();
        } else {
            mood_fsm_set_thresholds(b * 1000U, sl * 1000U);
        }
        mood_fsm_touch();               /* 从现在起重新计时，便于观察降级 */
        printf("[test] timer restarted: bored after %u s, sleep after %u s\r\n",
               mood_fsm_bored_ms() / 1000U, mood_fsm_sleep_ms() / 1000U);
        et_print_state();
        return;
    }

    printf("[test] unknown command '%s'   (send help)\r\n", tok[0]);
}

/* ==================== 入口 ==================== */

void emotion_test(void)
{
    if (moves_init() != 0) {
        printf("[test] moves_init fail\r\n");
        return;
    }

    /* 状态机三件套（复用正式模块，但不挂正式状态机的 tick 钩子 —— 钩子挂测试自己的）*/
    emo_reset();
    mood_fsm_init();
    moves_set_gain(MOVE_GAIN_UNIT, MOVE_GAIN_UNIT);
    moves_set_tick_hook(et_tick_hook);

    s_now_ms = 0;
    s_mode = ET_MODE_MANUAL;
    s_paused = 0;
    s_cur_move = MOVE_IDLE;
    s_manual_move = MOVE_IDLE;
    s_manual_amp = MOVE_GAIN_UNIT;
    s_manual_spd = MOVE_GAIN_UNIT;
    s_dirty = 1;
    s_cmd_ready = 0;
    s_last_mood = mood_fsm_mood();

    et_print_help();
    et_print_state();

    for (;;) {
        s_now_ms += (uint32_t)EMO_FSM_TICK_MS;

        /* ① 执行命令（在钩子外执行，可安全调动作库）*/
        if (s_cmd_ready != 0) {
            s_cmd_ready = 0;
            et_handle_cmd(s_cmd);
        }

        /* ② 收字节（暂停/回中时没有动作在跑，钩子不会回调，所以这里也要轮询）*/
        et_poll_uart();

        /* ③ 推进心情计时（降级阈值默认 60s/180s，可用 thr 临时缩短）*/
        {
            mood_id_t mood = mood_fsm_tick(s_now_ms);

            if (mood != s_last_mood) {
                s_last_mood = mood;
                printf("[test] mood: %s (idle=%ums)\r\n", mood_name(mood), mood_fsm_idle_ms());
                if (s_mode == ET_MODE_FSM) {
                    s_dirty = 1;
                } else {
                    printf("[test] (manual mode: move does not follow mood; use next/play)\r\n");
                }
            }
        }

        /* ④ 选定动作 + 应用增益（变了才打印，避免刷屏）*/
        {
            int want = (s_mode == ET_MODE_FSM)
                     ? (int)emotion_fsm_move_for(mood_fsm_mood(), emo_current())
                     : s_manual_move;

            if ((want != s_cur_move) || (s_dirty != 0)) {
                s_cur_move = want;
                s_dirty = 0;

                if (s_mode == ET_MODE_FSM) {
                    emotion_fsm_apply_gain(mood_fsm_mood(), emo_current());
                } else {
                    moves_set_gain(s_manual_amp, s_manual_spd);
                }

                printf("[test] push move=%s  amp=%u spd=%u  (mode=%s emo=%s mood=%s)\r\n",
                       moves_name((move_id_t)want), moves_amp_gain(), moves_spd_gain(),
                       (s_mode == ET_MODE_FSM) ? "fsm" : "manual",
                       emo_current_name(), mood_name(mood_fsm_mood()));
            }
        }

        /* ⑤ 播一个周期：默认随机挑一个变体（= 正式状态机的行为）；
         *    用 play <id> <variant> 固定过变体时，就一直播那一个（调参方便）。
         *    播放是阻塞的；期间由 et_tick_hook 保持收命令，命令到达即打断。*/
        if (s_paused == 0) {
            if (s_fixed_var < 0) {
                uint8_t v_before = moves_last_variant((move_id_t)s_cur_move);

                moves_play_variant((move_id_t)s_cur_move);

                if (moves_last_variant((move_id_t)s_cur_move) != v_before) {
                    printf("[test] variant #%u/%u\r\n",
                           (unsigned)(moves_last_variant((move_id_t)s_cur_move) + 1U),
                           (unsigned)moves_variant_count((move_id_t)s_cur_move));
                }
            } else {
                moves_play_variant_at((move_id_t)s_cur_move, (uint8_t)s_fixed_var);
            }
        }

        mdelay(EMO_FSM_TICK_MS);
    }
}
