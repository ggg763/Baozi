/**
 * hal_uart.c - 硬件抽象实现：UART 通信
 *
 * 当前为 printf 模拟实现。
 * 真机接入：替换 TODO 注释块为 SDK UART 代码，
 * 并在接收中断里调用 UART_OnByteReceived()。
 */

#include "hal_uart.h"
#include <stdio.h>

void HAL_UART_Init(void)
{
    /* TODO(真机): UART 初始化（波特率 HAL_UART_BAUDRATE），开启 RX 中断 */
    printf("[HAL-UART] init @%u baud\n", (unsigned)HAL_UART_BAUDRATE);
}

void HAL_UART_Send(const uint8_t *data, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        /* TODO(真机): 发送一个字节 */
        putchar((int)data[i]);
    }
    fflush(stdout);
}
