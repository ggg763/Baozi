/******************************************************************************
 * @file     moves_sad.c
 * @brief    难过（SAD）动作：3 个变体，随机呼出（每个都是多段长链）
 *
 * 情绪来源：PC 下发 EMO:SAD → 连续状态 happiness=200 / energy=300 / curiosity=400
 *          → 低落、没劲 → emotion_fsm 的调制算法把幅度调小、速度调慢。
 *
 * 身体语义：垂头丧气 —— 头低下去，在低位无力地晃；节奏比其它情绪慢一档。
 *
 * 【变体池】3 个变体（moves_play_variant 随机呼出，不与上次相同）：
 *   ① moves_sad_run()     垂头丧气 —— 低头 → 低位左右各晃 → 停很久 → 回中        ≈7.5s
 *   ② moves_sad_v2_run()  长叹    —— 头垂得更低 → 许久不动 → 缓慢抬头（叹气）→ 又垂下去  ≈13s
 *   ③ moves_sad_v3_run()  无力摇头 —— 头低着，极慢地左右摇（1.8s 一下）            ≈8s
 *
 * 【参数位置约定】主变体①的参数在 moves.h 的 SAD_*；②③的参数就近定义在本文件顶部
 *   （前缀 SAD2_/SAD3_），①里新增的停顿用 SAD_V1_PAUSE_MS。
 *
 * 行程（实测 YAW 300~1700、PITCH 800~1600）：①②③ 都用小幅度（YAW ≤ ±84、
 *   PITCH 向下 ≤ 170），乘整体基准与调制后仍有余量，不会顶极限。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ---- 变体①（垂头丧气）新增的停顿 ---- */
#define SAD_V1_PAUSE_MS       800U  /* 每次摆动后停住的时长（无力感的关键）*/

/* ---- 变体②（长叹）参数 ---- */
#define SAD2_PITCH_DEEP       170   /* 头垂得更低（us，向下）—— 比①的 150 更深 */
#define SAD2_PITCH_UP          40   /* 叹气时抬起的量（us，向上）*/
#define SAD2_TILT              80   /* 叹气时头微偏（us，±）*/
#define SAD2_DOWN_MS          700U  /* 垂下去的时间 */
#define SAD2_QUIET_MS        2000U  /* 长时间不动（"发呆"）*/
#define SAD2_LIFT_MS         1500U  /* 缓慢抬头（叹气）的时间 */
#define SAD2_HOLD_MS          800U  /* 抬到位的停顿 */
#define SAD2_TILT_MS         1200U  /* 头微偏的时间 */
#define SAD2_FALL_MS         1500U  /* 又慢慢垂回去的时间 */
#define SAD2_REST_MS         1500U  /* 垂回去后的停顿 */

/* ---- 变体③（无力摇头）参数 ---- */
#define SAD3_YAW_SWING         84   /* 摇头幅度（us，±）—— 比①的 70 大一点但极慢 */
#define SAD3_SLOW_MS         1800U  /* 从一侧摇到另一侧的时间（"一秒八"一下）*/
#define SAD3_PAUSE_MS         600U  /* 每侧停住的时长 */
#define SAD3_END_MS          1500U  /* 最后一次回摆 */

/* ==================== 变体①：垂头丧气 ==================== */
void moves_sad_run(void)
{
    /* ① 缓慢回中 */
    move_center(SAD_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 慢慢低头（垂头）*/
    move_to(0, -(int16_t)SAD_PITCH_DOWN, SAD_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD_V1_PAUSE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 在低位左右各晃一次（每次行程 2×摆幅，摆动后停一下再换向）*/
    move_to(-(int16_t)SAD_YAW_SWING, -(int16_t)SAD_PITCH_DOWN, SAD_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD_V1_PAUSE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    move_to((int16_t)SAD_YAW_SWING, -(int16_t)SAD_PITCH_DOWN, SAD_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD_V1_PAUSE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    move_to(-(int16_t)SAD_YAW_SWING, -(int16_t)SAD_PITCH_DOWN, SAD_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD_V1_PAUSE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 回到中间但仍然低着头，停久一点（叹气的留白）*/
    move_to(0, -(int16_t)SAD_PITCH_DOWN, SAD_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 回中位 */
    move_center(SAD_RETURN_MS);
}

/* ==================== 变体②：长叹 ====================
 * 轨迹：回中 → 头垂得更低 → 很久不动 → 缓慢抬头（像叹口气）→ 停 → 头微偏 → 又垂回去 → 停 → 回中
 * 与①的区别：幅度层次更多、停顿更长，是三条里最"沉"的一条。 */
void moves_sad_v2_run(void)
{
    /* ① 回中 */
    move_center(SAD_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 头垂下去（比①更深）*/
    move_to(0, -(int16_t)SAD2_PITCH_DEEP, SAD2_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 很久不动（这条变体的"重"就靠这个停顿）*/
    move_hold(SAD2_QUIET_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 缓慢抬头 —— 像叹出一口气 */
    move_to(0, (int16_t)SAD2_PITCH_UP, SAD2_LIFT_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 头微偏（看着别处发呆）*/
    move_to(-(int16_t)SAD2_TILT, (int16_t)SAD2_PITCH_UP, SAD2_TILT_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 又慢慢垂回去 */
    move_to(0, -(int16_t)SAD2_PITCH_DEEP, SAD2_FALL_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD2_REST_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑦ 回中位 */
    move_center(SAD_RETURN_MS);
}

/* ==================== 变体③：无力摇头 ====================
 * 轨迹：回中 → 低头 → 极慢地左摆 → 停 → 极慢地右摆 → 停 → 再摆回来 → 回中
 * 与①②的区别：头一直低着，只用 YAW 很慢地摇 —— "无可奈何地摇头"。 */
void moves_sad_v3_run(void)
{
    /* ① 回中并低头 */
    move_center(SAD_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_to(0, -(int16_t)SAD_PITCH_DOWN, SAD_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 极慢地摇到左边 */
    move_to(-(int16_t)SAD3_YAW_SWING, -(int16_t)SAD_PITCH_DOWN, SAD3_SLOW_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD3_PAUSE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 摇到右边（行程 2×摆幅）*/
    move_to((int16_t)SAD3_YAW_SWING, -(int16_t)SAD_PITCH_DOWN, SAD3_SLOW_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(SAD3_PAUSE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 再摆回左边（幅度小一点，收得无力）*/
    move_to(-(int16_t)(SAD3_YAW_SWING * 2 / 3), -(int16_t)SAD_PITCH_DOWN, SAD3_END_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 回中位 */
    move_center(SAD_RETURN_MS);
}
