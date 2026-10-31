/******************************************************************************
 * @file     emotion_fsm.c
 * @brief    情感状态机编排层实现（架构图见 emotion_fsm.h）
 *
 * 三件事：
 *   ① 输入：解析 PC 下发的 EMO:xxx / INTENT:xxx（协议见 Baozi_PC/communication.py）
 *   ② 状态：情绪（emotion_manager）+ 心情（mood_fsm，60s 无聊 / 180s 睡觉）
 *   ③ 输出：把 (心情, 情绪) 映射成动作 + 用连续状态调制动作参数，交给动作库播放
 *
 * ---- 舵机活动算法（本次开发的核心）----
 * 分两层，各管一件事，避免把两者揉在一起：
 *
 * 【语义层】emotion_fsm_move_for()：决定"此刻该演哪段"
 *     mood 优先于 emotion（睡着/无聊时，情绪动作先让位）：
 *         MOOD_SLEEPING → sleep；MOOD_BORED → bored；
 *         否则按情绪：happy→happy、sad→sad、angry→angry、
 *                     surprised→surprised、loving→loving、neutral→idle
 *
 * 【强度层】emotion_fsm_apply_gain()：决定"演出多大幅度、多快节奏"
 *     用 PC 端 EmotionManager 的连续状态（千分制）算两个增益：
 *
 *       幅度 amp = 700 + 500 × (happiness + energy) / 2 / 1000      → 700~1200
 *       速度 spd = 700 + 700 × energy / 1000                        → 700~1400
 *
 *     ⚠️ 这里给的是【调制增益】。动作库还会再乘一个【整体幅度基准】MOVE_AMP_BASE（=1200）：
 *        最终输出偏移 = 动作文件里的偏移 × 1.2 × amp/1000
 *        （整体 +20% 是因为 2026-09 上机发现所有动作幅度偏小，见 movements/moves.h）
 *
 *     校验（PC 的映射表取值代入）：
 *         happy      (900,800) → amp 1125 · spd 1260   大而快   （开心）
 *         surprised  (600,1000)→ amp 1100 · spd 1400   最大最快 （一惊）
 *         angry      (200,900) → amp  975 · spd 1330   快而硬   （生气）
 *         loving     (800,600) → amp 1050 · spd 1120   中等偏柔 （爱意）
 *         sad        (200,300) → amp  825 · spd  910   小而慢   （难过）
 *         neutral    (500,500) → amp  950 · spd 1050   基准     （待机）
 *     再叠两层修正：
 *        · curiosity 只作用在【待机】(neutral) 动作上 —— 没人搭理时好奇心越强，
 *          探头张望的幅度越大；amp ×(850 + 300×curiosity/1000)/1000 → 0.85~1.15 倍
 *        · 心情修正：BORED 全体放慢（spd ×0.7）；SLEEPING 固定"到位但极慢"
 *          （amp 1000、spd 600），让垂头和呼吸都清清楚楚。
 *
 *     于是同一段 sad 轨迹，在"刚被批评(energy 低)"与"撒娇式难过"时会呈现不同的
 *     幅度与节奏，而动作的语义（低头、晃、回中）不变 —— 这就是"固定轨迹打底 +
 *     连续状态调制"。
 *
 * ---- 为什么播放期间还能响应命令 ----
 * moves_play_once() 是阻塞式的（内部 mdelay 走轨迹），所以这里用动作库的 tick 钩子
 * （moves_set_tick_hook）：动作每走 20ms 就回调一次 fsm_tick_hook()，在里面
 * 轮询串口 + 推进心情计时，必要时 moves_interrupt() 打断当前周期，
 * 主循环下一拍就会切到新动作。因此从"收到 EMO:xxx"到"动作换过去"最多一个插值步。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/uart.h>
#include <drv/tick.h>                       /* csi_tick_get：SysTick，1 tick = 1ms */
#include "console/sys_console.h"            /* console.uart */
#include "drv/common.h"                     /* mdelay */
#include "board_config.h"
#include "movements/moves.h"                /* 动作库（唯一写舵机的层）*/
#include "emotion/emotion_manager.h"        /* 情绪 + 连续状态 */
#include "emotion/mood_fsm.h"               /* 心情状态机 */
#include "emotion/emotion_fsm.h"
#include "anim/face_ui.h"            /* 屏幕表情接口：情感 -> 【舵机 + 屏幕】双输出 */
#include "anim/face_render.h"        /* face_set_bus_callback：屏幕传输窗口回调 */
#include "servo_test/servo_core.h"   /* servo_bus_event：舵机写 PWM 避让屏幕 DMA */
#include "tracking/track_dual.h"     /* ★ MODE_TRACK：红外跟随四件套（init/step/exit/lost_ms）*/

/* 控制台实例（src/board_init.c 中定义并初始化）*/
extern sys_console_t console;

/* ==================== 串口所有权开关 ====================
 * 1 = 本文件自己轮询 UART0（只适合"只有情感状态机在跑"的入口）；
 * 0 = 默认：UART0 由 src/uart_hal.c 独占（异步中断 + 帧/文本字节级分流），
 *     文本命令经 uart_hal_take_line() → emotion_fsm_feed_line() 送进来。
 * 两者绝不能同时开：会抢同一个 FIFO，丢字节 = 花屏/数据缺条。 */
#ifndef EMO_FSM_OWN_UART
#define EMO_FSM_OWN_UART   0
#endif

/* ==================== 内部状态 ==================== */

static uint32_t   s_now_ms = 0;             /* 距 init 的【真实】毫秒数（csi_tick_get 换算）*/
static uint32_t   s_tick_base = 0;          /* init 时的 SysTick 基准（ms）*/
static move_id_t  s_playing = MOVE_MAX;     /* 当前正在播的动作（MOVE_MAX = 尚未选）*/
static mood_id_t  s_play_mood = MOOD_MAX;   /* 选动作时的心情（用于探测播放中的降级）*/
#if EMO_FSM_OWN_UART
static char       s_rx[40];                 /* 命令行缓冲（本文件自己收串口时用）*/
static int        s_rx_len = 0;
#endif
static char       s_intent[16] = "none";    /* 最近一次意图（预留：屏幕/语调用）*/

/* ==================== 运行模式（★ MODE_TRACK）====================
 * 舵机同一时刻只能有一个所有者：情绪动作（movements）与红外跟随（track_dual）
 * 都会写舵机，所以用模式把它们互斥开。
 *
 *   EMO_MODE_FSM   : 情绪动作 + 屏幕表情（默认）
 *   EMO_MODE_TRACK : 红外跟随独占舵机；屏幕继续跑表情（face_ui_tick 照常调）
 *
 * 切换：串口 TRACK:ON / TRACK:OFF（裸名 track / follow / stop）；
 *       收到任何 EMO: / INTENT: 命令也会自动退出跟随（那是"要它做表情动作"的语义）。
 * 自动退出：由 track_dual 内部决定 —— 丢失 5s（TD_LOST_SWEEP_MS）后先**扫一圈**找目标，
 *           一圈扫完仍没找到才置 track_dual_sweep_done()，这时才退出跟随。*/
#define EMO_MODE_FSM        0
#define EMO_MODE_TRACK      1

static uint8_t  s_mode = EMO_MODE_FSM;
static uint16_t s_step_ms = EMO_FSM_TICK_MS;    /* 主循环节拍：跟随 5ms / 情绪 10ms */
static uint32_t s_face_acc = 0U;                /* 跟随模式下给 face_ui_tick 做 20ms 节流 */

/* ==================== 外部抑制与请求（v2.0 · 合并版新增）====================
 *
 * 【抑制】用户手按在屏幕上时，机器人必须立刻停手（防误触、防夹手）。
 *   （红外"贴太近"那条距离保护已按用户要求关闭，见 behavior.c。）
 *   本层不直接依赖触摸，由 app 层注册一个查询函数：
 *       emotion_fsm_set_inhibit_cb(behavior_inhibit);
 *   返回 1 时：本拍不启动新动作，并 moves_interrupt() 打断正在播的这一段。
 *
 * 【动作请求】卡片 UI 上"摸头 / 点头 / 摇头"这类按钮不自己写舵机，
 *   而是把它扔进请求槽，由本层在下一拍按正式流程播放（限位、总线仲裁、
 *   变体池、增益调制全部照旧）。这样 UI 与后端命令走的是同一条路径。
 */
static int (*s_inhibit_cb)(void) = 0;
static int8_t  s_req_move = -1;             /* -1 = 无请求 */
static int8_t  s_req_track = -1;            /* -1 = 无请求，0 = 要退出跟随，1 = 要进跟随 */
static uint8_t s_inhibited = 0;             /* 上一拍是否处于抑制（用于打印跳变）*/

/* ==================== 小工具 ==================== */

/* 把 s_now_ms 刷成【真实】毫秒（距 init 的 SysTick 差值）。
 * ★ 以前是"每拍自己 +10ms"的虚拟时基：主循环一拍的实际长度并不固定
 *   （分片刷屏 6~16ms、空闲 5ms 延时、动作阻塞播放时靠钩子补 20ms），
 *   于是 30s 的心情阈值在实机上会漂成 20~60s —— 表现为"老是莫名其妙就睡了"。
 * 用 SysTick 差值后，idle 计时与实际墙钟一致，阈值写多少就是多少。 */
static void sync_now_ms(void)
{
    s_now_ms = (uint32_t)(csi_tick_get() - s_tick_base);
}

/* 大小写不敏感比较：line 是否以 prefix 开头 */
static int prefix_eq(const char *line, const char *prefix)
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

/* 大小写不敏感比较：整串相等 */
static int text_eq(const char *a, const char *b)
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

/* 记录"最近一次意图"（空/0 归一化为 "none"）。
 * 抽成函数是因为两处要写：INTENT:xxx 命令、以及 EMO:xxx 时清空。 */
static void set_intent(const char *v)
{
    int i = 0;

    if ((v == 0) || (v[0] == '\0')) {
        v = "none";
    }
    while ((v[i] != '\0') && (i < (int)sizeof(s_intent) - 1)) {
        s_intent[i] = v[i];
        i++;
    }
    s_intent[i] = '\0';
}

/* ==================== 命令处理 ==================== */

/* EMO:HAPPY 这类显式情绪：更新情绪 + 记录一次交互（= PC update_interaction）*/
static void cmd_emotion(const char *value)
{
    emo_id_t e = emo_parse(value);
    int was_sleeping = (mood_fsm_mood() == MOOD_SLEEPING);

    /* ★ 显式情绪命令要压过"意图表情"：屏幕同步是三级优先（心情 > 意图 > 情绪），
     * 若不清掉上一次 INTENT，发过 INTENT:THINK 之后再发 EMO:HAPPY 屏幕会没反应
     * （意图表情一直压着情绪）。这里清成 "none"，下一拍就回到情绪表情。 */
    set_intent(0);

    if (e == EMO_MAX) {
        /* = PC EmotionManager.normalize()：不认识的情绪归一化为 neutral */
        printf("[fsm] unknown emotion '%s' -> neutral\r\n", value);
        e = EMO_NEUTRAL;
    }

    emo_apply(e);
    mood_fsm_touch();       /* 有互动 → 打断无聊/睡眠，回到 ACTIVE（= PC update_interaction）*/

    {
        emo_state_t st = emo_state();
        printf("[fsm] emotion=%s %s (state h=%d e=%d c=%d)\r\n",
               emo_name(e),
               (was_sleeping != 0) ? "[wake]" : "",
               st.happiness, st.energy, st.curiosity);
    }
}

/* EMO:IDLE / EMO:BORED / EMO:SLEEPING 这类显式【心情】下发 */
static void cmd_mood(mood_id_t m)
{
    if (m == MOOD_IDLE) {
        emo_apply(EMO_NEUTRAL);     /* = PC EMOTION_MAP: neutral → "IDLE" */
    }
    mood_fsm_force(m);
    printf("[fsm] mood=%s\r\n", mood_name(m));
}

/* ==================== 模式切换：红外跟随（MODE_TRACK）====================
 * 与情绪动作【互斥】—— 进跟随前必须先让动作库停手（moves_interrupt）；
 * 跟随期间舵机归 track_dual（它自己按 5ms 采样、100ms 走一步、丢失分档）；
 * 退出时 track_dual_exit() 把两轴回中并交还舵机。
 * 屏幕两种模式都在跑：face_ui_tick() 照常。*/

static void mode_enter_track(void)
{
    if (s_mode == EMO_MODE_TRACK) {
        printf("[fsm] already in TRACK\r\n");
        return;
    }

    moves_interrupt();          /* ① 停掉正在播的动作，释放舵机 */
    s_playing   = MOVE_MAX;     /*    回到 FSM 时重新选动作 + 重设增益 */
    s_play_mood = MOOD_MAX;

    track_dual_init();          /* ② 引脚 + 舵机 + 复位内部状态（两轴回中位）*/

    /* ②.5 初始化没走完就别装作进来了：track_dual_step() 会每拍直接 return，
     *      舵机一步不动、也没有心跳日志 —— 那种"静默失败"最难查。 */
    if (track_dual_ready() == 0U) {
        printf("[fsm] TRACK init FAILED (see [TD]/[core] lines above) "
               "-> follow loop will NOT run\r\n");
    }

    s_mode     = EMO_MODE_TRACK;
    s_step_ms  = TD_SAMPLE_MS;  /* ③ 跟随要 5ms 节拍：track_dual_step 内部按
                                 *    TD_SAMPLE_MS 累加采样/去抖/控制节拍 */
    s_face_acc = 0U;

    mood_fsm_touch();               /* 跟随也算一次互动，别一退出就判成无聊/睡觉 */
    (void)face_ui_emotion("surprised");   /* 屏幕：专注地看着目标（想换表情改这一行）*/

    printf("[fsm] mode=TRACK (IR follow ON). exit: TRACK:OFF | stop | EMO:xxx\r\n");
}

static void mode_exit_track(const char *reason)
{
    if (s_mode != EMO_MODE_TRACK) {
        return;
    }

    track_dual_exit();          /* 两轴回中位 + 复位内部状态 */

    s_mode      = EMO_MODE_FSM;
    s_step_ms   = EMO_FSM_TICK_MS;
    s_playing   = MOVE_MAX;     /* 回 FSM 后重新选动作（会打一行 [fsm] mood=... -> move=...）*/
    s_play_mood = MOOD_MAX;

    printf("[fsm] mode=FSM (%s)\r\n", reason);
}

/* 从命令串里取若干个无符号整数（先跳过命令词，再按空格/冒号/等号/逗号分隔）*/
static int parse_u32_list(const char *s, uint32_t *out, int max)
{
    int n = 0;

    while (((*s >= 'A') && (*s <= 'Z')) || ((*s >= 'a') && (*s <= 'z'))) { s++; }

    while ((*s != '\0') && (n < max)) {
        uint32_t v = 0U;

        while ((*s == ' ') || (*s == '\t') || (*s == ':') || (*s == '=') || (*s == ',')) { s++; }
        if ((*s < '0') || (*s > '9')) { break; }

        while ((*s >= '0') && (*s <= '9')) {
            v = (v * 10U) + (uint32_t)(*s - '0');
            s++;
        }
        out[n++] = v;
    }

    return n;
}

static void cmd_help(void)
{
    printf("\r\n[fsm] commands (115200-8N1, CR/LF ending):\r\n");
    printf("  EMO:HAPPY|SAD|ANGRY|SURPRISED|LOVING|NEUTRAL   PC-proto emotion\r\n");
    printf("  EMO:IDLE|BORED|SLEEPING                        PC-proto mood\r\n");
    printf("  INTENT:CHAT|GREETING|QUESTION|COMMAND          PC-proto intent\r\n");
    printf("  TRACK:ON | TRACK:OFF                           IR follow mode (servo mutex)\r\n");
    printf("  TRK:SPD|ACC|GLIDE|TICK|SWEEP|SHOW|DEF          live-tune follow feel\r\n");
    printf("  happy|sad|angry|surprised|loving|neutral       bare-name debug\r\n");
    printf("  idle|bored|sleeping|wake|track|follow|stop     bare-name debug\r\n");
    printf("  state | help\r\n\r\n");
}

static void cmd_state(void)
{
    printf("[fsm] mode=%s mood=%s(%ums idle) emo=%s move=%s amp=%u spd=%u intent=%s\r\n",
           (s_mode == EMO_MODE_TRACK) ? "TRACK" : "FSM",
           mood_name(mood_fsm_mood()), mood_fsm_idle_ms(),
           emo_current_name(), moves_name(s_playing),
           moves_amp_gain(), moves_spd_gain(), s_intent);

    if (s_mode == EMO_MODE_TRACK) {
        printf("[fsm] TRACK: lost=%ums sweep=%s\r\n",
               (unsigned)track_dual_lost_ms(),
               (track_dual_sweep_done() != 0U) ? "DONE(exit next tick)" : "idle/scanning");
        track_dual_show_params();       /* 顺手把手感参数打出来，方便现场调 */
    }
}

/* 处理一个"显式情绪/心情"名字 —— 这是 EMO:xxx 的值部分，也是
 * src/baozi_proto_app.c（协议帧 0x01 / ESP32 状态码 / ESP32 文本 EMO:）的入口。
 *
 * ★ 为什么要把这段抽成公开函数（2026-11 实机问题）：
 *   协议/ESP32 链路原来只调 ui_app_force_emotion()，那是**【只画一张脸】**——
 *   情绪层（emotion_manager / mood_fsm）一点没变。而 emotion_fsm_step() 每拍
 *   都会用 emo_current_name() 重新同步屏幕（见 ④ 段），于是：
 *       触摸手势（脸颊滑动 → emo_force(LOVING) / 额头连点 → emo_force(ANGRY)）
 *       把情绪定在 loving/angry 之后，PC 再发 EMO:HAPPY 只会"闪一下笑脸"，
 *       下一拍就被拉回 loving/angry —— 用户看到的就是"表情改不动了"。
 *   统一走本函数后，任何链路下发的情绪都会同时改掉情绪层，屏幕自然跟得住。
 *
 * 值域（大小写不敏感）：
 *   HAPPY/SAD/ANGRY/SURPRISED/LOVING/NEUTRAL -> 情绪（cmd_emotion）
 *   IDLE / BORED / SLEEPING(=SLEEP/ASLEEP)   -> 心情（cmd_mood）
 *   其它                                       -> 归一化为 neutral（= PC normalize）*/
void emotion_fsm_command_emotion(const char *value)
{
    if ((value == 0) || (value[0] == '\0')) {
        return;
    }

    /* 要它做表情/动作 → 先退出跟随（跟随与动作库互斥，都写舵机）*/
    mode_exit_track("EMO command");

    if (text_eq(value, "idle")) {
        cmd_mood(MOOD_IDLE);
    } else if (text_eq(value, "bored")) {
        cmd_mood(MOOD_BORED);
    } else if (text_eq(value, "sleeping") || text_eq(value, "sleep") ||
               text_eq(value, "asleep")) {
        /* "asleep" 是屏幕侧的名字（face_model 只有 asleep 这个姿势），
         * 协议/ESP32 链路可能把 SLEEPY 直接转成它送进来 —— 一并认。 */
        cmd_mood(MOOD_SLEEPING);
    } else {
        cmd_emotion(value);
    }
}

/* 处理一整行命令（支持 PC 协议前缀，也支持裸名调试）。
 * 【公开】= 协议解析入口：测试入口把收到的行原样交给它，从而与正式状态机共用同一份解析。 */
void emotion_fsm_feed_line(const char *line)
{
    if (line[0] == '\0') {
        return;
    }

    if (prefix_eq(line, "EMO:")) {
        emotion_fsm_command_emotion(line + 4);
        return;
    }

    if (prefix_eq(line, "INTENT:")) {
        const char *v = line + 7;

        /* 意图命令同样意味着"回到情绪表达"，先退出跟随 */
        mode_exit_track("INTENT command");

        set_intent(v);          /* 屏幕会优先显示"有这个意图专用表情"的那个（见下）*/

        /* INTENT 也是一次交互（PC 下发意图必然伴随一次对话）*/
        if (mood_fsm_mood() == MOOD_SLEEPING) {
            mood_fsm_wake();
        } else {
            mood_fsm_touch();
        }
        printf("[fsm] intent=%s\r\n", s_intent);
        return;
    }

    /* ---- 跟随手感实时调参（不用烧录；跟随时调最直观）----
     *   TRK:SPD <yaw> <pitch>           追踪速度（每拍位移 us）
     *   TRK:ACC <us>                    加速度（每拍速度增量）
     *   TRK:GLIDE <yaw> <pitch> <ticks>  死区滑行总量 / 拍数（减速尾巴）
     *   TRK:TICK <ms>                   控制周期（默认 25ms）
     *   TRK:SWEEP <wait_ms> <step_us>   丢失多久开始"扫一圈" / 扫视步长（默认 5000ms / 15us）
     *   TRK:SHOW / TRK:DEF              查看当前参数 / 恢复编译默认 */
    if (prefix_eq(line, "TRK:")) {
        const char *v = line + 4;
        uint32_t p[4];
        int n = parse_u32_list(v, p, 4);

        if (prefix_eq(v, "SPD") && (n >= 2)) {
            track_dual_set_speed(p[0], p[1]);
            printf("[fsm] TRK speed = Y:%u P:%u us/tick\r\n", (unsigned)p[0], (unsigned)p[1]);
        } else if (prefix_eq(v, "ACC") && (n >= 1)) {
            track_dual_set_accel(p[0]);
            printf("[fsm] TRK accel = %u us/tick\r\n", (unsigned)p[0]);
        } else if (prefix_eq(v, "GLIDE") && (n >= 3)) {
            track_dual_set_glide(p[0], p[1], (uint16_t)p[2]);
            printf("[fsm] TRK glide = Y:%u P:%u us / %u ticks\r\n",
                   (unsigned)p[0], (unsigned)p[1], (unsigned)p[2]);
        } else if (prefix_eq(v, "TICK") && (n >= 1)) {
            track_dual_set_ctrl_ms(p[0]);
            printf("[fsm] TRK ctrl = %u ms\r\n", (unsigned)p[0]);
        } else if (prefix_eq(v, "SWEEP") && (n >= 2)) {
            track_dual_set_sweep(p[0], p[1]);
            printf("[fsm] TRK sweep = wait %ums then scan at %uus/tick\r\n",
                   (unsigned)p[0], (unsigned)p[1]);
        } else if (prefix_eq(v, "SHOW")) {
            track_dual_show_params();
        } else if (prefix_eq(v, "DEF")) {
            track_dual_reset_params();
            printf("[fsm] TRK params -> compile defaults\r\n");
            track_dual_show_params();
        } else {
            printf("[fsm] usage: TRK:SPD <yaw> <pitch> | TRK:ACC <us> | TRK:GLIDE <yaw> <pitch> <ticks>"
                   " | TRK:TICK <ms> | TRK:SWEEP <wait_ms> <step_us> | TRK:SHOW | TRK:DEF\r\n");
        }
        return;
    }

    /* TRACK:ON / TRACK:OFF —— 红外跟随模式（上位机/ESP32 用这个切）*/
    if (prefix_eq(line, "TRACK:")) {
        const char *v = line + 6;

        if (text_eq(v, "on") || text_eq(v, "start") || text_eq(v, "1")) {
            mode_enter_track();
        } else if (text_eq(v, "off") || text_eq(v, "stop") || text_eq(v, "0")) {
            mode_exit_track("TRACK:OFF");
        } else {
            printf("[fsm] usage: TRACK:ON | TRACK:OFF\r\n");
        }
        return;
    }

    /* ---- 以下为本机调试用的裸命令（不经过 PC）---- */
    if (text_eq(line, "idle")) {
        cmd_mood(MOOD_IDLE);
    } else if (text_eq(line, "bored")) {
        cmd_mood(MOOD_BORED);
    } else if (text_eq(line, "sleeping") || text_eq(line, "sleep")) {
        cmd_mood(MOOD_SLEEPING);
    } else if (text_eq(line, "wake")) {
        mood_fsm_wake();
    } else if (text_eq(line, "track") || text_eq(line, "follow")) {
        mode_enter_track();
    } else if (text_eq(line, "stop")) {
        mode_exit_track("stop");
    } else if (text_eq(line, "state")) {
        cmd_state();
    } else if (text_eq(line, "help") || text_eq(line, "?")) {
        cmd_help();
    } else {
        mode_exit_track("emotion command");     /* 裸名情绪同样退出跟随 */
        cmd_emotion(line);
    }
}

/* 非阻塞轮询串口：按行解析（CR/LF 结束）
 *
 * ★ 2026-11 合并改动：本函数默认**关闭**（EMO_FSM_OWN_UART = 0）。
 *   原因：合并后的固件里 UART0 由 src/uart_hal.c 独占（异步中断接收 + 字节级
 *   分流），文本命令由 uart_hal_take_line() 取出后调 emotion_fsm_feed_line()。
 *   若这里再自己 csi_uart_receive() 轮询，就会和异步接收抢同一个 FIFO ——
 *   帧里丢几个字节 = 屏幕花屏/数据缺条，而且极难查。
 *   想单独跑情感状态机（main 的 TEST_ENTRY == 3 且不启用 app_core）时，
 *   把宏改回 1 即可恢复"本文件自己是串口所有者"。 */
/* EMO_FSM_OWN_UART 的定义在文件头部（串口所有权开关），此处不再重复定义 */

static void poll_uart(void)
{
#if EMO_FSM_OWN_UART
    char c;

    while (csi_uart_receive(console.uart, &c, 1, 0) == 1) {
        /* 只用 CR/LF 结束一行：空格可能是协议/JSON 消息里的分隔符，不能当行尾 */
        if ((c == '\r') || (c == '\n')) {
            s_rx[s_rx_len] = '\0';
            emotion_fsm_feed_line(s_rx);    /* 空行由 feed_line 内部忽略 */
            s_rx_len = 0;
        } else if (s_rx_len < (int)sizeof(s_rx) - 1) {
            s_rx[s_rx_len++] = c;
        }
    }
#endif
    /* EMO_FSM_OWN_UART == 0：命令来自 app_core_step() 里的 uart_hal_take_line() */
}

/* ==================== 舵机活动算法：语义层 ==================== */

/* (心情, 情绪) → 该演哪段动作。心情优先：睡着/无聊时先演心情动作。
 * 【公开】供测试入口（UserFolder/test/emotion_test.c）复用同一份映射，避免"测试测的是另一套逻辑"。 */
move_id_t emotion_fsm_move_for(mood_id_t mood, emo_id_t emo)
{
    if (mood == MOOD_SLEEPING) {
        return MOVE_SLEEP;      /* 睡姿 + 呼吸（PC: 头回正 + 低头垂眼）*/
    }
    if (mood == MOOD_BORED) {
        return MOVE_BORED;      /* 歪头发呆（PC: 歪头 + LED 变暗）*/
    }

    switch (emo) {
    case EMO_HAPPY:     return MOVE_HAPPY;
    case EMO_SAD:       return MOVE_SAD;
    case EMO_ANGRY:     return MOVE_ANGRY;
    case EMO_SURPRISED: return MOVE_SURPRISED;
    case EMO_LOVING:    return MOVE_LOVING;
    case EMO_NEUTRAL:
    default:            return MOVE_IDLE;   /* 待机：V 字摇摆点头 */
    }
}

/* ==================== 舵机活动算法：强度层 ==================== */

/* 连续情绪状态 + 心情 → 两个调制增益（千分比），写入动作库。
 * 【公开】同上，测试入口用它对比"同一动作在不同情绪下的幅度/节奏"。 */
void emotion_fsm_apply_gain(mood_id_t mood, emo_id_t emo)
{
    emo_state_t st = emo_state();
    uint32_t amp;
    uint32_t spd;

    /* ① 幅度：越开心、越有精神 → 动作越大（700~1200）*/
    amp = 700U + ((uint32_t)(st.happiness + st.energy) * 500U / 2U) / (uint32_t)EMO_SCALE;

    /* ② 速度：精力越足 → 节奏越快（700~1400）*/
    spd = 700U + ((uint32_t)st.energy * 700U) / (uint32_t)EMO_SCALE;

    /* ③ 好奇心只作用在【待机】动作：没事的时候探头张望（0.85~1.15 倍）*/
    if ((emo == EMO_NEUTRAL) && (mood != MOOD_SLEEPING) && (mood != MOOD_BORED)) {
        amp = (amp * (850U + ((uint32_t)st.curiosity * 300U) / (uint32_t)EMO_SCALE)) / 1000U;
    }

    /* ④ 心情修正 */
    if (mood == MOOD_BORED) {
        spd = spd * 7U / 10U;           /* 无聊：整体放慢 */
    } else if (mood == MOOD_SLEEPING) {
        amp = 1000U;                    /* 睡眠：垂头到位 */
        spd = 600U;                     /* 但要极慢（呼吸感）*/
    }

    moves_set_gain((uint16_t)amp, (uint16_t)spd);
}

/* ==================== 模式查询 ==================== */

/* 当前运行模式：0 = 情绪动作（FSM），1 = 红外跟随（TRACK）*/
uint8_t emotion_fsm_mode(void)
{
    return s_mode;
}

/* ==================== 外部抑制 / 动作请求 / 刺激（v2.0）==================== */

/* 注册"该停手了吗"的查询函数（由 app 层提供：手指按在屏上；红外近距那条
 * 距离保护已按用户要求关闭，见 behavior.c 的 BEHAVIOR_IR_INHIBIT）。
 * 传 0 取消。返回 1 = 立刻停手。 */
void emotion_fsm_set_inhibit_cb(int (*cb)(void))
{
    s_inhibit_cb = cb;
}

static int inhibited_now(void)
{
    return (s_inhibit_cb != 0) ? (s_inhibit_cb() ? 1 : 0) : 0;
}

/* UI 请求播一个动作（下一拍生效，走与后端命令完全相同的播放路径）
 *
 * 日志里带上 mode / inhibit / amp 三个现场值，是因为"按了没反应"有三种完全
 * 不同的原因，光看 "ui request move=happy" 分不出来：
 *   mode=TRACK  → 舵机归跟随模式，请求排在队里没人消费（见 step 的 TRACK 分支）
 *   inhibit=1   → 手指还按在屏上，FSM 整拍早返回，动作起不来
 *   amp 很低    → 情绪调制把幅度压小了，动了但看着像没动 */
void emotion_fsm_request_action(uint8_t move_id)
{
    if ((int)move_id >= (int)MOVE_MAX) {
        return;
    }
    s_req_move = (int8_t)move_id;
    printf("[fsm] ui request move=%s (mode=%s inhibit=%d amp=%u)\r\n",
           moves_name((move_id_t)move_id),
           (s_mode == EMO_MODE_TRACK) ? "TRACK" : "FSM",
           inhibited_now(), (unsigned)moves_amp_gain());
}

/* 请求切换红外跟随（UI 的 PLAY 页开关；PC 语音那条走串口命令，最终也落到这里）
 *
 * 为什么 UI 不直接调 mode_enter_track() / mode_exit_track()：
 *   这两个函数会 moves_interrupt() + track_dual_init()（含 io/pwm 配置与
 *   printf），只能在状态机自己那一拍里做 —— UI 是从 app_core_step 的触摸
 *   分支里进来的，而 app_core_step 还可能被 moves 的 20ms 钩子重入调用。
 *   所以 UI 只排队，由 emotion_fsm_step() 在下一拍按正式流程切模式，
 *   与动作请求（s_req_move）完全同一条纪律。 */
void emotion_fsm_request_track(int on)
{
    s_req_track = (on != 0) ? (int8_t)1 : (int8_t)0;
    printf("[fsm] ui request track=%d (mode=%s inhibit=%d)\r\n",
           (int)(s_req_track != 0),
           (s_mode == EMO_MODE_TRACK) ? "TRACK" : "FSM",
           inhibited_now());
}

/* 上报一次"人真的动了"的交互（触摸屏上任何操作：点击 / 滑动 / 按住 /
 * 按卡片按钮 / 脸颊滑动 / 额头连点）。
 *
 * 为什么要单独一个接口：心情层只认"最近一次交互时刻"，而触摸路径
 * （app_core_step → ui_app_handle_gesture）过去只重置了 UI 自己的待机计时
 * （ui_app 的 g_idle_ms），从没告诉过心情层 —— 于是用户在屏幕上玩了两分钟，
 * 30s 一到照样被判成"没人理我"直接睡觉。这是"老是进休眠"的主因之一。
 *
 * 触摸不改情绪，所以这里只重置心情计时；睡着时被摸则按"唤醒"处理。 */
void emotion_fsm_note_interaction(void)
{
    sync_now_ms();      /* 用当前真实时刻重置 idle（触摸发生在 step 之后）*/

    if (mood_fsm_mood() == MOOD_SLEEPING) {
        mood_fsm_wake();
    } else {
        mood_fsm_touch();
    }
}

/* 施加情绪刺激（抚摸 / 夸奖 / 责备 / 玩耍）：**不直接改情绪**，
 * 走 emotion_manager 的连续状态，漂移结果再同步到屏幕。 */
void emotion_fsm_stimulus(uint8_t kind, uint8_t amount)
{
    emo_id_t   e  = emo_stimulus(kind, amount);
    emo_state_t st = emo_state();

    sync_now_ms();                           /* 用当前真实时刻当"交互时刻" */
    mood_fsm_touch();                        /* 有互动：打断无聊/睡眠 */
    printf("[fsm] stimulus kind=%u amount=%u -> emo=%s (h=%d e=%d c=%d)\r\n",
           (unsigned)kind, (unsigned)amount, emo_name(e),
           (int)st.happiness, (int)st.energy, (int)st.curiosity);
    face_ui_emotion(emo_current_name());     /* 屏幕同步（卡片层里只记录不抢屏）*/
}

/* ==================== 播放期钩子 ==================== */

/* 动作库每 20ms 回调一次：轮询串口 + 刷新真实时基 + 保活屏幕；
 * 心情在播放中变化（例如命令把心情改了）时请求打断当前周期。
 * 注意：心情降级判断只在主循环的 emotion_fsm_step() 里做（这里只刷新时基），
 * 所以一段阻塞动作播到一半不会被判成"睡着"而中断 —— 降级最多延后一轮动作。 */
static void fsm_tick_hook(void)
{
    sync_now_ms();      /* 动作阻塞播放期间也要让 idle 计时跟上真实时间 */
    poll_uart();

    /* 屏幕也跟着心跳 —— 关键：动作是【阻塞播放】的（一播 1.5~7 秒），
     * 屏幕 tick 挂在这里，眨眼与待机动作才不会因为舵机在动而停下。
     * MOVE_TICK_MS 与 FACE_TICK_MS 同为 20ms，节奏天然一致。 */
    face_ui_tick();

    /* ★ 播放途中一旦被摸到屏幕（现在唯一的抑制依据）→ 立刻打断本轮动作 */
    if (inhibited_now()) {
        if (!s_inhibited) {
            s_inhibited = 1;
            printf("[fsm] inhibit ON (touch/near) -> stop moving\r\n");
        }
        moves_interrupt();
    }

    if (mood_fsm_mood() != s_play_mood) {
        moves_interrupt();
    }

    /* ★ UI 按钮点名的动作必须【立刻】接上。
     *   动作是阻塞播放的，一轮待机变体 18~23 秒；不在这里打断，用户按了"点头"
     *   要等当前那轮扫视整个走完才有反应 —— 实机表现就是"按了没反应"。
     *   （不是靠抑制 gate 兜底：抑制只在手指按下的那 20ms 内有效，且 TRACK
     *     模式下根本不走播放分支，请求会一直挂在队列里。） */
    if (s_req_move >= 0) {
        moves_interrupt();
    }
}

/* ==================== 生命周期 ==================== */

int emotion_fsm_init(void)
{
    if (moves_init() != 0) {
        printf("[fsm] moves_init fail\r\n");
        return -1;
    }

    emo_reset();
    mood_fsm_init();

    s_now_ms = 0;
    s_playing = MOVE_MAX;
    s_play_mood = MOOD_MAX;
#if EMO_FSM_OWN_UART
    s_rx_len = 0;
#endif

    /* 挂上播放期钩子：动作一播几秒也不影响收命令 */
    moves_set_tick_hook(fsm_tick_hook);

    /* ★ 总线仲裁：屏幕整屏刷新是 60 段 DMA，若舵机同时也在写 PWM 寄存器，
     *   两者抢 AHB 会把 SPI FIFO 抽空 → 屏幕出白块（实测：不做舵机输出就
     *   完全不闪）。注册回调后，渲染端会在每段前后的空隙里放行舵机的写。
     *   必须在 face_ui_init() 之前 —— 它内部的 face_init() 就有开机整屏清屏。 */
    face_set_bus_callback(servo_bus_event);

    /* 屏幕：初始化 + 交给情感状态机驱动（此后屏幕侧不再自己读串口）*/
    face_ui_init();
    (void)face_ui_emotion(emo_current_name());

    /* ★ 真实毫秒的 0 点：放在所有初始化之后，开机清屏那几百毫秒不该算进 idle。
     *   mood_fsm 的 s_last_ms 也是 0，两者对齐后"上电 = 刚有人操作过"。 */
    s_tick_base = csi_tick_get();
    s_now_ms = 0;

    printf("[fsm] emotion_fsm init ok. type 'help' for commands\r\n");
    return 0;
}

void emotion_fsm_step(void)
{
    mood_id_t mood;
    move_id_t want;

    sync_now_ms();      /* 真实毫秒时基（见 sync_now_ms 的注释）*/

    /* ① 收命令（非阻塞）—— 命令里会更新情绪/心情，也可能切模式 */
    poll_uart();

    /* ①.5 ★ 抑制 gate：用户手指按在屏上 → 立刻停手（距离保护已关闭，只看触摸）。
     *   两种模式都生效（跟随模式下按屏同样不该继续转舵机）。
     *   本拍直接返回：不启动动作、不推进跟随，把 CPU 让给触摸/屏幕。 */
    if (inhibited_now()) {
        if (!s_inhibited) {
            s_inhibited = 1;
            printf("[fsm] inhibit ON (touch) -> hold\r\n");
        }
        moves_interrupt();
        face_ui_tick();
        return;
    }
    if (s_inhibited) {
        s_inhibited = 0;
        printf("[fsm] inhibit OFF -> resume\r\n");
    }

    /* ② 推进心情计时（60s 无聊 / 180s 睡觉）—— 两种模式都要走，
     *    否则跟随几分钟后一退出就会被判成"很久没互动"、直接进无聊/睡眠 */
    mood = mood_fsm_tick(s_now_ms);

    /* ★ 跟随开关（UI 的 PLAY 页按钮 / PC 语音 TRACK:ON 都落到这里排队）：
     *   必须在下面的 TRACK 分支之前消费 —— 那个分支是直接 return 的，
     *   晚一拍处理就意味着"点了按钮没反应"。
     *   同一拍若还挂着动作请求，一并作废：两者语义互斥，否则紧接着那句
     *   "TRACK 模式 + 有动作请求 → 退出跟随"会立刻把刚进去的模式踢回来。 */
    if (s_req_track >= 0) {
        int8_t want_trk = s_req_track;

        s_req_track = -1;
        s_req_move  = -1;
        if (want_trk != 0) {
            mode_enter_track();
        } else {
            mode_exit_track("track request");
        }
    }

    /* ★ 手动按钮优先级最高：跟随模式下也认。
     *   不这么写的话，TRACK 模式会在下面的分支里直接 return，UI 请求永远
     *   排不到队首 —— 用户按"点头"时脖子正在跟着红外转，看着就是彻底没反应。 */
    if ((s_mode == EMO_MODE_TRACK) && (s_req_move >= 0)) {
        mode_exit_track("ui request");
    }

    /* ★ MODE_TRACK：舵机交给红外跟随；动作库停手；屏幕照常跑表情 */
    if (s_mode == EMO_MODE_TRACK) {
        s_face_acc += (uint32_t)TD_SAMPLE_MS;
        if (s_face_acc >= 20U) {        /* face_ui_tick 期望 ~20ms 节拍 */
            s_face_acc = 0U;
            face_ui_tick();
        }

        track_dual_step();              /* 采样 → 去抖 → 每 25ms 走一步（非阻塞）*/

        /* 退出条件：丢失 5s 后 track_dual 会主动扫一圈；一圈扫完仍没目标才退出
         * （不再"丢 3s 就退"——扫视期间只要瞄到目标就自动恢复跟随）*/
        if (track_dual_sweep_done() != 0U) {
            mode_exit_track("sweep done, no target");
        }
        return;
    }

    /* ②.5 ★ UI 请求的动作（卡片上的"点头 / 摇头 / 摸头"按钮）优先播一段。
     *     走与后端命令完全相同的播放路径（限位/变体池/增益调制/总线仲裁）。
     *     播放是阻塞的，期间由 fsm_tick_hook 保活屏幕与触摸 —— 所以用户
     *     随时能再摸屏幕把它打断（抑制 gate 在上方）。 */
    if (s_req_move >= 0) {
        move_id_t req = (move_id_t)s_req_move;

        s_req_move = -1;
        s_playing  = req;
        s_play_mood = mood_fsm_mood();
        printf("[fsm] play ui-requested move=%s\r\n", moves_name(req));
        moves_play_variant(req);
        return;
    }

    /* ③ 选动作：心情或情绪变了就切换（并把新参数写进动作库）*/
    want = emotion_fsm_move_for(mood, emo_current());
    if ((want != s_playing) || (mood != s_play_mood)) {
        s_playing = want;
        s_play_mood = mood;
        emotion_fsm_apply_gain(mood, emo_current());

        printf("[fsm] mood=%s emo=%s -> move=%s (amp=%u spd=%u)\r\n",
               mood_name(mood), emo_current_name(), moves_name(want),
               moves_amp_gain(), moves_spd_gain());
    }

    /* ④ 播一个周期：从该状态的【变体池】里随机挑一个（不与上次相同）——
     *    这样同一个状态连续播几轮，动作序列也不会重复，消除"同一动作循环"的机械感。
     *    播放是阻塞的；期间由 fsm_tick_hook 保持收命令，命令一到就打断本轮。*/
    {
        uint8_t v_before = moves_last_variant(s_playing);

        /* 同步屏幕：三级优先 —— 心情 > 意图专用表情 > 情绪。
         *   · 心情（睡着 / 无聊）优先，与 emotion_fsm_move_for() 同一优先级；
         *   · 其次是意图：INTENT:THINK / LISTEN / QUESTION 有专用表情（会动），
         *     而 INTENT:CHAT 这类没有专用表情 —— face_ui_intent() 返回 -1 就落回情绪；
         *   · 最后才是情绪（六个情绪各有模型 + 动作）。
         * face_ui_* 内部有“已是同一姿势就返回”的判断，每拍都调也很便宜。
         * 注：意图表情会一直保持到下一个 INTENT / EMO 命令（没有超时回退，
         *     那属于"瞬时表情"层，等做被抚摸等事件反应时一起补）。 */
        if ((mood == MOOD_SLEEPING) || (mood == MOOD_BORED)) {
            (void)face_ui_mood(mood_name(mood));    /* sleeping -> asleep；bored -> neutral */
        } else if (face_ui_intent(s_intent) != 0) {
            (void)face_ui_emotion(emo_current_name());  /* 该意图无专用表情 -> 跟情绪走 */
        }

        moves_play_variant(s_playing);

        if (moves_last_variant(s_playing) != v_before) {
            printf("[fsm] %s variant #%u/%u\r\n",
                   moves_name(s_playing),
                   (unsigned)(moves_last_variant(s_playing) + 1U),
                   (unsigned)moves_variant_count(s_playing));
        }
    }
}

void emotion_fsm_run(void)
{
    if (emotion_fsm_init() != 0) {
        return;
    }

    cmd_help();
    cmd_state();

    for (;;) {
        emotion_fsm_step();
        mdelay(s_step_ms);      /* 节拍跟着模式走：跟随 5ms / 情绪 10ms */
    }
}
