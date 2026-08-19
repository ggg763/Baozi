/**
 * screen_display.h - TD1601 LCD 表情显示层
 *
 * 职责：
 *   根据 EmotionState 显示对应表情动画。
 *   表情素材（bmp 帧）由真机调试时导入，此处只做接口与状态机。
 */

#ifndef SCREEN_DISPLAY_H
#define SCREEN_DISPLAY_H

#include "robot_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LCD
 */
void Screen_Init(void);

/**
 * 显示情绪表情
 * @param emo       情绪状态
 * @param anim_name 动画名（如 "happy"/"sad"，用于查找素材；可传 NULL）
 */
void Screen_ShowEmotion(EmotionState emo, const char *anim_name);

/**
 * 周期驱动（帧动画推进）
 * @param now_ms 当前系统时刻（毫秒）
 */
void Screen_Tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_DISPLAY_H */
