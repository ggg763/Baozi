/******************************************************************************
 * @file     servo_mvp_test.c
 * @brief    MVP：先用 PA0 做最直观的输出测试（三种模式，从最容易观察的开始）
 *
 * 三个模式（改 MVP_MODE 即可）：
 *
 *   1 = 正弦调制（SPWM）【默认，本次要做的正弦波】
 *       载波：周期 1000us（1kHz）的 PWM
 *       调制：占空比在 5%~95% 之间按正弦规律变化，16 点/周期、每点 60ms
 *             → 一个完整正弦周期 ≈ 960ms（约 1Hz）
 *       示波器：时基 200ms/div 能直接看到"脉宽由窄 → 宽 → 窄"的正弦包络；
 *               若在引脚后面串一个 RC 低通（1kΩ 串 + 100nF 到 GND），低通输出
 *               就是真正的正弦波。
 *
 *   2 = GPIO 慢方波（不经过 PWM）
 *       PA0 每 500ms 翻转一次 → 1Hz 方波。
 *       这个【不需要示波器】：万用表直流档能看到电压在 0V ↔ 1.8V 之间跳（平均≈0.9V），
 *       接 LED + 1kΩ 电阻会明显闪烁。用来先确认"引脚本身能不能输出"。
 *
 *   0 = 官方 PWM 方波（官方示例原样）
 *       周期 30us、低电平 10us（≈33kHz）。
 *       ⚠️ 33kHz 需要示波器时基给到 10~20us/div；若还停在 5ms/div 会糊成一片亮带，
 *          看起来就像"没有波形"。
 *
 * 建议顺序：**先 2（确认引脚活着）→ 再 1（看正弦）→ 最后 0（官方方波，注意时基）**
 *
 * ── 关键参数速查 ──────────────────────────────────────────────────
 *   csi 通道号规则：csi_ch = pinmap channel ÷ 2
 *       PA0  (PA0_PWM_CH0, pinmap 0) -> csi_ch 0
 *       PA27 (PA27_PWM_CH4, pinmap 4) -> csi_ch 2
 *       PA31 (PA31_PWM_CH8, pinmap 8) -> csi_ch 4   ← 官方示例用的引脚
 *   MVP_TARGET 选引脚（三模式都会用到，模式 2 的 GPIO 也用同一引脚）：
 *       0 = PA0 / csi_ch 0      1 = PA27 / csi_ch 2
 *       2 = PA31 / csi_ch 4     —— 已让出（同步控制/LCD 背光），仅诊断时临时用
 * ─────────────────────────────────────────────────────────────────
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <soc.h>
#include <drv/pwm.h>
#include <drv/pin.h>
#include <drv/gpio.h>       /* 模式 2 用 GPIO */
#include <drv/common.h>     /* mdelay */
#include "board_config.h"

/* ==================== 模式与目标 ==================== */
#define MVP_MODE            1       /* 1=正弦调制(默认)  2=GPIO慢方波  0=官方PWM方波 */

#define MVP_TARGET          0       /* 0=PA0/csi0  1=PA27/csi2  2=PA31/csi4(已让出，仅诊断时临时用) */

#if MVP_TARGET == 0
#define MVP_PORT            PORTA
#define MVP_PIN             PIN0
#define MVP_FUNC            PA0_PWM_CH0
#define MVP_CH              0U
#define MVP_TXT             "PA0  / PA0_PWM_CH0  / csi_ch 0"
#elif MVP_TARGET == 1
#define MVP_PORT            PORTA
#define MVP_PIN             PIN27
#define MVP_FUNC            PA27_PWM_CH4
#define MVP_CH              2U
#define MVP_TXT             "PA27 / PA27_PWM_CH4 / csi_ch 2"
#else
#define MVP_PORT            PORTA
#define MVP_PIN             PIN31
#define MVP_FUNC            PA31_PWM_CH8
#define MVP_CH              4U
#define MVP_TXT             "PA31 / PA31_PWM_CH8 / csi_ch 4 (official example)"
#endif

#define MVP_IDX             0U      /* PWM 控制器序号 */

/* 模式 0：官方 PWM 方波 */
#define MVP0_PERIOD_US      30U
#define MVP0_PULSE_US       10U

/* 模式 1：正弦调制（SPWM）*/
#define MVP1_CARRIER_US     1000U   /* 载波周期 1ms = 1kHz */
#define MVP1_MIN_PERMILLE   50U     /* 占空比下限 5.0% */
#define MVP1_MAX_PERMILLE   950U    /* 占空比上限 95.0% */
#define MVP1_STEP_MS        60U     /* 每个正弦点停留毫秒；16 点 ≈ 960ms 一个周期 */
/* ==================================================== */

#if (MVP_MODE == 0) || (MVP_MODE == 1)
static csi_pwm_t g_pwm_handle;      /* PWM 句柄（模式 0 / 1）*/
#endif
#if (MVP_MODE == 2)
static csi_gpio_t g_gpio;           /* GPIO 句柄（模式 2）*/
#endif

#if (MVP_MODE == 1)
/* 16 点正弦表：sin(2πi/16) 映射到 50~950 千分比（即 5%~95% 占空比）*/
static const uint16_t s_sine16[16] = {
    500U, 672U, 818U, 916U, 950U, 916U, 818U, 672U,
    500U, 328U, 182U,  90U,  50U,  90U, 182U, 328U
};

/* ---------------- 模式 1：正弦调制（SPWM）---------------- */
static void mvp_sine(void)
{
    int32_t state;
    uint32_t i;

    printf("[mvp] mode 1: SPWM sine on %s\r\n", MVP_TXT);
    printf("[mvp] carrier = %uus (1kHz), duty sweeps 5%%~95%% by a 16-point sine table\r\n",
           (uint32_t)MVP1_CARRIER_US);
    printf("[mvp] each point holds %ums -> one sine cycle = %ums (~1Hz)\r\n",
           (uint32_t)MVP1_STEP_MS, (uint32_t)(16U * MVP1_STEP_MS));

    csi_pin_set_mux(MVP_PORT, MVP_PIN, MVP_FUNC);
    printf("[mvp] step1 csi_pin_set_mux done\r\n");

    state = csi_pwm_init(&g_pwm_handle, MVP_IDX);
    if (state < 0) {
        printf("[mvp] csi_pwm_init error: %d\r\n", (int)state);
        return;
    }
    printf("[mvp] step2 csi_pwm_init ok (idx=%u)\r\n", MVP_IDX);
    printf("[mvp] scope: 200ms/div to see the sine envelope; 200us/div to see the carrier\r\n");

    for (;;) {
        for (i = 0U; i < 16U; i++) {
            uint32_t pulse = MVP1_CARRIER_US * s_sine16[i] / 1000U;   /* 单位 us */

            csi_pwm_out_config(&g_pwm_handle, MVP_CH, MVP1_CARRIER_US, pulse,
                               PWM_POLARITY_HIGH);
            csi_pwm_out_start(&g_pwm_handle, MVP_CH);
            mdelay(MVP1_STEP_MS);
        }
    }
}
#endif

#if (MVP_MODE == 2)
/* ---------------- 模式 2：GPIO 慢方波（1Hz，不经 PWM）---------------- */
static void mvp_gpio_slow(void)
{
    const uint32_t mask = (1U << MVP_PIN);

    printf("[mvp] mode 2: GPIO slow square wave (1Hz) on %s\r\n", MVP_TXT);
    printf("[mvp] PA0 toggles every 500ms -> DMM shows 0V <-> 1.8V (avg ~0.9V), LED blinks\r\n");

    csi_pin_set_mux(MVP_PORT, MVP_PIN, PIN_FUNC_GPIO);
    csi_gpio_init(&g_gpio, MVP_PORT);
    csi_gpio_dir(&g_gpio, mask, GPIO_DIRECTION_OUTPUT);
    printf("[mvp] GPIO output ready, toggling...\r\n");

    for (;;) {
        csi_gpio_write(&g_gpio, mask, 1U);
        mdelay(500U);
        csi_gpio_write(&g_gpio, mask, 0U);
        mdelay(500U);
    }
}
#endif

#if (MVP_MODE == 0)
/* ---------------- 模式 0：官方 PWM 方波（官方示例原样）---------------- */
static void mvp_official(void)
{
    int32_t state;

    printf("[mvp] mode 0: official PWM square wave on %s\r\n", MVP_TXT);
    printf("[mvp] period=%uus, low level=%uus (~33kHz) -> scope needs 10~20us/div!\r\n",
           (uint32_t)MVP0_PERIOD_US, (uint32_t)MVP0_PULSE_US);

    csi_pin_set_mux(MVP_PORT, MVP_PIN, MVP_FUNC);
    printf("[mvp] step1 csi_pin_set_mux done\r\n");

    state = csi_pwm_init(&g_pwm_handle, MVP_IDX);
    if (state < 0) {
        printf("[mvp] csi_pwm_init error: %d\r\n", (int)state);
        return;
    }
    printf("[mvp] step2 csi_pwm_init ok (idx=%u)\r\n", MVP_IDX);

    state = csi_pwm_out_config(&g_pwm_handle, MVP_CH, MVP0_PERIOD_US,
                               MVP0_PULSE_US, PWM_POLARITY_LOW);
    if (state < 0) {
        printf("[mvp] csi_pwm_out_config error: %d\r\n", (int)state);
        return;
    }
    printf("[mvp] step3 csi_pwm_out_config ok: csi_ch=%u period=%uus pulse=%uus\r\n",
           MVP_CH, (uint32_t)MVP0_PERIOD_US, (uint32_t)MVP0_PULSE_US);

    state = csi_pwm_out_start(&g_pwm_handle, MVP_CH);
    if (state < 0) {
        printf("[mvp] csi_pwm_out_start error: %d\r\n", (int)state);
        return;
    }
    printf("[mvp] step4 csi_pwm_out_start ok -> waveform should be on the pin NOW\r\n");

    for (;;) {
        mdelay(5000U);
        printf("[mvp] still running (mode 0)\r\n");
    }
}
#endif

/*****************************************************************************
 * servo_mvp_test: 入口（被 src/main.c 调用）
 *****************************************************************************/
void servo_mvp_test(void)
{
    printf("\r\n[mvp] BOOT: MVP output test\r\n");
    printf("[mvp] MVP_MODE = %d  (0=official PWM, 1=SPWM sine, 2=GPIO 1Hz)\r\n", (int)MVP_MODE);
    printf("[mvp] target  : %s\r\n", MVP_TXT);

#if (MVP_MODE == 1)
    mvp_sine();
#elif (MVP_MODE == 2)
    mvp_gpio_slow();
#else
    mvp_official();
#endif
}
