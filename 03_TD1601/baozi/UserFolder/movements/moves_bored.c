/******************************************************************************
 * @file     moves_bored.c
 * @brief    无聊（BORED）动作：3 个变体，随机呼出（每个都是多段长链）
 *
 * 触发来源：【心情层】不是情绪层 —— 心情状态机在 60 秒没有互动时判定为 bored
 * （mood_fsm.c 的降级判断，PC 版是 20~30 秒，本机已放宽），PC 端此刻的做法是
 * "歪头 + 低 LED"。本工程把它落成两条"没事干"的长链：
 *
 *   ① moves_bored_run()     歪头发呆 —— 歪到一侧垂着头、发很久的呆、换边、回正
 *   ② moves_bored_v2_run()  垂头发呆 —— 直接垂下头，极慢地小幅挪，再懒懒回正
 *   ③ moves_bored_v3_run()  东倒西歪 —— 歪左/歪右/直接低头三个方向各来一遍，停顿最长
 *
 * 心情 BORED 时 emotion_fsm 还会把速度增益再 ×0.7（emotion_fsm_apply_gain），
 * 所以这里看着不算长的时间，实际播起来还要慢一档 —— 这正是"无聊"该有的节奏。
 * 幅度（YAW ±90/±70、PITCH -80/-150）在实测行程内（YAW 300~1700、PITCH 800~1600）。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ---- 变体③（东倒西歪）参数（①②的参数见 moves.h 的 BORED_* / BORED2_*）---- */
#define BORED3_PITCH_DOWN     110   /* 直接低头时的低头量（us，向下）—— 比①②都深 */
#define BORED3_DOWN_MS       1400U  /* 低头时间 */
#define BORED3_QUIET_MS      2200U  /* 低头后长时间不动（这条变体最"散"的地方）*/

/* ==================== 变体①：歪头发呆（主变体）====================
 * 轨迹：歪到右侧并垂头 → 长时间发呆 → 慢吞吞歪到左侧 → 再发呆 → 懒懒回正 */
void moves_bored_run(void)
{
    /* ① 歪到右侧并垂下头（"没劲"）*/
    move_to((int16_t)BORED_YAW_TILT, -(int16_t)BORED_PITCH_DOWN, BORED_TILT_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 发呆（长时间不动 —— 2026-09 把停顿拉长，无聊感更足）*/
    move_hold(BORED_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 慢吞吞地歪到另一侧（行程 2×摆幅）*/
    move_to(-(int16_t)BORED_YAW_TILT, -(int16_t)BORED_PITCH_DOWN, BORED_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 再发一会儿呆 */
    move_hold(BORED_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 懒懒地回正，停一下 */
    move_center(BORED_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(BORED_REST_MS);
}

/* ==================== 变体②：垂头发呆 ====================
 * 轨迹：回中 → 垂下头（比变体1 更低）→ 长时间不动 → 极慢歪向一侧 → 再发呆
 *       → 换到另一侧 → 回正
 * 和变体① 的区别：不"歪头张望"，而是直接垂着头一动不动，只在很低的位置慢慢挪。 */
void moves_bored_v2_run(void)
{
    /* ① 先从上一个动作的位置慢慢立起来 */
    move_center(BORED2_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 垂下头（比变体1 更低）*/
    move_to(0, -(int16_t)BORED2_PITCH_DOWN, BORED2_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 长时间不动 */
    move_hold(BORED2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 极慢地挪向一侧（仍垂着头）*/
    move_to(-(int16_t)BORED2_YAW_TILT, -(int16_t)BORED2_PITCH_DOWN, BORED2_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 再发一会儿呆 */
    move_hold(BORED2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 换到另一侧 */
    move_to((int16_t)BORED2_YAW_TILT, -(int16_t)BORED2_PITCH_DOWN, BORED2_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑦ 懒懒地回正 */
    move_center(BORED2_RETURN_MS);
}

/* ==================== 变体③：东倒西歪 ====================
 * 轨迹：歪左（停）→ 歪右（停）→ 直接低头（停很久）→ 回正（停）
 * 与①②的区别：把"左歪 / 右歪 / 低头"三个方向都试一遍、停顿最长 —— 最"散"的一条。 */
void moves_bored_v3_run(void)
{
    /* ① 先慢慢回中 */
    move_center(BORED_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 歪到左边垂着头，发呆 */
    move_to(-(int16_t)BORED_YAW_TILT, -(int16_t)BORED_PITCH_DOWN, BORED_TILT_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(BORED_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 慢吞吞歪到右边，再发呆 */
    move_to((int16_t)BORED_YAW_TILT, -(int16_t)BORED_PITCH_DOWN, BORED_SWAY_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(BORED_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 不歪了，直接低下头（比歪头时更深），停很久 */
    move_to(0, -(int16_t)BORED3_PITCH_DOWN, BORED3_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(BORED3_QUIET_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 回正，停一下 */
    move_center(BORED_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(BORED_REST_MS);
}
