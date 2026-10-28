/******************************************************************************
 * @file     servo_range_test.h
 * @brief    舵机活动范围测试（串口交互单步）—— 接口与默认参数
 *
 * 本文件夹（UserFolder/servo_test/）集中存放舵机调试/测试代码。
 *   通路配置统一在 servo_pins.h（换脚只改那里）：
 *     水平自由度 YAW   = PA29 -> PA29_PWM_CH6（pinmap 6 -> csi ch 3）
 *     垂直自由度 PITCH = PA27 -> PA27_PWM_CH4（pinmap 4 -> csi ch 2）
 *
 * 所有参数都是"实测可改"的量：先靠本测试交互探出真实极限，
 * 再把极限值填到控制层限位宏里。
 ******************************************************************************/
#ifndef _SERVO_RANGE_TEST_H_
#define _SERVO_RANGE_TEST_H_

#include <stdint.h>
#include "servo_core.h"     /* 中位/限位唯一来源（SERVO_YAW_CENTER 等）；间接带回 servo_pins.h 的通路宏 */

/* ---- 中位脉宽（us）：上电先回到这里 ----
 * 【重要】中位的唯一来源是 servo_core.h 的 SERVO_YAW_CENTER / SERVO_PITCH_CENTER，
 * 本文件【不再】重复 #define —— 早前这里与 servo_core.h 各定义一份，值虽相同，
 * 但改一处另一处就漂移（2026-09 换脚时就差点踩到）。
 * 本工具另保持一套较宽的软限位（SERVO_DEFAULT_MIN_US~MAX_US）便于换舵机后重新标定；
 * 运行时真正生效的安全限位在 servo_core.h（水平 300~1700、垂直 800~1600）。 */

/* ---- 步进与限位（us） ---- */
#define SERVO_STEP_US        20U        /* +/- 命令默认步长 */
#define SERVO_HARD_MIN_US    200U       /* 绝对下限：软限位不得低于此值 */
#define SERVO_HARD_MAX_US    2700U      /* 绝对上限：软限位不得高于此值 */
#define SERVO_DEFAULT_MIN_US 200U       /* 各通道默认软限位下限 */
#define SERVO_DEFAULT_MAX_US 2700U      /* 各通道默认软限位上限 */

/* ---- 串口 ---- */
#define SERVO_CMD_BAUD       115200U    /* 与 board_init.c 的 console 保持一致 */
#define SERVO_RX_TIMEOUT_MS  100U       /* 单字节接收超时 */

/* 交互式活动范围测试入口（被 src/main.c 调用，内部死循环不返回） */
void servo_range_test(void);

#endif /* _SERVO_RANGE_TEST_H_ */
