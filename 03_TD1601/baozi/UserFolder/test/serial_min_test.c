/******************************************************************************
 * @file     serial_min_test.c
 * @brief    最简串口自检 —— 排查"串口有点小问题"用
 *
 * 设计目标：用最少的代码、最少的依赖（只有 printf + 轮询接收，不用中断、
 *           不碰屏幕/舵机/其它外设），把四类典型故障一次区分开：
 *
 *   ① TX 完全没输出   → banner 一个字都看不到
 *   ② 波特率不对/丢字 → banner 能看到但乱码；或 [scale]/[ascii] 行对不齐、缺字符
 *   ③ RX 收不到       → 敲键盘没有 [rx] 回应（但 [hb] 心跳正常）
 *   ④ 中途卡死        → [hb] 心跳停住（tick 不再增加）
 *
 * 屏幕上的四样东西（烧录后从上往下看）：
 *   1) [serial] banner  —— 说明 TX 通路 + 波特率
 *   2) [scale] 0123456789 × 8 —— 80 个字符连排，丢字/错位一眼可见
 *   3) [ascii] !"#$... 64 个可见字符 —— 哪个字符区间出问题一眼可见
 *   4) 主循环：[hb] tick=N rx=M 每秒一行（证明没卡死）+
 *              [rx] 0xNN 'x' n=M 每收到一个字节回显一行
 *
 * 为什么用轮询而不是中断：这就是"最简"——中断入口、DMA、屏幕刷新都可能引入
 * 额外变量；排查串口本身时应该先把它们排除掉。
 *
 * ⚠️ 串口输出一律纯 ASCII 英文（本项目约定，中文只放注释）。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/uart.h>
#include "console/sys_console.h"        /* sys_console_t / console_init */
#include "drv/common.h"                 /* mdelay */
#include "board_config.h"
#include "test/serial_min_test.h"

/* console 对象定义在 src/board_init.c（那里 board_init() 调 console_init(&console)）。
 * 发送走 printf，这里只借它的 .uart 做接收 ——
 * 与 emotion_test.c / emotion_fsm.c 同款写法。 */
extern sys_console_t console;

/* 收到不可打印字节（含 \r \n）时用这个占位，避免终端里看不出东西 */
#define SERIAL_DOT              '.'

#define SERIAL_SCALE_GROUPS     8       /* [scale] 行重复几组 0123456789 */
#define SERIAL_ASCII_COUNT      64      /* [ascii] 行连续可见字符个数 */
#define SERIAL_HB_MS            1000U   /* 心跳间隔 */

void serial_min_test(void)
{
    uint32_t tick = 0U;
    uint32_t rx_cnt = 0U;
    uint8_t  c;
    int      i;

    /* ==================== 1. banner ==================== */
    printf("\r\n");
    printf("==============================================\r\n");
    printf("[serial] minimal UART self test\r\n");
    printf("[serial] UART0  TX=PA18  RX=PA17  115200-8N1\r\n");
    printf("[serial] garbled banner  -> baudrate / clock\r\n");
    printf("[serial] no banner       -> TX path dead\r\n");
    printf("==============================================\r\n");

    /* ==================== 2. 刻度行（看丢字/错位）==================== */
    printf("[scale] ");
    for (i = 0; i < SERIAL_SCALE_GROUPS; i++) {
        printf("0123456789");
    }
    printf("\r\n");

    /* ==================== 3. ASCII 连续行（看哪段字符有问题）==================== */
    printf("[ascii] ");
    for (i = 0; i < SERIAL_ASCII_COUNT; i++) {
        printf("%c", (char)('!' + i));
    }
    printf("\r\n");

    printf("[hint] type any character, it will echo as: [rx] 0xNN 'c' n=N\r\n");

    /* ==================== 4. 主循环：回显 + 心跳 ==================== */
    for (;;) {
        /* 回显：把收到的每个字节立刻打回来（16 进制 + 字符），并累计计数 */
        while (csi_uart_receive(console.uart, &c, 1, 0) == 1) {
            rx_cnt++;
            if ((c >= 0x20U) && (c < 0x7FU)) {
                printf("[rx] 0x%02X '%c' n=%u\r\n",
                       (unsigned int)c, (char)c, (unsigned int)rx_cnt);
            } else {
                printf("[rx] 0x%02X '%c' n=%u\r\n",
                       (unsigned int)c, SERIAL_DOT, (unsigned int)rx_cnt);
            }
        }

        /* 心跳：每秒一行。它停住 = 程序卡死；它正常但没 [rx] = 只有 RX 有问题 */
        mdelay(SERIAL_HB_MS);
        tick++;
        printf("[hb] tick=%u rx=%u\r\n", (unsigned int)tick, (unsigned int)rx_cnt);
    }
}
