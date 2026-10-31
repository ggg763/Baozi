/******************************************************************************
 * @file     servo_dual_test.c
 * @brief    双路回归验证 + 动作演示（走 servo_core 控制层）
 *
 * 本版特意改成调用控制层 `servo_core`（而不是自己直接操作 csi_pwm），
 * 一次同时验证三件事：
 *   ① 控制层可用：servo_core_init() + servo_set_pulse() 正常
 *   ② 限位钳制生效：两路的往复范围都在 servo_core.h 的实测限位内，全程不应该
 *      出现异响/顶极限；就算故意给超范围值也会被钳制并打印提示
 *   ③ 两路独立：两路 csi_ch 不同（YAW=3 / PITCH=2），各按自己节奏走、互不牵连
 *
 *  引脚/通道来自 servo_pins.h（控制端）：YAW=PA29/csi_ch 3、PITCH=PA27/csi_ch 2
 *  中位/限位来自 servo_core.h（2026-09 实测）：YAW 1000us(300~1700)、PITCH 1100us(800~1600)
 *
 * ── 动作 ──────────────────────────────────────────────────────────
 *   上电：servo_core_init() 把两路回到实测中位（YAW 1000us / PITCH 1100us），
 *         舵机应停住、无异响
 *   之后：路 A（YAW  ）在  800 ~ 1500us 往复，每 500ms 走 20us
 *         路 B（PITCH）在 1200 ~ 1600us 往复，每 700ms 走 40us（节奏与范围都不同）
 *
 *   本文件的往复范围必须整体落在 servo_core.h 的限位内，否则超出的那半段会被
 *   servo_set_pulse 静默钳平 —— 看起来就像"舵机走到某处就不动了"，容易误判成故障。
 *
 * ── 怎么看才算"回归通过" ─────────────────────────────────────────
 *   ✅ 上电两路稳定停在中位，无嗡响
 *   ✅ 之后两个舵机各按自己节奏、在自己的范围内摆动，互不牵连
 *   ✅ 串口持续打印两路脉宽，且**没有** "clamped" 之类被限位钳制的提示
 *   ❌ 若出现异响/顶死 -> 说明某个范围值仍超实际行程，把对应 MAX/MIN 再收一点
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "board_config.h"
#include "drv/common.h"     /* mdelay */
#include "servo_core.h"     /* servo_core_init / servo_set_pulse / 中位限位宏 / servo_pins.h */

/* ==================== 两路（通道号直用 servo_pins.h 的 csi_ch）==================== */
#define CH_A                SERVO_YAW_CH
#define TXT_A               SERVO_YAW_PIN_TXT
#define CH_B                SERVO_PITCH_CH
#define TXT_B               SERVO_PITCH_PIN_TXT

/* ==================== 节奏参数（均落在 servo_core.h 的限位内）==================== */
/* 路 A（YAW）：800~1500us，每 500ms 走 20us */
#define A_MIN_US            800U
#define A_MAX_US            1500U
#define A_START_US          1000U
#define A_STEP_US           20U
#define A_STEP_MS           500U

/* 路 B（PITCH）：1200~1600us，每 700ms 走 40us */
#define B_MIN_US            1200U
#define B_MAX_US            1600U
#define B_START_US          1300U
#define B_STEP_US           40U
#define B_STEP_MS           700U

#define TICK_MS             100U        /* 主循环节拍 */
/* ==================================================== */

/* 两路状态 */
static uint32_t s_a = A_START_US;
static int32_t  s_a_dir = 1;
static uint32_t s_b = B_START_US;
static int32_t  s_b_dir = 1;

static void print_state(void)
{
    /* 用整数算百分比（×10 保留一位小数），避免依赖 printf 的浮点支持 */
    uint32_t a_pct10 = s_a * 1000U / SERVO_PERIOD_US;
    uint32_t b_pct10 = s_b * 1000U / SERVO_PERIOD_US;

    printf("[dual] %s(ch%u)=%uus (%u.%u%%)   %s(ch%u)=%uus (%u.%u%%)\r\n",
           TXT_A, CH_A, s_a, a_pct10 / 10U, a_pct10 % 10U,
           TXT_B, CH_B, s_b, b_pct10 / 10U, b_pct10 % 10U);
}

/* 推进一步（各自往复），设置时走控制层 servo_set_pulse（自带限位钳制）*/
static void step_a(void)
{
    if (s_a_dir > 0) {
        s_a += A_STEP_US;
        if (s_a >= A_MAX_US) { s_a = A_MAX_US; s_a_dir = -1; }
    } else {
        s_a -= A_STEP_US;
        if (s_a <= A_MIN_US) { s_a = A_MIN_US; s_a_dir = 1; }
    }
    servo_set_pulse(CH_A, s_a);
}

static void step_b(void)
{
    if (s_b_dir > 0) {
        s_b += B_STEP_US;
        if (s_b >= B_MAX_US) { s_b = B_MAX_US; s_b_dir = -1; }
    } else {
        s_b -= B_STEP_US;
        if (s_b <= B_MIN_US) { s_b = B_MIN_US; s_b_dir = 1; }
    }
    servo_set_pulse(CH_B, s_b);
}

/*****************************************************************************
 * servo_dual_test: 入口（被 src/main.c 调用，内部死循环不返回）
 *****************************************************************************/
void servo_dual_test(void)
{
    uint32_t acc_a = 0U;
    uint32_t acc_b = 0U;

    printf("\r\n[dual] BOOT: regression + demo via servo_core\r\n");

    /* 1) 控制层初始化：引脚 mux + PWM 控制器 + 两路回中位 */
    if (servo_core_init() != 0) {
        printf("[dual] servo_core_init fail\r\n");
        return;
    }
    printf("[dual] servo_core_init ok: %s mid=%uus, %s mid=%uus\r\n",
           TXT_A, SERVO_YAW_CENTER, TXT_B, SERVO_PITCH_CENTER);
    printf("[dual] limits: %s %u~%uus, %s %u~%uus\r\n",
           TXT_A, SERVO_YAW_MIN, SERVO_YAW_MAX,
           TXT_B, SERVO_PITCH_MIN, SERVO_PITCH_MAX);

    /* 2) 挪到各自的起始位置 */
    servo_set_pulse(CH_A, s_a);
    servo_set_pulse(CH_B, s_b);
    print_state();

    printf("[dual] %s sweeps %u~%uus every %ums ; %s sweeps %u~%uus every %ums\r\n",
           TXT_A, A_MIN_US, A_MAX_US, A_STEP_MS,
           TXT_B, B_MIN_US, B_MAX_US, B_STEP_MS);
    printf("[dual] PASS if: both hold at center on boot, then swing independently,\r\n");
    printf("[dual]          no buzzing / no clamping messages\r\n");

    /* 3) 主循环：两路各按自己节奏推进 */
    for (;;) {
        mdelay(TICK_MS);

        acc_a += TICK_MS;
        if (acc_a >= A_STEP_MS) {
            acc_a = 0U;
            step_a();
        }

        acc_b += TICK_MS;
        if (acc_b >= B_STEP_MS) {
            acc_b = 0U;
            step_b();
        }

        if ((acc_a == 0U) || (acc_b == 0U)) {
            print_state();
        }
    }
}
