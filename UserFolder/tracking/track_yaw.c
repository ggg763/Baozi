/******************************************************************************
 * @file     track_yaw.c
 * @brief    红外 YAW 单轴跟随（闭环）—— 只驱动水平舵机
 *
 * 控制思路（为什么不用 PID）：
 *   IR 是**二值**传感器（有/无目标），没有距离、没有误差幅值 —— 做不出 PID 的
 *   连续误差项。所以用**步进爬坡**：每个控制周期朝误差方向走固定一小步，
 *   目标居中（死区）就停。配合去抖、限速、软限位，就是稳定可用的跟随。
 *
 * 节拍：
 *   IR 采样 5ms（去抖 12 次 = 60ms）→ 控制 100ms 一步 → 打印按变化/心跳
 *
 * 上电表现（正常的话）：
 *   - 上电先回中位，串口打一行 stat
 *   - 手放在探头【左侧】-> YAW 向左转（yaw 值减小），直到目标进入中央视区停下
 *   - 手放在【右侧】-> 向右转（yaw 值增大）
 *   - 手放在【正前中央】-> 不动（死区，这是"锁定"状态）
 *   - 移开手 -> 200ms 内保持不动；1s 内按最后方向慢速找；超过 1s 缓慢回中位
 *
 * 调参：见 track_yaw.h 的 TRK_* 宏
 *   - 追不上 / 反应太慢 -> 增大 TRK_STEP_US，或减小 TRK_CTRL_MS
 *   - 左右抖 / 冲过头     -> 减小 TRK_STEP_US，或增大 TRK_DEBOUNCE_N
 *   - 目标在中央还老动    -> 说明去抖不够（视野边界抖动），增大 TRK_DEBOUNCE_N
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <soc.h>
#include <drv/gpio.h>
#include <drv/pin.h>
#include <drv/common.h>             /* mdelay */
#include "board_config.h"
#include "servo_test/servo_core.h"  /* servo_core_init / servo_set_pulse / 中位限位 */
#include "track_yaw.h"

/* 方位判定结果 */
typedef enum {
    TRK_DIR_NONE  = 0,  /* 无目标 */
    TRK_DIR_LEFT,       /* 目标在左 -> 脉宽减小 */
    TRK_DIR_RIGHT,      /* 目标在右 -> 脉宽增大 */
    TRK_DIR_STOP        /* 居中 / 跨中 / 全覆盖 -> 不动 */
} trk_dir_t;

static const char *s_dir_name[4] = {
    "NONE ", "LEFT ", "RIGHT", "STOP "
};

static csi_gpio_t s_ir_gpio;

/*****************************************************************************
 * 读三路原始电平 -> 位掩码（1 = 引脚为高 = 未遮挡）
 *****************************************************************************/
static uint8_t ir_read_raw(void)
{
    uint8_t raw = 0U;

    if (csi_gpio_read(&s_ir_gpio, TRK_IR_LEFT_PIN)   != 0U) { raw |= 0x04U; }
    if (csi_gpio_read(&s_ir_gpio, TRK_IR_CENTER_PIN) != 0U) { raw |= 0x02U; }
    if (csi_gpio_read(&s_ir_gpio, TRK_IR_RIGHT_PIN)  != 0U) { raw |= 0x01U; }

    return raw;
}

/*****************************************************************************
 * 方位判定（YAW 单轴：只用 L / R 的水平信息）
 *
 *   seen = 1 1 0 (L+C)  -> 左
 *   seen = 1 0 0 (L)    -> 左
 *   seen = 0 1 1 (C+R)  -> 右
 *   seen = 0 0 1 (R)    -> 右
 *   seen = 0 1 0 (C)    -> 居中，不动（死区）
 *   seen = 1 0 1 (L+R)  -> 跨中/很近，水平方向无法判断，不动
 *   seen = 1 1 1 (全)   -> 很近，不动
 *   seen = 0 0 0        -> 无目标
 *****************************************************************************/
static trk_dir_t trk_decide(uint8_t seen)
{
    uint8_t l = (uint8_t)TRK_SEEN_L(seen);
    uint8_t r = (uint8_t)TRK_SEEN_R(seen);

    if (seen == 0U) {
        return TRK_DIR_NONE;
    }
    if ((l != 0U) && (r == 0U)) {
        return TRK_DIR_LEFT;
    }
    if ((r != 0U) && (l == 0U)) {
        return TRK_DIR_RIGHT;
    }
    return TRK_DIR_STOP;
}

static int32_t trk_clamp(int32_t v)
{
    if (v < (int32_t)SERVO_YAW_MIN) { v = (int32_t)SERVO_YAW_MIN; }
    if (v > (int32_t)SERVO_YAW_MAX) { v = (int32_t)SERVO_YAW_MAX; }
    return v;
}

/*****************************************************************************
 * 一个控制周期的步进
 *****************************************************************************/
static void trk_step_one(int32_t *yaw, trk_dir_t *last_dir, uint32_t *lost_ms, uint8_t seen)
{
    trk_dir_t dir = trk_decide(seen);
    int32_t target = *yaw;

    switch (dir) {
        case TRK_DIR_LEFT:
            target   = *yaw - (int32_t)TRK_STEP_US;   /* 脉宽减小 = 左转 */
            *last_dir = TRK_DIR_LEFT;
            *lost_ms  = 0U;
            break;

        case TRK_DIR_RIGHT:
            target   = *yaw + (int32_t)TRK_STEP_US;   /* 脉宽增大 = 右转 */
            *last_dir = TRK_DIR_RIGHT;
            *lost_ms  = 0U;
            break;

        case TRK_DIR_STOP:
            target   = *yaw;                          /* 死区：锁定不动 */
            *last_dir = TRK_DIR_STOP;
            *lost_ms  = 0U;
            break;

        case TRK_DIR_NONE:
        default:
            *lost_ms += TRK_CTRL_MS;
            if (*lost_ms < TRK_LOST_HOLD_MS) {
                target = *yaw;                        /* 区域过渡中：保持 */
            } else if (*lost_ms < TRK_LOST_SEARCH_MS) {
                /* 保持最后方向慢速搜索（步长减半） */
                if (*last_dir == TRK_DIR_LEFT) {
                    target = *yaw - (int32_t)(TRK_STEP_US / 2U);
                } else if (*last_dir == TRK_DIR_RIGHT) {
                    target = *yaw + (int32_t)(TRK_STEP_US / 2U);
                } else {
                    target = *yaw;
                }
            } else {
                /* 丢失过久：缓慢回中位
                 * ⚠️ 必须"吸附"：若 yaw 与中位只差不到一个步长（例如 1001 与 1000），
                 *    直接 ±CENTER_STEP 会跨过中位（1001-5=996），下一拍又反向跨回来
                 *    （996+5=1001）—— 于是每 100ms 在中位两侧来回跳，舵机持续微抖。 */
                if (*yaw > (int32_t)SERVO_YAW_CENTER) {
                    target = *yaw - (int32_t)TRK_CENTER_STEP_US;
                    if (target < (int32_t)SERVO_YAW_CENTER) {
                        target = (int32_t)SERVO_YAW_CENTER;
                    }
                } else if (*yaw < (int32_t)SERVO_YAW_CENTER) {
                    target = *yaw + (int32_t)TRK_CENTER_STEP_US;
                    if (target > (int32_t)SERVO_YAW_CENTER) {
                        target = (int32_t)SERVO_YAW_CENTER;
                    }
                } else {
                    target = *yaw;
                }
            }
            break;
    }

    *yaw = trk_clamp(target);
    (void)servo_set_pulse(SERVO_YAW_CH, (uint32_t)(*yaw));
}

/*****************************************************************************
 * track_yaw_test: YAW 单轴跟随入口（被 src/main.c 调用，不返回）
 *****************************************************************************/
void track_yaw_test(void)
{
    uint8_t  stable = 0U;                       /* 去抖后的 seen */
    uint8_t  pend[3];
    uint8_t  cnt[3];
    uint32_t lost_ms   = 0U;
    uint32_t ctrl_acc  = 0U;
    uint32_t print_acc = 0U;
    uint32_t i;
    int32_t  yaw = (int32_t)SERVO_YAW_CENTER;
    trk_dir_t last_dir = TRK_DIR_STOP;
    trk_dir_t dir_now  = TRK_DIR_STOP;
    uint8_t   last_seen = 0xFFU;

    printf("\r\n[TRK] YAW single-axis tracking  L=PB22 C=PB23 R=PB29\r\n");
    printf("[TRK] step=%uus ctrl=%ums debounce=%u x %ums\r\n",
           (unsigned)TRK_STEP_US, (unsigned)TRK_CTRL_MS,
           (unsigned)TRK_DEBOUNCE_N, (unsigned)TRK_SAMPLE_MS);

    /* ---- 三路 IR：切 GPIO + 内部上拉 ---- */
    csi_pin_set_mux(PORTB, TRK_IR_LEFT_PIN,   PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, TRK_IR_CENTER_PIN, PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, TRK_IR_RIGHT_PIN,  PIN_FUNC_GPIO);
    csi_pin_mode(PORTB, TRK_IR_LEFT_PIN,   GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, TRK_IR_CENTER_PIN, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, TRK_IR_RIGHT_PIN,  GPIO_MODE_PULLUP);

    if (csi_gpio_init(&s_ir_gpio, (uint32_t)TRK_IR_GPIO_IDX) != CSI_OK) {
        printf("[TRK] gpio init fail\r\n");
        return;
    }
    csi_gpio_dir(&s_ir_gpio, TRK_IR_ALL_PIN, GPIO_DIRECTION_INPUT);

    /* ---- 舵机：引脚复用 + PWM 初始化 + 两路回中位 ---- */
    if (servo_core_init() != 0) {
        printf("[TRK] servo init fail\r\n");
        return;
    }

    /* 初值：采用上电瞬间的状态，避免开头输出一段假状态 */
    stable = (uint8_t)((~ir_read_raw()) & 0x07U);
    pend[0] = (uint8_t)TRK_SEEN_L(stable);
    pend[1] = (uint8_t)TRK_SEEN_C(stable);
    pend[2] = (uint8_t)TRK_SEEN_R(stable);
    cnt[0] = cnt[1] = cnt[2] = 0U;

    printf("[TRK] stat: yaw=%dus center=%uus range=[%u,%u] ir_seen=%u%u%u\r\n",
           (int)yaw, (unsigned)SERVO_YAW_CENTER,
           (unsigned)SERVO_YAW_MIN, (unsigned)SERVO_YAW_MAX,
           (unsigned)TRK_SEEN_L(stable), (unsigned)TRK_SEEN_C(stable), (unsigned)TRK_SEEN_R(stable));

    for (;;) {
        uint8_t raw  = ir_read_raw();
        uint8_t seen = (uint8_t)((~raw) & 0x07U);

        /* ---- 三路各自阈值去抖 ---- */
        for (i = 0U; i < 3U; i++) {
            uint8_t shift = (uint8_t)(2U - i);      /* 0->L(bit2) 1->C(bit1) 2->R(bit0) */
            uint8_t v     = (uint8_t)((seen >> shift) & 1U);

            if (v == pend[i]) {
                if (cnt[i] < TRK_DEBOUNCE_N) { cnt[i]++; }
            } else {
                pend[i] = v;
                cnt[i]  = 0U;
            }

            if (cnt[i] >= TRK_DEBOUNCE_N) {
                if (v != 0U) { stable |=  (uint8_t)(1U << shift); }
                else         { stable &= (uint8_t)~(1U << shift); }
            }
        }
        stable &= 0x07U;

        /* ---- 每 TRK_CTRL_MS 走一步 ---- */
        ctrl_acc += TRK_SAMPLE_MS;
        if (ctrl_acc >= TRK_CTRL_MS) {
            ctrl_acc = 0U;
            dir_now  = trk_decide(stable);
            trk_step_one(&yaw, &last_dir, &lost_ms, stable);
        }

        /* ---- 打印：状态变化立即打，否则心跳 / 丢失期间每秒打 ---- */
        print_acc += TRK_SAMPLE_MS;
        if ((stable != last_seen) || (print_acc >= TRK_PRINT_MS)) {
            printf("[TRK] seen=%u%u%u dir=%s yaw=%dus lost=%ums%s\r\n",
                   (unsigned)TRK_SEEN_L(stable),
                   (unsigned)TRK_SEEN_C(stable),
                   (unsigned)TRK_SEEN_R(stable),
                   s_dir_name[(uint8_t)dir_now & 0x03U],
                   (int)yaw,
                   (unsigned)lost_ms,
                   (stable != last_seen) ? "  <-changed" : "");
            last_seen = stable;
            print_acc = 0U;
        }

        mdelay(TRK_SAMPLE_MS);
    }
}
