// uart_esp.h — ESP32 -> TD1601 的"情感链路"接收（UART3：PB18-RX / PB19-TX）
// ============================================================================
// 落位说明：本文件属于**干净主线工程** projects(A)/baozi_LCD（main.c V2.1：
//   参数化脸 + 触摸 + 舵机 + 卡片 UI）。此前一版曾误落在上一代副本
//   projects(A) - fuben(3)/baozi_LCD（面部栈还是 emo_draw/emotions 那套），
//   两代工程的 BAOZI_SetEmotion 行为不同 —— 见 uart_esp.c 顶部"两代差异"。
//
// 背景：声音由 ESP32-S3 负责。PC 把"情绪/意图/强度"发给 ESP32，
// ESP32 经 UART2(GPIO14) 转发到 TD1601 的 UART3(PB18)，由本模块解析并驱动屏幕。
//
// ⚠️ 这条链路和 uart_hal.c 那条**完全无关**，别混（两代的 uart_hal 还不一样）：
//
//   uart_hal.c（本工程）= UART0（PA18-TX / PA17-RX）→ 接 **PC 桥接脚本**
//                        与 console printf 共线；帧格式 A5 | LEN | CMD | DATA | CHK
//   uart_hal.c（上一代）= UART2（PB12-RX / PB13-TX）→ 接 ASRPRO，已弃用
//   本文件              = UART3（PB18-RX / PB19-TX）→ 接 **ESP32**
//                         帧格式：ESP32 固件直接写字节，**没有信封**，只有两种形态：
//                           (a) 状态字节帧：0xAA <code>          ← 正式通道
//                           (b) 文本行    ："EMO:HAPPY\n" 之类    ← 调试/兼容通道
//
// 两种形态都支持。**优先用 (a)**：(b) 是给人看的，(a) 是给机器看的。
//
// 为什么 (a) 要把三个量压成 1 个字节：一轮对话原来要发
// INTENSITY+EMO+INTENT 三条文本（约 47 字节），TD1601 的 UART3 FIFO 中途会溢出，
// 命令被截成 "INTENSITY:EMO:SAD"。压成 1 字节后 3 帧变 1 帧、47 字节变 3 字节。
//   code = (emo_id*8 + intent_id)*3 + intensity_id   （0..215）
//
// ⚠️ 电平：TD1601 是 **1.8V** IO，ESP32 是 3.3V —— 直连会灌流损伤芯片，
//    必须做电平转换（详见 UserFolder/doc/pin_mux_table.md 第 7.4 节）。
// ============================================================================
#ifndef _UART_ESP_H_
#define _UART_ESP_H_

#include <stdint.h>

// 初始化 UART3（PB18/PB19，115200 8N1）。返回 0 = 成功。
int  uart_esp_init(void);

// 主循环里轮询调用。返回 1 = 本轮处理了一帧（可用于重置空闲计时）。
int  uart_esp_poll(void);

// ---- 统计（排障用，串口日志会打）----
typedef struct {
    uint32_t rx_bytes;      // 收到的总字节数
    uint32_t state_frames;  // 收到的 0xAA + code 状态帧数
    uint32_t text_lines;    // 收到的文本行数
    uint32_t dropped;       // 丢弃的（越界 code / 缓冲区溢出）
    uint8_t  last_code;     // 最近一次状态码
} uart_esp_stat_t;

const uart_esp_stat_t *uart_esp_stats(void);

// 把 3 个 id 压成状态码 / 反解（自检与日志用，与 ESP32 侧 state_code 同式）
uint8_t  uart_esp_pack_state(uint8_t emo_id, uint8_t intent_id, uint8_t intensity_id);
void     uart_esp_unpack_state(uint8_t code, uint8_t *emo_id, uint8_t *intent_id,
                               uint8_t *intensity_id);

#endif
