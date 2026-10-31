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

/* ★ 2026-11：去重写 —— 上次真正写进 compare 的脉宽。
 *
 * 为什么需要：moves.c 的播放引擎每 20ms 无条件调一次
 *   servo_set_pulse(YAW) + servo_set_pulse(PITCH)，
 * **哪怕这一拍的目标位置和上一拍一模一样**。于是静止待机时也在
 * 以 50Hz 反复读-改-写 PWMxCMP 寄存器（每路每周期各一次，共 100 次/秒）。
 *
 * 写入本身有一次「读 32 位 → 改字段 → 写回」的窗口，而硬件比较器是随时
 * 在跑的：万一落点正好压在比较时刻上，这一周期的边沿就可能延迟/错拍，
 * 输出一个宽度不对的脉冲。舵机看到的就是"静止也在微微抖"。
 * 10-31 已经把"连调两个 ll 函数导致寄存器被写 4 次"修成单次读-改-写，
 * 这里是同一类风险的剩余部分：**值没变就根本不该碰硬件**。
 *
 * 收益：静止/微幅动作时 PWM 寄存器零写入（总线也不抢了），
 * 只有目标真的变了才写。副作用为零 —— 值相同，写与不写硬件状态一致。 */
static uint32_t s_last_pulse[SERVO_CH_NUM];
static uint8_t  s_last_valid[SERVO_CH_NUM];

/* ★ 2026-11：最小有效步长（us）—— 目标变化小于它就先不下发，攒够再写。
 *
 * 为什么需要（上机症状，三个现象互相对得上）：
 *   · PITCH「缓变/小步走会卡住、手推一下才动，但一下跳到目标位置反而不卡」
 *   · YAW「静止不抖，只有小幅度动作抖」
 *   · 供电是独立舵机电源（排除欠压）
 *
 * 舵机是【位置伺服】不是步进电机：内部有死区（模拟舵机 5~10us≈0.5~0.9°，
 * 垂直抗重力时更大），再加上齿轮静摩擦。播放引擎每 20ms 下一拍，
 * 如果目标只挪 1~2us（缓动段的开头/结尾就是这样，ease 速度趋近 0），
 * 误差永远落在死区内 → 电机不转 → 目标继续漂移 → 攒够死区才"咯"一下动：
 * 表现就是小步不走 / 一顿一顿 / 顶在那儿要手推。而一次跳到位给了大误差，
 * 电机有足够力矩，所以反而不卡。
 *
 * 做法：把「目标值」和「上次真正写进寄存器的值」比，差值不足 min_step 就
 * 不动硬件（无损：差值是累积的，目标继续走就一定会超过阈值）。等价于把
 * 缓慢的连续微动量化成"每 T 毫秒一次、每次 ≥ min_step"的台阶。
 *
 * 代价只有一个：动作最终停下的位置可能与目标差 < min_step（YAW 3us≈0.27°，
 * PITCH 4us≈0.36°），肉眼看不出。反过来，**总行程本身就小于死区的动作
 * （比如峰峰值 10us）在物理上就是不可见的**，以前那种"每拍写 1us"只是让
 * 舵机在原地较劲，现在会干脆不动。
 *
 * PITCH 给得比 YAW 大：垂直方向要抗重力，静摩擦和死区都更大。 */
#ifndef SERVO_YAW_MIN_STEP_US
#define SERVO_YAW_MIN_STEP_US       3U
#endif
#ifndef SERVO_PITCH_MIN_STEP_US
#define SERVO_PITCH_MIN_STEP_US     4U
#endif

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
 * 故：cmp = cmp_ref - (pulse - pulse_ref) × s_cmp_num / s_cmp_den
 *
 * ★ 2026-10-31 修正：一次读-改-写。
 *   原实现连调 wj_pwm_counter_write_compare_a_ch / _b_ch，而这两个 ll 函数
 *   **各自都是「先 &~ 清字段、再 |= 置位」两次写**。于是每改一次脉宽，
 *   PWMxCMP 这个寄存器会被写 4 次，中间留下两段「该字段 = 0」的瞬态。
 *   如果计数器恰好在这几十 ns 内做比较，本周期会被判成「已匹配」，
 *   输出一个宽度错误（甚至反方向）的脉冲 —— 舵机就会抽一下。
 *   小步长（20us）时这个错误只有 20us，完全看不出来；
 *   大跳变（几百 us）时错误脉冲可达几百 us 甚至上千 us，表现为卡顿/抖动。
 *   现在合并成单次读-改-写，A/B 两个字段在同一个 store 里更新，瞬态消失。
 *   （一帧 60 段 DMA 的总线仲裁仍然有效，见 servo_bus_event。）*/
static void write_compare(uint32_t ch, uint32_t pulse)
{
    int     idx = ch_index(ch);
    int32_t delta;
    int32_t cmp;
    wj_pwm_regs_t *base;
    uint32_t reg;

    if (idx < 0) {
        return;
    }

    /* ★ 去重写 + 最小有效步长：与上次真正写下去的值相差不到 min_step 就什么都不做。
     *   理由见文件头部「最小有效步长」——小步长小于舵机死区时写下去只是让舵机
     *   在原地较劲（抖 / 卡），攒够一次再写才有实际位移。
     *   总线仲裁的挂起路径也会走到这里，所以"挂起后发现没到阈值"同样被吃掉。 */
    {
        uint32_t min_step = (idx == 0) ? SERVO_YAW_MIN_STEP_US
                                       : SERVO_PITCH_MIN_STEP_US;
        uint32_t diff = (pulse > s_last_pulse[idx])
                        ? (pulse - s_last_pulse[idx])
                        : (s_last_pulse[idx] - pulse);

        if ((s_last_valid[idx] != 0U) && (diff < min_step)) {
            return;
        }
    }
    s_last_pulse[idx] = pulse;
    s_last_valid[idx] = 1U;

    delta = (int32_t)pulse - (int32_t)s_pulse_ref[idx];
    /* delta ≤ ±800、s_cmp_num ≤ 65535 → 乘积 ≤ 5.2e7，32 位足够 */
    cmp = (int32_t)s_cmp_ref[idx] - (delta * (int32_t)s_cmp_num) / (int32_t)s_cmp_den;

    if (cmp < 0) {
        cmp = 0;
    }

    base = pwm_base();
    reg  = *(&base->PWM0CMP + ch);
    reg &= ~(WJ_PWM_COUNTER_COMP_CH_A_Msk | WJ_PWM_COUNTER_COMP_CH_B_Msk);
    reg |= ((uint32_t)cmp << WJ_PWM_COUNTER_COMP_CH_A_Pos) |
           ((uint32_t)cmp << WJ_PWM_COUNTER_COMP_CH_B_Pos);
    *(&base->PWM0CMP + ch) = reg;
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

    /* ★ 幂等保护（2026-11）：本函数会被调用两次 —— moves_init() 开机一次，
     *   track_dual_init() 每次进红外跟随再调一次。
     *   重复跑的危险在第 3 步校准：它把 YAW 的 compare 依次设成
     *   SERVO_CAL_P_A(1000us) / SERVO_CAL_P_B(19000us)。**上电首次调用时**
     *   "输出使能还没打开"，所以注释里说舵机看不到这段；但第二次调用时
     *   csi_pwm_out_start() 早就把输出打开了 —— 19000us 在 20ms 周期里等于
     *   把舵机驱到一端的死点堵转（电流尖峰，可能把板子拉复位）。
     *   已经初始化过就只把两路收回中位，不重做 mux/pwm_init/校准。 */
    if (s_inited != 0U) {
        (void)servo_set_pulse(SERVO_YAW_CH,   SERVO_YAW_CENTER);
        (void)servo_set_pulse(SERVO_PITCH_CH, SERVO_PITCH_CENTER);
        printf("[core] servo already inited -> recenter only "
               "(yaw=%uus pitch=%uus)\r\n",
               SERVO_YAW_CENTER, SERVO_PITCH_CENTER);
        return 0;
    }

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
    s_last_pulse[idx] = SERVO_YAW_CENTER;      /* 去重写的基准点 */
    s_last_valid[idx] = 1U;

    if (csi_pwm_out_config(&s_pwm, SERVO_PITCH_CH, SERVO_PERIOD_US,
                           SERVO_PITCH_CENTER, PWM_POLARITY_HIGH) != CSI_OK) {
        printf("[core] pitch config fail\r\n");
        return -1;
    }
    idx = ch_index(SERVO_PITCH_CH);
    s_pulse_ref[idx] = SERVO_PITCH_CENTER;
    s_cmp_ref[idx]   = wj_pwm_counter_read_compare_a_ch(base, SERVO_PITCH_CH);
    s_last_pulse[idx] = SERVO_PITCH_CENTER;    /* 去重写的基准点 */
    s_last_valid[idx] = 1U;

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
