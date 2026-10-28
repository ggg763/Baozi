/******************************************************************************
 * @file     track_yaw.h
 * @brief    红外 YAW 单轴跟随 —— 引脚与可调参数（跑通闭环后再加 PITCH）
 *
 * 目标：让目标保持在探头正前方（视角中央），只驱动水平舵机 YAW。
 *       PITCH 现在不动（servo_core_init 会把两路都回中位）。
 *
 * 布局与方位约定（详见 UserFolder/doc/ir_tracking_plan.md）：
 *   IR L = PB22（屏幕左下）  C = PB23（屏幕上方中点）  R = PB29（屏幕右下）
 *   模块输出：未遮挡 = 1（高），遮挡/有目标 = 0（低）
 *   -> 代码里 seen = 看到目标 = !raw
 *
 * YAW 单轴只用**水平**信息：
 *   只看到 L         -> 目标在左 -> 脉宽【减小】（本项目约定：脉宽增大=右转）
 *   只看到 R         -> 目标在右 -> 脉宽【增大】
 *   只看到 C（居中）  -> 死区，不动
 *   L 与 R 同时看到   -> 目标跨中/很近，水平方向无法判断 -> 不动
 ******************************************************************************/
#ifndef _TRACK_YAW_H_
#define _TRACK_YAW_H_

/* ---- 三路红外输入（PORTB，位掩码） ---- */
#define TRK_IR_LEFT_PIN     PIN22
#define TRK_IR_CENTER_PIN   PIN23
#define TRK_IR_RIGHT_PIN    PIN29
#define TRK_IR_ALL_PIN      (TRK_IR_LEFT_PIN | TRK_IR_CENTER_PIN | TRK_IR_RIGHT_PIN)
#define TRK_IR_GPIO_IDX     1U      /* PORTB 的 GPIO 控制器 idx */

/* seen 位序：bit2 = L，bit1 = C，bit0 = R（1 = 看到目标） */
#define TRK_SEEN_L(seen)    (((seen) >> 2) & 1U)
#define TRK_SEEN_C(seen)    (((seen) >> 1) & 1U)
#define TRK_SEEN_R(seen)    (((seen) >> 0) & 1U)

/* ---- 采样与去抖 ---- */
#define TRK_SAMPLE_MS       5U      /* IR 采样周期（5ms，抓短脉冲） */
#define TRK_DEBOUNCE_N      12U     /* 连续 N 次一致才认可（12 × 5ms = 60ms） */

/* ---- 控制节拍与步长 ---- */
#define TRK_CTRL_MS         100U    /* 控制周期：每 100ms 最多走一步（舵机响应不了更快） */
#define TRK_STEP_US         15U     /* 每步脉宽变化（us）—— 大了会冲过头，小了追不上 */
#define TRK_CENTER_STEP_US  5U      /* 丢失后回中位的步长（更慢更柔和） */

/* ---- 目标丢失分档（ms） ---- */
#define TRK_LOST_HOLD_MS    200U    /* < 200ms：视为区域过渡，保持不动 */
#define TRK_LOST_SEARCH_MS  1000U   /* 200ms ~ 1s：保持最后方向慢速搜索；> 1s：回中位 */

/* ---- 调试打印 ---- */
#define TRK_PRINT_MS        1000U   /* 状态无变化时每 1s 打一行心跳 */

/* 入口：被 src/main.c 调用（不返回） */
void track_yaw_test(void);

#endif /* _TRACK_YAW_H_ */
