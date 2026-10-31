/******************************************************************************
 * @file     servo_motion_test.c
 * @brief    动作库测试台（串口直接播放 moves 里的动作，115200-8N1）
 *
 * ★ 2026-10-31 改版：原来这里是「正弦 / 扫掠 / 8 字」的运动状态测试，需要自己
 *   算轨迹、自己调速度振幅。现在改成【直接跑动作库】—— 串口选一个动作就播，
 *   走的是和正式固件完全相同的代码路径（moves_* → apply_off → servo_set_pulse），
 *   所以这里能跑通，正式固件里就一定跑得通；限位、限速、总线仲裁全都一致。
 *
 * 入口：src/main.c 的 TEST_ENTRY = 5（只初始化舵机 + 动作库 + console，
 *       不初始化屏幕/触摸/UI，所以不会和 UI 抢总线，也不受刷屏干扰）。
 *
 * ============================ 串口命令 ============================
 *   h / ?              帮助
 *   i                  状态（当前增益、两路脉宽、是否在播放）
 *   l                  列出所有动作 id 与变体数
 *   p<id>              播放该动作（随机变体），如 p7
 *   p<id>.<v>          播放指定变体，如 p7.1（v 从 0 起）
 *   d                  演示：从 IDLE 到 SLEEP 依次播一遍
 *   g<amp>,<spd>       设调制增益（千分比），如 g1200,1400
 *   r                  增益复位 1000,1000
 *   s                  中断当前动作（停在原地）
 *   0 / 1              选通道（手动定位用）
 *   w<us>              平滑走到该脉宽，如 w1200
 *   j<us>              立即跳到该脉宽（看跳变/供电，慎用）
 *   +[us] / -[us]      在当前脉宽上微调，默认 20us
 *   c / b              当前通道 / 两路 平滑回中位
 *
 * 播放期间【任意键都会立刻打断】，打完的那一行会在动作停下后自动执行 ——
 * 所以动到一半直接敲 p4 切下一个、或者回车停住，都能用。
 *
 * ============================ 典型用法 ============================
 *   1) 上电自动回中位（servo_core_init 在 moves_init 里），两路应停住无异响
 *   2) `l` 看动作表 → `p4` 播生气、`p5` 播惊讶，听有没有嗡响/堵转
 *   3) 如果串口开始刷 `[mv] CLIP: ...`，说明该动作的幅度顶到了实测限位，
 *      后面那行括号里就是允许范围，一眼能看出超了多少（见 moves.c apply_off）
 *   4) `g1200,1400` 把增益拉到上限再播一遍 —— 这是正式固件里"情绪最激动"
 *      时的幅度/速度，能跑通才算真的没问题
 *   5) `g700,700` 是"最没精神"的组合，可对比同一动作的大小/快慢
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/common.h>     /* mdelay */
#include <drv/tick.h>       /* csi_tick_get：SysTick，1 tick = 1ms */
#include <drv/uart.h>       /* csi_uart_receive（超时 0 = 非阻塞）*/
#include "servo_core.h"     /* 中位/限位宏（servo_pins.h 也由它带进来）*/
#include "movements/moves.h"/* moves_init / moves_play_variant / moves_set_gain ... */
#include "servo_motion_test.h"

/* printf 用的就是它，定义在 components/minilibc/v1.0/src/minilibc_port.c
 * （与 uart_hal.c / servo_range_test.c 同一做法：复用 console 句柄，不新建实例）*/
extern csi_uart_t console_uart;

/* ============================== 参数 ============================== */
#define TICK_MS             10U         /* 手动定位的平滑节拍（动作库自己是 20ms）*/
#define DT_MAX_MS           100U        /* 单拍 dt 上限：卡顿后不要一次跳太远 */
#define SMOOTH_SPEED        4000U       /* 手动定位的平滑速度 us/s（≈0.35s 走完全程）*/
#define STEP_DEFAULT_US     20U         /* +/- 微调步长 */
#define LINE_MAX            40U

/* 增益可调范围：正式固件里 amp 是 700~1200（中性再 ×1.15），spd 是 700~1400。
 * 这里放宽到 600~1500，方便试探"比正式固件更极端"的情况。*/
#define GAIN_MIN            600U
#define GAIN_MAX            1500U

/* ============================== 手动定位状态 ============================== */
typedef struct {
    uint32_t    ch;
    const char *axis;
    const char *pin;
    uint32_t    center;
    uint32_t    lim_lo;
    uint32_t    lim_hi;
    uint32_t    cur;        /* 当前实际输出脉宽 us */
    uint32_t    tgt;        /* 平滑目标 */
    uint8_t     moving;     /* 1 = 正在平滑走向 tgt */
} mch_t;

#define MCH_YAW             0U
#define MCH_PITCH           1U
#define MCH_NUM             2U

static mch_t   s_ch[MCH_NUM];
static uint32_t s_sel = MCH_YAW;

/* ============================== 播放状态 ============================== */
static uint8_t  s_playing = 0U;         /* 1 = moves 正在阻塞播放 */
static uint8_t  s_abort_demo = 0U;      /* 1 = demo 播到一半被新命令打断 */

/* 播放期间敲下的命令：等动作停下后再执行，避免在 moves 的调用栈里递归进新动作 */
static char     s_pending[LINE_MAX];
static uint8_t  s_pending_ok = 0U;
static char     s_pline[LINE_MAX];
static uint8_t  s_pline_len = 0U;

/* ============================== 工具 ============================== */

static uint32_t clamp_lim(const mch_t *c, int32_t v)
{
    if (v < (int32_t)c->lim_lo) {
        return c->lim_lo;
    }
    if (v > (int32_t)c->lim_hi) {
        return c->lim_hi;
    }
    return (uint32_t)v;
}

/* 直接输出（跳过平滑），只在手动定位用；动作播放期间由 moves 自己下发 */
static void out_pulse(mch_t *c, int32_t pulse)
{
    uint32_t p = clamp_lim(c, pulse);

    if (p != c->cur) {
        c->cur = p;
        servo_set_pulse(c->ch, p);
    }
}

static int parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0U;

    if ((s == 0) || (*s < '0') || (*s > '9')) {
        return -1;
    }
    while ((*s >= '0') && (*s <= '9')) {
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

/* 解析 "a,b" */
static int parse_pair(const char *s, uint32_t *a, uint32_t *b)
{
    if (parse_u32(s, a) != 0) {
        return -1;
    }
    while ((*s != '\0') && (*s != ',')) {
        s++;
    }
    if (*s != ',') {
        return -1;
    }
    return parse_u32(s + 1, b);
}

/* 解析 "id" 或 "id.v"；var = -1 表示没指定变体 */
static int parse_id_var(const char *s, uint32_t *id, int *var)
{
    uint32_t v = 0U;
    uint32_t w = 0U;

    *var = -1;
    if (parse_u32(s, &v) != 0) {
        return -1;
    }
    while ((*s != '\0') && (*s != '.')) {
        s++;
    }
    if (*s == '.') {
        if (parse_u32(s + 1, &w) != 0) {
            return -1;
        }
        *var = (int)w;
    }
    *id = v;
    return 0;
}

/* ============================== 打印 ============================== */

static void print_off(const mch_t *c)
{
    int32_t  off = (int32_t)c->cur - (int32_t)c->center;
    uint32_t mag = (off >= 0) ? (uint32_t)off : (uint32_t)(-off);
    uint32_t d10 = mag * 10U / 11U;     /* 0.1 度（11.11us ≈ 1°）*/

    printf("%c%uus(%u.%u deg)", (off >= 0) ? '+' : '-', mag, d10 / 10U, d10 % 10U);
}

static void print_one(uint32_t idx)
{
    const mch_t *c = &s_ch[idx];

    printf("[mot] %-5s %s ch%u pulse=%uus ", c->axis, c->pin, c->ch, c->cur);
    print_off(c);
    printf("%s%s\r\n", c->moving ? " -> slewing" : "",
           (idx == s_sel) ? "  <=sel" : "");
}

static void print_status(void)
{
    printf("[mot] gain amp=%u spd=%u (1000=as-designed)   %s\r\n",
           moves_amp_gain(), moves_spd_gain(),
           s_playing ? "PLAYING" : "idle");
    print_one(MCH_YAW);
    print_one(MCH_PITCH);
}

static void print_actions(void)
{
    int i;

    printf("\r\n--- moves (id: name, variants) ---\r\n");
    for (i = 0; i < (int)MOVE_MAX; i++) {
        printf("  %d  %-10s x%u\r\n", i, moves_name((move_id_t)i),
               moves_variant_count((move_id_t)i));
    }
    printf("  play: p<id>  or  p<id>.<variant>   (e.g. p7 , p7.1)\r\n\r\n");
}

static void print_help(void)
{
    printf("\r\n==== motion test: play the real move library (YAW %s ch%u / PITCH %s ch%u) ====\r\n",
           SERVO_YAW_PIN_TXT, SERVO_YAW_CH, SERVO_PITCH_PIN_TXT, SERVO_PITCH_CH);
    printf(" h / ?          help\r\n");
    printf(" i              status (gain / pulse / playing)\r\n");
    printf(" l              list actions and their variant count\r\n");
    printf(" p<id>          play action, random variant  e.g. p4\r\n");
    printf(" p<id>.<v>      play one specific variant    e.g. p4.1\r\n");
    printf(" d              demo: play IDLE..SLEEP one by one\r\n");
    printf(" g<amp>,<spd>   set gain permille %u~%u  e.g. g1200,1400\r\n", GAIN_MIN, GAIN_MAX);
    printf(" r              reset gain to 1000,1000\r\n");
    printf(" s              stop current action (hold where it is)\r\n");
    printf(" --- manual positioning (not needed for playing actions) ---\r\n");
    printf(" 0 / 1          select channel 0=YAW %s  1=PITCH %s\r\n",
           SERVO_YAW_PIN_TXT, SERVO_PITCH_PIN_TXT);
    printf(" w<us>          slew to pulse    e.g. w1200\r\n");
    printf(" j<us>          JUMP to pulse    e.g. j1200\r\n");
    printf(" +[us] / -[us]  nudge, default %uus\r\n", STEP_DEFAULT_US);
    printf(" c / b          center current / both\r\n");
    printf(" limits: YAW %u~%u  PITCH %u~%u us (center %u / %u)\r\n",
           SERVO_YAW_MIN, SERVO_YAW_MAX, SERVO_PITCH_MIN, SERVO_PITCH_MAX,
           SERVO_YAW_CENTER, SERVO_PITCH_CENTER);
    printf(" during playback: ANY key interrupts, the typed line runs afterwards.\r\n\r\n");
}

/* ============================== 手动定位 ============================== */

static void cmd_goto(uint32_t us)
{
    mch_t *c = &s_ch[s_sel];

    c->tgt    = clamp_lim(c, (int32_t)us);
    c->moving = 1U;
    if (c->tgt != us) {
        printf("[mot] %u out of limit, clamped to %uus\r\n", us, c->tgt);
    }
}

static void cmd_jump(uint32_t us)
{
    mch_t *c = &s_ch[s_sel];

    c->moving = 0U;
    c->tgt    = clamp_lim(c, (int32_t)us);
    out_pulse(c, (int32_t)c->tgt);
    printf("[mot] %s JUMP to %uus\r\n", c->axis, c->cur);
}

static void cmd_center(int both)
{
    uint32_t i;
    uint32_t first = (both != 0) ? 0U : s_sel;
    uint32_t last  = (both != 0) ? (MCH_NUM - 1U) : s_sel;

    for (i = first; i <= last; i++) {
        s_ch[i].tgt    = s_ch[i].center;
        s_ch[i].moving = 1U;
    }
    printf("[mot] center -> %uus / %uus\r\n",
           s_ch[MCH_YAW].center, s_ch[MCH_PITCH].center);
}

/* 每拍推进平滑定位（动作播放期间主循环不跑，这里也就不会动） */
static void motion_step(uint32_t dt)
{
    uint32_t step = SMOOTH_SPEED * dt / 1000U;
    uint32_t i;

    if (step == 0U) {
        step = 1U;
    }

    for (i = 0U; i < MCH_NUM; i++) {
        mch_t *c = &s_ch[i];

        if (c->moving == 0U) {
            continue;
        }
        if (c->cur < c->tgt) {
            uint32_t next = c->cur + step;

            if (next >= c->tgt) {
                next = c->tgt;
                c->moving = 0U;
            }
            out_pulse(c, (int32_t)next);
        } else if (c->cur > c->tgt) {
            uint32_t next = (c->cur > step) ? (c->cur - step) : 0U;

            if (next <= c->tgt) {
                next = c->tgt;
                c->moving = 0U;
            }
            out_pulse(c, (int32_t)next);
        } else {
            c->moving = 0U;
        }
    }
}

/* 动作播放结束后，把手动定位的"当前位置"同步成动作库的真实位置
 * （moves 有 move_yaw_off / move_pitch_off，被中断时也能拿到半路的值）。
 *
 * 这里要和 moves.c 的 apply_off() 一样留 SAFE_MARGIN 的余量：动作被钳制时，
 * move_*_off() 返回的仍是【未钳制】的逻辑偏移，直接拿来当起点，下一次 +/- 
 * 会从一个根本到不了的位置算起，表现就是"按一下跳一大截"。*/
#define SYNC_MARGIN_US      10U

static void sync_cur_from_moves(void)
{
    int32_t y = (int32_t)SERVO_YAW_CENTER + (int32_t)move_yaw_off();
    int32_t p = (int32_t)SERVO_PITCH_CENTER + (int32_t)move_pitch_off();

    if (y > (int32_t)(SERVO_YAW_MAX - SYNC_MARGIN_US)) {
        y = (int32_t)(SERVO_YAW_MAX - SYNC_MARGIN_US);
    } else if (y < (int32_t)(SERVO_YAW_MIN + SYNC_MARGIN_US)) {
        y = (int32_t)(SERVO_YAW_MIN + SYNC_MARGIN_US);
    }
    if (p > (int32_t)(SERVO_PITCH_MAX - SYNC_MARGIN_US)) {
        p = (int32_t)(SERVO_PITCH_MAX - SYNC_MARGIN_US);
    } else if (p < (int32_t)(SERVO_PITCH_MIN + SYNC_MARGIN_US)) {
        p = (int32_t)(SERVO_PITCH_MIN + SYNC_MARGIN_US);
    }

    s_ch[MCH_YAW].cur      = (uint32_t)y;
    s_ch[MCH_YAW].tgt      = (uint32_t)y;
    s_ch[MCH_YAW].moving   = 0U;

    s_ch[MCH_PITCH].cur    = (uint32_t)p;
    s_ch[MCH_PITCH].tgt    = (uint32_t)p;
    s_ch[MCH_PITCH].moving = 0U;
}

/* ============================== 播放 ============================== */

static void handle_line(char *line);

/* 播放期的 20ms 钩子：轮询串口。
 * 只要收到任何字节就请求中断（别让用户干等 7 秒），
 * 打完整的一行则先存下来，等动作停下后再执行。 */
static void play_hook(void)
{
    char c;

    while (csi_uart_receive(&console_uart, &c, 1U, 0U) > 0) {
        if ((c == '\r') || (c == '\n')) {
            if (s_pline_len > 0U) {
                uint8_t i;

                for (i = 0U; (i < (LINE_MAX - 1U)) && (s_pline[i] != '\0'); i++) {
                    s_pending[i] = s_pline[i];
                }
                s_pending[i] = '\0';
                s_pending_ok = 1U;
                s_pline_len  = 0U;
            }
            moves_interrupt();
            continue;
        }
        if ((c == 0x08) || (c == 0x7F)) {
            if (s_pline_len > 0U) {
                s_pline_len--;
            }
            continue;
        }
        if ((c < 32) || (c > 126)) {
            continue;
        }
        if (s_pline_len < (LINE_MAX - 1U)) {
            s_pline[s_pline_len++] = c;
        }
        moves_interrupt();      /* 一打字就停，屏幕上马上有反馈 */
    }
}

/* 播放一个动作（阻塞）。var < 0 表示随机变体。 */
static void play_action(uint32_t id, int var)
{
    if (id >= (uint32_t)MOVE_MAX) {
        printf("[mot] bad id %u (0~%d, see `l`)\r\n", id, (int)MOVE_MAX - 1);
        return;
    }

    s_ch[MCH_YAW].moving   = 0U;    /* 手动平滑先停掉，别和动作抢位置 */
    s_ch[MCH_PITCH].moving = 0U;

    printf("[mot] play %s", moves_name((move_id_t)id));
    if (var >= 0) {
        printf(" variant %d", var);
    }
    printf(" (gain %u/%u)\r\n", moves_amp_gain(), moves_spd_gain());

    s_playing = 1U;
    moves_set_tick_hook(play_hook);     /* 播放期间靠它轮询串口 */

    if (var >= 0) {
        moves_play_variant_at((move_id_t)id, (uint8_t)var);
    } else {
        moves_play_variant((move_id_t)id);
    }

    s_playing = 0U;
    moves_set_tick_hook(0);             /* 空闲时由主循环自己收串口 */

    sync_cur_from_moves();
    printf("[mot] done: yaw=%uus pitch=%uus\r\n",
           s_ch[MCH_YAW].cur, s_ch[MCH_PITCH].cur);

    /* 播放期间敲下的命令，现在执行（最多一层递归：p → p 是正常的） */
    if (s_pending_ok != 0U) {
        char    tmp[LINE_MAX];
        uint8_t i;

        s_pending_ok = 0U;
        s_abort_demo = 1U;          /* 有新命令了，demo 不要再往下播 */
        for (i = 0U; (i < (LINE_MAX - 1U)) && (s_pending[i] != '\0'); i++) {
            tmp[i] = s_pending[i];
        }
        tmp[i] = '\0';
        printf("[mot] run pending: %s\r\n", tmp);
        handle_line(tmp);
    }
}

static void play_demo(void)
{
    uint32_t i;

    s_abort_demo = 0U;
    printf("[mot] demo: %d actions, any key to skip\r\n", (int)MOVE_MAX);
    for (i = 0U; i < (uint32_t)MOVE_MAX; i++) {
        play_action(i, -1);
        if (s_abort_demo != 0U) {
            s_abort_demo = 0U;
            printf("[mot] demo aborted at id %u\r\n", i);
            break;
        }
    }
}

/* ============================== 命令处理 ============================== */

static void handle_line(char *line)
{
    char cmd;
    uint32_t v = 0U;

    while ((*line == ' ') || (*line == '\t')) {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    cmd = line[0];

    switch (cmd) {
    case 'h':
    case 'H':
    case '?':
        print_help();
        return;

    case 'i':
    case 'I':
        print_status();
        return;

    case 'l':
    case 'L':
        print_actions();
        return;

    case 'p':
    case 'P': {
        uint32_t id = 0U;
        int var = -1;

        if (parse_id_var(line + 1, &id, &var) != 0) {
            printf("[mot] usage: p<id> or p<id>.<variant>, e.g. p4 , p4.1\r\n");
            return;
        }
        play_action(id, var);
        return;
    }

    case 'd':
    case 'D':
        play_demo();
        return;

    case 'g':
    case 'G': {
        uint32_t amp = 0U;
        uint32_t spd = 0U;

        if (parse_pair(line + 1, &amp, &spd) != 0) {
            printf("[mot] usage: g<amp>,<spd>, e.g. g1200,1400\r\n");
            return;
        }
        if (amp < GAIN_MIN) {
            amp = GAIN_MIN;
        } else if (amp > GAIN_MAX) {
            amp = GAIN_MAX;
        }
        if (spd < GAIN_MIN) {
            spd = GAIN_MIN;
        } else if (spd > GAIN_MAX) {
            spd = GAIN_MAX;
        }
        moves_set_gain((uint16_t)amp, (uint16_t)spd);
        printf("[mot] gain = %u/%u\r\n", moves_amp_gain(), moves_spd_gain());
        return;
    }

    case 'r':
    case 'R':
        moves_set_gain(1000U, 1000U);
        printf("[mot] gain reset to 1000/1000\r\n");
        return;

    case 's':
    case 'S':
        moves_interrupt();
        s_ch[MCH_YAW].moving   = 0U;
        s_ch[MCH_PITCH].moving = 0U;
        sync_cur_from_moves();
        printf("[mot] stop\r\n");
        break;

    case '0':
        s_sel = MCH_YAW;
        printf("[mot] select YAW %s\r\n", SERVO_YAW_PIN_TXT);
        return;

    case '1':
        s_sel = MCH_PITCH;
        printf("[mot] select PITCH %s\r\n", SERVO_PITCH_PIN_TXT);
        return;

    case 'w':
    case 'W':
        if (parse_u32(line + 1, &v) != 0) {
            printf("[mot] usage: w<us>, e.g. w1200\r\n");
            return;
        }
        cmd_goto(v);
        break;

    case 'j':
    case 'J':
        if (parse_u32(line + 1, &v) != 0) {
            printf("[mot] usage: j<us>, e.g. j1200\r\n");
            return;
        }
        cmd_jump(v);
        break;

    case '+': {
        mch_t *c = &s_ch[s_sel];

        v = STEP_DEFAULT_US;
        (void)parse_u32(line + 1, &v);
        if (v == 0U) {
            v = STEP_DEFAULT_US;
        }
        cmd_goto(c->cur + v);
        break;
    }

    case '-': {
        mch_t *c = &s_ch[s_sel];

        v = STEP_DEFAULT_US;
        (void)parse_u32(line + 1, &v);
        if (v == 0U) {
            v = STEP_DEFAULT_US;
        }
        cmd_goto((c->cur > v) ? (c->cur - v) : 0U);
        break;
    }

    case 'c':
    case 'C':
        cmd_center(0);
        break;

    case 'b':
    case 'B':
        cmd_center(1);
        break;

    default:
        printf("[mot] unknown \"%s\" (h for help)\r\n", line);
        return;
    }

    print_one(s_sel);
}

/* ============================== 串口（空闲期）============================== */

static void poll_uart(void)
{
    char c;

    while (csi_uart_receive(&console_uart, &c, 1U, 0U) > 0) {
        if ((c == '\r') || (c == '\n')) {
            if (s_pline_len == 0U) {
                continue;
            }
            s_pline[s_pline_len] = '\0';
            s_pline_len = 0U;
            handle_line(s_pline);
            continue;
        }
        if ((c == 0x08) || (c == 0x7F)) {
            if (s_pline_len > 0U) {
                s_pline_len--;
            }
            continue;
        }
        if ((c < 32) || (c > 126)) {
            continue;
        }
        if (s_pline_len < (LINE_MAX - 1U)) {
            s_pline[s_pline_len++] = c;
        } else {
            s_pline_len = 0U;
            printf("[mot] line too long, dropped\r\n");
        }
    }
}

/* ============================== 入口 ============================== */

static void channels_init(void)
{
    mch_t *y = &s_ch[MCH_YAW];
    mch_t *p = &s_ch[MCH_PITCH];

    y->ch     = SERVO_YAW_CH;
    y->axis   = "YAW";
    y->pin    = SERVO_YAW_PIN_TXT;
    y->center = SERVO_YAW_CENTER;
    y->lim_lo = SERVO_YAW_MIN;
    y->lim_hi = SERVO_YAW_MAX;
    y->cur    = SERVO_YAW_CENTER;
    y->tgt    = SERVO_YAW_CENTER;
    y->moving = 0U;

    p->ch     = SERVO_PITCH_CH;
    p->axis   = "PITCH";
    p->pin    = SERVO_PITCH_PIN_TXT;
    p->center = SERVO_PITCH_CENTER;
    p->lim_lo = SERVO_PITCH_MIN;
    p->lim_hi = SERVO_PITCH_MAX;
    p->cur    = SERVO_PITCH_CENTER;
    p->tgt    = SERVO_PITCH_CENTER;
    p->moving = 0U;
}

void servo_motion_test(void)
{
    uint32_t last;
    uint32_t now;
    uint32_t dt;

    printf("\r\n[mot] BOOT: move-library test bench\r\n");

    /* 1) 动作库初始化（内部会调 servo_core_init：引脚 mux + PWM + compare 校准 + 回中位）*/
    if (moves_init() != 0) {
        printf("[mot] moves_init FAILED, check 5V supply / PA29 PA27 wiring\r\n");
        for (;;) {
            mdelay(1000);
        }
    }

    channels_init();
    last = csi_tick_get();

    print_help();
    print_status();
    print_actions();
    printf("[mot] ready. try `p0` (idle) or `d` (demo).\r\n");

    /* 2) 主循环：只有手动定位需要在这里推进；播放动作时是阻塞的，
     *    这段时间串口由 moves 的 tick 钩子（play_hook）负责。*/
    for (;;) {
        poll_uart();

        now = csi_tick_get();
        dt  = now - last;
        if (dt < TICK_MS) {
            mdelay(1);
            continue;
        }
        last = now;
        if (dt > DT_MAX_MS) {
            dt = DT_MAX_MS;
        }
        motion_step(dt);
    }
}
