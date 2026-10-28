/******************************************************************************
 * @file     behavior.h
 * @brief    UI ↔ 行为层的桥：安全抑制、红外近距、动作请求、情绪刺激
 *
 * 三条职责（都来自用户 2026-11 的明确需求）：
 *
 *  ① 安全抑制（behavior_inhibit）
 *     用户手指按在屏幕上时机器人必须停手（防误触/夹手）；另外复用三路红外
 *     做"贴太近"检测，人/物靠得极近时也停手。该函数注册给 emotion_fsm：
 *         emotion_fsm_set_inhibit_cb(behavior_inhibit);
 *     返回 1 → FSM 立刻 moves_interrupt() 且本拍不启动新动作。
 *
 *  ② 情绪刺激（behavior_stimulus）
 *     UI 的抚摸/夸奖/责备按钮、以及屏幕上的"抚摸手势"，都不直接改情绪，
 *     只作为刺激交给 emotion_manager 的连续状态，由阈值漂移出情绪。
 *
 *  ③ 动作请求（behavior_request_move）
 *     UI 按钮不自己写舵机，把动作 ID 交给 emotion_fsm 在下一拍按正式流程播放。
 ******************************************************************************/
#ifndef _BEHAVIOR_H_
#define _BEHAVIOR_H_

#include <stdint.h>

/* 初始化：三路红外引脚 + 把 behavior_inhibit 注册给情感状态机。
 * 必须在 emotion_fsm_init() 之后调用（FSM 内部状态要先就绪）。*/
void behavior_init(void);

/* 1 = 现在必须停手（手指在屏上，或红外测到贴得极近）*/
int  behavior_inhibit(void);

/* 1 = 三路红外里至少一路检测到反射（已去抖）。
 * 注意：跟随模式（TRACK）下这条路本来就是"看到目标"，所以那时
 * 不作为抑制依据（见 behavior.c 的 BEHAVIOR_NEAR_INHIBIT_IN_TRACK）。*/
int  behavior_ir_near(void);

/* 情绪刺激：kind 见 emotion_manager.h 的 EMO_STIM_*，amount = 1..100 */
void behavior_stimulus(uint8_t kind, uint8_t amount);

/* 请求脖子播一个动作：move_id 见 movements/moves.h 的 move_id_t */
void behavior_request_move(uint8_t move_id);

#endif /* _BEHAVIOR_H_ */
