/**
 * sensor_manager.h - TD1601 传感器管理
 *
 * 职责：
 *   周期扫描机器人本体传感器（触摸、红外追踪等），
 *   事件直接驱动情绪状态机——不经过 PC。
 *
 * 例：
 *   触摸（TTP223）→ Emotion_Update(EMO_LOVING) + 动作反馈
 *   红外（IR Sensor）→ 方向判断 → 舵机跟随
 */

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "robot_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化传感器
 */
void Sensor_Init(void);

/**
 * 周期扫描（主循环调用，建议 10ms~50ms 一次）
 * @param now_ms 当前系统时刻（毫秒）
 */
void Sensor_Scan(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_MANAGER_H */
