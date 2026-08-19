/**
 * behavior.h - TD1601 行为规划层（Behavior Layer）
 *
 * 职责：
 *   接收 Emotion FSM 的情绪变化，
 *   决策"当前情绪该做什么"——表情、动作、灯光，
 *   并调度 Motion / Screen / LED 三个执行模块。
 *
 * 关键原则（架构调整 v1.0）：
 *   动作由 TD1601 自主决策，PC 端绝不发动作指令。
 */

#ifndef BEHAVIOR_H
#define BEHAVIOR_H

#include "robot_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化行为层（注册为 Emotion FSM 的回调）
 */
void Behavior_Init(void);

/**
 * 情绪变化回调（由 Emotion_Init 传入 FSM）
 */
void Behavior_OnEmotionChanged(EmotionState new_state);

/**
 * 周期驱动（可选，用于动作的插值推进）
 * @param now_ms 当前系统时刻（毫秒）
 */
void Behavior_Tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BEHAVIOR_H */
