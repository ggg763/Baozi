/******************************************************************************
 * @file     behavior.c
 * @brief    UI ↔ 行为层的桥（见 behavior.h）
 *
 * 【红外近距】三路 IR 沿用已定案引脚（PB22 左 / PB23 中 / PB29 右，PORTB，
 * 上拉输入，读到 1 = 有反射）。这是"避障模块"的数字输出，只有阈值没有距离，
 * 所以这里只把它当"有没有东西贴近"用，不做距离量化。
 *
 * ⚠️ 跟随模式（TRACK）下必须关掉近距抑制：
 *    跟随的语义就是"看到目标 → 转过去"，若把"看到"当"贴太近就停手"，
 *    跟随会永远动不了。需要强行打开时改 BEHAVIOR_NEAR_INHIBIT_IN_TRACK = 1。
 *
 * 【去抖】IR 每拍采一次（主循环 ~5~10ms 一轮），连续 N 次为近才算近，
 * 避免手一晃就停、一走就动地抽搐。
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <drv/gpio.h>
#include <drv/pin.h>

#include "behavior.h"
#include "app_core.h"                        /* app_core_touch_down */
#include "emotion/emotion_fsm.h"             /* set_inhibit_cb / request_action / stimulus */
#include "emotion/emotion_manager.h"         /* EMO_STIM_* */
#include "movements/moves.h"                 /* move_id_t / MOVE_MAX */
#include "anim/face_ui.h"                    /* face_ui_reaction */

/* ---- 三路红外（与你已定案的 IR 引脚一致，见 UserFolder/doc/pin_mux_table.md）---- */
#define IR_GPIO_IDX      1                   /* PORTB 的 GPIO 控制器 idx = 1 */
#define IR_LEFT_PIN      PIN22
#define IR_CENTER_PIN    PIN23
#define IR_RIGHT_PIN     PIN29
#define IR_ALL_PIN       (IR_LEFT_PIN | IR_CENTER_PIN | IR_RIGHT_PIN)

/* 跟随模式下是否也按近距抑制（默认否，理由见文件头）*/
#ifndef BEHAVIOR_NEAR_INHIBIT_IN_TRACK
#define BEHAVIOR_NEAR_INHIBIT_IN_TRACK   0
#endif

#define IR_NEAR_DEBOUNCE 3                   /* 连续 3 拍为近才算"贴太近" */

static csi_gpio_t s_ir;
static uint8_t    s_ir_inited = 0;
static uint8_t    s_near_cnt  = 0;
static uint8_t    s_near      = 0;

void behavior_init(void)
{
    csi_pin_set_mux(PORTB, IR_LEFT_PIN,   PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, IR_CENTER_PIN, PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, IR_RIGHT_PIN,  PIN_FUNC_GPIO);

    csi_pin_mode(PORTB, IR_LEFT_PIN,   GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, IR_CENTER_PIN, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, IR_RIGHT_PIN,  GPIO_MODE_PULLUP);

    csi_gpio_init(&s_ir, IR_GPIO_IDX);
    csi_gpio_dir(&s_ir, IR_ALL_PIN, GPIO_DIRECTION_INPUT);

    s_ir_inited = 1;
    s_near_cnt  = 0;
    s_near      = 0;

    /* 把自己挂到情感状态机上：FSM 每拍/每个 20ms 播放钩子都会问一次 */
    emotion_fsm_set_inhibit_cb(behavior_inhibit);

    printf("[bhv] ir proximity ready (PB22/PB23/PB29)\r\n");
}

int behavior_ir_near(void)
{
    int any;

    if (!s_ir_inited) {
        return 0;
    }
    /* 读到 1 = 该路探头检测到反射（项目里三路都是这么用的，见 track_dual.c）*/
    any = (csi_gpio_read(&s_ir, IR_ALL_PIN) != 0U) ? 1 : 0;

    if (any) {
        if (s_near_cnt < 255) {
            s_near_cnt++;
        }
        if (s_near_cnt >= IR_NEAR_DEBOUNCE) {
            s_near = 1;
        }
    } else {
        s_near_cnt = 0;
        s_near     = 0;
    }
    return (int)s_near;
}

int behavior_inhibit(void)
{
    /* ① 手指按在屏幕上：无条件停手（这条最硬，防夹手）*/
    if (app_core_touch_down()) {
        return 1;
    }

    /* ② 红外贴太近：跟随模式下不生效（见文件头）*/
#if BEHAVIOR_NEAR_INHIBIT_IN_TRACK
    if (behavior_ir_near()) {
        return 1;
    }
#else
    if ((emotion_fsm_mode() != 1U) && behavior_ir_near()) {
        return 1;
    }
#endif
    return 0;
}

void behavior_stimulus(uint8_t kind, uint8_t amount)
{
    if (kind > EMO_STIM_PLAY) {
        return;
    }
    if (amount == 0) {
        amount = 50;                        /* 没写强度：给个中等力度 */
    }
    emotion_fsm_stimulus(kind, amount);      /* 先把连续状态改掉（可能漂移情绪）*/
    face_ui_reaction(kind);                  /* 再盖一个 2 秒的瞬时表情 */
}

void behavior_request_move(uint8_t move_id)
{
    if ((int)move_id >= (int)MOVE_MAX) {
        return;
    }
    emotion_fsm_request_action(move_id);
}
