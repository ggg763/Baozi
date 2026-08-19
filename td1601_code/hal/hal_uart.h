/**
 * hal_uart.h - 硬件抽象：UART 通信
 *
 * 真机接入步骤：
 *   1. 在 hal_uart.c 中实现 UART 收发（如 CH32V USART + 中断）；
 *   2. 接收中断中调用 uart_comm.h 的 UART_OnByteReceived()。
 */

#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PC 通信串口参数（真机按需调整） */
#define HAL_UART_BAUDRATE    (115200U)

void HAL_UART_Init(void);
void HAL_UART_Send(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UART_H */
