/******************************************************************************
 * @file     moves_angry.c
 * @brief    生气（ANGRY）动作：3 个变体，随机呼出（每个都是多段长链）
 *
 * 情绪来源：PC 下发 EMO:ANGRY → 连续状态 happiness=200 / energy=900 / curiosity=300
 *          → 【低愉悦 + 高精力】正是"生气"的数值特征 → 调制算法放大速度（spd 接近上限）。
 *
 * 身体语义：昂着头（不服气）+ 甩头。节奏刻意做成"硬"：与 SAD 的慢、LOVING 的柔相反。
 *
 * 【变体池】3 个变体（moves_play_variant 随机呼出，不与上次相同）：
 *   ① moves_angry_run()     甩头     —— 昂头 → 甩头两轮（中间低头瞪一下）→ 回中     ≈4s
 *   ② moves_angry_v2_run()  瞪着你   —— 昂头长定格 → 小幅快抖（不满）→ 转头不看你了  ≈4.5s
 *   ③ moves_angry_v3_run()  生闷气   —— 低头 → 久停 → 慢慢抬一半 → 突然甩头 → 又低下去 ≈6.5s
 *
 * 生气动作【本质短促】，所以这三条链比 idle/sad 短；但每条内部有 5~7 个段落变化、
 *   三个变体性格完全不同，配合随机呼出同样不会显得机械。
 *
 * 【到位与速度】（必须先读）原来单次甩头 90ms 意味着需求速度 6.4us/ms，贴着舵机极限
 *   （约 6.7us/ms）→ 实际是"没甩到位就反向"，看着摆幅偏小。现在：
 *     · 单次甩头 ANGRY_SWING_MS = 130ms（265×1.2×2/130 ≈ 4.9us/ms）
 *     · 每个极点后加 ANGRY_SETTLE_MS(40ms) 让舵机咬住位置
 *
 * 【参数位置约定】主变体①的参数在 moves.h 的 ANGRY_*；②③的参数就近定义在本文件顶部
 *   （前缀 ANGRY2_/ANGRY3_），①里新增的段落用 ANGRY_V1_*。
 *
 * 限位余量（YAW 中位 1000、限位 300~1700；angry 的 amp≈975、上限 1200）：
 *   常态 265×1.2×0.975 ≈ 310us → 690~1310；极端 265×1.2×1.2 ≈ 382us → 618~1382。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

/* ---- 变体①（甩头）新增的段落 ---- */
#define ANGRY_V1_GLARE_MS      500U  /* 昂头瞪着的定格 */
#define ANGRY_V1_LOWER         70    /* "低头瞪一下"的低头量（us，向下）*/
#define ANGRY_V1_LOWER_MS      400U  /* 低下去的时间 */
#define ANGRY_V1_LOWER_HOLD_MS 500U  /* 低位停住 */
#define ANGRY_V1_UP_MS         300U  /* 再昂起来的时间 */
#define ANGRY_V1_FAST_SWING   176   /* 第二轮小甩的幅度（us，±）= 265×2/3 */
#define ANGRY_V1_FAST_MS       100U  /* 第二轮小甩的时间（更快）*/

/* ---- 变体②（瞪着你）参数 ---- */
#define ANGRY2_UP_MS           200U  /* 昂头时间 */
#define ANGRY2_GLARE_MS       2000U  /* 长定格瞪着的时长（"我就看着你"）*/
#define ANGRY2_TWITCH         100   /* 不满时小幅快抖的幅度（us，±）*/
#define ANGRY2_TWITCH_MS       90U   /* 抖一下的时间 */
#define ANGRY2_TWITCH_HOLD_MS  60U   /* 抖动的极点停顿 */
#define ANGRY2_LOOK           120   /* 转头到一侧的幅度（us，±，不看你了）*/
#define ANGRY2_LOOK_MS        500U   /* 转过去的时间 */
#define ANGRY2_SULK_MS       1000U   /* 转过去后赌气的停顿 */

/* ---- 变体③（生闷气）参数 ---- */
#define ANGRY3_DOWN           100   /* 低头的量（us，向下）*/
#define ANGRY3_DOWN_MS        700U  /* 低头时间 */
#define ANGRY3_QUIET_MS      1500U  /* 闷着不动的时长 */
#define ANGRY3_LIFT_MS        900U  /* 慢慢抬到一半的时间 */
#define ANGRY3_SWING          212   /* 突然甩头的幅度（us，±）= 265×0.8 */
#define ANGRY3_SWING_MS       150U  /* 甩头时间 */
#define ANGRY3_FALL_MS        900U  /* 又垂下去的时间 */
#define ANGRY3_REST_MS       1200U  /* 垂下去后的停顿 */

/* ==================== 变体①：甩头 ==================== */
void moves_angry_run(void)
{
    uint8_t n;

    /* ① 昂头（不服气）*/
    move_to(0, (int16_t)ANGRY_PITCH_UP, ANGRY_LIFT_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 第一轮：左右各甩两次（每个极点停一下，等舵机到位）*/
    for (n = 0U; n < 4U; n++) {
        int16_t side = ((n & 1U) == 0U) ? -(int16_t)ANGRY_YAW_SWING
                                        :  (int16_t)ANGRY_YAW_SWING;

        move_to(side, (int16_t)ANGRY_PITCH_UP, ANGRY_SWING_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(ANGRY_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ③ 昂着头定格瞪着 */
    move_hold(ANGRY_V1_GLARE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 低头瞪一下（把怒气"压"下去）→ 停 → 又昂起来 */
    move_to(0, -(int16_t)ANGRY_V1_LOWER, ANGRY_V1_LOWER_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(ANGRY_V1_LOWER_HOLD_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_to(0, (int16_t)ANGRY_PITCH_UP, ANGRY_V1_UP_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 第二轮：更小幅度但更快的甩头（比第一轮更"冲"）*/
    for (n = 0U; n < 4U; n++) {
        int16_t side = ((n & 1U) == 0U) ? (int16_t)ANGRY_V1_FAST_SWING
                                        : -(int16_t)ANGRY_V1_FAST_SWING;

        move_to(side, (int16_t)ANGRY_PITCH_UP, ANGRY_V1_FAST_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(ANGRY_SETTLE_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ⑥ 摆正但仍抬着头（"哼"），再回中 */
    move_to(0, (int16_t)ANGRY_PITCH_UP, ANGRY_RETURN_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_center(ANGRY_RETURN_MS);
}

/* ==================== 变体②：瞪着你 ====================
 * 轨迹：昂头 → 长定格瞪着 → 小幅快抖两下（不满）→ 转头到一侧（不看你了）→ 赌气停住 → 回中
 * 与①的区别：几乎不摆头，靠"长定格 + 快速小抖 + 赌气转开"表达生气，更"憋着"。 */
void moves_angry_v2_run(void)
{
    uint8_t n;

    /* ① 昂头 */
    move_to(0, (int16_t)ANGRY_PITCH_UP, ANGRY2_UP_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 长定格瞪着（这条变体的主体）*/
    move_hold(ANGRY2_GLARE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 小幅快抖两下（"啧"的那种不满），每个极点停一下 */
    for (n = 0U; n < 2U; n++) {
        int16_t side = ((n & 1U) == 0U) ? -(int16_t)ANGRY2_TWITCH
                                        :  (int16_t)ANGRY2_TWITCH;

        move_to(side, (int16_t)ANGRY_PITCH_UP, ANGRY2_TWITCH_MS);
        if (moves_interrupted() != 0) {
            return;
        }
        move_hold(ANGRY2_TWITCH_HOLD_MS);
        if (moves_interrupted() != 0) {
            return;
        }
    }

    /* ④ 甩一眼转开（不看你了）*/
    move_to((int16_t)ANGRY2_LOOK, (int16_t)ANGRY_PITCH_UP, ANGRY2_LOOK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 赌气停一会儿 */
    move_hold(ANGRY2_SULK_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 回中位 */
    move_center(ANGRY_RETURN_MS);
}

/* ==================== 变体③：生闷气 ====================
 * 轨迹：低头 → 闷着不动很久 → 慢慢抬到一半 → 突然甩一下头 → 又垂下去 → 停 → 回中
 * 与①②的区别：幅度最小、停顿最长，是"闷在心里"的那种生气。 */
void moves_angry_v3_run(void)
{
    /* ① 慢慢低头（憋着）*/
    move_to(0, -(int16_t)ANGRY3_DOWN, ANGRY3_DOWN_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 闷着不动（长停顿）*/
    move_hold(ANGRY3_QUIET_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 慢慢抬起来一半 */
    move_to(0, (int16_t)(ANGRY3_DOWN / 2), ANGRY3_LIFT_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ④ 突然甩一下头（爆发），左右各一次，极点停一下 */
    move_to(-(int16_t)ANGRY3_SWING, (int16_t)(ANGRY3_DOWN / 2), ANGRY3_SWING_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(ANGRY_SETTLE_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_to((int16_t)ANGRY3_SWING, (int16_t)(ANGRY3_DOWN / 2), ANGRY3_SWING_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(ANGRY_SETTLE_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑤ 又垂下去（火气下去了）*/
    move_to(0, -(int16_t)ANGRY3_DOWN, ANGRY3_FALL_MS);
    if (moves_interrupted() != 0) {
        return;
    }
    move_hold(ANGRY3_REST_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ⑥ 回中位 */
    move_center(ANGRY_RETURN_MS);
}
