/******************************************************************************
 * @file     moves_surprised.c
 * @brief    惊讶（SURPRISED）动作：3 个变体，随机呼出（每个都是多段长链）
 *
 * 情绪来源：PC 下发 EMO:SURPRISED → 连续状态 happiness=600 / energy=1000 / curiosity=900
 *          → 【精力拉满 + 好奇心最高】→ 调制算法给最大幅度与最快速度。
 *
 * 身体语义：突然抬头（一愣）→ 定住 → 慢慢确认周围。惊讶的关键是"快起 + 长定"。
 *
 * 【变体池】3 个变体（moves_play_variant 随机呼出，不与上次相同）：
 *   ① moves_surprised_run()     猛抬头 —— 极快抬头 → 定格 → 左右慢慢看一遍 → 回落   ≈5s
 *   ② moves_surprised_v2_run()  吓一跳 —— 抬头 → 两轴小抖 → 低头看一眼 → 又抬头       ≈3.5s
 *   ③ moves_surprised_v3_run()  疑惑定住 —— 抬头 → 慢慢歪头左/右（各定格很久）→ 回中   ≈5.5s
 *
 * 【参数位置约定】主变体①的参数在 moves.h 的 SURPRISED_*；②③的参数就近定义在本文件
 *   顶部（前缀 SURPRISED2_/SURPRISED3_），①里新增的段落用 SURPRISED_V1_*。
 *
 * 幅度：单次抬升最大的是 SURPRISED_PITCH_UP=180us（PITCH 中位 1100 → 1280），
 *   乘整体基准与调制上限后约 1359us，仍在限位 800~1600 内（余量 40us+）。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ---- 变体①（猛抬头）新增的段落 ---- */
#define SURPRISED_V1_LOOK      150   /* 左右慢慢看时的转头幅度（us，±）*/
#define SURPRISED_V1_LOOK_MS   900U  /* 转到一侧的时间 */
#define SURPRISED_V1_HOLD_MS   700U  /* 每处停住看的时长 */

/* ---- 变体②（吓一跳）参数 ---- */
#define SURPRISED2_UP         160   /* 抬头的量（us，向上）*/
#define SURPRISED2_UP_MS      100U  /* 抬头时间（要"猛"）*/
#define SURPRISED2_SHAKE       80   /* 两轴小抖的幅度（us，±）*/
#define SURPRISED2_SHAKE_MS    80U  /* 抖一下的时间 */
#define SURPRISED2_SHAKE_HOLD  60U  /* 抖动的极点停顿 */
#define SURPRISED2_HOLD_MS    700U  /* 抖完定住 */
#define SURPRISED2_DOWN        90   /* 低头看一眼的量（us，向下）*/
#define SURPRISED2_DOWN_MS    700U  /* 低头的时间 */
#define SURPRISED2_DOWN_HOLD  500U  /* 低头看的停顿 */
#define SURPRISED2_BACK_MS    600U  /* 再抬头的时间 */

/* ---- 变体③（疑惑定住）参数 ---- */
#define SURPRISED3_UP          80   /* 抬头的量（us，向上）*/
#define SURPRISED3_UP_MS      300U  /* 抬头时间（不像①②那么急）*/
#define SURPRISED3_TILT       120   /* 歪头幅度（us，±）*/
#define SURPRISED3_TILT_MS   1000U  /* 歪过去的时间（慢）*/
#define SURPRISED3_HOLD_MS   1200U  /* 每侧定格（"想不明白"）*/
#define SURPRISED3_RETURN_MS  400U  /* 回中时间 */

/* ==================== 变体①：猛抬头 ==================== */
void moves_surprised_run(void)
{
    /* ① 猛抬头（时间很短 → 快）*/
    move_to(0, (int16_t)SURPRISED_PITCH_UP, SURPRISED_UP_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 定格（"愣住了"）—— 这段时间状态机仍能收命令并打断 */
    move_hold(SURPRISED_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 慢慢转头看左边 → 停住 */
    move_to(-(int16_t)SURPRISED_V1_LOOK, (int16_t)(SURPRISED_PITCH_UP / 2),
            SURPRISED_V1_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SURPRISED_V1_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 再慢慢看右边 → 停住（"发生什么了？"）*/
    move_to((int16_t)SURPRISED_V1_LOOK, (int16_t)(SURPRISED_PITCH_UP / 2),
            SURPRISED_V1_LOOK_MS + 500U);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SURPRISED_V1_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 缓缓回落回中位 */
    move_center(SURPRISED_RETURN_MS);
}

/* ==================== 变体②：吓一跳 ====================
 * 轨迹：猛抬头 → 两轴一起小幅快抖（"一激灵"）→ 定住 → 低头看一眼 → 又抬头 → 回中
 * 与①的区别：有"抖"和"低头确认"两个动作，读起来更像被吓到。 */
void moves_surprised_v2_run(void)
{
    uint8_t n;

    /* ① 猛抬头 */
    move_to(0, (int16_t)SURPRISED2_UP, SURPRISED2_UP_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 两轴一起小幅快抖两次（脖子"一激灵"），每个极点停一下 */
    for (n = 0U; n < 2U; n++) {
        int16_t side = ((n & 1U) == 0U) ? -(int16_t)SURPRISED2_SHAKE
                                        :  (int16_t)SURPRISED2_SHAKE;

        move_to(side, (int16_t)SURPRISED2_UP, SURPRISED2_SHAKE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(SURPRISED2_SHAKE_HOLD);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ③ 定住 */
    move_hold(SURPRISED2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 低头看一眼（看看是什么吓到我了）*/
    move_to(0, -(int16_t)SURPRISED2_DOWN, SURPRISED2_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SURPRISED2_DOWN_HOLD);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 又抬起来 */
    move_to(0, (int16_t)SURPRISED2_UP, SURPRISED2_BACK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 回中位 */
    move_center(SURPRISED_RETURN_MS);
}

/* ==================== 变体③：疑惑定住 ====================
 * 轨迹：抬头 → 慢慢歪头到左（定格很久）→ 慢慢歪到右（定格很久）→ 回中
 * 与①②的区别：不快，是"歪着头想不明白"的惊讶，两个长定格就是主体。 */
void moves_surprised_v3_run(void)
{
    /* ① 抬起头 */
    move_to(0, (int16_t)SURPRISED3_UP, SURPRISED3_UP_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 慢慢歪到左边，定住 */
    move_to(-(int16_t)SURPRISED3_TILT, (int16_t)SURPRISED3_UP, SURPRISED3_TILT_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SURPRISED3_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 慢慢歪到右边，定住 */
    move_to((int16_t)SURPRISED3_TILT, (int16_t)SURPRISED3_UP, SURPRISED3_TILT_MS + 400U);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SURPRISED3_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 回中位 */
    move_center(SURPRISED3_RETURN_MS);
}
