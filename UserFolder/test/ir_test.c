/******************************************************************************
 * @file     ir_test.c
 * @brief    三线红外避障模块 简单测试（PB22 / PB23 / PB29，电平变化打印到串口）
 *
 * 定案引脚（2026-08-30，理由见 UserFolder/doc/pin_mux_table.md 第 8 节）：
 *   IR Left   = PB22   （原 TDM_SCLKOUT，已实测可用）
 *   IR Center = PB23   （原 TDM_WSOUT）
 *   IR Right  = PB29   （原 TDM_D1）
 *   三脚同属 TDM 族、功能值都是 func0、TDM 未被使用 —— 是 PORTB 上唯一
 *   "谁都不沾"的一组（避开 UART3 流控 PB20/PB21、LCD 数据线 PB8~PB11、
 *   console 的 DE/RE PB0/PB1、JTAG 的 PA20）。
 *
 *   模块供电：VCC 接 3V3（或 5V）、GND 与核心板共地。
 *   若左右探头接反了，只需交换下面 IR_LEFT_PIN / IR_RIGHT_PIN 两行。
 *
 * 行为：
 *   - 上电打印一行引脚/寄存器自检（下面说明怎么看）；
 *   - 每 50ms 轮询三路，电平变化时打印一次；
 *   - 无论有无变化，每 1s 打一行心跳，避免"没输出"和"程序跑死"分不清。
 *   极性：常见避障模块"检测到障碍时输出低电平"，也可能相反，看打印判断。
 *
 * ⚠️ 自检行怎么看：
 *   [IR] selfcheck: PORTCTL1=0x00000000 IOMUX0H=0x00000000 IOPU0=... ANA1=...
 *   - PORTCTL1 (0x60030100, PORTB 的 GPIO/复用总开关，1=还在复用模式)：
 *     bit22 / bit23 / bit29 应为 0（说明已切到 GPIO）；
 *   - IOMUX0H  (0x6003010C, PB16~PB31 的功能选择)：
 *     PB22 占 bit12~13、PB23 占 bit14~15、PB29 占 bit26~27，都应为 0；
 *   - IOPU0    (0x60030120, 内部上拉使能)：
 *     bit22 / bit23 / bit29 应为 1（否则开漏输出的模块读不到高电平）；
 *   - ANA1     (0x60030170, 模拟功能/ADC 使能)：
 *     bit22(PB22) / bit23(PB23) / bit29(PB29) 应为 0。
 *     位号 = 该脚 ADC 功能值 - 5，恰好等于引脚号；若为 1，说明该脚被切到
 *     模拟通路，数字输入会失效（典型表现就是恒定读到 0）。
 *
 *   ⚠️ 为什么要专门打印：官方 pinmux.c 在 PORTB 分支漏了 offset-=16，移位量
 *     会 ≥32（对 uint32_t 是未定义行为），只是靠 RISC-V 的 sll 取低 5 位才
 *     "碰巧正确"。所以这三个脚到底切没切过去，必须看寄存器，不能凭感觉。
 *
 * ⚠️ 某一路"恒定低电平"怎么查（三步，按顺序做）：
 *   ① 把那一路探头的信号线从排针上拔掉，再看打印：
 *        拔掉后变高(1) -> 引脚与上拉都正常，问题在探头/接线：模块没供电、
 *                         VCC/GND/OUT 接反、探头一直对着障碍物，或模块是
 *                         "无遮挡也输出低"的型号；
 *        拔掉后仍低(0) -> 引脚被板上别的东西拉低，或 mux/上拉没生效 -> 做 ② ③。
 *   ② 看自检行：PORTCTL1 对应位非 0 = mux 没切过来；IOPU0 对应位为 0 = 上拉
 *      没生效；ANA1 对应位为 1 = 该脚处在模拟通路。
 *   ③ 把同一个探头换到已知可用的 PB22 上试：
 *        换过去也恒低 -> 是探头/线/供电的问题，与引脚无关；
 *        换过去正常   -> 是该引脚位置特有的问题（改用 PB30 / PB31）。
 *
 * ⚠️ 电平注意：TD1601 GPIO 为 1.8V 域。
 *   1) 模块输出为开漏/集电极开路(OC) -> 直接用（本文件已开内部上拉）；
 *   2) 模块输出为 3.3V 推挽 -> 会向 1.8V IO 灌入过高电压，需分压/电平转换，
 *      否则损伤芯片。⚠️ 若某一路"接了探头就恒低、拔掉就正常"，且模块是 3.3V/
 *      5V 供电的推挽输出，要先怀疑灌流导致的钳位 —— 用万用表量一下模块输出
 *      高电平是几伏，别把该脚继续接在这种模块上。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <soc.h>
#include <drv/gpio.h>
#include <drv/pin.h>
#include <drv/common.h>     /* mdelay */
#include "board_config.h"

/* ---- 三路红外输入（PORTB，全部用位掩码 PINxx） ---- */
#define IR_LEFT_PIN     PIN22
#define IR_CENTER_PIN   PIN23
#define IR_RIGHT_PIN    PIN29
#define IR_ALL_PIN      (IR_LEFT_PIN | IR_CENTER_PIN | IR_RIGHT_PIN)

/* PORTB 的 GPIO 控制器 idx = 1（PORTA 是 0，见 devices.c） */
#define IR_GPIO_IDX     1

/* IO 控制寄存器（地址来源：chip_td1601/v1.0/drivers/pinmux.c），仅用于启动自检 */
#define REG_PORTB_PORTCTL   0x60030100U
#define REG_PORTB_IOMUX0H   0x6003010CU
#define REG_PORTB_IOPU0     0x60030120U   /* PORTB 内部上拉使能 */
#define CSKY_ANA1_IO_EN_REG 0x60030170U   /* PORTB 模拟功能(ADC)使能 */

static csi_gpio_t s_ir_gpio;

/*****************************************************************************
 * ir_test: 红外传感器测试入口（被 src/main.c 调用）
 *****************************************************************************/
void ir_test(void)
{
    uint8_t  last = 0xFFU;
    uint32_t tick = 0U;

    printf("\r\n[IR] test start: L=PB22 C=PB23 R=PB29 (1=high 0=low)\r\n");

    /* 三个脚默认是 TDM 的 func0，必须显式切 GPIO：
     * 清 PORTCTL 位 + 清 IOMUX 功能字段（由 csi_pin_set_mux 完成） */
    csi_pin_set_mux(PORTB, IR_LEFT_PIN,   PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, IR_CENTER_PIN, PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTB, IR_RIGHT_PIN,  PIN_FUNC_GPIO);

    /* 内部上拉到 1.8V：适配开漏输出的模块（未触发时读到高） */
    csi_pin_mode(PORTB, IR_LEFT_PIN,   GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, IR_CENTER_PIN, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, IR_RIGHT_PIN,  GPIO_MODE_PULLUP);

    if (csi_gpio_init(&s_ir_gpio, IR_GPIO_IDX) != CSI_OK) {
        printf("[IR] gpio init fail\r\n");
        return;
    }
    csi_gpio_dir(&s_ir_gpio, IR_ALL_PIN, GPIO_DIRECTION_INPUT);

    /* 一行自检：确认 mux/上拉/模拟功能都对（判读方法见文件头注释） */
    printf("[IR] selfcheck: PORTCTL1=0x%08X IOMUX0H=0x%08X IOPU0=0x%08X ANA1=0x%08X\r\n",
           *(volatile uint32_t *)REG_PORTB_PORTCTL,
           *(volatile uint32_t *)REG_PORTB_IOMUX0H,
           *(volatile uint32_t *)REG_PORTB_IOPU0,
           *(volatile uint32_t *)CSKY_ANA1_IO_EN_REG);

    /* 先无条件打印一次初始电平 */
    {
        uint8_t st0 = 0U;
        if (csi_gpio_read(&s_ir_gpio, IR_LEFT_PIN)   != 0U) { st0 |= 0x01U; }
        if (csi_gpio_read(&s_ir_gpio, IR_CENTER_PIN) != 0U) { st0 |= 0x02U; }
        if (csi_gpio_read(&s_ir_gpio, IR_RIGHT_PIN)  != 0U) { st0 |= 0x04U; }
        last = st0;
        printf("[IR] L=%u C=%u R=%u  (0x%02X)\r\n",
               (unsigned)((st0 >> 0) & 1U),
               (unsigned)((st0 >> 1) & 1U),
               (unsigned)((st0 >> 2) & 1U),
               (unsigned)st0);
    }

    for (;;) {
        uint8_t st = 0U;

        if (csi_gpio_read(&s_ir_gpio, IR_LEFT_PIN)   != 0U) { st |= 0x01U; }
        if (csi_gpio_read(&s_ir_gpio, IR_CENTER_PIN) != 0U) { st |= 0x02U; }
        if (csi_gpio_read(&s_ir_gpio, IR_RIGHT_PIN)  != 0U) { st |= 0x04U; }

        /* 变化立即打印；没变化也每 1s 打一行心跳（50ms x 20） */
        if ((st != last) || ((++tick % 20U) == 0U)) {
            printf("[IR] L=%u C=%u R=%u  (0x%02X)%s\r\n",
                   (unsigned)((st >> 0) & 1U),
                   (unsigned)((st >> 1) & 1U),
                   (unsigned)((st >> 2) & 1U),
                   (unsigned)st,
                   (st != last) ? "  <-changed" : "");
            last = st;
        }

        mdelay(50);
    }
}
