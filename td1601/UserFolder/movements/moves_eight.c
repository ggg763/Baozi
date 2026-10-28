/******************************************************************************
 * @file     moves_eight.c
 * @brief    八字环绕（EIGHT）动作：脖子画一个横 8 字（∞）
 *
 * 身体语义：两个舵机是机器人的"脖子"——YAW 左右转头、PITCH 抬头/低头。
 * 这个动作让脖子走一条 8 字（利萨茹）曲线：
 *     yaw   = EIGHT_YAW_AMP   × sin(θ)     ← 左右转一整圈
 *     pitch = EIGHT_PITCH_AMP × sin(2θ)    ← 上下点两圈
 * 两者合成，就是一笔横着的 ∞ —— 脖子"环绕"一圈一圈地转。
 *
 * 【与摇头/点头类动作的区别】8 字是【曲线轨迹】，按参数方程逐点算出来
 * （实现见 moves.c 的 move_figure8 + 整数正弦查表 sin1000），
 * 所以不用手写几十个点；改参数就是改幅度/圈速，轨迹自动跟着变。
 *
 * 幅度（±250us 宽 / ±120us 高）都在实测行程内（YAW 300~1700、PITCH 800~1600），
 * 不会顶极限；参数集中在 moves.h 的 EIGHT_* 宏里。
 ******************************************************************************/

#include <stdint.h>
#include "moves.h"

void moves_eight_run(void)
{
    /* ① 先回中：8 字的起笔就在中心 */
    move_center(EIGHT_ENTER_MS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ② 画 EIGHT_LAPS 圈 8 字（内部逐点插值、可中断、画完自动回中）*/
    move_figure8((int16_t)EIGHT_YAW_AMP, (int16_t)EIGHT_PITCH_AMP,
                 EIGHT_LAP_MS, EIGHT_LAPS);
    if (moves_interrupted() != 0) {
        return;
    }

    /* ③ 收笔再回一次中位，保证停在正前方 */
    move_center(EIGHT_RETURN_MS);
}
