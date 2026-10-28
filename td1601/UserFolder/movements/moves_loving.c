/******************************************************************************
 * @file     moves_loving.c
 * @brief    爱意（LOVING）动作：3 个变体，随机呼出（每个都是多段长链）
 *
 * 情绪来源：PC 下发 EMO:LOVING → 连续状态 happiness=800 / energy=600 / curiosity=700
 *          → "开心但不激动" → 调制算法给中等幅度 + 偏慢速度 → 读起来是"温柔地歪头看你"。
 *
 * 身体语义：轻轻抬头、慢慢左右歪头（像小动物蹭人），弧线长、节奏慢。
 * 与 HAPPY（快而大）、ANGRY（快而硬）刻意区分：这里每段都慢一倍以上。
 *
 * 【变体池】3 个变体（moves_play_variant 随机呼出，不与上次相同）：
 *   ① moves_loving_run()     温柔歪头 —— 抬头 → 慢歪左/右 → 靠近 → 抬头看你 → 回中   ≈7s
 *   ② moves_loving_v2_run()  轻轻蹭   —— 小幅慢摆 4 次（像蹭）→ 抬头 → 停 → 回中      ≈8s
 *   ③ moves_loving_v3_run()  慢慢看你 —— 缓慢小范围环视（左看/前看/右看）→ 抬头定格    ≈8s
 *
 * 【参数位置约定】主变体①的参数在 moves.h 的 LOVING_*；②③的参数就近定义在本文件顶部
 *   （前缀 LOVING2_/LOVING3_），①里新增的段落用 LOVING_V1_*。
 *
 * 幅度（YAW ±90 / PITCH +60）远小于实测行程（YAW 300~1700、PITCH 800~1600）。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ---- 变体①（温柔歪头）新增的段落 ---- */
#define LOVING_V1_GAZE_MS      900U  /* 抬头看着你的定格时长 */
#define LOVING_V1_NEAR_DOWN     40   /* "靠近"时的低头量（us，向下）*/
#define LOVING_V1_NEAR_MS      800U  /* 靠近的时间 */
#define LOVING_V1_NEAR_HOLD_MS 600U  /* 靠近后停住 */

/* ---- 变体②（轻轻蹭）参数 ---- */
#define LOVING2_YAW_SWING       60   /* 蹭的摆幅（us，±）—— 比①小，幅度层次不同 */
#define LOVING2_PITCH_WARM      35   /* 保持的抬头量（us，向上）*/
#define LOVING2_SWING_MS      1200U  /* 单次蹭的时间（很慢）*/
#define LOVING2_SETTLE_MS      300U  /* 每侧停住（贴一下）*/
#define LOVING2_SWING_N          4    /* 蹭几次 */
#define LOVING2_LIFT_MS        600U  /* 最后抬头的时间 */
#define LOVING2_GAZE_MS       1000U  /* 抬头看你 */

/* ---- 变体③（慢慢看你）参数 ---- */
#define LOVING3_YAW_SWING      100   /* 环视幅度（us，±）*/
#define LOVING3_PITCH_UP        50   /* 环视时保持的抬头量（us）*/
#define LOVING3_TURN_MS       1100U  /* 转到一侧的时间（很慢）*/
#define LOVING3_LOOK_MS        900U  /* 每一处停住看的时长 */
#define LOVING3_GAZE_UP        80   /* 最后抬头的量（us）*/
#define LOVING3_GAZE_MS       1200U  /* 抬头的时长 */

/* ==================== 变体①：温柔歪头 ==================== */
void moves_loving_run(void)
{
    /* ① 缓慢回中 */
    move_center(LOVING_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 微微抬头（温柔地看过来）*/
    move_to(0, (int16_t)LOVING_PITCH_UP, LOVING_LIFT_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 慢慢歪到左边 → 停一下 */
    move_to(-(int16_t)LOVING_YAW_SWING, (int16_t)LOVING_PITCH_WARM, LOVING_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 左 → 右（行程 2×摆幅）→ 停一下 */
    move_to((int16_t)LOVING_YAW_SWING, (int16_t)LOVING_PITCH_WARM, LOVING_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 回到中偏左（幅度小一点，收得柔和）*/
    move_to(-(int16_t)(LOVING_YAW_SWING * 2 / 3), (int16_t)LOVING_PITCH_WARM,
            LOVING_NEAR_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ "靠近"你一点（低头收一点 + 回中），停住 */
    move_to(0, -(int16_t)LOVING_V1_NEAR_DOWN, LOVING_V1_NEAR_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING_V1_NEAR_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑦ 再抬眼看一下你（定格），然后回中 */
    move_to(0, (int16_t)LOVING_PITCH_UP, LOVING_V1_GAZE_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING_V1_GAZE_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_center(LOVING_RETURN_MS);
}

/* ==================== 变体②：轻轻蹭 ====================
 * 轨迹：回中 → 小幅慢摆 4 次（每次贴到一侧停一下）→ 抬头 → 看着你停住 → 回中
 * 与①的区别：摆幅只有一半、但次数更多、每侧都有"贴住"的停顿 —— 是"蹭"不是"晃"。 */
void moves_loving_v2_run(void)
{
    uint8_t n;

    /* ① 回中 + 保持一点抬头 */
    move_center(LOVING_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_to(0, (int16_t)LOVING2_PITCH_WARM, LOVING_LIFT_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 左右各蹭 LOVING2_SWING_N 次（一侧 → 另一侧，每侧停一下）*/
    for (n = 0U; n < (uint8_t)LOVING2_SWING_N; n++) {
        int16_t side = ((n & 1U) == 0U) ? -(int16_t)LOVING2_YAW_SWING
                                        :  (int16_t)LOVING2_YAW_SWING;

        move_to(side, (int16_t)LOVING2_PITCH_WARM, LOVING2_SWING_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(LOVING2_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ③ 抬头看着你，停住 */
    move_to(0, (int16_t)LOVING_PITCH_UP, LOVING2_LIFT_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING2_GAZE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 回中位 */
    move_center(LOVING_RETURN_MS);
}

/* ==================== 变体③：慢慢看你 ====================
 * 轨迹：回中 → 慢慢转到左看（停）→ 转回中间看（停）→ 转到右看（停）→ 重新抬头看你 → 回中
 * 与①②的区别：不是左右摆动，而是"三个位置逐一看过去"，最安静的一条。 */
void moves_loving_v3_run(void)
{
    /* ① 缓缓回中 */
    move_center(LOVING_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 转到左边看，停住 */
    move_to(-(int16_t)LOVING3_YAW_SWING, (int16_t)LOVING3_PITCH_UP, LOVING3_TURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING3_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 转回正前方看，停住 */
    move_to(0, (int16_t)LOVING3_PITCH_UP, LOVING3_TURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING3_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 转到右边看，停住 */
    move_to((int16_t)LOVING3_YAW_SWING, (int16_t)LOVING3_PITCH_UP, LOVING3_TURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING3_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 重新转回来、抬起一点头（"最后还是看你"），停住 */
    move_to(0, (int16_t)LOVING3_GAZE_UP, LOVING3_GAZE_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(LOVING3_GAZE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 回中位 */
    move_center(LOVING_RETURN_MS);
}
