/******************************************************************************
 * @file     face_render.h
 * @brief    屏幕传输层 —— 只负责"把一组部件画到屏上"，不含任何表情语义
 *
 * 分工（v9 重构后的边界，比原来更窄）：
 *   face_draw.c    几何 → 逐行覆盖度（纯计算，可在 PC 上跑）
 *   face_model.c   姿势表 + 眨眼/视线/张嘴的几何变换（纯计算，可在 PC 上跑）
 *   face_action.c  时间 → 表情参数（纯计算）
 *   face_app.c     状态机：切姿势、过渡、节流重绘（不碰 DMA）
 *   face_render.c  ← 本文件：行缓冲 → 分段 DMA，以及总线仲裁回调
 *
 * 这样切的原因：本文件是唯一依赖外设头文件（video.h / drv）的地方，
 * 其余四个都能用 mingw gcc 在 PC 上编译出图，与浏览器模拟器逐像素对比。
 * 上板前就能把形状、动作、过渡全部验证掉。
 *
 * 传输形态保持 2026-10 已验证的那一套（不要改）：
 *   整屏窗口 480×480，每 8 行一段 DMA，双缓冲交替，每段后 LCD_WaitIdle()，
 *   段前后各调一次 s_bus_cb 让舵机 PWM 有空窗可写 —— 这是"舵机一动屏幕就白块"
 *   的解法，已经上板验证过。
 ******************************************************************************/

#ifndef _FACE_RENDER_H_
#define _FACE_RENDER_H_

#include <stdint.h>
#include "face_draw.h"              /* fd_part_t / FD_W / FD_H */

/* 黑底。屏幕是 RGB565，背景固定黑（省电、对比度高）*/
#define FACE_COL_BG   0x0000

/* 单帧最多部件数（含镜像前）。真值在 face_draw.h 的 FD_MAX_PARTS。 */
#define FACE_MAX_PARTS FD_MAX_PARTS

/* ★ 总线忙回调（解决"舵机一动、屏幕就闪白块"）
 *
 * 整屏刷新是 60 段 DMA，DMA 要持续从 SRAM 取数喂 SPI FIFO（走 AHB）。
 * 若此时有别的外设也写寄存器（典型：舵机每 20ms 写 PWM0CMP），AHB 被抢占
 * → DMA 取数延迟 → SPI FIFO 被抽空 → 那段像素时序断 → 屏幕出白块。
 *
 *   cb(1) = 即将开始一段 DMA（外部应挂起会走 AHB 的寄存器写）
 *   cb(0) = 一段刚发完、DMA 空闲（外部可安全补写）
 * 每帧各调用 60 次，空窗 ≈1ms/次。
 *
 * 注册示例：face_set_bus_callback(servo_bus_event);   （见 servo_core.h）*/
typedef void (*face_bus_cb_t)(int busy);
void face_set_bus_callback(face_bus_cb_t cb);

/* 屏幕初始化（含官方 video_init 与三色自检），调用一次 */
void face_init(void);

/* 画一帧：parts 里的部件逐个累加覆盖度，按 fg 调色后整屏发出。
 * 调用一次 ≈20ms（480×480 @ QSPI 4线 50MHz），非阻塞式 DMA 但本函数会等发完，
 * 所以上层必须节流（见 face_app.c 的 FACE_REDRAW_MIN_MS）。 */
void face_show_parts(const fd_part_t *parts, int n, uint16_t fg);

/* 分段行数（1..8，默认 8）。仅用于排查闪烁：
 * 若屏幕上的矩形块个数随它变化，就说明问题在"分段/DMA"侧。 */
void face_set_segrows(int n);
int  face_get_segrows(void);

#endif /* _FACE_RENDER_H_ */
