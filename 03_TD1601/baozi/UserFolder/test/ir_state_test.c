/******************************************************************************
 * @file     ir_state_test.c
 * @brief    三路红外「状态解码」验证版 —— 只打印，不驱动舵机
 *
 * 【要回答的问题】
 *   接云台之前必须先确认：三个探头到底能不能分辨「左 / 中 / 右 / 上 / 下」？
 *   如果三路总是同时被触发（state 在 NONE / LCR 之间来回跳），说明三者视野
 *   完全重叠 —— 那么云台跟随会表现为"永远判定居中、根本不动"，看起来像
 *   固件坏了。必须先做光学隔离（遮光套筒）或拉开间距。
 *
 * 【硬件】
 *   IR L = PB22、C = PB23、R = PB29（与 ir_test.c 同一组）
 *   模块输出极性：未遮挡 = 1（高），遮挡/有目标 = 0（低）
 *   -> 本文件里 "seen" = 看到目标 = !raw；打印时 raw 与 seen 都给，
 *      避免把极性看反。
 *
 * 【用法】⚠️ 手法很重要
 *   反射式红外模块有【最小探测距离】（典型 2~3cm）：手掌直接贴上去、或按在屏幕上
 *   盖住两个探头，反而不会触发 —— 三路都读到高，state 只会显示 NONE。
 *   这不是固件的问题。正确做法：拿一张纸（或手掌）竖直立在离屏幕 5~8cm 处，
 *   从最左慢慢平移到最右，再上下走一遍。
 *   要出现 LC / CR / LCR 这类组合，遮盖物必须【同时】进入两个（或三个）探头的视锥 ——
 *   手掌太小、离得太远、或只扫到单个探头，就只会出现单独状态。
 *
 *   每 2 秒打印两行汇总，注意区分：
 *     observed (stable)  = 去抖后真正生效的状态（短暂组合会被 60ms 去抖滤掉）
 *     observed (instant) = 每个采样点看到的原始组合
 *   两行对比即可判断：某个组合是"物理上从没出现"，还是"出现过但太短、被滤掉了"。
 *
 * 【输出示例】
 *   [IRST] raw=1 1 0 seen(L,C,R)=0 0 1 state=R_ONLY  hold=  320ms
 *   [IRST] observed (stable):  NONE=1 R=1 C=1 CR=0 L=1 LR=0 LC=1 LCR=0
 *   [IRST] observed (instant): NONE=1 R=1 C=1 CR=1 L=1 LR=0 LC=1 LCR=1
 *          ^ raw=引脚原始电平(未遮挡=1)；seen=看到目标=1
 *          ^ 上例表示 CR / LCR 只瞬时出现过、没能维持 60ms，所以 stable 行是 0
 *            而 instant 行是 1 —— 属于测试手法问题，不是固件漏写。
 *          ^ 若 L / C / R 三项在 stable 行都出现过 1 -> 三路能分开，视野没糊在一起
 *
 * 去抖：连续 12 次一致才认可（5ms × 12 = 60ms）；状态变化立即打印，
 *       无变化每 1s 打一行心跳（避免"没输出"和"程序跑死"分不清）。
 *       采样 5ms 是为了抓短脉冲 —— 见上面【用法】里关于"指示灯亮但读不到"的排查。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <soc.h>
#include <drv/gpio.h>
#include <drv/pin.h>
#include <drv/common.h>     /* mdelay */
#include "board_config.h"

/* ---- 三路红外输入（PORTB，位掩码） ---- */
#define IR_LEFT_PIN     PIN22
#define IR_CENTER_PIN   PIN23
#define IR_RIGHT_PIN    PIN29
#define IR_ALL_PIN      (IR_LEFT_PIN | IR_CENTER_PIN | IR_RIGHT_PIN)

#define IR_GPIO_IDX     1       /* PORTB 的 GPIO 控制器 idx */

/* ---- 采样与去抖参数 ----
 * ⚠️ 采样周期从 20ms 降到 5ms：现场现象是"模块指示灯已亮（模块内部检测到了），
 *    但串口读到的还是高电平"。若叠加态只维持几十毫秒，20ms/50ms 的采样很可能
 *    整段错过 —— 降采样率是为了先把"到底有没有读到"这件事确定下来。
 *    去抖/心跳/汇总三者的【时间】保持不变（只改次数），行为与原来一致。 */
#define SAMPLE_MS       5U      /* 采样周期（原 20ms） */
#define DEBOUNCE_N      12U     /* 连续 N 次一致才认可（12 × 5ms = 60ms，与原 3 × 20ms 等效） */
#define HEARTBEAT_N     200U    /* 无变化时每 N 次采样打一行（200 × 5ms = 1s） */
#define SUMMARY_N       400U    /* 每 N 次采样打一行 observed 汇总（400 × 5ms = 2s） */

/* 位序统一：bit2 = L，bit1 = C，bit0 = R（seen 里 1 = 看到目标） */
#define BIT_L           2U
#define BIT_C           1U
#define BIT_R           0U

static const char *s_state_name[8] = {
    "NONE  ",   /* 000 无目标              */
    "R_ONLY",   /* 001 只看到右            */
    "C_ONLY",   /* 010 只看到中            */
    "CR    ",   /* 011 中 + 右（目标右前） */
    "L_ONLY",   /* 100 只看到左            */
    "LR    ",   /* 101 左右都看到（目标很宽 / 很近）*/
    "LC    ",   /* 110 左 + 中（目标左前） */
    "LCR   ",   /* 111 全覆盖（目标很近）  */
};

static const char *s_observed_name[8] = {
    "NONE", "R", "C", "CR", "L", "LR", "LC", "LCR"
};

static csi_gpio_t s_ir_gpio;

/*****************************************************************************
 * 读三路引脚原始电平 -> 位掩码（1 = 引脚为高 = 未遮挡）
 *****************************************************************************/
static uint8_t ir_read_raw(void)
{
    uint8_t raw = 0U;

    if (csi_gpio_read(&s_ir_gpio, IR_LEFT_PIN)   != 0U) { raw |= (1U << BIT_L); }
    if (csi_gpio_read(&s_ir_gpio, IR_CENTER_PIN) != 0U) { raw |= (1U << BIT_C); }
    if (csi_gpio_read(&s_ir_gpio, IR_RIGHT_PIN)  != 0U) { raw |= (1U << BIT_R); }

    return raw;
}

/*****************************************************************************
 * 打印一行状态
 *****************************************************************************/
static void ir_print_state(uint8_t raw, uint8_t seen, uint32_t hold_ms, uint8_t changed)
{
    printf("[IRST] raw=%u %u %u seen(L,C,R)=%u %u %u state=%s hold=%ums%s\r\n",
           (unsigned)((raw  >> BIT_L) & 1U),
           (unsigned)((raw  >> BIT_C) & 1U),
           (unsigned)((raw  >> BIT_R) & 1U),
           (unsigned)((seen >> BIT_L) & 1U),
           (unsigned)((seen >> BIT_C) & 1U),
           (unsigned)((seen >> BIT_R) & 1U),
           s_state_name[seen & 0x07U],
           (unsigned)hold_ms,
           (changed != 0U) ? "  <-changed" : "");
}

/*****************************************************************************
 * 打印「已经观察到哪些状态」的累积汇总（去抖后 / 瞬时，两行对比）
 *   stable  行：真正生效的状态（短暂组合会被去抖滤掉）
 *   instant 行：每个采样点看到的原始组合
 *   两行一对比就能判断：某个组合是"物理上从没出现"，还是"出现过但太短、
 *   被 60ms 去抖滤掉了" —— 不用再猜固件有没有漏写。
 *****************************************************************************/
static void ir_print_summary(uint32_t observed, uint32_t observed_inst)
{
    uint32_t i;

    printf("[IRST] observed (stable): ");
    for (i = 0U; i < 8U; i++) {
        printf(" %s=%u", s_observed_name[i], (unsigned)((observed >> i) & 1U));
    }
    printf("\r\n");

    printf("[IRST] observed (instant):");
    for (i = 0U; i < 8U; i++) {
        printf(" %s=%u", s_observed_name[i], (unsigned)((observed_inst >> i) & 1U));
    }
    printf("\r\n");
}

/*****************************************************************************
 * ir_state_test: 三路红外状态解码验证入口（被 src/main.c 调用）
 *****************************************************************************/
void ir_state_test(void)
{
    uint8_t  stable = 0U;                 /* 去抖后的 seen（1 = 看到目标） */
    uint8_t  pend[3];                     /* 待确认的 seen（[0]=L [1]=C [2]=R） */
    uint8_t  cnt[3];
    uint32_t observed = 0U;               /* 去抖后出现过的状态集合 */
    uint32_t observed_inst = 0U;          /* 瞬时（不去抖）出现过的状态集合 */
    uint32_t hold_ms = 0U;
    uint32_t tick = 0U;
    uint32_t i;

    printf("\r\n[IRST] three-channel IR state decode (no servo)  L=PB22 C=PB23 R=PB29\r\n");
    printf("[IRST] target present -> pin LOW -> seen=1 ; move hand to  L / C / R / UP / DOWN\r\n");

    /* 三个脚默认是 TDM 的 func0，必须切 GPIO 并开内部上拉 */
    csi_pin_set_mux(PORTB, IR_LEFT_PIN,   PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, IR_CENTER_PIN, PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, IR_RIGHT_PIN,  PIN_FUNC_GPIO);
    csi_pin_mode(PORTB, IR_LEFT_PIN,   GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, IR_CENTER_PIN, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, IR_RIGHT_PIN,  GPIO_MODE_PULLUP);

    if (csi_gpio_init(&s_ir_gpio, IR_GPIO_IDX) != CSI_OK) {
        printf("[IRST] gpio init fail\r\n");
        return;
    }
    csi_gpio_dir(&s_ir_gpio, IR_ALL_PIN, GPIO_DIRECTION_INPUT);

    /* 初值：直接采用上电瞬间的状态，避免开头输出一段假状态 */
    stable   = (uint8_t)((~ir_read_raw()) & 0x07U);
    pend[0]  = (uint8_t)((stable >> BIT_L) & 1U);
    pend[1]  = (uint8_t)((stable >> BIT_C) & 1U);
    pend[2]  = (uint8_t)((stable >> BIT_R) & 1U);
    cnt[0] = cnt[1] = cnt[2] = 0U;

    for (;;) {
        uint8_t raw  = ir_read_raw();
        uint8_t seen = (uint8_t)((~raw) & 0x07U);
        uint8_t changed = 0U;

        /* ---- 三路各自阈值去抖 ---- */
        for (i = 0U; i < 3U; i++) {
            uint8_t shift = (uint8_t)(2U - i);      /* i=0->L(bit2) 1->C(bit1) 2->R(bit0) */
            uint8_t v     = (uint8_t)((seen >> shift) & 1U);

            if (v == pend[i]) {
                if (cnt[i] < DEBOUNCE_N) { cnt[i]++; }
            } else {
                pend[i] = v;
                cnt[i]  = 0U;
            }

            if (cnt[i] >= DEBOUNCE_N) {
                if (((stable >> shift) & 1U) != v) {
                    changed = 1U;
                }
                if (v != 0U) { stable |=  (uint8_t)(1U << shift); }
                else         { stable &= (uint8_t)~(1U << shift); }
            }
        }

        stable &= 0x07U;
        observed      |= (1U << stable);        /* 去抖后出现过的状态 */
        observed_inst |= (1U << seen);          /* 瞬时组合也记：用来区分"真的没出现"和"被去抖滤掉了" */

        hold_ms = (changed != 0U) ? 0U : (hold_ms + SAMPLE_MS);

        /* ⚠️ tick 必须无条件递增。
         * 早先写成 (changed || (++tick % HEARTBEAT_N) == 0) 是错的：C 的 || 会短路，
         * 只要状态在变化，右边就不求值 —— 手在移动时 tick 几乎不涨，心跳和 2s 汇总
         * 行迟迟不出来，看起来像"程序卡住了"。 */
        tick++;

        /* 状态变化立即打印；无变化每 1s 打一行心跳 */
        if ((changed != 0U) || ((tick % HEARTBEAT_N) == 0U)) {
            ir_print_state(raw, stable, hold_ms, changed);
        }
        /* 每 2s 打两行累积汇总：去抖后 / 瞬时 */
        if ((tick % SUMMARY_N) == 0U) {
            ir_print_summary(observed, observed_inst);
        }

        mdelay(SAMPLE_MS);
    }
}
