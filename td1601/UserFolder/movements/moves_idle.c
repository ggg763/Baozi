/******************************************************************************
 * @file     moves_idle.c
 * @brief    待机（IDLE）动作：缓慢巡视四周 —— 3 个变体，随机呼出
 *
 * 身体语义：两个舵机是机器人的"脖子"——YAW 左右转头、PITCH 抬头/低头。
 * 待机时它应该像在"看"：慢慢地、漫无目的地环顾四周，而不是僵着不动、也不该抖。
 *
 * 【2026-09 改版】原来的"V 字摇摆点头"（±80us、300ms 一步）节奏偏快、幅度偏小，
 * 看着像在抖而不像在看。现在改成三条【缓慢巡视】的长链：
 *   幅度开到 ±300us（原来的近 4 倍）、每段 1.5~7s（原来的十几倍），
 *   并且每到一侧会【停住张望】一下，而不是立刻摆回去。
 *
 * 三个变体（moves_play_variant 随机挑一个，且不与上次相同）：
 *   ① moves_idle_run()      缓慢环视     —— 慢慢从左扫到右，两侧各停一下   ≈20s
 *   ② moves_idle_v2_run()   远望与俯视   —— 抬头看远、左右看、低头看桌面   ≈23s
 *   ③ moves_idle_v3_run()   扁 8 字巡游  —— 用 move_figure8 慢慢画两圈 ∞   ≈18s
 *
 * 坐标约定（实测）：脉宽增大 → YAW 右转 / PITCH 抬头，所以
 *   -YAW = 左看、+YAW = 右看；+PITCH = 抬头、-PITCH = 低头（看桌面）。
 * 所有数值都是【相对中位的偏移 us】，参数在 moves.h 的 IDLEP1_* / IDLEP2_* / IDLEP3_*。
 * 幅度（±300us）乘上调制增益（neutral 的 amp≈950~1170）后仍在实测限位内：
 *   1000±351 → 649~1351us（限位 300~1700），不会顶极限。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ==================== 变体①：缓慢环视（主变体）====================
 * 轨迹：缓慢回中 → 慢慢左看（略抬头）→ 停住张望 → 极慢地扫到右边 → 再停 → 缓慢回中
 * 特点：最"平静"的一条，像人闲着没事左右看看。 */
void moves_idle_run(void)
{
    /* ① 从任意位置缓慢回中（时间给足，不能像换动作那样"甩"回去）*/
    move_center(IDLEP1_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 慢慢左看，顺带略抬头 —— 弧线比纯水平更有"看"的感觉 */
    move_to(-(int16_t)IDLEP1_YAW, (int16_t)IDLEP1_PITCH_UP, IDLEP1_SWEEP_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 停住，张望一会儿 */
    move_hold(IDLEP1_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 从左边极慢地扫到右边（本变体最慢的一段；途中自然经过中位并略低头）*/
    move_to((int16_t)IDLEP1_YAW, -(int16_t)IDLEP1_PITCH_DOWN, IDLEP1_CROSS_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 在右边也停一下 */
    move_hold(IDLEP1_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 缓慢收尾回中 */
    move_center(IDLEP1_RETURN_MS);
}

/* ==================== 变体②：远望与俯视 ====================
 * 轨迹：回中 → 抬头看远处 → 左看 → 低头看桌面 → 右看 → 回中，每个姿势都停一下
 * 特点：有"远近变化"，比只左右转更生动（也顺带展示了 PITCH 轴）。 */
void moves_idle_v2_run(void)
{
    /* ① 缓慢回中 */
    move_center(IDLEP2_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 抬头看远处 */
    move_to(0, (int16_t)IDLEP2_PITCH_UP, IDLEP2_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(IDLEP2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 左看（保持一点抬头）*/
    move_to(-(int16_t)IDLEP2_YAW, (int16_t)IDLEP2_PITCH_SIDE, IDLEP2_SIDE_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(IDLEP2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 低头看桌面（像在看看桌上有什么）*/
    move_to(0, -(int16_t)IDLEP2_PITCH_DOWN, IDLEP2_SIDE_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(IDLEP2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 抬头转向右边 */
    move_to((int16_t)IDLEP2_YAW, (int16_t)IDLEP2_PITCH_SIDE, IDLEP2_SIDE_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(IDLEP2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 收尾回中 */
    move_center(IDLEP2_RETURN_MS);
}

/* ==================== 变体③：扁 8 字巡游 ====================
 * 轨迹：回中 → 慢慢画一圈扁 8 字 → 停 → 再画一圈（更慢、上下更开）→ 回中
 * 特点：用曲线轨迹（move_figure8）而不是直线来回，视线像"扫过四周一圈"，
 *       连续运动最不像机械往复；两圈参数不同，避免看出周期。 */
void moves_idle_v3_run(void)
{
    /* ① 缓慢回中 */
    move_center(IDLEP3_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 第一圈：横向大、纵向小的扁 8 字（5.5s 一圈，很慢）*/
    move_figure8((int16_t)IDLEP3_YAW_AMP, (int16_t)IDLEP3_PITCH_AMP,
                 (uint16_t)IDLEP3_LAP_MS, 1U);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 停一下（像看完一圈在想事情）*/
    move_hold(IDLEP3_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 第二圈：横向收一点、纵向更开、时间更长 —— 两圈不重样 */
    move_figure8((int16_t)IDLEP3_YAW_AMP2, (int16_t)IDLEP3_PITCH_AMP2,
                 (uint16_t)IDLEP3_LAP2_MS, 1U);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 收尾回中（move_figure8 画完已停在中心，这里只是补足停顿感）*/
    move_center(IDLEP3_RETURN_MS);
}
