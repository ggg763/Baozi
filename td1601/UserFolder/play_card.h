/******************************************************************************
 * @file     play_card.h
 * @brief    卡片层第 4 页：动作 / 情绪 / 跟随开关（"陪它玩"）
 *
 * 设计约定（用户 2026-11）：
 *   · 按钮**不直接写舵机、也不直接改情绪** ——
 *     动作按钮把 move_id 交给 behavior_request_move()，由情感状态机下一拍
 *     按正式流程播放（限位/变体池/增益/总线仲裁全部照旧）；
 *     情绪按钮把刺激交给 behavior_stimulus()（EMO_STIM_PRAISE / SCOLD），
 *     由连续状态阈值漂移出情绪；
 *     TRACK 开关交给 emotion_fsm_request_track()，同样由状态机下一拍切模式。
 *   · 用户手指在屏幕上时机器人本来就停手（behavior_inhibit），所以按下按钮
 *     的瞬间不会夹手；手指抬起后动作才开始播。
 ******************************************************************************/
#ifndef _PLAY_CARD_H_
#define _PLAY_CARD_H_

/* play_card_tap() 的返回值（决定调用方要不要关掉卡片层）*/
#define PLAY_TAP_NONE   0   /* 没点到按钮 */
#define PLAY_TAP_CLOSE  1   /* 点到动作/情绪按钮 —— 反馈在机器人身上，回表情态 */
#define PLAY_TAP_KEEP   2   /* 点到 TRACK 开关 —— 留在本页，原地重绘 ON/OFF */

/* 画第 4 页内容。必须在 ui_clear() 之后、刷屏之前调用，
 * 卡片外框由 ui_app.c 画好（60,122,360,290）—— 本函数只填内部。 */
void play_card_draw(void);

/* 点击命中：返回 PLAY_TAP_xxx */
int  play_card_tap(int x, int y);

#endif /* _PLAY_CARD_H_ */
