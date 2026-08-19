/**
 * motion.h - TD1601 动作组合层（Motion Layer）
 *
 * 职责：
 *   用 Servo_Move 等原语组合出"高层动作"（点头、摇摆、歪头、睡姿...），
 *   每个动作由若干舵机指令按时间顺序组成。
 *
 * 说明：动作参数（角度/时长）为预置初始值，真机需按舵机安装调参。
 */

#ifndef MOTION_H
#define MOTION_H

#include "robot_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化动作层
 */
void Motion_Init(void);

/**
 * 周期驱动（推进正在执行的动作序列/缓动插值）
 * @param now_ms 当前系统时刻（毫秒）
 */
void Motion_Tick(uint32_t now_ms);

/* ---------- 高层动作 ---------- */

/** 开心：轻微点头（2 次） */
void Motion_HappySway(void);

/** 难过：头部下垂 */
void Motion_SadNod(void);

/** 生气：歪头 + 快速复位 */
void Motion_AngryTilt(void);

/** 惊讶：头部后仰 */
void Motion_SurprisedLean(void);

/** 亲昵：靠近（前倾） */
void Motion_LovingLean(void);

/** 无聊：缓慢歪头 */
void Motion_BoredSlouch(void);

/** 睡眠：低头垂眼 */
void Motion_SleepPose(void);

/** 待机：回到正中 */
void Motion_IdlePose(void);

/** 复位到正中（90/90） */
void Motion_ResetPose(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_H */
