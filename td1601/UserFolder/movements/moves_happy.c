/******************************************************************************
 * @file     moves_happy.c
 * @brief    开心（HAPPY）动作：3 个变体，随机呼出（每个都是多段长链）
 *
 * 身体语义：两个舵机是机器人的"脖子"——YAW 左右转头、PITCH 抬头/低头。
 * 开心时脖子"得意洋洋"：仰头、左右晃、偶尔定住看你一眼。
 *
 * 【变体池】本状态的 3 个变体由 moves_play_variant() 随机呼出（且不与上次相同）：
 *   ① moves_happy_run()     欢快摇晃 —— 快晃两轮（中段抬头得意）+ 尾部轻点头   ≈7s
 *   ② moves_happy_v2_run()  点头雀跃 —— 点头 → 小摆 → 左看右看（像在炫耀）      ≈6s
 *   ③ moves_happy_v3_run()  绕圈庆祝 —— 手写 8 段圆弧走一整圈（视线扫过四周）   ≈9s
 *
 * 【参数位置约定】主变体①的参数在 moves.h 的 HAPPY_*（保持"改参数去 moves.h"的习惯）；
 *   变体②③以及①里新增的段落参数【就近定义在本文件顶部】（前缀 HAPPY_V1_/HAPPY2_/HAPPY3_）。
 *
 * 【到位与速度】（2026-09 经验，见 moves.h）：舵机开环、脉宽换向就反向，段太短会让
 *   实际摆幅被削平。所以每次摆动的时间要留够（需求速度 ≤5us/ms），并且每个极点后
 *   加一次 HAPPY_SETTLE_MS 停顿让它咬住位置。
 *
 * 幅度都在实测行程内：YAW 1000±240、PITCH +80/+40（乘整体基准与调制后仍有余量）。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ---- 变体①（欢快摇晃）新增段落的参数（主参数见 moves.h 的 HAPPY_*）---- */
#define HAPPY_V1_LOOK_MS      700U  /* 第一轮摆动后抬头定格的时长（"得意一下"）*/
#define HAPPY_V1_ROUND2_MS    260U  /* 第二轮摆动时间（比第一轮慢一点，显从容）*/
#define HAPPY_V1_NOD_DOWN      60   /* 尾部轻点头量（us，向下）*/
#define HAPPY_V1_NOD_MS       400U  /* 单次点头时间（慢，和前面快晃形成对比）*/
#define HAPPY_V1_NOD_UP        30   /* 点头回抬的量（us，向上）*/

/* ---- 变体②（点头雀跃）参数 ---- */
#define HAPPY2_NOD_DOWN        70   /* 点头量（us，向下）*/
#define HAPPY2_NOD_UP          40   /* 抬头量（us，向上）*/
#define HAPPY2_NOD_MS         220U  /* 单次点头时间 */
#define HAPPY2_NODS             3    /* 连续点几次 */
#define HAPPY2_YAW_SWING      180   /* 小摆/左看右看的摆幅（us，±）*/
#define HAPPY2_SWING_MS       230U  /* 单次小摆时间 */
#define HAPPY2_LOOK_MS        800U  /* 转头看一侧后停住的时长 */
#define HAPPY2_HOLD_MS        600U  /* 抬头得意定格 */

/* ---- 变体③（绕圈庆祝）参数 ----
 * 手写一圈圆弧（8 段，每段 45°）而不是用 move_figure8：幅度小、速度更平缓，
 * 也便于单独调这一段。0.707 是对角方向的分量（cos45°≈sin45°≈0.707）。*/
#define HAPPY3_YAW_AMP        200   /* 圆的横向半宽（us）*/
#define HAPPY3_PITCH_AMP       90   /* 圆的纵向半高（us）*/
#define HAPPY3_DIAG_YAW       141   /* 对角 YAW 分量 = 200×0.707 */
#define HAPPY3_DIAG_PITCH      64   /* 对角 PITCH 分量 = 90×0.707 */
#define HAPPY3_STEP_MS        900U  /* 每一段的时间（8 段 ≈ 7.2s 一圈）*/
#define HAPPY3_HOLD_MS        500U  /* 走完一圈停一下 */
#define HAPPY3_RETURN_MS      300U  /* 收尾回中 */

/* ==================== 变体①：欢快摇晃 ==================== */
void moves_happy_run(void)
{
    uint8_t n;

    /* ① 缓慢回中（从任意位置起步，别"甩"回来）*/
    move_center(HAPPY_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 先摆到右侧 + 扬头（把起点放在一侧，后面每段摆动都是完整 2n）*/
    move_to((int16_t)HAPPY_YAW_SWING, (int16_t)HAPPY_PITCH_HI, HAPPY_LIFT_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 第一轮：左右快摆 4 次，每个极点停一下（否则没到位就反向、摆幅被削平）*/
    for (n = 0U; n < 4U; n++) {
        int16_t side = ((n & 1U) == 0U) ? -(int16_t)HAPPY_YAW_SWING
                                        :  (int16_t)HAPPY_YAW_SWING;
        int16_t hgt  = ((n & 1U) == 0U) ? (int16_t)HAPPY_PITCH_LO
                                        : (int16_t)HAPPY_PITCH_HI;

        move_to(side, hgt, HAPPY_SWING_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(HAPPY_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ④ 抬头定格一下（"得意"）*/
    move_to(0, (int16_t)HAPPY_PITCH_HI, HAPPY_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(HAPPY_V1_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 第二轮：同样左右摆，但节奏放慢（两轮节奏不同 → 整条链不单调）*/
    for (n = 0U; n < 4U; n++) {
        int16_t side = ((n & 1U) == 0U) ? (int16_t)HAPPY_YAW_SWING
                                        : -(int16_t)HAPPY_YAW_SWING;
        int16_t hgt  = ((n & 1U) == 0U) ? (int16_t)HAPPY_PITCH_HI
                                        : (int16_t)HAPPY_PITCH_LO;

        move_to(side, hgt, HAPPY_V1_ROUND2_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(HAPPY_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ⑥ 抬头定格一下（第二轮收尾，用 moves.h 的 HAPPY_HOLD_MS）*/
    move_to(0, (int16_t)HAPPY_PITCH_HI, HAPPY_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(HAPPY_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑦ 尾部：慢速轻点头 2 次（和前两轮的快形成对比，收得温和）*/
    for (n = 0U; n < 2U; n++) {
        move_to(0, -(int16_t)HAPPY_V1_NOD_DOWN, HAPPY_V1_NOD_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_to(0, (int16_t)HAPPY_V1_NOD_UP, HAPPY_V1_NOD_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ⑧ 回中位 */
    move_center(HAPPY_RETURN_MS);
}

/* ==================== 变体②：点头雀跃 ====================
 * 轨迹：回中 → 连续点头 3 次 → 抬头得意 → 左右小摆 3 次 → 转头看左停住 → 看右停住 → 回中
 * 与①的区别：以"点头 + 左看右看"为主，摆幅小一半，更像"雀跃地跟你互动"。 */
void moves_happy_v2_run(void)
{
    uint8_t n;

    /* ① 回中 */
    move_center(HAPPY_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 连续点头 3 次（低点停一下，让它"点"得清楚）*/
    for (n = 0U; n < (uint8_t)HAPPY2_NODS; n++) {
        move_to(0, -(int16_t)HAPPY2_NOD_DOWN, HAPPY2_NOD_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(HAPPY_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_to(0, (int16_t)HAPPY2_NOD_UP, HAPPY2_NOD_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(HAPPY_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ③ 抬头得意定格 */
    move_hold(HAPPY2_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 左右小摆 3 次（保持抬头，像"晃脑袋"）*/
    for (n = 0U; n < 3U; n++) {
        int16_t side = ((n & 1U) == 0U) ? -(int16_t)HAPPY2_YAW_SWING
                                        :  (int16_t)HAPPY2_YAW_SWING;

        move_to(side, (int16_t)HAPPY2_NOD_UP, HAPPY2_SWING_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(HAPPY_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ⑤ 转头看左边 → 停住（像在"看你看我看什么"）；再看右边 */
    move_to(-(int16_t)HAPPY2_YAW_SWING, (int16_t)HAPPY2_NOD_UP, HAPPY2_SWING_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(HAPPY2_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    move_to((int16_t)HAPPY2_YAW_SWING, (int16_t)HAPPY2_NOD_UP, HAPPY2_SWING_MS + 100U);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(HAPPY2_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 回中位 */
    move_center(HAPPY_RETURN_MS);
}

/* ==================== 变体③：绕圈庆祝 ====================
 * 轨迹：回中 → 沿一个椭圆走一整圈（8 段，每段 45°）→ 停一下 → 回中
 * 与①②的区别：是"连续位移"而不是往复摆动，观感最像"高兴地转了个圈"。
 * 幅度比 8 字动作（EIGHT）小得多（±200/±90 vs ±250/±120），负载更轻。 */
void moves_happy_v3_run(void)
{
    /* ① 回中 */
    move_center(HAPPY3_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 顺时针走一圈：8 个方位点，两轴始终同时（几乎）匀速 */
    move_to((int16_t)HAPPY3_YAW_AMP, 0, HAPPY3_STEP_MS);                  /* 右 */
    if (moves_interrupted() != 0) { return; }
    move_to((int16_t)HAPPY3_DIAG_YAW, -(int16_t)HAPPY3_DIAG_PITCH, HAPPY3_STEP_MS); /* 右下 */
    if (moves_interrupted() != 0) { return; }
    move_to(0, -(int16_t)HAPPY3_PITCH_AMP, HAPPY3_STEP_MS);               /* 下 */
    if (moves_interrupted() != 0) { return; }
    move_to(-(int16_t)HAPPY3_DIAG_YAW, -(int16_t)HAPPY3_DIAG_PITCH, HAPPY3_STEP_MS);/* 左下 */
    if (moves_interrupted() != 0) { return; }
    move_to(-(int16_t)HAPPY3_YAW_AMP, 0, HAPPY3_STEP_MS);                 /* 左 */
    if (moves_interrupted() != 0) { return; }
    move_to(-(int16_t)HAPPY3_DIAG_YAW, (int16_t)HAPPY3_DIAG_PITCH, HAPPY3_STEP_MS); /* 左上 */
    if (moves_interrupted() != 0) { return; }
    move_to(0, (int16_t)HAPPY3_PITCH_AMP, HAPPY3_STEP_MS);                /* 上 */
    if (moves_interrupted() != 0) { return; }
    move_to((int16_t)HAPPY3_DIAG_YAW, (int16_t)HAPPY3_DIAG_PITCH, HAPPY3_STEP_MS);  /* 右上 */
    if (moves_interrupted() != 0) { return; }

    /* ③ 停一下（像转完圈"耶"了一下）*/
    move_hold(HAPPY3_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 收尾回中 */
    move_center(HAPPY3_RETURN_MS);
}
