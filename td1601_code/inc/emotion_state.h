/**
 * emotion_state.h - TD1601 情绪状态机（Emotion FSM）
 *
 * 职责：
 *   - 维护当前情绪状态（EMO_IDLE/HAPPY/SAD/...）
 *   - 接收外部输入：PC 下发（UART）、本地传感器（触摸）
 *   - 空闲超时自动降级：IDLE -> BORED -> SLEEPING
 *   - 情绪变化时通过回调通知 Behavior 层
 *
 * 设计说明：
 *   这是从 PC 端 Python 状态机迁移过来的核心行为逻辑。
 *   空闲降级的时间阈值放在 TD1601 本地，
 *   即使 PC 断开，TD1601 也能自主表现出"无聊/睡觉"。
 */

#ifndef EMOTION_STATE_H
#define EMOTION_STATE_H

#include "robot_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 空闲降级阈值（毫秒） */
#define EMO_IDLE_TIMEOUT_MS    (20000U)  /**< 20 秒无互动 -> BORED */
#define EMO_BORED_TIMEOUT_MS   (10000U)  /**< 无聊再 10 秒无互动 -> SLEEPING */

/* 情绪变化回调：当情绪状态改变时由 FSM 调用 */
typedef void (*EmotionChangeCb)(EmotionState new_state);

/**
 * 初始化情绪状态机（置为 EMO_IDLE，重置计时）
 */
void Emotion_Init(EmotionChangeCb on_change);

/**
 * 更新情绪（外部输入：PC 下发 / 传感器触发）
 * @param emo 新情绪
 * @return 若情绪发生变化返回 true
 */
bool Emotion_Update(EmotionState emo);

/**
 * 获取当前情绪
 */
EmotionState Emotion_Get(void);

/**
 * 记录一次互动（重置空闲计时）
 * 主循环在"收到 PC 消息 / 传感器事件"时调用
 * @param now_ms 当前系统时刻（毫秒）；0 表示"互动已发生但无时间基准"，由下次 Tick 重建
 */
void Emotion_Touch(uint32_t now_ms);

/**
 * 心跳节拍（周期性调用，建议 10ms~100ms 一次）
 * 内部根据空闲时长自动降级 IDLE->BORED->SLEEPING
 */
void Emotion_Tick(uint32_t now_ms);

/**
 * 手动唤醒（如触摸唤醒）
 */
void Emotion_WakeUp(void);

#ifdef __cplusplus
}
#endif

#endif /* EMOTION_STATE_H */
