/******************************************************************************
 * @file     track_dual.c
 * @brief    红外双轴跟随（YAW + PITCH）—— 按 UserFolder/doc/ir_tracking_plan.md §1.3 的二维表实现
 *
 * 【两种用法】
 *   ① 自测（当前 main.c 用的）：track_dual_test() —— init + 死循环 step，不返回；
 *   ② 被情感状态机驱动：track_dual_init() → 循环调用 track_dual_step() → track_dual_exit()。
 *      非阻塞，状态机可以在两次 step 之间处理别的事（触摸/命令/表情等）。
 *      骨架见 track_dual.h 顶部注释；模式互斥的设计见 UserFolder/doc/README.md §5.3。
 *
 * 与单轴版（track_yaw.c）的区别：
 *   - 判定改成二维：每个状态**同时**给出两轴的方向（不再"一次只动一个轴"）
 *   - 竖直方向靠叠加态编码：`L+R`（下面两路都看到、上方 C 没看到）= 目标偏低 -> 低头
 *   - 两轴各自限位、各自记住最后方向
 *
 * 控制方式（IR 只有二值，做不了 PID）：5ms 采样 + 60ms 去抖 -> 每 25ms 走一步 ->
 * 软限位；丢失分档：200ms 保持 -> 1s 慢搜 -> 缓慢回中位。
 *
 * ★ 丝滑化（2026-10）：在"走一步"里加了两件事，解决"到位置一顿、走起来一格一格"：
 *   ① 加速度斜坡：每拍速度增量 TD_ACCEL_US，0→满速只要两拍（起步/反向不"弹射"）
 *   ② 死区滑行尾巴：目标居中（只见 C）不再原地急停，而是用 TD_GLIDE_US
 *      （默认 YAW 28us ≈ 2.5°）分 TD_GLIDE_TICKS 拍逐拍递减走完 —— 减速收尾
 *   手感可串口实时调（TRK:SPD / TRK:ACC / TRK:GLIDE / TRK:TICK / TRK:SHOW / TRK:DEF）
 *
 * 上电表现（正常的话）：
 *   手放左侧 -> 左转（yaw 减小）；右侧 -> 右转（yaw 增大）
 *   手抬高   -> 抬头（pitch 增大）；压低 -> 低头（pitch 减小）
 *   手在正前 -> 缓缓滑行一小段后停稳（不再急停）
 *   移开手   -> 200ms 保持 -> 1s 慢搜 -> 缓慢回中位（yaw 1000 / pitch 1100）
 *
 * 方向不对时怎么改：见文件末尾的"方向整定"注释。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <soc.h>
#include <drv/gpio.h>
#include <drv/pin.h>
#include <drv/common.h>             /* mdelay */
#include "board_config.h"
#include "servo_test/servo_core.h"  /* servo_core_init / servo_set_pulse / 中位限位 */
#include "track_dual.h"

/* 两轴方向：-1 / 0 / +1 */
typedef struct {
    int8_t yaw;     /* -1 左转（脉宽减小）, 0 停, +1 右转（脉宽增大） */
    int8_t pitch;   /* -1 低头（脉宽减小）, 0 停, +1 抬头（脉宽增大） */
} td_move_t;

/* ---- 内部状态（原来是 track_dual_test 的局部变量；抽出来供 step 使用）---- */
static csi_gpio_t s_ir_gpio;
static uint8_t  s_inited;                       /* init 成功后才允许 step/exit 操作硬件 */
static uint8_t  s_stable;                       /* 去抖后的 seen */
static uint8_t  s_pend[3];                      /* 待确认的 seen（[0]=L [1]=C [2]=R） */
static uint8_t  s_cnt[3];
static uint8_t  s_last_seen;
static uint32_t s_lost_ms;
static uint32_t s_ctrl_acc;
static uint32_t s_print_acc;
static int32_t  s_yaw;
static int32_t  s_pitch;
static int32_t  s_prev_yaw;
static int32_t  s_prev_pitch;
static int8_t   s_last_yaw_dir;
static int8_t   s_last_pitch_dir;
static td_move_t s_mv_now;

/* ---- ★ 丝滑化：运行时参数（默认取 track_dual.h 的宏；可串口实时改）---- */
static uint32_t s_p_step[2]  = { TD_YAW_STEP_US, TD_PITCH_STEP_US };   /* [0]=YAW [1]=PITCH 每拍位移 */
static uint32_t s_p_glide[2] = { TD_GLIDE_US,    TD_GLIDE_PITCH_US };  /* 死区滑行总量 */
static uint32_t s_p_accel       = TD_ACCEL_US;                         /* 每拍速度增量 */
static uint16_t s_p_glide_ticks = (uint16_t)TD_GLIDE_TICKS;            /* 滑行分几拍走完 */
static uint32_t s_p_ctrl_ms     = TD_CTRL_MS;                          /* 控制周期 */
static uint32_t s_p_sweep_ms    = TD_LOST_SWEEP_MS;                    /* 丢失多久开始扫一圈 */
static uint32_t s_p_sweep_step  = TD_SWEEP_STEP_US;                    /* 扫视步长（us/拍）*/

/* ---- ★ 扫一圈的状态（丢失后主动找目标）---- */
#define TD_SWEEP_IDLE   0U      /* 没在扫（有目标，或还没到触发时间）*/
#define TD_SWEEP_TO_A   1U      /* 正朝"第一端"扫 */
#define TD_SWEEP_TO_B   2U      /* 已到第一端，反向扫向另一端 */
#define TD_SWEEP_DONE   3U      /* 一个来回扫完、仍无目标 -> 可以退出了 */

static uint8_t s_sweep = TD_SWEEP_IDLE;
static int8_t  s_sweep_first_dir = (int8_t)1;   /* 第一端方向（= 丢失前的最后运动方向）*/

/* ---- ★ 丝滑化：每轴的速度与滑行状态 ----
 * v          : 当前速度（us/拍，带符号）—— 用加速度斜坡逼近 ±s_p_step[idx]，不再瞬间满速
 * glide_left : 进死区后还剩多少 us 的"滑行尾巴"要慢慢走完（0 = 已停稳）
 * glide_dir  : 滑行方向（= 进死区前的运动方向）
 * was_dead   : 上一拍是否在死区（用来识别"刚进死区"那一拍，只在那一拍安排滑行）*/
typedef struct {
    int32_t  v;
    int32_t  glide_left;
    int8_t   glide_dir;
    uint16_t glide_ticks;
    uint8_t  was_dead;
} td_axis_rt_t;

static td_axis_rt_t s_ax[2];        /* [0]=YAW [1]=PITCH */

/*****************************************************************************
 * 读三路原始电平 -> 位掩码（1 = 引脚为高 = 未遮挡）
 *****************************************************************************/
static uint8_t td_read_raw(void)
{
    uint8_t raw = 0U;

    if (csi_gpio_read(&s_ir_gpio, TD_IR_LEFT_PIN)   != 0U) { raw |= 0x04U; }
    if (csi_gpio_read(&s_ir_gpio, TD_IR_CENTER_PIN) != 0U) { raw |= 0x02U; }
    if (csi_gpio_read(&s_ir_gpio, TD_IR_RIGHT_PIN)  != 0U) { raw |= 0x01U; }

    return raw;
}

/*****************************************************************************
 * 二维方位判定（严格对应 UserFolder/doc/ir_tracking_plan.md §1.3 的表）
 *****************************************************************************/
static td_move_t td_decide(uint8_t seen)
{
    td_move_t m;
    uint8_t l = (uint8_t)TD_SEEN_L(seen);
    uint8_t c = (uint8_t)TD_SEEN_C(seen);
    uint8_t r = (uint8_t)TD_SEEN_R(seen);

    m.yaw   = 0;
    m.pitch = 0;

    if (seen == 0U) {
        return m;                       /* 无目标：由丢失分档处理 */
    }

    /* YAW：只看到单侧才转；两侧都看到（跨中/很近）或只看到 C（居中）-> 不转 */
    if ((l != 0U) && (r == 0U)) {
        m.yaw = -1;                     /* 1x0 -> 左 */
    } else if ((r != 0U) && (l == 0U)) {
        m.yaw = +1;                     /* 0x1 -> 右 */
    }

    /* PITCH：
     *   C 看到且未"全覆盖" -> 目标偏高 -> 抬头     （010 / 110 / 011）
     *   L+R 都看到但 C 没看到 -> 目标偏低 -> 低头   （101）
     *   全覆盖 111 / 单侧 100、001 -> 不动 */
    if ((c != 0U) && !((l != 0U) && (r != 0U))) {
        m.pitch = +1;
    } else if ((l != 0U) && (r != 0U) && (c == 0U)) {
        m.pitch = -1;
    }

    return m;
}

/*****************************************************************************
 * 单轴走一步（dir > 0 增 / dir < 0 减 / dir == 0 不动），带软限位
 *****************************************************************************/
static int32_t td_axis_step(int32_t val, int8_t dir, uint32_t step, uint32_t mn, uint32_t mx)
{
    int32_t v = val;

    if (dir > 0) {
        v = val + (int32_t)step;
    } else if (dir < 0) {
        v = val - (int32_t)step;
    }

    if (v < (int32_t)mn) { v = (int32_t)mn; }
    if (v > (int32_t)mx) { v = (int32_t)mx; }
    return v;
}

/*****************************************************************************
 * 单轴缓慢回中位（必须"吸附"，否则会在中位两侧来回跳 —— 单轴版踩过的坑）
 *****************************************************************************/
static int32_t td_axis_recenter(int32_t val, uint32_t center, uint32_t step)
{
    int32_t c = (int32_t)center;
    int32_t v = val;

    if (val > c) {
        v = val - (int32_t)step;
        if (v < c) { v = c; }
    } else if (val < c) {
        v = val + (int32_t)step;
        if (v > c) { v = c; }
    }
    return v;
}

static int32_t td_clamp(int32_t v, uint32_t mn, uint32_t mx)
{
    if (v < (int32_t)mn) { v = (int32_t)mn; }
    if (v > (int32_t)mx) { v = (int32_t)mx; }
    return v;
}

/*****************************************************************************
 * 复位某轴的速度/滑行状态（丢失分档、退出时用；避免残留惯性）
 *****************************************************************************/
static void td_axis_rt_reset(int idx)
{
    s_ax[idx].v           = 0;
    s_ax[idx].glide_left  = 0;
    s_ax[idx].glide_dir   = 0;
    s_ax[idx].glide_ticks = 0U;
    s_ax[idx].was_dead    = 0U;
}

/*****************************************************************************
 * ★ 丝滑化的核心：单轴平滑步进
 *
 *   dir != 0（目标在那一侧）
 *      速度按 accel 斜坡逼近 ±vmax，位移 = 当前速度
 *      —— 起步不"弹射"、反向不猛拽（原来是一拍直接满步长）
 *
 *   dir == 0（死区：目标已在中央）
 *      不再原地急停！进死区那一拍安排一段"滑行尾巴"：
 *        滑行总量 = s_p_glide[idx]（默认 YAW 28us ≈ 2.5°、PITCH 22us）
 *        分 s_p_glide_ticks 拍走完（默认 14 拍 = 350ms）
 *        每拍位移 = 剩余量 / 剩余拍数（逐拍递减 → 自然的减速收尾）
 *      滑行期间目标若重新出现 -> 立刻取消滑行，改回追踪
 *
 * 返回：新的脉宽值（已过软限位）
 *****************************************************************************/
static int32_t td_axis_smooth(int32_t val, int8_t dir, int idx, uint32_t mn, uint32_t mx)
{
    td_axis_rt_t *a = &s_ax[idx];
    uint32_t vmax = s_p_step[idx];
    int32_t  step = 0;

    if (vmax == 0U) { vmax = 1U; }

    if (dir != 0) {
        /* ---- 追踪：加速度斜坡 → 满速 ---- */
        a->was_dead    = 0U;
        a->glide_left  = 0;         /* 目标又动了：滑行作废 */
        a->glide_ticks = 0U;

        if (((a->v > 0) && (dir < 0)) || ((a->v < 0) && (dir > 0))) {
            /* 反向：先按一个 accel 量反向起步（相当于先减速、再过零）*/
            a->v = (int32_t)dir * (int32_t)s_p_accel;
        } else {
            a->v += (int32_t)dir * (int32_t)s_p_accel;
        }

        if (a->v >  (int32_t)vmax) { a->v =  (int32_t)vmax; }
        if (a->v < -(int32_t)vmax) { a->v = -(int32_t)vmax; }

        step = a->v;
    } else {
        /* ---- 死区：滑行尾巴（用减速段收尾，代替急停）---- */
        if (a->was_dead == 0U) {
            /* 刚进死区这一拍：安排滑行（方向 = 刚走过的方向）*/
            if (a->v > 0)      { a->glide_dir = (int8_t)1;  }
            else if (a->v < 0) { a->glide_dir = (int8_t)-1; }
            else               { a->glide_dir = (int8_t)0;  }

            a->v           = 0;
            a->glide_left  = (a->glide_dir != 0) ? (int32_t)s_p_glide[idx] : 0;
            a->glide_ticks = (a->glide_dir != 0) ? s_p_glide_ticks : 0U;
        }
        a->was_dead = 1U;

        if ((a->glide_left > 0) && (a->glide_ticks > 0U)) {
            step = a->glide_left / (int32_t)a->glide_ticks;     /* 逐拍递减 */
            if (step < 1)            { step = 1; }
            if (step > a->glide_left) { step = a->glide_left; }

            a->glide_left -= step;
            a->glide_ticks--;

            step *= (int32_t)a->glide_dir;
        } else {
            a->glide_left  = 0;
            a->glide_ticks = 0U;
        }
    }

    return td_clamp(val + step, mn, mx);
}

/*****************************************************************************
 * 打印一行状态
 *****************************************************************************/
static void td_print_line(uint8_t seen, uint8_t changed)
{
    /* "<glide" = 正在走减速尾巴（丝滑化）；调参时看这一列确认收尾是否发生 */
    printf("[TD] seen=%u%u%u dir=Y:%d P:%d yaw=%d pitch=%d lost=%ums%s%s\r\n",
           (unsigned)TD_SEEN_L(seen),
           (unsigned)TD_SEEN_C(seen),
           (unsigned)TD_SEEN_R(seen),
           (int)s_mv_now.yaw, (int)s_mv_now.pitch,
           (int)s_yaw, (int)s_pitch,
           (unsigned)s_lost_ms,
           (changed != 0U) ? "  <-changed" : "",
           ((s_ax[0].glide_left > 0) || (s_ax[1].glide_left > 0)) ? "  <glide" : "");
}

/*****************************************************************************
 * 进入跟随模式：引脚 + 舵机初始化 + 复位内部状态
 * 调用方：状态机在"切到跟随模式"时调一次；自测入口 track_dual_test() 也会调。
 *****************************************************************************/
void track_dual_init(void)
{
    uint32_t i;

    printf("\r\n[TD] dual-axis tracking  L=PB22 C=PB23 R=PB29\r\n");
    printf("[TD] yaw_step=%uus pitch_step=%uus ctrl=%ums debounce=%u x %ums\r\n",
           (unsigned)TD_YAW_STEP_US, (unsigned)TD_PITCH_STEP_US,
           (unsigned)TD_CTRL_MS, (unsigned)TD_DEBOUNCE_N, (unsigned)TD_SAMPLE_MS);

    s_inited = 0U;

    /* ---- 三路 IR ---- */
    csi_pin_set_mux(PORTB, TD_IR_LEFT_PIN,   PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, TD_IR_CENTER_PIN, PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, TD_IR_RIGHT_PIN,  PIN_FUNC_GPIO);
    csi_pin_mode(PORTB, TD_IR_LEFT_PIN,   GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, TD_IR_CENTER_PIN, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, TD_IR_RIGHT_PIN,  GPIO_MODE_PULLUP);

    if (csi_gpio_init(&s_ir_gpio, (uint32_t)TD_IR_GPIO_IDX) != CSI_OK) {
        printf("[TD] gpio init fail\r\n");
        return;
    }
    csi_gpio_dir(&s_ir_gpio, TD_IR_ALL_PIN, GPIO_DIRECTION_INPUT);

    /* ---- 舵机（两路引脚 + PWM + 回中位） ---- */
    if (servo_core_init() != 0) {
        printf("[TD] servo init fail\r\n");
        return;
    }

    /* ---- 复位内部状态 ---- */
    s_yaw   = (int32_t)SERVO_YAW_CENTER;
    s_pitch = (int32_t)SERVO_PITCH_CENTER;
    s_prev_yaw   = s_yaw;
    s_prev_pitch = s_pitch;
    s_last_yaw_dir   = 0;
    s_last_pitch_dir = 0;
    s_mv_now.yaw     = 0;
    s_mv_now.pitch   = 0;
    s_lost_ms   = 0U;
    s_ctrl_acc  = 0U;
    s_print_acc = 0U;
    s_last_seen = 0xFFU;
    td_axis_rt_reset(0);        /* ★ 丝滑化：清掉速度/滑行状态 */
    td_axis_rt_reset(1);
    s_sweep = TD_SWEEP_IDLE;    /* ★ 扫一圈：状态复位 */

    s_stable = (uint8_t)((~td_read_raw()) & 0x07U);
    s_pend[0] = (uint8_t)TD_SEEN_L(s_stable);
    s_pend[1] = (uint8_t)TD_SEEN_C(s_stable);
    s_pend[2] = (uint8_t)TD_SEEN_R(s_stable);
    for (i = 0U; i < 3U; i++) {
        s_cnt[i] = 0U;
    }

    printf("[TD] stat: yaw=%dus[%u,%u] pitch=%dus[%u,%u] seen=%u%u%u\r\n",
           (int)s_yaw, (unsigned)SERVO_YAW_MIN, (unsigned)SERVO_YAW_MAX,
           (int)s_pitch, (unsigned)SERVO_PITCH_MIN, (unsigned)SERVO_PITCH_MAX,
           (unsigned)TD_SEEN_L(s_stable), (unsigned)TD_SEEN_C(s_stable), (unsigned)TD_SEEN_R(s_stable));

    s_inited = 1U;
}

/*****************************************************************************
 * 推进一次跟踪：采样 -> 去抖 -> 按节拍步进 -> 按需打印
 * 调用方每 TD_SAMPLE_MS(5ms) 调一次（节拍由调用方 mdelay 控制）。
 *****************************************************************************/
void track_dual_step(void)
{
    uint8_t raw, seen;
    uint32_t i;

    if (s_inited == 0U) {
        return;
    }

    raw  = td_read_raw();
    seen = (uint8_t)((~raw) & 0x07U);

    /* ---- 三路各自阈值去抖 ---- */
    for (i = 0U; i < 3U; i++) {
        uint8_t shift = (uint8_t)(2U - i);      /* 0->L(bit2) 1->C(bit1) 2->R(bit0) */
        uint8_t v     = (uint8_t)((seen >> shift) & 1U);

        if (v == s_pend[i]) {
            if (s_cnt[i] < TD_DEBOUNCE_N) { s_cnt[i]++; }
        } else {
            s_pend[i] = v;
            s_cnt[i]  = 0U;
        }

        if (s_cnt[i] >= TD_DEBOUNCE_N) {
            if (v != 0U) { s_stable |=  (uint8_t)(1U << shift); }
            else         { s_stable &= (uint8_t)~(1U << shift); }
        }
    }
    s_stable &= 0x07U;

    /* ---- 每 s_p_ctrl_ms(默认 25ms) 走一步（★ 细分 4 倍，台阶更小）---- */
    s_ctrl_acc += TD_SAMPLE_MS;
    if (s_ctrl_acc >= s_p_ctrl_ms) {
        s_ctrl_acc = 0U;
        s_mv_now   = td_decide(s_stable);

        if (s_stable != 0U) {
            /* 有目标：两轴同时按判定结果**平滑**步进（速度斜坡 + 死区滑行）*/
            s_lost_ms = 0U;
            s_sweep   = TD_SWEEP_IDLE;      /* 找到了：取消"扫一圈"（下次丢失重新计时）*/

            s_yaw   = td_axis_smooth(s_yaw,   s_mv_now.yaw,   0, SERVO_YAW_MIN,   SERVO_YAW_MAX);
            s_pitch = td_axis_smooth(s_pitch, s_mv_now.pitch, 1, SERVO_PITCH_MIN, SERVO_PITCH_MAX);

            if (s_mv_now.yaw   != 0) { s_last_yaw_dir   = s_mv_now.yaw;   }
            if (s_mv_now.pitch != 0) { s_last_pitch_dir = s_mv_now.pitch; }
        } else {
            /* 无目标：分档处理（先把速度/滑行状态清干净，别带残留惯性）*/
            td_axis_rt_reset(0);
            td_axis_rt_reset(1);
            s_lost_ms += s_p_ctrl_ms;

            if (s_lost_ms < TD_LOST_HOLD_MS) {
                /* ① < 200ms 区域过渡中：保持不动 */
            } else if (s_lost_ms < TD_LOST_SEARCH_MS) {
                /* ② 200ms~1s：按最后方向慢速搜索（步长减半） */
                s_yaw   = td_axis_step(s_yaw,   s_last_yaw_dir,   (uint32_t)(s_p_step[0] / 2U),
                                       SERVO_YAW_MIN,   SERVO_YAW_MAX);
                s_pitch = td_axis_step(s_pitch, s_last_pitch_dir, (uint32_t)(s_p_step[1] / 2U),
                                       SERVO_PITCH_MIN, SERVO_PITCH_MAX);
            } else if (s_lost_ms < s_p_sweep_ms) {
                /* ③ 1s ~ 5s：缓慢回中位，静静等目标回来（原"丢 3s 就退"太急）*/
                s_yaw   = td_axis_recenter(s_yaw,   SERVO_YAW_CENTER,   TD_CENTER_STEP_US);
                s_pitch = td_axis_recenter(s_pitch, SERVO_PITCH_CENTER, TD_CENTER_STEP_US);
                /* 再兜一次限位（回中路径本身不会越限，这里是防御性写法） */
                s_yaw   = td_clamp(s_yaw,   SERVO_YAW_MIN,   SERVO_YAW_MAX);
                s_pitch = td_clamp(s_pitch, SERVO_PITCH_MIN, SERVO_PITCH_MAX);
            } else {
                /* ④ ★ 5s 仍无信号 -> 主动"扫一圈"：先朝一端扫到限位，再反向扫到另一端
                 *    （一个来回 = 覆盖整个视野）；扫完还没有目标才置 SWEEP_DONE。
                 *    中途一旦看到目标，上面"有目标"分支会把 s_sweep 清回 IDLE。*/
                if (s_sweep == TD_SWEEP_IDLE) {
                    s_sweep = TD_SWEEP_TO_A;
                    s_sweep_first_dir = (s_last_yaw_dir != 0) ? s_last_yaw_dir : (int8_t)1;
                    printf("[TD] lost %ums -> sweep to find target\r\n", (unsigned)s_lost_ms);
                }

                if (s_sweep == TD_SWEEP_TO_A) {
                    /* 朝第一端扫 */
                    s_yaw = td_clamp(s_yaw + ((int32_t)s_sweep_first_dir * (int32_t)s_p_sweep_step),
                                     SERVO_YAW_MIN, SERVO_YAW_MAX);
                    if ((s_yaw <= (int32_t)SERVO_YAW_MIN) || (s_yaw >= (int32_t)SERVO_YAW_MAX)) {
                        s_sweep = TD_SWEEP_TO_B;    /* 到极限了，掉头扫另一边 */
                    }
                } else if (s_sweep == TD_SWEEP_TO_B) {
                    /* 反向扫到另一端 -> 这就算"扫了一圈" */
                    s_yaw = td_clamp(s_yaw - ((int32_t)s_sweep_first_dir * (int32_t)s_p_sweep_step),
                                     SERVO_YAW_MIN, SERVO_YAW_MAX);
                    if (((s_sweep_first_dir > 0) && (s_yaw <= (int32_t)SERVO_YAW_MIN)) ||
                        ((s_sweep_first_dir < 0) && (s_yaw >= (int32_t)SERVO_YAW_MAX))) {
                        s_sweep = TD_SWEEP_DONE;
                        printf("[TD] sweep done, no target -> exit signal\r\n");
                    }
                }

                /* 扫视期间顺带把 PITCH 收回中位（别一直仰着/低着找）*/
                s_pitch = td_axis_recenter(s_pitch, SERVO_PITCH_CENTER, TD_CENTER_STEP_US);
                s_pitch = td_clamp(s_pitch, SERVO_PITCH_MIN, SERVO_PITCH_MAX);
            }
        }

        /* 只有真正变化才写舵机：少一次 stop/start，也避免死区时无谓重写 */
        if (s_yaw != s_prev_yaw) {
            (void)servo_set_pulse(SERVO_YAW_CH, (uint32_t)s_yaw);
            s_prev_yaw = s_yaw;
        }
        if (s_pitch != s_prev_pitch) {
            (void)servo_set_pulse(SERVO_PITCH_CH, (uint32_t)s_pitch);
            s_prev_pitch = s_pitch;
        }
    }

    /* ---- 打印 ---- */
    s_print_acc += TD_SAMPLE_MS;
    if ((s_stable != s_last_seen) || (s_print_acc >= TD_PRINT_MS)) {
        td_print_line(s_stable, (uint8_t)(s_stable != s_last_seen));
        s_last_seen = s_stable;
        s_print_acc = 0U;
    }
}

/*****************************************************************************
 * 退出跟随模式：两轴回中位 + 复位内部状态，把舵机控制权交还动作库
 * 调用方：状态机在"离开跟随模式"时调一次（之后由 movements/ 接管）。
 *****************************************************************************/
void track_dual_exit(void)
{
    if (s_inited != 0U) {
        (void)servo_set_pulse(SERVO_YAW_CH,   (uint32_t)SERVO_YAW_CENTER);
        (void)servo_set_pulse(SERVO_PITCH_CH, (uint32_t)SERVO_PITCH_CENTER);
        printf("[TD] tracking exit -> center (yaw=%u pitch=%u)\r\n",
               (unsigned)SERVO_YAW_CENTER, (unsigned)SERVO_PITCH_CENTER);
    }

    s_yaw   = (int32_t)SERVO_YAW_CENTER;
    s_pitch = (int32_t)SERVO_PITCH_CENTER;
    s_prev_yaw   = s_yaw;
    s_prev_pitch = s_pitch;
    s_last_yaw_dir   = 0;
    s_last_pitch_dir = 0;
    s_mv_now.yaw     = 0;
    s_mv_now.pitch   = 0;
    s_lost_ms   = 0U;
    s_ctrl_acc  = 0U;
    s_print_acc = 0U;
    s_last_seen = 0xFFU;
    td_axis_rt_reset(0);        /* ★ 丝滑化：清掉速度/滑行状态 */
    td_axis_rt_reset(1);
    s_sweep     = TD_SWEEP_IDLE;
    s_inited    = 0U;
}

/*****************************************************************************
 * 当前"无目标"持续时长（ms）—— 只作显示/调试。
 * ⚠️ 退出跟随请用 track_dual_sweep_done()：丢失 5s 会先扫一圈，扫完还没有才退。
 *****************************************************************************/
uint32_t track_dual_lost_ms(void)
{
    return s_lost_ms;
}

/*****************************************************************************
 * ==================== 运行时调参（串口实时改手感）====================
 * 状态机在跟随模式下用这些接口现场调；改完下一拍即生效，不用重新烧录。
 *****************************************************************************/
void track_dual_set_speed(uint32_t yaw_us, uint32_t pitch_us)
{
    s_p_step[0] = (yaw_us   > 0U) ? yaw_us   : 1U;
    s_p_step[1] = (pitch_us > 0U) ? pitch_us : 1U;
}

void track_dual_set_accel(uint32_t accel_us)
{
    s_p_accel = (accel_us > 0U) ? accel_us : 1U;
}

void track_dual_set_glide(uint32_t yaw_us, uint32_t pitch_us, uint16_t ticks)
{
    s_p_glide[0]    = yaw_us;
    s_p_glide[1]    = pitch_us;
    s_p_glide_ticks = ticks;
    td_axis_rt_reset(0);
    td_axis_rt_reset(1);
}

void track_dual_set_ctrl_ms(uint32_t ms)
{
    s_p_ctrl_ms = (ms > 0U) ? ms : 1U;
}

void track_dual_reset_params(void)
{
    s_p_step[0]     = TD_YAW_STEP_US;
    s_p_step[1]     = TD_PITCH_STEP_US;
    s_p_glide[0]    = TD_GLIDE_US;
    s_p_glide[1]    = TD_GLIDE_PITCH_US;
    s_p_accel       = TD_ACCEL_US;
    s_p_glide_ticks = (uint16_t)TD_GLIDE_TICKS;
    s_p_ctrl_ms     = TD_CTRL_MS;
    s_p_sweep_ms    = TD_LOST_SWEEP_MS;
    s_p_sweep_step  = TD_SWEEP_STEP_US;
    td_axis_rt_reset(0);
    td_axis_rt_reset(1);
}

void track_dual_show_params(void)
{
    printf("[TD] params: ctrl=%ums speed=Y:%u P:%u accel=%u glide=Y:%u P:%u ticks=%u "
           "sweep=%ums/%uus center=%u\r\n",
           (unsigned)s_p_ctrl_ms,
           (unsigned)s_p_step[0], (unsigned)s_p_step[1],
           (unsigned)s_p_accel,
           (unsigned)s_p_glide[0], (unsigned)s_p_glide[1],
           (unsigned)s_p_glide_ticks,
           (unsigned)s_p_sweep_ms, (unsigned)s_p_sweep_step,
           (unsigned)TD_CENTER_STEP_US);
}

/* ==================== 丢失后的"扫一圈"==================== */

void track_dual_set_sweep(uint32_t wait_ms, uint32_t step_us)
{
    s_p_sweep_ms   = (wait_ms > 0U) ? wait_ms : 1U;
    s_p_sweep_step = (step_us > 0U) ? step_us : 1U;
    s_sweep        = TD_SWEEP_IDLE;
}

uint8_t track_dual_sweep_done(void)
{
    return (s_sweep == TD_SWEEP_DONE) ? 1U : 0U;
}

/*****************************************************************************
 * track_dual_test: 自测入口（init + 死循环 step，不返回）
 * 状态机接入后，main.c 会改调 init/step/exit 三件套，本函数保留作独立自测。
 *****************************************************************************/
void track_dual_test(void)
{
    track_dual_init();

    for (;;) {
        track_dual_step();
        mdelay(TD_SAMPLE_MS);
    }
}

/******************************************************************************
 * 方向整定（上机后如果发现某个轴转反了，按下面对应改一处即可）
 *
 * 本项目约定（servo_test 实测）：脉宽增大 = YAW 右转 / PITCH 抬头。
 *
 * ① 水平反了（手在左却右转）：把 td_decide() 里
 *       m.yaw = -1;  和  m.yaw = +1;
 *    两个赋值对调。
 * ② 竖直反了（手抬高却低头）：把 td_decide() 里
 *       m.pitch = +1;  和  m.pitch = -1;
 *    两个赋值对调（即 C 看到时给 -1、L+R 时给 +1）。
 * ③ 如果发现"只有 C 看到时不该抬头"（比如 C 装在屏幕下沿）：
 *    把 PITCH 那两行的 C / (L&&R) 判断互换即可。
 *
 * 只改 td_decide() 一个函数，其余（限位、丢失分档、节拍）都不用动。
 ******************************************************************************/
