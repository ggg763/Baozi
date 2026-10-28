/******************************************************************************
 * @file     moves_sleep.c
 * @brief    睡眠（SLEEPING）动作：3 个变体，随机呼出（每个都是多段长链）
 *
 * 触发来源：【心情层】—— PC 的 MoodStateMachine 在超过 30 秒没互动时判定 sleeping。
 * PC 那边的睡姿是"头回正 + 低头（垂眼）"；本工程落成"垂头 + 呼吸起伏"。
 *
 * 【变体池】3 个变体（moves_play_variant 随机呼出，不与上次相同）：
 *   ① moves_sleep_run()     垂头呼吸 —— 正着垂头，呼吸 3 次                              ≈10s
 *   ② moves_sleep_v2_run()  侧头睡   —— 歪着头垂下去，呼吸更慢更长（像睡熟了）           ≈14s
 *   ③ moves_sleep_v3_run()  浅眠     —— 头垂得浅、呼吸又快又小（像打盹）                 ≈13s
 *
 * 【为什么是"播一遍就返回"】动作库的动作都是"播一个周期就返回"，状态机在 SLEEPING
 *   期间会反复重播 —— 于是表现为持续打盹。每次重播开头都是一次 move_to(垂头)，
 *   从当前位置平滑过去，所以不会跳变；三个变体轮流出现，睡姿也不会一直一模一样。
 *
 * 【睡眠中也能被叫醒】动作库每 20ms 调一次 tick 钩子（emotion_fsm 挂的会轮询串口），
 *   所以哪怕正播着这里，PC 一发 EMO:xxx 就立刻打断、随即醒来。
 *
 * 幅度：PITCH 中位 1100，垂头量 200us（变体①）→ 900us，余量 100us（限位下限 800）。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ---- 变体②（侧头睡）参数 ---- */
#define SLEEP2_YAW_TILT        60   /* 歪头睡的角度（us，±）*/
#define SLEEP2_DOWN_MS       1000U  /* 垂下去的时间（比①慢）*/
#define SLEEP2_BREATH_AMP      20   /* 呼吸起伏（us，±）—— 比①更浅 */
#define SLEEP2_BREATH_MS     2000U  /* 单次呼吸时间（比①更长 → 睡得更沉）*/
#define SLEEP2_BREATHS          3    /* 呼吸几次 */
#define SLEEP2_STRAIGHT_MS    900U  /* 转正一点继续睡的时间 */
#define SLEEP2_REST_MS        800U  /* 结尾停顿 */

/* ---- 变体③（浅眠）参数 ---- */
#define SLEEP3_PITCH_DROP     140   /* 垂头量（us，向下）—— 比①浅，像只是在打盹 */
#define SLEEP3_DOWN_MS        600U  /* 垂下去的时间 */
#define SLEEP3_BREATH_AMP      15   /* 呼吸起伏（us，±）*/
#define SLEEP3_BREATH_MS     1500U  /* 单次呼吸时间（快一点）*/
#define SLEEP3_BREATHS          4    /* 呼吸几次（次数多 → 有点不踏实）*/

/* ==================== 变体①：垂头呼吸 ==================== */
void moves_sleep_run(void)
{
    int i;

    /* ① 垂下头（睡姿）*/
    move_to(0, -(int16_t)SLEEP_PITCH_DROP, SLEEP_DROP_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 呼吸：抬头一点 → 低头一点，来回 SLEEP_BREATHS 次（周期很慢）*/
    for (i = 0; i < (int)SLEEP_BREATHS; i++) {
        move_to(0, -(int16_t)(SLEEP_PITCH_DROP - SLEEP_BREATH_AMP), SLEEP_BREATH_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_to(0, -(int16_t)(SLEEP_PITCH_DROP + SLEEP_BREATH_AMP), SLEEP_BREATH_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ③ 保持低头的睡姿结束本周期（不回中位 —— 睡觉时不该抬着头）*/
}

/* ==================== 变体②：侧头睡 ====================
 * 轨迹：歪着头垂下去 → 慢呼吸 3 次（起伏更浅、周期更长）→ 转正一点 → 停住
 * 与①的区别：歪头 + 更慢更深的呼吸，读起来是"睡熟了"。 */
void moves_sleep_v2_run(void)
{
    int i;

    /* ① 歪着头垂下去（YAW 偏一点 + 垂头）*/
    move_to(-(int16_t)SLEEP2_YAW_TILT, -(int16_t)SLEEP_PITCH_DROP, SLEEP2_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 睡熟的呼吸（起伏更浅、周期更长）*/
    for (i = 0; i < (int)SLEEP2_BREATHS; i++) {
        move_to(-(int16_t)SLEEP2_YAW_TILT,
                -(int16_t)(SLEEP_PITCH_DROP - SLEEP2_BREATH_AMP), SLEEP2_BREATH_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_to(-(int16_t)SLEEP2_YAW_TILT,
                -(int16_t)(SLEEP_PITCH_DROP + SLEEP2_BREATH_AMP), SLEEP2_BREATH_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ③ 稍微转正一点（睡着时头会自己滑回来），继续垂着头 */
    move_to(0, -(int16_t)SLEEP_PITCH_DROP, SLEEP2_STRAIGHT_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SLEEP2_REST_MS);
}

/* ==================== 变体③：浅眠 ====================
 * 轨迹：浅浅垂头（没完全睡着的样子）→ 呼吸 4 次（又快又小）→ 停住
 * 与①②的区别：头垂得浅、呼吸节奏快 —— 像"刚睡着又没睡实"。 */
void moves_sleep_v3_run(void)
{
    int i;

    /* ① 浅浅垂头 */
    move_to(0, -(int16_t)SLEEP3_PITCH_DROP, SLEEP3_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 又快又小的呼吸（4 次）*/
    for (i = 0; i < (int)SLEEP3_BREATHS; i++) {
        move_to(0, -(int16_t)(SLEEP3_PITCH_DROP - SLEEP3_BREATH_AMP), SLEEP3_BREATH_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_to(0, -(int16_t)(SLEEP3_PITCH_DROP + SLEEP3_BREATH_AMP), SLEEP3_BREATH_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ③ 保持这个浅睡姿势结束本周期 */
}
