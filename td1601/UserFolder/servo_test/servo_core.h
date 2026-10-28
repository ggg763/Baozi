/******************************************************************************
 * @file     servo_core.h
 * @brief    舵机控制层（基于 servo_pins.h 的通路配置）—— 供动作/情绪代码调用
 *
 * 比"直接操作 csi_pwm"省事一层：
 *   servo_core_init()              引脚复用 + PWM 控制器初始化 + compare 校准 + 两路回中位 + 打开输出
 *   servo_set_pulse(ch, pulse_us)  软限位钳制 + **只写 compare**（脉宽）
 *
 * ★ 2026-09：输出方式由"每步 stop→config→start"改成【只写 compare】——
 *   前者每步都会关一下输出使能，两路关的时刻不同 → 8 字这类两轴同时运动的动作
 *   会看起来"两轴不同步/有时延"。详见 servo_core.c 头部说明。
 *
 * 通道直接用 servo_pins.h 的 SERVO_YAW_CH / SERVO_PITCH_CH（当前 YAW=PA29/csi_ch 3、
 * PITCH=PA27/csi_ch 2）。中位/限位沿用历史实测值，换舵机后需重新标定 —— 可烧
 * servo_range_test 用串口命令 `w<us>` 逐步试出中位与极限。
 ******************************************************************************/
#ifndef _SERVO_CORE_H_
#define _SERVO_CORE_H_

#include <stdint.h>
#include "servo_pins.h"

/* 中位脉宽（us）—— 【2026-09 上板实测】 */
#define SERVO_YAW_CENTER     1000U      /* 水平 PA29 实测中位（2026-09 由 PA0 换到 PA29，中位不变）*/
#define SERVO_PITCH_CENTER   1100U      /* 垂直 PA27 实测中位 */

/* 行程限位（us）—— 【2026-09 上板实测】servo_range_test 探出的安全范围（能到达且不堵转）
 * 想再留余量就把 MIN 加几十、MAX 减几十；换舵机后需重新标定。 */
#define SERVO_YAW_MIN        300U       /* 水平实测下限 */
#define SERVO_YAW_MAX        1700U      /* 水平实测上限 */
#define SERVO_PITCH_MIN      800U       /* 垂直实测下限 */
#define SERVO_PITCH_MAX      1600U      /* 垂直实测上限 */

/* 初始化：引脚复用 + PWM 控制器 + 两路回中位。返回 0 成功，-1 失败 */
int servo_core_init(void);

/* 设置某通道脉宽（us，自动限位钳制并做输出闸门修正）。
 * 返回 0 成功；传入不属于本工程的通道号返回 -1。 */
int servo_set_pulse(uint32_t ch, uint32_t pulse);

/* ★ 2026-10：总线仲裁钩子 —— 解决"舵机一动、屏幕就闪白块"
 *
 * 背景：LCD 整屏刷新是 60 段 DMA，DMA 要持续从 SRAM 取数喂 SPI FIFO（走 AHB）；
 *       而本文件改脉宽是写 PWM 寄存器（也走 AHB）。两者撞上 → DMA 取数延迟 →
 *       SPI FIFO 被抽空 → 那一段像素时序断 → 屏幕上出现错位白块。
 *       实测：把 PWM 写入短路掉（DRY_RUN）就完全不闪；舵机一动，尤其是眨眼
 *       这种高频重绘时就闪 —— 与"总线争用"的推断完全吻合。
 *
 * 解法：让 PWM 写入避开 DMA 窗口。渲染端在"开始/结束传输"以及"每段之间的空隙"
 *       调用本函数；busy=1 时写入只登记不落地，busy=0 时立刻补写。
 *       一帧有 60 个段间空隙，所以舵机最多滞后一个段（≈1ms），完全无感。
 *
 * 用法（渲染端）：face_set_bus_callback(servo_bus_event);
 */
void servo_bus_event(int busy);

#endif /* _SERVO_CORE_H_ */
