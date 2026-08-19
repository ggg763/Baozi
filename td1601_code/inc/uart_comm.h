/**
 * uart_comm.h - TD1601 UART 通信层
 *
 * 职责：
 *   接收并解析 PC 端下发的"语义状态"消息，
 *   转换后驱动情绪状态机。
 *
 * 支持两种协议（见 docs/protocol.md）：
 *   1. 轻量协议：  EMO:HAPPY\r\n   INTENT:CHAT\r\n
 *   2. JSON 协议： {"type":"emotion","value":"happy"}
 */

#ifndef UART_COMM_H
#define UART_COMM_H

#include "robot_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 UART（波特率等见 hal_uart.h）
 */
void UART_Init(void);

/**
 * 向 PC 发送一行文本（调试/心跳用）
 */
void UART_SendLine(const char *line);

/**
 * 周期处理：从接收缓冲解析完整消息
 * @param now_ms 当前系统时刻（毫秒）
 */
void UART_Process(uint32_t now_ms);

/**
 * 供 HAL 层中断回调：收到一个字节
 */
void UART_OnByteReceived(uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* UART_COMM_H */
