/**
 * emotion_state.c - TD1601 情绪状态机实现
 *
 * 迁移自 PC 端 Python MoodStateMachine：
 *   原 IDLE/ACTIVE/BORED/SLEEPING 的"空闲降级"逻辑在此等价实现，
 *   时间源改为 TD1601 的系统节拍（now_ms 由 main 循环提供）。
 *
 * 时序约定：
 *   - main 启动后调用一次 Emotion_Touch(now_ms) 建立待机基准；
 *   - 每次收到 PC 消息 / 传感器事件 → Emotion_Touch(now_ms)；
 *   - main 主循环周期调用 Emotion_Tick(now_ms) 驱动降级检查。
 */

#include "emotion_state.h"

/* ---------- 内部状态 ---------- */
static EmotionState s_emotion = EMO_IDLE;
static EmotionChangeCb s_cb = 0;
static uint32_t s_last_interaction_ms = 0U;  /**< 最近一次互动时刻（ms） */
static bool s_has_baseline = false;          /**< 空闲计时基准是否已建立 */

/* ---------- 内部工具 ---------- */
static void notify(EmotionState new_state)
{
    if (s_emotion != new_state) {
        s_emotion = new_state;
        if (s_cb != 0) {
            s_cb(new_state);
        }
    }
}

/* ---------- 对外接口 ---------- */

void Emotion_Init(EmotionChangeCb on_change)
{
    s_emotion = EMO_IDLE;
    s_cb = on_change;
    s_last_interaction_ms = 0U;
    s_has_baseline = false;
}

bool Emotion_Update(EmotionState emo)
{
    if (emo >= EMO_COUNT) {
        return false;   /* 非法情绪 */
    }
    Emotion_Touch(0U);  /* 基准失效：下次 Tick 重建，等价于"刚互动过" */
    notify(emo);
    return true;
}

EmotionState Emotion_Get(void)
{
    return s_emotion;
}

void Emotion_Touch(uint32_t now_ms)
{
    if (now_ms == 0U) {
        /* 0 = 互动已发生，但无时间基准：标记失效，由下次 Tick 重建 */
        s_has_baseline = false;
    } else {
        s_last_interaction_ms = now_ms;
        s_has_baseline = true;
    }
}

void Emotion_Tick(uint32_t now_ms)
{
    /* 睡眠中不再自动降级，等待互动/唤醒 */
    if (s_emotion == EMO_SLEEPING) {
        return;
    }

    /* 若尚无互动基准（刚 Update/Touch(0)），以当前时刻建立，避免误判超时 */
    if (!s_has_baseline) {
        s_last_interaction_ms = now_ms;
        s_has_baseline = true;
        return;
    }

    uint32_t idle = now_ms - s_last_interaction_ms;

    if (idle >= EMO_BORED_TIMEOUT_MS + EMO_IDLE_TIMEOUT_MS) {
        /* 累计 30 秒无互动 -> 睡眠（仅一次） */
        notify(EMO_SLEEPING);
    } else if (idle >= EMO_IDLE_TIMEOUT_MS) {
        /* 20 秒无互动 -> 无聊 */
        if (s_emotion != EMO_BORED) {
            notify(EMO_BORED);
        }
    }
    /* else: 互动期内保持当前情绪，不降级 */
}

void Emotion_WakeUp(void)
{
    Emotion_Touch(0U);
    notify(EMO_IDLE);
}
