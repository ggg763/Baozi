/******************************************************************************
 * @file     behavior.h
 * @brief    UI ↔ 行为层的桥：安全抑制、红外近距、动作请求、情绪刺激
 *
 * 四条职责（都来自用户 2026-11 的明确需求）：
 *
 *  ① 安全抑制（behavior_inhibit）
 *     用户手指按在屏幕上时机器人必须停手（防误触/夹手）。该函数注册给 emotion_fsm：
 *         emotion_fsm_set_inhibit_cb(behavior_inhibit);
 *     返回 1 → FSM 立刻 moves_interrupt() 且本拍不启动新动作。
 *     ★ 2026-11：原来的"红外贴太近也停手"这条**距离保护已按用户要求关闭**
 *       （BEHAVIOR_IR_INHIBIT 默认 0，见 behavior.c 文件头），现在只认触摸。
 *
 *  ② 情绪刺激（behavior_stimulus）
 *     UI 的抚摸/夸奖/责备按钮、以及屏幕上的"抚摸手势"，都不直接改情绪，
 *     只作为刺激交给 emotion_manager 的连续状态，由阈值漂移出情绪。
 *
 *  ③ 动作请求（behavior_request_move）
 *     UI 按钮不自己写舵机，把动作 ID 交给 emotion_fsm 在下一拍按正式流程播放。
 *
 *  ④ 交互上报（behavior_note_interaction）
 *     触摸屏上的任何操作（点击/滑动/按住/按卡片按钮/脸颊滑动/额头连点）
 *     都算一次"人和它互动了"，用来刷新心情层的 3 分钟睡眠计时。
 *     不这么做的话，用户在屏幕上玩，心情层却以为"没人理我" → 到点就睡。
 ******************************************************************************/
#ifndef _BEHAVIOR_H_
#define _BEHAVIOR_H_

#include <stdint.h>

/* 初始化：三路红外引脚 + 把 behavior_inhibit 注册给情感状态机。
 * 必须在 emotion_fsm_init() 之后调用（FSM 内部状态要先就绪）。*/
void behavior_init(void);

/* 1 = 现在必须停手（★ 只看"手指按在屏幕上"；红外近距抑制默认已关）*/
int  behavior_inhibit(void);

/* 1 = 三路红外里至少一路检测到反射（已去抖）。
 * ★ 距离保护关闭后本函数不再参与停手判断，保留给跟随/调试用；
 *   要恢复老行为见 behavior.c 的 BEHAVIOR_IR_INHIBIT（=1）。*/
int  behavior_ir_near(void);

/* 情绪刺激：kind 见 emotion_manager.h 的 EMO_STIM_*，amount = 1..100 */
void behavior_stimulus(uint8_t kind, uint8_t amount);

/* 请求脖子播一个动作：move_id 见 movements/moves.h 的 move_id_t */
void behavior_request_move(uint8_t move_id);

/* 上报一次人类交互（触摸屏上任何操作）。刷新"3 分钟没互动才睡"的计时；
 * 睡着时被摸会把机器人叫醒。由 app_core_step() 的触摸分支调用。 */
void behavior_note_interaction(void);

#endif /* _BEHAVIOR_H_ */
