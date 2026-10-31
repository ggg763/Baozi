/******************************************************************************
 * @file     behavior.c
 * @brief    UI ↔ 行为层的桥（见 behavior.h）
 *
 * 【红外近距·已按用户要求关闭】三路 IR 沿用已定案引脚（PB22 左 / PB23 中 /
 * PB29 右，PORTB，上拉输入）。极性：**高 = 未遮挡，低 = 看到反射**（与
 * track_dual.c 的 td_read_raw 一致）—— 这是"避障模块"的数字输出，只有阈值没有
 * 距离，所以原先把它当"有没有东西贴近"用来抑制舵机。
 *
 * ★ 2026-11 用户要求**去掉这条距离保护**（BEHAVIOR_IR_INHIBIT 默认 0）：
 *   实机上桌沿/机身/凑近的手都会让某一路读到反射，于是 inhibit 常开 →
 *   舵机一步不动、动作一播就被掐（表现就是"不怎么动 / PLAY 页按了没反应"）。
 *   现在 `behavior_inhibit()` 只认"手指按在屏幕上"这一条（防夹手，硬约束）。
 *   需要恢复老行为：编译时加 -DBEHAVIOR_IR_INHIBIT=1（三路 IR 仍在初始化，
 *   `behavior_ir_near()` 也仍可用，只是不再参与停手判断）。
 *
 * ⚠️ 若把近距抑制开回来，跟随模式（TRACK）下必须豁免：
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

/* 红外近距抑制总开关。★ 默认【关】（2026-11 用户要求去掉距离保护），
 * 理由见文件头：避障模块只有阈值没有距离，桌面场景经常误判"贴太近"。
 * 关掉之后"手指按屏幕停手"这一条仍然有效（那条是防夹手的硬约束）。
 * 想恢复老行为：在 Baozi.mk 的 CFLAGS 里加 -DBEHAVIOR_IR_INHIBIT=1，或改这里。*/
#ifndef BEHAVIOR_IR_INHIBIT
#define BEHAVIOR_IR_INHIBIT   0
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

    /* 上电自检：把三路原始电平打出来。极性是"高 = 未遮挡"，所以正常情况
     * 应该看到 `IR=L1 C1 R1`（三路都高 = 前面什么都没有）。如果上电就是
     * 0（低 = 看到反射），说明探头没接好、或者一直挡着 —— 距离保护虽然已经
     * 关掉，红外跟随（TRACK）还要靠这三路，所以这一眼还是值得看。 */
    {
        int l = (csi_gpio_read(&s_ir, IR_LEFT_PIN)   != 0U) ? 1 : 0;
        int c = (csi_gpio_read(&s_ir, IR_CENTER_PIN) != 0U) ? 1 : 0;
        int r = (csi_gpio_read(&s_ir, IR_RIGHT_PIN)  != 0U) ? 1 : 0;

        printf("[bhv] ir level L%d C%d R%d (1=未遮挡, 0=看到反射)\r\n", l, c, r);
    }

    /* 把自己挂到情感状态机上：FSM 每拍/每个 20ms 播放钩子都会问一次 */
    emotion_fsm_set_inhibit_cb(behavior_inhibit);

    printf("[bhv] ir proximity ready (PB22/PB23/PB29), inhibit=%s\r\n",
           (BEHAVIOR_IR_INHIBIT != 0) ? "touch+near" : "touch only (near-hold disabled)");
}

int behavior_ir_near(void)
{
    int any;
    int l, c, r;

    if (!s_ir_inited) {
        return 0;
    }

    /* ★ 极性（2026-10-31 修正）：三路探头都是【高 = 未遮挡、低 = 看到反射】。
     *   依据 track_dual.c 的 td_read_raw()——那里的注释写得很清楚
     *   "读三路原始电平 -> 位掩码（1 = 引脚为高 = 未遮挡）"，而且跟随模式是
     *   上板验证过的。
     *   本函数原来写的是"读到 1 = 检测到反射"，正好反了：前面什么都没有的时候
     *   三路都是高，于是被当成"贴太近"→ behavior_inhibit() 恒为 1 →
     *   emotion_fsm_step() 每拍都在抑制 gate 早返回 → 舵机上电后只停在初始脉宽、
     *   一步都不走，UI 的动作请求也永远排不到队。表现就是
     *   "舵机不怎么动 / PLAY 页按了没反应"。 */
    l = (csi_gpio_read(&s_ir, IR_LEFT_PIN)   != 0U) ? 1 : 0;
    c = (csi_gpio_read(&s_ir, IR_CENTER_PIN) != 0U) ? 1 : 0;
    r = (csi_gpio_read(&s_ir, IR_RIGHT_PIN)  != 0U) ? 1 : 0;

    any = (!l || !c || !r) ? 1 : 0;      /* 只要有一路为低 = 那一路看到东西 */

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
    /* ① 手指按在屏幕上：无条件停手（这条最硬，防夹手）。
     *    ★ 现在这是【唯一】依据 —— 红外"贴太近"那条距离保护已按用户要求关掉。 */
    if (app_core_touch_down()) {
        return 1;
    }

    /* ② 红外贴太近（默认关；开回来时跟随模式下仍豁免，见文件头）*/
#if BEHAVIOR_IR_INHIBIT == 0
    /* 距离保护关闭：只保留"手指按在屏上"这一条 */
    (void)0;
#elif BEHAVIOR_NEAR_INHIBIT_IN_TRACK
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

void behavior_note_interaction(void)
{
    /* 触摸屏上的任何操作都算"人还在跟我玩"：重置心情计时（睡着则唤醒）。
     * 注意只动心情，不动情绪 —— 摸一下不该改变喜怒哀乐。 */
    emotion_fsm_note_interaction();
}
