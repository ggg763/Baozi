/******************************************************************************
 * @file     moves.c
 * @brief    动作库基础：轨迹插值 + 动作注册表 + 播放引擎 + 可打断
 *
 * 所有轨迹最终都通过 servo_set_pulse() 输出，因此自动享受 servo_core.h 里的
 * 实测限位保护（YAW 300~1700us、PITCH 800~1600us）——动作代码写超了也不会顶极限。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "board_config.h"
#include "drv/common.h"                 /* mdelay */
#include "drv/tick.h"                   /* csi_tick_get：真实毫秒（节拍补偿用）*/
#include "servo_test/servo_core.h"      /* servo_core_init / servo_set_pulse / 中位限位 */
#include "moves.h"

/* 插值步长 MOVE_TICK_MS 定义在 moves.h（emotion_fsm 的钩子节拍也用它）*/

/* 当前偏移（相对各自中位，us）*/
static int16_t s_yaw_off = 0;
static int16_t s_pitch_off = 0;

/* 中断标志：动作播放期间被要求停止时置 1 */
static volatile int s_intr = 0;

/* 动作调制增益（千分比；1000 = 原样不调制）—— 由上层按连续情绪状态设置 */
static uint16_t s_amp_gain = MOVE_GAIN_UNIT;
static uint16_t s_spd_gain = MOVE_GAIN_UNIT;

/* 播放期步进钩子（默认不挂；见 moves.h 说明）*/
static move_tick_hook_t s_hook = 0;

/* 幅度缩放：整体基准（MOVE_AMP_BASE）+ 上层调制（s_amp_gain），分两步算避免溢出
 *   r = v × MOVE_AMP_BASE/1000 × s_amp_gain/1000      （v ≤ ±600 → 中间量不会溢出）*/
static int16_t scale_amp(int32_t v)
{
    int32_t r;

    if ((s_amp_gain == MOVE_GAIN_UNIT) && (MOVE_AMP_BASE == MOVE_GAIN_UNIT)) {
        return (int16_t)v;
    }

    r = (v * (int32_t)MOVE_AMP_BASE) / (int32_t)MOVE_GAIN_UNIT;    /* 整体幅度基准 */
    r = (r * (int32_t)s_amp_gain) / (int32_t)MOVE_GAIN_UNIT;       /* 上层连续状态调制 */

    if (r > 32767) {
        r = 32767;
    } else if (r < -32768) {
        r = -32768;
    }
    return (int16_t)r;
}

/* 时间缩放：spd 越大耗时越短（动作更快）*/
static uint16_t scale_ms(uint32_t ms)
{
    uint32_t r;

    if (s_spd_gain == MOVE_GAIN_UNIT) {
        return (uint16_t)ms;
    }

    r = (ms * (uint32_t)MOVE_GAIN_UNIT) / (uint32_t)s_spd_gain;

    if (r < (uint32_t)MOVE_TICK_MS) {
        r = (uint32_t)MOVE_TICK_MS;
    } else if (r > 60000U) {
        r = 60000U;
    }
    return (uint16_t)r;
}

/* 走一个插值步 / 一个 hold 小块后调用一次钩子（钩子里通常轮询串口）*/
static void tick_hook(void)
{
    if (s_hook != 0) {
        s_hook();
    }
}

/* ==================== 节拍补偿（2026-11：让每步真的只有 20ms）====================
 *
 * ★ 为什么需要：插值步里要调 tick_hook()，而状态机的钩子会顺带跑一次
 *   app_core_step()（触摸 + 串口 + 分片刷屏 2 片，共 6~16ms）。于是"20ms 一步"
 *   实际变成 26~36ms，且长短随机 —— 观感就是【忽快忽慢、一顿一顿】。
 *   （慢动作更吃亏：整段比名义时长慢 30~80%，节奏完全不是写的那样。）
 *
 * 做法：记下本步开始时刻，钩子跑完后按 csi_tick_get() 的差值把【剩余时间】补上。
 * 钩子开销 ≤ 20ms 时步长被拉回 20ms；开销更大时无法缩短，只能如实变长。
 * 注意：这不会改变轨迹形状，只是让"每步 20ms"重新成立（PWM 周期也是 20ms）。 */
static void wait_rest(uint32_t step_start_ms)
{
    uint32_t el = (uint32_t)(csi_tick_get() - step_start_ms);

    if (el < (uint32_t)MOVE_TICK_MS) {
        mdelay((uint32_t)MOVE_TICK_MS - el);
    }
}

/* ==================== 输出限位（留余量 + 超限告警）====================
 *
 * ★ 2026-10-31：为什么不是「交给 servo_set_pulse 钳到极限就完事」
 *
 *   servo_core.h 的限位是「能到达且不堵转」的边界，但**不是「可以长期停在上面」
 *   的边界**。动作在强情绪下幅度会被放大：
 *       偏移 × MOVE_AMP_BASE(1.2) × amp_gain(700~1200，中性还 ×1.15 好奇系数)
 *   只要算出来的落点越过限位，剩下的几百 ms 里舵机就一直顶在墙上 ——
 *   嗡响、发热、动作到某处突然不动，也就是"堵转 + 卡顿"。
 *
 *   （这不是假设：当天把限位按"手动单点能到哪"收窄到 1440/1320，动作库立刻全顶上，
 *     最后只能把限位改回去。教训是收限位必须同时收动作幅度。）
 *
 *   所以这里主动留 SAFE_MARGIN_US 的余量：动作最多走到差 10us 的位置，
 *   舵机安静停住，只是幅度比设计值略小（≤1°），而不是顶着机械限位硬耗。
 *   超限时打一行告警（限流，每 ~1s 最多一条），把"哪个动作超了、超多少"
 *   直接暴露到串口上，避免靠猜 —— 以后再有人想收限位，先看这行告警。 */

#define SAFE_MARGIN_US      10      /* 距实测极限留的余量（≈1°）*/
#define CLIP_WARN_EVERY     50U     /* 每 50 次超限（≈1s）打一条，避免刷屏 */

static uint16_t s_clip_cnt = 0U;

/* 把偏移变成绝对脉宽下发。s_yaw_off / s_pitch_off 记的是【未钳制】的逻辑位置，
 * 这样轨迹规划不受影响；只有真正写给 PWM 的值才被收进安全区。 */
static void apply_off(int16_t yaw_off, int16_t pitch_off)
{
    int32_t y;
    int32_t p;

    s_yaw_off = yaw_off;
    s_pitch_off = pitch_off;

    y = (int32_t)SERVO_YAW_CENTER + (int32_t)yaw_off;
    p = (int32_t)SERVO_PITCH_CENTER + (int32_t)pitch_off;

    if ((y > (int32_t)(SERVO_YAW_MAX - SAFE_MARGIN_US)) ||
        (y < (int32_t)(SERVO_YAW_MIN + SAFE_MARGIN_US)) ||
        (p > (int32_t)(SERVO_PITCH_MAX - SAFE_MARGIN_US)) ||
        (p < (int32_t)(SERVO_PITCH_MIN + SAFE_MARGIN_US))) {
        if ((s_clip_cnt % CLIP_WARN_EVERY) == 0U) {
            printf("[mv] CLIP: yaw=%d (ok %u~%u)  pitch=%d (ok %u~%u)  off=(%d,%d)\r\n",
                   (int)y, SERVO_YAW_MIN + SAFE_MARGIN_US, SERVO_YAW_MAX - SAFE_MARGIN_US,
                   (int)p, SERVO_PITCH_MIN + SAFE_MARGIN_US, SERVO_PITCH_MAX - SAFE_MARGIN_US,
                   (int)yaw_off, (int)pitch_off);
        }
        s_clip_cnt++;
    }

    if (y > (int32_t)(SERVO_YAW_MAX - SAFE_MARGIN_US)) {
        y = (int32_t)(SERVO_YAW_MAX - SAFE_MARGIN_US);
    } else if (y < (int32_t)(SERVO_YAW_MIN + SAFE_MARGIN_US)) {
        y = (int32_t)(SERVO_YAW_MIN + SAFE_MARGIN_US);
    }

    if (p > (int32_t)(SERVO_PITCH_MAX - SAFE_MARGIN_US)) {
        p = (int32_t)(SERVO_PITCH_MAX - SAFE_MARGIN_US);
    } else if (p < (int32_t)(SERVO_PITCH_MIN + SAFE_MARGIN_US)) {
        p = (int32_t)(SERVO_PITCH_MIN + SAFE_MARGIN_US);
    }

    servo_set_pulse(SERVO_YAW_CH, (uint32_t)y);
    servo_set_pulse(SERVO_PITCH_CH, (uint32_t)p);
}

/* ==================== 轨迹原语 ==================== */

/* 【2026-10-31】梯形速度剖面：把"归一化时间 t(0~1000)"映射成"归一化位置 s(0~1000)"。
 *
 * 为什么要这一步：原来是线性插值 —— 每步位移恒定，等价于整段匀速。
 * 于是起点那一下速度从 0 阶跃到满速、终点又从满速阶跃回 0，
 * 舵机表现就是"起步一顿、到点一冲再回弹"。小幅度时看不出来，
 * 幅度一大（尤其配合 amp_gain 放大）就很明显。
 *
 * 梯形剖面：前 R 千分比的时长用来加速、中间匀速、最后 R 减速，两端速度为 0。
 * 位置 = 速度的积分，所以 s(t) 是分段二次，运算只有乘法。
 *
 *   v(t):  0 →(线性)→ 满速 →(保持)→ 满速 →(线性)→ 0
 *   s(t):  t²/(2R)  /  (R/2 + t-R)  /  1 - (1-t)²/(2R)     （均已归一化）
 *
 * 峰值速度 = 平均速度 / (1 - R/1000)，R=200 时是 1.25 倍 —— 限速那边要按这个折算，
 * 否则同样行程里中段会比原来快 25%，反而更容易跟不上。 */
/* 【2026-11】缓动剖面（起/停两端可分别开关）
 *
 *   in/out 都开 = 原来的梯形（起止速度为 0）—— 单段动作最正确的形状。
 *   关掉某一端 = 那一端"带着速度"接进/接出。用途：把【多段同向】的轨迹串成
 *   一条连续弧线（HAPPY 变体③ 的绕圈 8 段就是典型），段与段之间不再减速到 0，
 *   观感从"一段一段挪"变成"一口气走完"。
 *
 * 归一化（刻度 1000×1000）：
 *     面积 = 1000×1000 − (in ? RAMP×500 : 0) − (out ? RAMP×500 : 0)
 *     峰值速度 = 1000×1000 / 面积     （in=out=1、RAMP=200 → 1.25，与旧实现一致）
 *   两端都关时面积 = 1000×1000，即线性匀速，峰值 = 1。 */
static uint32_t ease_area_total(uint8_t ease_in, uint8_t ease_out)
{
    uint32_t total = 1000U * 1000U;

    if (ease_in != 0U) {
        total -= (uint32_t)MOVE_EASE_RAMP * 500U;
    }
    if (ease_out != 0U) {
        total -= (uint32_t)MOVE_EASE_RAMP * 500U;
    }
    return total;
}

static uint32_t ease_profile_ex(uint32_t t, uint8_t ease_in, uint8_t ease_out)
{
    uint32_t r = MOVE_EASE_RAMP;
    uint32_t total;
    uint32_t lin_start;     /* 匀速段起点 */
    uint32_t out_start;     /* 减速段起点 */
    uint32_t base;          /* 加速段面积 */
    uint32_t area;

    if (t >= 1000U) {
        return 1000U;       /* 末步必须精确落在终点 */
    }
    if (r == 0U) {
        return t;           /* 退化成线性 */
    }
    if (r >= 1000U) {
        r = 999U;
    }

    total     = ease_area_total(ease_in, ease_out);
    lin_start = (ease_in  != 0U) ? r : 0U;
    out_start = 1000U - ((ease_out != 0U) ? r : 0U);
    base      = (ease_in  != 0U) ? ((uint32_t)r * 500U) : 0U;

    if ((ease_in != 0U) && (t <= r)) {                  /* 加速段：t²/(2R) */
        area = (t * t * 1000U) / (2U * r);
    } else if (t <= out_start) {                        /* 匀速段 */
        area = base + (t - lin_start) * 1000U;
    } else {                                            /* 减速段：1-(1-t)²/(2R) */
        uint32_t d = 1000U - t;

        area = base + (out_start - lin_start) * 1000U
             + (((uint32_t)r * 500U) - (d * d * 1000U) / (2U * r));
    }

    return (area * 1000U) / total;
}

/* 轨迹单段插值。
 * ease_in / ease_out = 1 表示该端缓动（速度为 0）；0 表示"带速度接续"。
 * 只在需要把多段同向轨迹串成一条弧线时才传 0（见 moves.h 的 move_to_ex）。 */
void move_line_ex(int16_t yaw0, int16_t pitch0, int16_t yaw1, int16_t pitch1,
                  uint16_t ms, uint8_t ease_in, uint8_t ease_out)
{
    uint16_t steps = (uint16_t)(ms / MOVE_TICK_MS);
    uint16_t i;
    int32_t  dy;
    int32_t  dp;
    int32_t  dmax;
    uint32_t t0;

#if MOVE_EASE_MODE == 0
    ease_in  = 0U;          /* 剖面整个关掉：线性匀速（原来的行为）*/
    ease_out = 0U;
#endif

    if (steps == 0U) {
        steps = 1U;
    }

    /* ★ 2026-10-31 限速：单个 20ms 插值步不许超过 MOVE_MAX_STEP_US。
     *
     * 背景：动作时长会被 spd_gain(700~1400) 压缩，幅度会被 amp_gain(700~1200，
     * 中性再 ×1.15) 放大。两头一夹，angry 甩头最坏能到 ~9.5us/ms，而舵机空载
     * 也就 6.7us/ms、带载更低 —— 指令跑在舵机前面，舵机全程"追不上"，
     * 观感就是一顿一顿的卡顿（严重时一直全力堵着转，还以为是堵转）。
     *
     * 这里不改动作参数，只在"算出来的步长会超速"时把步数加够（时长同比延长）。
     * 好处：动作形状完全不变，只是最快的那几个动作变慢到舵机跟得上。
     * 嫌慢就调大 MOVE_MAX_STEP_US（每 20ms 允许走的 us 数）。 */
    dy   = (int32_t)yaw1 - (int32_t)yaw0;
    dp   = (int32_t)pitch1 - (int32_t)pitch0;
    dy   = (dy < 0) ? -dy : dy;
    dp   = (dp < 0) ? -dp : dp;
    dmax = (dy > dp) ? dy : dp;

    /* 剖面中段最快：峰值 = 1000×1000 / 面积。先按峰值折算行程，再和限速比，
     * 保证"最快的那一小段"也不超速（两端都缓动 + RAMP=200 时峰值 1.25 倍）。 */
    if (dmax > 0) {
        dmax = (int32_t)(((uint32_t)dmax * 1000000U) / ease_area_total(ease_in, ease_out));
    }

    if (dmax > (int32_t)steps * (int32_t)MOVE_MAX_STEP_US) {
        steps = (uint16_t)((dmax + (int32_t)MOVE_MAX_STEP_US - 1) / (int32_t)MOVE_MAX_STEP_US);
        if (steps == 0U) {
            steps = 1U;
        }
    }

    for (i = 1U; i <= steps; i++) {
        int32_t  y;
        int32_t  p;
        uint32_t s = ease_profile_ex((uint32_t)i * 1000U / (uint32_t)steps, ease_in, ease_out);

        y = (int32_t)yaw0 + ((int32_t)yaw1 - (int32_t)yaw0) * (int32_t)s / 1000;
        p = (int32_t)pitch0 + ((int32_t)pitch1 - (int32_t)pitch0) * (int32_t)s / 1000;

        t0 = csi_tick_get();
        apply_off((int16_t)y, (int16_t)p);

        tick_hook();
        if (s_intr != 0) {
            return;
        }
        wait_rest(t0);          /* ★ 把钩子耗掉的时间补回来：每步仍是 20ms */
    }
}

void move_line(int16_t yaw0, int16_t pitch0, int16_t yaw1, int16_t pitch1, uint16_t ms)
{
    move_line_ex(yaw0, pitch0, yaw1, pitch1, ms, 1U, 1U);
}

void move_to_ex(int16_t yaw_off, int16_t pitch_off, uint16_t ms,
                uint8_t ease_in, uint8_t ease_out)
{
    /* 目标偏移按幅度增益缩放、时间按速度增益缩放（增益 1000 时与原行为一致）*/
    move_line_ex(s_yaw_off, s_pitch_off,
                 scale_amp(yaw_off), scale_amp(pitch_off),
                 scale_ms(ms), ease_in, ease_out);
}

void move_to(int16_t yaw_off, int16_t pitch_off, uint16_t ms)
{
    /* 目标偏移按幅度增益缩放、时间按速度增益缩放（增益 1000 时与原行为一致）*/
    move_line(s_yaw_off, s_pitch_off,
              scale_amp(yaw_off), scale_amp(pitch_off), scale_ms(ms));
}

void move_by(int16_t d_yaw, int16_t d_pitch, uint16_t ms)
{
    move_line(s_yaw_off, s_pitch_off,
              scale_amp((int32_t)s_yaw_off + (int32_t)d_yaw),
              scale_amp((int32_t)s_pitch_off + (int32_t)d_pitch),
              scale_ms(ms));
}

void move_center(uint16_t ms)
{
    move_to(0, 0, ms);
}

void move_hold(uint16_t ms)
{
    uint16_t left = ms;

    while ((left > 0U) && (s_intr == 0)) {
        uint16_t d    = (left > MOVE_TICK_MS) ? MOVE_TICK_MS : left;
        uint32_t t0   = csi_tick_get();

        tick_hook();            /* hold 期间也要能响应串口/推进心情计时 */
        wait_rest(t0);          /* 钩子耗时补掉：停顿长度才是写的那个数 */
        left = (uint16_t)(left - d);
    }
}

int16_t move_yaw_off(void)
{
    return s_yaw_off;
}

int16_t move_pitch_off(void)
{
    return s_pitch_off;
}

/* ==================== 曲线轨迹（8 字环绕）==================== */

/* sin 查表：0~90° 等分 16 段，单位千分之一（1000 = 1.0）
 * 用查表而不用 sinf()：minilibc 的浮点不可靠，而且这里只要整数精度就够了。 */
static const int16_t s_sin1000[17] = {
      0,  /*   0.000° */
     98,  /*   5.625° */
    195,  /*  11.250° */
    290,  /*  16.875° */
    383,  /*  22.500° */
    471,  /*  28.125° */
    556,  /*  33.750° */
    634,  /*  39.375° */
    707,  /*  45.000° */
    773,  /*  50.625° */
    831,  /*  56.250° */
    882,  /*  61.875° */
    924,  /*  67.500° */
    957,  /*  73.125° */
    981,  /*  78.750° */
    995,  /*  84.375° */
   1000,  /*  90.000° */
};

/* 返回 sin(deg) 的千分值（-1000~1000），deg 单位是【度】 */
static int16_t sin1000(uint16_t deg)
{
    uint16_t idx;
    int16_t  v;
    int      neg = 0;

    deg = (uint16_t)(deg % 360U);
    if (deg >= 180U) {
        deg = (uint16_t)(deg - 180U);   /* sin(180°+x) = -sin(x) */
        neg = 1;
    }
    if (deg > 90U) {
        deg = (uint16_t)(180U - deg);   /* sin(180°-x) = sin(x) */
    }

    /* 90° 分成 16 段，四舍五入取最近表项 */
    idx = (uint16_t)(((uint32_t)deg * 16U + 45U) / 90U);
    v = s_sin1000[idx];

    return (neg != 0) ? (int16_t)(-v) : v;
}

void move_figure8(int16_t yaw_amp, int16_t pitch_amp, uint16_t ms_per_lap, uint8_t laps)
{
    uint16_t steps;
    uint16_t i;
    uint8_t  lap;

    /* 幅度与速度同样受调制增益影响（画 8 字也随情绪"变大变小、变快变慢"）*/
    ms_per_lap = scale_ms(ms_per_lap);
    yaw_amp    = scale_amp(yaw_amp);
    pitch_amp  = scale_amp(pitch_amp);

    steps = (uint16_t)(ms_per_lap / MOVE_TICK_MS);
    if (steps < 16U) {
        steps = 16U;        /* 点太少画不出 8 字 */
    }

    for (lap = 0U; lap < laps; lap++) {
        for (i = 0U; i < steps; i++) {
            /* θ 从 0 走到 360°：yaw 走一个周期、pitch 走两个周期 → 一笔 ∞ */
            uint16_t deg = (uint16_t)((uint32_t)i * 360U / (uint32_t)steps);
            int16_t  y = (int16_t)((int32_t)yaw_amp * (int32_t)sin1000(deg) / 1000);
            int16_t  p = (int16_t)((int32_t)pitch_amp *
                                   (int32_t)sin1000((uint16_t)((uint32_t)deg * 2U)) / 1000);
            uint32_t t0 = csi_tick_get();

            apply_off(y, p);

            tick_hook();
            if (s_intr != 0) {
                return;
            }
            wait_rest(t0);      /* 节拍补偿：曲线才不会被刷屏开销拉成忽快忽慢 */
        }
    }

    /* 画完停在中心，避免半空中收尾 */
    apply_off(0, 0);
}

/* ==================== 中断 ==================== */

void moves_interrupt(void)
{
    s_intr = 1;
}

int moves_interrupted(void)
{
    int v = s_intr;

    if (v != 0) {
        s_intr = 0;         /* 查询即清除，避免影响下一次播放 */
    }
    return v;
}

/* ==================== 动作调制（增益）+ 播放期钩子 ==================== */

void moves_set_gain(uint16_t amp_permille, uint16_t spd_permille)
{
    /* 下限 100（0.1 倍，避免除零/几乎不动），上限 4000（4 倍）*/
    if (amp_permille < 100U) {
        amp_permille = 100U;
    } else if (amp_permille > 4000U) {
        amp_permille = 4000U;
    }
    if (spd_permille < 100U) {
        spd_permille = 100U;
    } else if (spd_permille > 4000U) {
        spd_permille = 4000U;
    }

    s_amp_gain = amp_permille;
    s_spd_gain = spd_permille;
}

uint16_t moves_amp_gain(void)
{
    return s_amp_gain;
}

uint16_t moves_spd_gain(void)
{
    return s_spd_gain;
}

void moves_set_tick_hook(move_tick_hook_t fn)
{
    s_hook = fn;
}

/* ==================== 动作表 ==================== */

/* 各状态的动作实现（在 moves_<状态>.c 里定义）*/
extern void moves_idle_run(void);
extern void moves_happy_run(void);
extern void moves_eight_run(void);
extern void moves_sad_run(void);        /* 情绪：难过 */
extern void moves_angry_run(void);      /* 情绪：生气 */
extern void moves_surprised_run(void);  /* 情绪：惊讶 */
extern void moves_loving_run(void);     /* 情绪：爱意 */
extern void moves_bored_run(void);      /* 心情：无聊 */
extern void moves_sleep_run(void);      /* 心情：睡眠 */

typedef void (*move_fn_t)(void);

typedef struct {
    const char *name;
    move_fn_t   fn;
} move_entry_t;

static const move_entry_t s_moves[MOVE_MAX] = {
    { "idle",      moves_idle_run      },   /* 待机：V 字摇摆点头（neutral 的动作）*/
    { "happy",     moves_happy_run     },   /* 开心：快速摇头晃脑 */
    { "eight",     moves_eight_run     },   /* 八字环绕：脖子画横 8 字（∞）*/
    { "sad",       moves_sad_run       },   /* 难过：低头 + 缓慢左右摇 */
    { "angry",     moves_angry_run     },   /* 生气：昂头 + 快速摇头 */
    { "surprised", moves_surprised_run },   /* 惊讶：猛抬头 + 定格 */
    { "loving",    moves_loving_run    },   /* 爱意：微抬头 + 缓慢歪头摆动 */
    { "bored",     moves_bored_run     },   /* 无聊（心情）：歪头 + 慢摆 + 发呆 */
    { "sleep",     moves_sleep_run     },   /* 睡眠（心情）：低头 + 呼吸起伏 */
};

const char *moves_name(move_id_t id)
{
    if ((int)id >= (int)MOVE_MAX) {
        return "?";
    }
    return s_moves[id].name;
}

/* ==================== 变体池 ==================== */

/* 变体实现（都写在自己状态的 moves_<状态>.c 里）*/
extern void moves_idle_v2_run(void);        /* 待机②：远望与俯视 */
extern void moves_idle_v3_run(void);        /* 待机③：扁 8 字巡游 */
extern void moves_happy_v2_run(void);       /* 开心②：点头雀跃 */
extern void moves_happy_v3_run(void);       /* 开心③：绕圈庆祝 */
extern void moves_sad_v2_run(void);         /* 难过②：长叹 */
extern void moves_sad_v3_run(void);         /* 难过③：无力摇头 */
extern void moves_angry_v2_run(void);       /* 生气②：瞪着你 */
extern void moves_angry_v3_run(void);       /* 生气③：生闷气 */
extern void moves_surprised_v2_run(void);   /* 惊讶②：吓一跳 */
extern void moves_surprised_v3_run(void);   /* 惊讶③：疑惑定住 */
extern void moves_loving_v2_run(void);      /* 爱意②：轻轻蹭 */
extern void moves_loving_v3_run(void);      /* 爱意③：慢慢看你 */
extern void moves_bored_v2_run(void);       /* 无聊②：垂头发呆 */
extern void moves_bored_v3_run(void);       /* 无聊③：东倒西歪 */
extern void moves_sleep_v2_run(void);       /* 睡眠②：侧头睡 */
extern void moves_sleep_v3_run(void);       /* 睡眠③：浅眠 */

/* 各状态的变体清单（顺序与 move_id_t 一致；count = 0 表示没有变体，退回主实现）*/
static const move_fn_t s_idle_vars[MOVE_MAX_VARIANTS] = {
    moves_idle_run, moves_idle_v2_run, moves_idle_v3_run
};
static const move_fn_t s_happy_vars[MOVE_MAX_VARIANTS] = {
    moves_happy_run, moves_happy_v2_run, moves_happy_v3_run
};
static const move_fn_t s_sad_vars[MOVE_MAX_VARIANTS] = {
    moves_sad_run, moves_sad_v2_run, moves_sad_v3_run
};
static const move_fn_t s_angry_vars[MOVE_MAX_VARIANTS] = {
    moves_angry_run, moves_angry_v2_run, moves_angry_v3_run
};
static const move_fn_t s_surprised_vars[MOVE_MAX_VARIANTS] = {
    moves_surprised_run, moves_surprised_v2_run, moves_surprised_v3_run
};
static const move_fn_t s_loving_vars[MOVE_MAX_VARIANTS] = {
    moves_loving_run, moves_loving_v2_run, moves_loving_v3_run
};
static const move_fn_t s_bored_vars[MOVE_MAX_VARIANTS] = {
    moves_bored_run, moves_bored_v2_run, moves_bored_v3_run
};
static const move_fn_t s_sleep_vars[MOVE_MAX_VARIANTS] = {
    moves_sleep_run, moves_sleep_v2_run, moves_sleep_v3_run
};

typedef struct {
    const move_fn_t *fn;
    uint8_t          count;
} move_variant_t;

static const move_variant_t s_variants[MOVE_MAX] = {
    { s_idle_vars,      3U },   /* MOVE_IDLE      ：缓慢环视 / 远望俯视 / 扁 8 字巡游 */
    { s_happy_vars,     3U },   /* MOVE_HAPPY     ：欢快摇晃 / 点头雀跃 / 绕圈庆祝 */
    { 0,                0U },   /* MOVE_EIGHT     ：暂不做变体（8 字有堵转待定位，先不叠负载）*/
    { s_sad_vars,       3U },   /* MOVE_SAD       ：垂头丧气 / 长叹 / 无力摇头 */
    { s_angry_vars,     3U },   /* MOVE_ANGRY     ：甩头 / 瞪着你 / 生闷气 */
    { s_surprised_vars, 3U },   /* MOVE_SURPRISED ：猛抬头 / 吓一跳 / 疑惑定住 */
    { s_loving_vars,    3U },   /* MOVE_LOVING    ：温柔歪头 / 轻轻蹭 / 慢慢看你 */
    { s_bored_vars,     3U },   /* MOVE_BORED     ：歪头发呆 / 垂头发呆 / 东倒西歪 */
    { s_sleep_vars,     3U },   /* MOVE_SLEEP     ：垂头呼吸 / 侧头睡 / 浅眠 */
};

/* 上次随机到的变体下标（0xFF = 还没播过），用于"不与上次相同"*/
static uint8_t s_last_var[MOVE_MAX];

/* 小伪随机：32 位 LCG。种子固定，但每播一次动作都会推进若干步、再加上
 * "不与上次相同"的规则，实际观感足够不机械。
 * （裸机没有 RTC/ADC 随机源；将来若要更随机，可把种子混入自由运行计数器的低位。）*/
static uint32_t s_rand = 0x5A5A1234U;

uint32_t moves_rand(void)
{
    s_rand = s_rand * 1103515245U + 12345U;
    return (s_rand >> 16) & 0x7FFFU;
}

uint8_t moves_variant_count(move_id_t id)
{
    if ((int)id >= (int)MOVE_MAX) {
        return 1U;
    }
    return (s_variants[(int)id].count != 0U) ? s_variants[(int)id].count : 1U;
}

uint8_t moves_last_variant(move_id_t id)
{
    if ((int)id >= (int)MOVE_MAX) {
        return 0U;
    }
    return (s_last_var[(int)id] == 0xFFU) ? 0U : s_last_var[(int)id];
}

void moves_play_variant(move_id_t id)
{
    const move_variant_t *v;
    uint8_t n;
    uint8_t pick;

    if ((int)id >= (int)MOVE_MAX) {
        return;
    }

    v = &s_variants[(int)id];
    if (v->count == 0U) {
        moves_play_once(id);        /* 这个状态没有变体：退回主实现 */
        return;
    }

    n = v->count;
    pick = (n > 1U) ? (uint8_t)(moves_rand() % (uint32_t)n) : 0U;

    /* 不与上次相同；变体多于 2 个时再随机跳一下，避免 "1→2→1→2" 的明显交替 */
    if ((n > 1U) && (pick == s_last_var[(int)id])) {
        if (n > 2U) {
            pick = (uint8_t)((pick + 1U + (uint8_t)(moves_rand() % (uint32_t)(n - 1U))) % n);
        } else {
            pick = (uint8_t)((pick + 1U) % n);
        }
    }

    s_last_var[(int)id] = pick;
    s_intr = 0;
    v->fn[pick]();
}

/* 固定播该状态的第 idx 个变体（调试用：想反复看某一个变体时用）*/
void moves_play_variant_at(move_id_t id, uint8_t idx)
{
    const move_variant_t *v;

    if ((int)id >= (int)MOVE_MAX) {
        return;
    }

    v = &s_variants[(int)id];
    if ((v->count == 0U) || (idx >= v->count)) {
        moves_play_once(id);        /* 没有这个变体：退回主实现 */
        return;
    }

    s_last_var[(int)id] = idx;
    s_intr = 0;
    v->fn[idx]();
}

void moves_play_once(move_id_t id)
{
    if ((int)id >= (int)MOVE_MAX) {
        return;
    }
    s_intr = 0;
    s_moves[id].fn();
}

int moves_init(void)
{
    int i;

    if (servo_core_init() != 0) {
        printf("[moves] servo_core_init fail\r\n");
        return -1;
    }
    s_yaw_off = 0;
    s_pitch_off = 0;
    s_intr = 0;
    s_amp_gain = MOVE_GAIN_UNIT;    /* 调制增益复位为"原样"（钩子保持上层设置）*/
    s_spd_gain = MOVE_GAIN_UNIT;

    for (i = 0; i < (int)MOVE_MAX; i++) {
        s_last_var[i] = 0xFFU;      /* 变体历史清空：第一次随机不受"上次"约束 */
    }
    printf("[moves] init ok (center: YAW %uus / PITCH %uus)\r\n",
           SERVO_YAW_CENTER, SERVO_PITCH_CENTER);
    return 0;
}
