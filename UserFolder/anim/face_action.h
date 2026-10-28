/******************************************************************************
 * @file     face_action.h
 * @brief    动作层：时间 → 表情参数（纯计算，不碰屏幕）
 *
 * 姿势（face_model.c）决定"长什么样"，本文件决定"怎么动"。
 * 每个动作就是一个小函数：给毫秒时间，吐出这一刻的视线/眨眼/眼睛大小/张嘴。
 * 全部是正弦叠加 —— 没有关键帧、没有插值表，所以永远不会有"播完停住"的僵硬感，
 * 也不需要为每个动作准备数据，代码量是每个动作一两行。
 *
 * 为什么用正弦而不是关键帧：
 *   桌面机器人的脸要"活着"——呼吸、视线漂移、微颤都是连续量。
 *   关键帧播完会回到原点并停住，一眼就能看出是循环；正弦叠加天然无缝。
 *   例外是"一次性"动作（打哈欠、眨眼），那由上层 face_app.c 处理。
 *
 * 单位：与 face_model.h 的 face_expr_t 完全一致。
 *   gaze_dx/dy = 1/2 像素    blink = 0..255（255 闭合）
 *   eye_scale  = 百分比      mouth_open = 0..255
 ******************************************************************************/

#ifndef _FACE_ACTION_H_
#define _FACE_ACTION_H_

#include <stdint.h>
#include "face_model.h"             /* face_expr_t */

/* 一个动作：名字 + 采样函数。
 * t 是【该动作已播放的毫秒数】，由上层累加；动作内部自己取模做周期。 */
typedef struct {
    const char *name;
    void      (*fn)(uint32_t t_ms, face_expr_t *out);
} face_act_t;

int                 face_act_count(void);
const face_act_t   *face_act_get(int idx);
const face_act_t   *face_act_find(const char *name);   /* 大小写不敏感，0 = 没找到 */

/* 采样一次。a 为 0 时输出静止姿态（blink=0, eye_scale=100）。 */
void                face_act_sample(const face_act_t *a, uint32_t t_ms, face_expr_t *out);

#endif /* _FACE_ACTION_H_ */
