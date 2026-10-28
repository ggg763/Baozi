/******************************************************************************
 * @file     servo_pins.h
 * @brief    舵机 PWM 通路统一配置（基准）—— 换引脚/通道只改这一个文件
 *
 * ★ 舵机控制端（两路 csi_ch 不同，可同时独立输出）——【两路均已上板验证】
 *      水平 YAW   : PA29 -> PA29_PWM_CH6  (pinmap 6 -> csi_ch 3)
 *      垂直 PITCH : PA27 -> PA27_PWM_CH4  (pinmap 4 -> csi_ch 2)
 *
 *   【YAW 为什么从 PA0 改到 PA29（2026-09，为做 PCB）】
 *     PA0 不在开发板底部排针上、引出要额外走线；PA29 是底部排针【外排第 3 脚】，
 *     直接接线即可，省 PCB 工作量。选脚时的硬约束：
 *       ① 排针上能出 PWM 的只有 5 个脚：外排 PA29/PA30（csi 对 3）、
 *          内排 PA28/PA27/PA16（csi 对 2）—— PB 口【没有任何 PWM 复用】；
 *       ② 同一 csi 对的脚共享 load 寄存器、不能同时做独立 PWM
 *          → 两路必须"一对出一个"，没法把两个舵机都塞到同一侧；
 *       ③ 其中 PA16 被 MPU6050 占用、PA27 是现用 PITCH
 *          → 只剩 PA29 / PA30 可选（这两个同属 csi 对 3，只能挑一个）。
 *     ⚠️ 选了 PA29 之后，PA30 就【不能】再拿去做独立 PWM（同一对）
 *        —— 以后若加第三路舵机，请选 csi_ch ≠ 2 且 ≠ 3 的脚。
 *
 *   PA31（pinmap 8 -> csi_ch 4）【已让出】，留给后续同步控制 / LCD 背光等用途。
 *
 * ★★ 关键规则：csi_pwm_out_config() / csi_pwm_out_start() 的 channel 参数
 *     【不是】引脚手册上的 PWM 通道号，而是「pinmap channel ÷ 2」：
 *
 *         csi_ch = pinmap_channel / 2
 *
 *     依据：官方示例 board_config.h 里 EXAMPLE_PWM_CH_IDX = 4，而它配的引脚是
 *           PA31（复用宏 PA31_PWM_CH8，pinmap channel = 8）→ 8 / 2 = 4 ✓
 *     对照 devices.c 的 pwm_pinmap（{port, pin, idx, channel, func}）：
 *           PA0  (CH0, pinmap 0) -> csi_ch 0    PA1  (CH1, pinmap 1) -> csi_ch 0
 *           PA2  (CH2, pinmap 2) -> csi_ch 1    PA3  (CH3, pinmap 3) -> csi_ch 1
 *           PA4  (CH4, pinmap 4) -> csi_ch 2    PA15 (CH4, pinmap 4) -> csi_ch 2
 *           PA16 (CH5, pinmap 5) -> csi_ch 2    PA24 (CH1, pinmap 1) -> csi_ch 0
 *           PA25 (CH2, pinmap 2) -> csi_ch 1    PA26 (CH3, pinmap 3) -> csi_ch 1
 *           PA27 (CH4, pinmap 4) -> csi_ch 2    PA28 (CH5, pinmap 5) -> csi_ch 2
 *           PA29 (CH6, pinmap 6) -> csi_ch 3    PA30 (CH7, pinmap 7) -> csi_ch 3
 *           PA31 (CH8, pinmap 8) -> csi_ch 4
 *
 *     坑一：曾经把 PA27 配成 csi_ch 4（照搬引脚手册的 CH4），实际写到了"输出 8/9"
 *           那对寄存器（PA31 那一组）→ PA27 一直没有波形。正确是 csi_ch 2。
 *     坑二：csi_ch 相同的两个引脚属于同一对、共享一个 load 寄存器 —— PA0/PA1 都是
 *           csi_ch 0，波形必然同步，不能当两路独立 PWM（这才是早期"PA0/PA1 波形
 *           同源"的真正原因，与短路、pad 映射无关）。
 *     坑三：不同引脚的 PWM 复用【码值不同】：PA0~PA9 / PA15~PA18 是 3U，
 *           PA23~PA31 是 2U（见 soc.h）。换脚时必须换成对应引脚的枚举名，
 *           照抄旧引脚的码值会配不出 PWM。
 *
 * 已占用、不可用于舵机的引脚（与规划表一致）：
 *   PA2  LCD_CS        PA3  LCD_SCK        PB4~PB7  LCD D0~D3
 *   PB10 LCD_WAIT      PB15 LCD_RST        PA8/PA9  触摸 TP_SCL/SDA
 *   PA16/PA23 MPU6050 I2C0                 PA17/PA18 Console UART0（禁复用）
 *   PB18/PB19 ESP32 UART3                  PB29/PB22/PB23 IR 三路输入
 *   PB12/PB13 ASR-PRO UART                 PA19/PA20 JTAG
 *   PA31 预留（同步控制 / LCD 背光）       PA0 【换脚后已空闲】
 *
 * 可用的备选 PWM 引脚（引脚 / pinmap_ch / csi_ch）：
 *   PA0 (CH0/0 -> csi 0)    PA4 (CH4/4 -> csi 2)    PA5 (CH5/5 -> csi 2)
 *   PA6 (CH6/6 -> csi 3)    PA7 (CH7/7 -> csi 3)    PA24(CH1/1 -> csi 0)
 *   PA25(CH2/2 -> csi 1)    PA26(CH3/3 -> csi 1)    PA28(CH5/5 -> csi 2)
 *   PA30(CH7/7 -> csi 3)    PA31(CH8/8 -> csi 4)
 *   两路要选 csi_ch【不同】的引脚，否则共享 load 寄存器、输出必然同步。
 *   其中【底部排针可达】的只有：外排 PA29（已用）/ PA30、内排 PA28。
 *   PA11~PA14 无外设、也无 PWM 复用。
 *
 * 换脚方法：只改下面 YAW / PITCH 两组 5 行宏，其余代码不用动
 * （servo_core.c 的 ch_index() / clamp_pulse() 都是拿 SERVO_*_CH 宏比较的，
 *   不会硬编码通道号；servo_range_test.c 用 IDX_YAW/IDX_PITCH 作数组下标）。
 ******************************************************************************/
#ifndef _SERVO_PINS_H_
#define _SERVO_PINS_H_

#include <soc.h>        /* PORTA / PIN29 / pin_func_t 等 */

/* PWM 控制器序号（csi_pwm_init 的 idx） */
#define SERVO_PWM_IDX       0U

/* 舵机周期：20ms = 50Hz */
#define SERVO_PERIOD_US     20000U

/* ---- 水平 YAW：PA29 / PA29_PWM_CH6（底部排针【外排第 3 脚】）----
 * csi_ch = pinmap 6 / 2 = 3。
 * ⚠️ PA29~PA31 这批的复用码是 2U（不是 PA0 那批的 3U），已用宏名表达，勿改数值。*/
#define SERVO_YAW_CH        3U          /* csi_ch = pinmap 6 / 2 */
#define SERVO_YAW_PORT      PORTA
#define SERVO_YAW_PIN       PIN29
#define SERVO_YAW_FUNC      PA29_PWM_CH6
#define SERVO_YAW_PIN_TXT   "PA29"

/* ---- 垂直 PITCH：PA27 / PA27_PWM_CH4（底部排针【内排第 14 脚】）----
 * csi_ch = pinmap 4 / 2 = 2 —— 注意【不是】引脚手册上的 CH4，用 4 会写到 PA31 那组寄存器 */
#define SERVO_PITCH_CH      2U          /* csi_ch = pinmap 4 / 2 */
#define SERVO_PITCH_PORT    PORTA
#define SERVO_PITCH_PIN     PIN27
#define SERVO_PITCH_FUNC    PA27_PWM_CH4
#define SERVO_PITCH_PIN_TXT "PA27"

#endif /* _SERVO_PINS_H_ */
