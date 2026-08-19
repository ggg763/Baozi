/**
 * behavior.c - TD1601 行为规划层实现
 *
 * 情绪 -> 行为映射表：
 *   每个情绪定义一组 {屏幕表情, LED 颜色, 舵机动作函数}。
 *
 * 说明：以下为预置骨架，具体动作参数（角度/时长）待真机调参。
 *       HAL 层当前为 printf 模拟，接真实 SDK 后无需改动本文件。
 */

#include "behavior.h"

#include "screen_display.h"
#include "led_control.h"
#include "motion.h"
#include "servo_control.h"

/* 动作函数类型：执行一个高层动作 */
typedef void (*ActionFunc)(void);

/* 行为配置项：一个情绪对应的完整表现 */
typedef struct {
    EmotionState emotion;
    const char  *screen_anim;   /**< 屏幕动画名（Screen_ShowEmotion 用） */
    RgbColor     led_color;     /**< LED 颜色 */
    ActionFunc   action;        /**< 高层动作函数（NULL 表示无动作） */
} BehaviorConfig;

/* 各情绪对应的动作（高层动作见 motion.c） */
extern void Motion_HappySway(void);
extern void Motion_SadNod(void);
extern void Motion_AngryTilt(void);
extern void Motion_SurprisedLean(void);
extern void Motion_LovingLean(void);
extern void Motion_BoredSlouch(void);
extern void Motion_SleepPose(void);
extern void Motion_IdlePose(void);
extern void Motion_ResetPose(void);

/* 情绪 -> 行为映射表 */
static const BehaviorConfig s_behavior_table[] = {
    { EMO_HAPPY,     "happy",     {255, 200,   0}, Motion_HappySway     },
    { EMO_SAD,       "sad",       {  0,  80, 255}, Motion_SadNod        },
    { EMO_ANGRY,     "angry",     {255,   0,   0}, Motion_AngryTilt     },
    { EMO_SURPRISED, "surprised", {255, 255,   0}, Motion_SurprisedLean },
    { EMO_LOVING,    "loving",    {255,  80, 180}, Motion_LovingLean    },
    { EMO_BORED,     "bored",     { 80,  80,  80}, Motion_BoredSlouch   },
    { EMO_SLEEPING,  "sleep",     { 40,  40,  40}, Motion_SleepPose     },
    { EMO_IDLE,      "neutral",   {255, 255, 255}, Motion_IdlePose      },
};

#define BEHAVIOR_TABLE_SIZE \
    (sizeof(s_behavior_table) / sizeof(s_behavior_table[0]))

/* ---------- 内部工具 ---------- */
static const BehaviorConfig *find_config(EmotionState emo)
{
    uint32_t i;
    for (i = 0; i < BEHAVIOR_TABLE_SIZE; i++) {
        if (s_behavior_table[i].emotion == emo) {
            return &s_behavior_table[i];
        }
    }
    return &s_behavior_table[BEHAVIOR_TABLE_SIZE - 1]; /* 默认 IDLE */
}

/* ---------- 对外接口 ---------- */

void Behavior_Init(void)
{
    Screen_Init();
    LED_Init();
    Servo_Init();
    Motion_ResetPose();
}

void Behavior_OnEmotionChanged(EmotionState new_state)
{
    const BehaviorConfig *cfg = find_config(new_state);

    /* 1. 屏幕表情 */
    Screen_ShowEmotion(new_state, cfg->screen_anim);

    /* 2. LED 颜色 */
    LED_SetColor(cfg->led_color);

    /* 3. 舵机高层动作 */
    if (cfg->action != 0) {
        cfg->action();
    }
}

void Behavior_Tick(uint32_t now_ms)
{
    /* 预留：驱动动作插值/动画帧推进 */
    Motion_Tick(now_ms);
}
