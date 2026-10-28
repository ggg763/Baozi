/******************************************************************************
 * @file     face_ui.h
 * @brief    屏幕表情的【对外接口】—— 供情感状态机（emotion/emotion_fsm.c）驱动
 *
 * 背景：
 *   本工程的屏幕表情（UserFolder/anim/face_emo_test.c）原本是一个【独立入口】：
 *   自己的主循环 + 自己读串口 + 自己收命令。情感状态机（emotion/）也是独立入口。
 *   两者都独占串口、都有主循环，所以不能同时跑。
 *
 *   本头文件把屏幕表情【拆出成可被调用的接口】：
 *
 *       ┌──────────────────────────────────────────────┐
 *       │ emotion_fsm（唯一主循环 + 唯一串口所有者）      │
 *       │   EMO:HAPPY / INTENT:CHAT  ──┐               │
 *       │                              ▼               │
 *       │   emotion_manager → 情绪      mood_fsm → 心情 │
 *       │        │                          │          │
 *       │        ├──→ moves_*  ──→ 舵机（脖子）         │
 *       │        └──→ face_ui_* ──→ 屏幕（脸）          │
 *       └──────────────────────────────────────────────┘
 *
 * 重要：一旦调用 face_ui_init()，屏幕侧就【不再自己读串口】——
 *       串口归 emotion_fsm 独占（否则两个 poll_uart 会互相抢字节）。
 *       此时屏幕表情的调试命令（blink / gaze / model …）不再可用，
 *       但仍可用 emotion_fsm 的命令（EMO:xxx / INTENT:xxx / help）。
 *
 * 用法（在 emotion_fsm.c 里）：
 *       emotion_fsm_init()     →  face_ui_init();
 *       fsm_tick_hook()        →  face_ui_tick();         每 20ms 一次
 *       换动作的同时            →  face_ui_emotion(emo_name(e));
 *                                 face_ui_mood(mood_name(m));
 ******************************************************************************/
#ifndef _FACE_UI_H_
#define _FACE_UI_H_

#include <stdint.h>

/* 初始化屏幕并进入参数化模型模式（显示 neutral）。
 * 调用后屏幕侧放弃串口所有权。 */
void face_ui_init(void);

/* 推进一帧（非阻塞）。由调用方按 ~20ms 的节奏反复调用。
 * 内含：眨眼状态机 + 待机视线动作；需要重绘时自己重绘一次。 */
void face_ui_tick(void);

/* 设置情绪（"happy" / "sad" / "angry" / "surprised" / "loving" / "neutral"，
 * 以及新增的姿势 "think" / "pet" / "look" / "listen" / "question" / "greet"）。
 * 返回 0 = 切换成功，-1 = 没有这个姿势（调用方应保持原表情）。
 * 每个姿势 = 【模型】+【动作脚本】（见 anim/face_action.h），所以表情是会动的。 */
int face_ui_emotion(const char *name);

/* 设置心情（"active" / "idle" / "bored" / "sleeping"）。
 * 心情优先于情绪，与 emotion_fsm_move_for() 的优先级一致：
 *     sleeping → asleep（闭眼 + 泡泡）
 *     bored    → neutral（没有专门的"无聊"模型，降级处理）
 *     active / idle → 回到当前情绪的表情 */
int face_ui_mood(const char *name);

/* 记录意图并尽量切到该意图的专用表情（"think" / "listen" / "question" / "greet"）。
 * 返回 0 = 该意图有专用表情并且已切换，-1 = 没有（调用方应回落到情绪表情）。
 * 这条返回值是 emotion_fsm 实现"意图表情优先于情绪表情"的依据。 */
int face_ui_intent(const char *name);

/* 刺激 → 一次性表情（夸奖 / 责备 / 玩耍）。kind 取 emotion_manager.h 的 EMO_STIM_*。
 *
 * 为什么要单独一条：这三种事件在情绪上是"慢慢攒"，但脸上应该立刻给一个反应
 * （被夸得意、被凶紧张、一起玩坏笑），2 秒后自动回落到新的情绪表情。
 * 实现是 face_app 的临时表情层，不会与情绪表情打架。
 * 抚摸不走这里 —— 它要按手指移动续期，由 ui_app.c 自己调 face_app_play。 */
void face_ui_reaction(uint8_t kind);

#endif /* _FACE_UI_H_ */
