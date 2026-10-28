/******************************************************************************
 * @file     app_core.h
 * @brief    主循环编排层（合并版唯一入口的"心跳"）
 *
 * 为什么需要这一层：合并后工程里有两套"会阻塞很久"的动作
 *   ① 舵机动作库 moves_play_variant() —— 一次播 1.5~7 秒；
 *   ② 整屏刷屏 —— 一次 480x480 要 76~200ms。
 * 如果屏幕/触摸只在 main 的 while(1) 里推进，①一播，屏幕就冻住、触摸也没反应。
 *
 * 所以把"触摸 + UI 状态机 + 分片刷屏 + 串口"收成一个 app_core_step()：
 *   · main 的主循环调它；
 *   · emotion_fsm 的 20ms 播放钩子（moves 播放期间每 20ms 回调）也调它。
 * 二者不会重复计时 —— 内部用 csi_tick_get() 算【真实】dt，谁先来谁推进。
 ******************************************************************************/
#ifndef _APP_CORE_H_
#define _APP_CORE_H_

#include <stdint.h>

/* 初始化：屏幕 + 参数化脸（ui_app_init）、触摸（ICNT8952BN）、
 * 协议串口（UART0 与 printf 共线）、ESP32 串口（UART3）。
 * 调用顺序要求：必须在 emotion_fsm_init() 之前（屏幕先就绪）。*/
void app_core_init(void);

/* 推进一步（非阻塞，内部按真实时间算 dt）。可反复调用，自带重入保护。*/
void app_core_step(void);

/* ---- 供动作/情感层查询"用户是不是正在操作屏幕" ----
 * 用途（2026-11 需求）：用户手指在屏幕上时机器人必须停手，避免误触/夹手。 */
int      app_core_touch_down(void);    /* 1 = 手指此刻按在屏上 */
uint32_t app_core_touch_idle_ms(void); /* 距最后一次触摸过去了多少毫秒 */

#endif /* _APP_CORE_H_ */
