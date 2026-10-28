/******************************************************************************
 * @file     servo_core.c
 * @brief    舵机控制层实现：软限位钳制 + 脉宽输出
 *
 * 通路定义见 servo_pins.h（YAW=PA29/csi_ch3、PITCH=PA27/csi_ch2，两路属于不同的
 * PWM 通道对，因此互不影响）。
 *
 * ==================== 【2026-09 关键改动：改为"只写 compare"】====================
 * 背景：上机看 8 字（move_figure8）时发现 YAW 与 PITCH "有时延、不同步"。
 * 排查结论：**8 字的相位算法没问题**（yaw=sinθ / pitch=sin2θ、查表都对），
 * 根因在本文件的输出方式 —— 原来每步都做 stop → config → start，而驱动里：
 *     csi_pwm_out_stop()  = wj_pwm_config_output_dis()   → 关掉该通道的输出使能
 *     csi_pwm_out_start() = wj_pwm_config_output_en()    → 再打开
 *     csi_pwm_out_config() 会重写 load（周期值），可能重置计数器相位
 * 所以每个 20ms 插值步，两轴各"关一次输出"：
 *   · stop 期间没有输出，若正好跨在高电平段，这一周期的脉冲被【截断】；
 *   · 两路的 stop 时刻相差 config 的执行时间（几微秒），于是两轴脉冲被截断的
 *     相位不同 → 视觉上就是"两轴不同步 / 有时延"（8 字这种两轴同时连续
 *     运动的动作最明显）；每步都发生，所以是持续性的抖动而不是偶发。
 *
 * 现在的做法（每次只写 compare，PWM 自由运行）：
 *   ① 初始化时用驱动 API 配好 负载/分频/compare，并 start 一次 —— 之后不再 stop/start；
 *   ② 每次改脉宽只写 compare_a/compare_b 两个寄存器，新脉宽在下一个周期边界生效；
 *   ③ 两轴的 compare 是在同一次调用序列里几乎同时写入（相差几微秒 << 20ms 周期），
 *      因此总是落在【同一个 PWM 周期】里，一起在同一个周期边界生效 → 真正同步；
 *   ④ compare 的换算不自己复刻驱动的分频公式，而是初始化时用【两个已知脉宽读回
 *      compare】反推出斜率（s_cmp_num / s_cmp_den），与驱动口径完全一致，也跟随
 *      驱动的任何改动。
 *
 * ⚠️ 校准用的是两个"极端"脉宽（1000us / 19000us），必须【在 start 之前】做 ——
 *    那时输出使能还没打开，舵机看不到这些脉宽，不会顶极限。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <soc.h>
#include <drv/pwm.h>
#include <drv/pin.h>
#include "board_config.h"
#include "wj_pwm_ll.h"                  /* 低层：只写 compare（不关输出）*/
#include "servo_core.h"                 /* 含 servo_pins.h → SERVO_PERIOD_US / *_CH / *_PIN 等，勿在此重复定义 */

/* compare 校准用的两个脉宽（单位 us）。间隔取大一些 → 斜率精度高。
 * 这两个值只在输出使能打开【之前】使用，不会真的发给舵机。 */
#define SERVO_CAL_P_A       1000U
#define SERVO_CAL_P_B       19000U

static csi_pwm_t s_pwm;

/* ---- 每路的 compare 换算基准（初始化时测量一次，之后线性外推）---- */
#define SERVO_CH_NUM        2U
static uint32_t s_cmp_ref[SERVO_CH_NUM];    /* 参考脉宽对应的 compare 值 */
static uint32_t s_pulse_ref[SERVO_CH_NUM];  /* 参考脉宽（us）*/
static uint32_t s_cmp_num = 0U;             /* 斜率分子：对应 s_cmp_den 微秒的 compare 增量 */
static uint32_t s_cmp_den = 1U;             /* 斜率分母（必须非 0）*/
static uint8_t  s_inited = 0U;

/* ---- 总线仲裁（见 servo_core.h）：s_hold=1 时 servo_set_pulse() 只登记不落地 ---- */
static volatile uint8_t s_hold = 0U;        /* 1 = 屏幕 DMA 传输窗口内，禁止写 PWM 寄存器 */
static uint32_t s_pend_ch[SERVO_CH_NUM];    /* 挂起的通道号 */
static uint32_t s_pend_pulse[SERVO_CH_NUM]; /* 挂起的脉宽（us）*/
static uint8_t  s_pend_has[SERVO_CH_NUM];   /* 该通道是否有挂起的写 */

/* csi 通道号 → 内部索引（只认本工程两路；不认识的返回 -1）*/
static int ch_index(uint32_t ch)
{
    if (ch == SERVO_YAW_CH) {
        return 0;
    }
    if (ch == SERVO_PITCH_CH) {
        return 1;
    }
    return -1;
}

static wj_pwm_regs_t *pwm_base(void)
{
    /* HANDLE_REG_BASE 宏要求传【指针变量】（不能传表达式），故单独一行 */
    csi_pwm_t *pwm = &s_pwm;

    return (wj_pwm_regs_t *)HANDLE_REG_BASE(pwm);
}

/* 限位钳制：只接受本工程使用的两个通道，未知通道返回 -1（不默认当 YAW） */
static int clamp_pulse(uint32_t ch, uint32_t pulse, uint32_t *out)
{
    uint32_t min;
    uint32_t max;

    if (ch == SERVO_YAW_CH) {
        min = SERVO_YAW_MIN;
        max = SERVO_YAW_MAX;
    } else if (ch == SERVO_PITCH_CH) {
        min = SERVO_PITCH_MIN;
        max = SERVO_PITCH_MAX;
    } else {
        return -1;
    }

    if (pulse < min) {
        pulse = min;
    } else if (pulse > max) {
        pulse = max;
    }

    *out = pulse;
    return 0;
}

/* 把脉宽换算成 compare 并写入（**只写 compare**，不动 load、不关输出）。
 * 方向（与驱动 csi_pwm_out_config 一致）：HIGH 极性下 compare 对应【低电平宽度】
 *     width = period - pulse      →  脉宽变大 ⇒ compare 变小
 * 故：cmp = cmp_ref - (pulse - pulse_ref) × s_cmp_num / s_cmp_den */
static void write_compare(uint32_t ch, uint32_t pulse)
{
    int     idx = ch_index(ch);
    int32_t delta;
    int32_t cmp;
    wj_pwm_regs_t *base;

    if (idx < 0) {
        return;
    }

    delta = (int32_t)pulse - (int32_t)s_pulse_ref[idx];
    /* delta ≤ ±800、s_cmp_num ≤ 65535 → 乘积 ≤ 5.2e7，32 位足够 */
    cmp = (int32_t)s_cmp_ref[idx] - (delta * (int32_t)s_cmp_num) / (int32_t)s_cmp_den;

    if (cmp < 0) {
        cmp = 0;
    }

    base = pwm_base();
    wj_pwm_counter_write_compare_a_ch(base, ch, (uint32_t)cmp);
    wj_pwm_counter_write_compare_b_ch(base, ch, (uint32_t)cmp);
}

/* 总线仲裁钩子（见 servo_core.h）
 *   busy != 0 : 屏幕开始一段 DMA 传输 → 之后只登记，不写寄存器
 *   busy == 0 : 段间空隙、DMA 空闲 → 把挂起的写落地
 * 一帧会各调用 60 次，所以舵机最多滞后一个段（≈1ms），机械上完全无感。 */
void servo_bus_event(int busy)
{
    int i;

    if (busy != 0) {
        s_hold = 1U;
        return;
    }

    s_hold = 0U;
    for (i = 0; i < (int)SERVO_CH_NUM; i++) {
        if (s_pend_has[i] != 0U) {
            s_pend_has[i] = 0U;
            write_compare(s_pend_ch[i], s_pend_pulse[i]);
        }
    }
}

int servo_set_pulse(uint32_t ch, uint32_t pulse)
{
    uint32_t p;
    int      idx;

    if (clamp_pulse(ch, pulse, &p) != 0) {
        printf("[core] ch%u invalid\r\n", ch);
        return -1;
    }
    if (s_inited == 0U) {
        printf("[core] not inited\r\n");
        return -1;
    }

    if (s_hold != 0U) {
        /* 屏幕正在传输：此刻写 PWM 寄存器会抢 AHB、打断 DMA 取数（屏幕出白块），
         * 所以只登记，等渲染端在段间空隙调 servo_bus_event(0) 时再落地。 */
        idx = ch_index(ch);
        if (idx >= 0) {
            s_pend_ch[idx]    = ch;
            s_pend_pulse[idx] = p;
            s_pend_has[idx]   = 1U;
        }
        return 0;
    }

    write_compare(ch, p);
    return 0;
}

int servo_core_init(void)
{
    wj_pwm_regs_t *base;
    uint32_t cmp_a;
    uint32_t cmp_b;
    int      idx;

    /* 1) 引脚复用（通路定义在 servo_pins.h） */
    csi_pin_set_mux(SERVO_YAW_PORT, SERVO_YAW_PIN, SERVO_YAW_FUNC);
    csi_pin_set_mux(SERVO_PITCH_PORT, SERVO_PITCH_PIN, SERVO_PITCH_FUNC);

    /* 2) PWM 控制器初始化 */
    if (csi_pwm_init(&s_pwm, SERVO_PWM_IDX) != CSI_OK) {
        printf("[core] pwm init fail\r\n");
        return -1;
    }

    /* 3) 校准 compare 斜率：同一个通道写两个不同脉宽、读回 compare 求差
     *    （此时输出使能还没打开 → 舵机看不到这 1000us/19000us，不会顶极限） */
    base = pwm_base();
    if (csi_pwm_out_config(&s_pwm, SERVO_YAW_CH, SERVO_PERIOD_US,
                           SERVO_CAL_P_A, PWM_POLARITY_HIGH) != CSI_OK) {
        printf("[core] cal config A fail\r\n");
        return -1;
    }
    cmp_a = wj_pwm_counter_read_compare_a_ch(base, SERVO_YAW_CH);
    if (csi_pwm_out_config(&s_pwm, SERVO_YAW_CH, SERVO_PERIOD_US,
                           SERVO_CAL_P_B, PWM_POLARITY_HIGH) != CSI_OK) {
        printf("[core] cal config B fail\r\n");
        return -1;
    }
    cmp_b = wj_pwm_counter_read_compare_a_ch(base, SERVO_YAW_CH);

    if (cmp_a > cmp_b) {
        s_cmp_num = cmp_a - cmp_b;              /* 脉宽 B 更大 → compare 更小 */
        s_cmp_den = SERVO_CAL_P_B - SERVO_CAL_P_A;
    } else {
        printf("[core] cal fail (cmp_a %u <= cmp_b %u)\r\n", cmp_a, cmp_b);
        return -1;
    }

    /* 4) 两路各自设到中位并记录参考点（仍用驱动 config，保证 load/分频口径一致）*/
    if (csi_pwm_out_config(&s_pwm, SERVO_YAW_CH, SERVO_PERIOD_US,
                           SERVO_YAW_CENTER, PWM_POLARITY_HIGH) != CSI_OK) {
        printf("[core] yaw config fail\r\n");
        return -1;
    }
    idx = ch_index(SERVO_YAW_CH);
    s_pulse_ref[idx] = SERVO_YAW_CENTER;
    s_cmp_ref[idx]   = wj_pwm_counter_read_compare_a_ch(base, SERVO_YAW_CH);

    if (csi_pwm_out_config(&s_pwm, SERVO_PITCH_CH, SERVO_PERIOD_US,
                           SERVO_PITCH_CENTER, PWM_POLARITY_HIGH) != CSI_OK) {
        printf("[core] pitch config fail\r\n");
        return -1;
    }
    idx = ch_index(SERVO_PITCH_CH);
    s_pulse_ref[idx] = SERVO_PITCH_CENTER;
    s_cmp_ref[idx]   = wj_pwm_counter_read_compare_a_ch(base, SERVO_PITCH_CH);

    /* 5) 打开两路输出使能 —— 【只在这一处】，之后一律不再 stop/start */
    if (csi_pwm_out_start(&s_pwm, SERVO_YAW_CH) != CSI_OK) {
        printf("[core] yaw start fail\r\n");
        return -1;
    }
    if (csi_pwm_out_start(&s_pwm, SERVO_PITCH_CH) != CSI_OK) {
        printf("[core] pitch start fail\r\n");
        return -1;
    }

    s_inited = 1U;

    printf("[core] servo init ok: %s(ch%u)=%uus, %s(ch%u)=%uus "
           "(cmp/u %u/%u, compare-only mode)\r\n",
           SERVO_YAW_PIN_TXT, SERVO_YAW_CH, SERVO_YAW_CENTER,
           SERVO_PITCH_PIN_TXT, SERVO_PITCH_CH, SERVO_PITCH_CENTER,
           s_cmp_num, s_cmp_den);
    return 0;
}
