/**
 * uart_comm.c - TD1601 UART 通信层实现
 *
 * 解析流程：
 *   字节流 -> 行缓冲（\r\n 或 \n 结尾）-> 协议解析 -> Emotion_Update()
 *
 * 当前实现为"行协议 + 简单 JSON 提取"，
 * 无动态内存分配，适合 MCU 环境。
 */

#include "uart_comm.h"
#include <string.h>
#include <stdio.h>
#include "hal/hal_uart.h"
#include "emotion_state.h"

/* ---------- 接收缓冲 ---------- */
#define UART_RX_BUF_SIZE   (128U)

static char s_rx_buf[UART_RX_BUF_SIZE];
static uint16_t s_rx_len = 0U;

/* ---------- 情绪名映射 ---------- */
typedef struct {
    const char   *name;
    EmotionState emo;
} EmotionNameMap;

static const EmotionNameMap s_emo_map[] = {
    { "IDLE",     EMO_IDLE     },
    { "HAPPY",    EMO_HAPPY    },
    { "SAD",      EMO_SAD      },
    { "ANGRY",    EMO_ANGRY    },
    { "SURPRISED",EMO_SURPRISED},
    { "LOVING",   EMO_LOVING   },
    { "BORED",    EMO_BORED    },
    { "SLEEPING", EMO_SLEEPING },
};

static EmotionState name_to_emotion(const char *name)
{
    uint32_t i;
    for (i = 0; i < sizeof(s_emo_map) / sizeof(s_emo_map[0]); i++) {
        if (strcmp(s_emo_map[i].name, name) == 0) {
            return s_emo_map[i].emo;
        }
    }
    return EMO_COUNT;   /* 未识别 */
}

/* ---------- 轻量协议：EMO:HAPPY ---------- */
static bool parse_lightweight(const char *line)
{
    if (strncmp(line, "EMO:", 4) == 0) {
        EmotionState emo = name_to_emotion(line + 4);
        if (emo != EMO_COUNT) {
            Emotion_Update(emo);
            return true;
        }
    }
    return false;
}

/* ---------- JSON 协议：{"type":"emotion","value":"happy"} ----------
 * 简化提取：查找 "value":"xxx" 片段（不引入 JSON 库）。
 */
static bool parse_json_emotion(const char *line)
{
    const char *p = strstr(line, "\"value\"");
    if (p == 0) {
        return false;
    }
    const char *q = strchr(p, ':');
    if (q == 0) {
        return false;
    }
    q++;                                  /* 跳过冒号 */
    while (*q == ' ' || *q == '\t') { q++; }
    if (*q != '"') {
        return false;
    }
    q++;
    char value[32];
    uint16_t n = 0;
    while (*q != '\0' && *q != '"' && n < sizeof(value) - 1) {
        value[n++] = *q++;
    }
    value[n] = '\0';
    if (n == 0) {
        return false;
    }
    /* 转大写后匹配 */
    uint16_t i;
    for (i = 0; i < n; i++) {
        if (value[i] >= 'a' && value[i] <= 'z') {
            value[i] = (char)(value[i] - 'a' + 'A');
        }
    }
    EmotionState emo = name_to_emotion(value);
    if (emo != EMO_COUNT) {
        Emotion_Update(emo);
        return true;
    }
    return false;
}

/* ---------- 行处理 ---------- */
static void handle_line(char *line)
{
    /* 去掉行尾空白 */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' || line[len-1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    if (!parse_lightweight(line)) {
        parse_json_emotion(line);
    }
}

/* ---------- 对外接口 ---------- */

void UART_Init(void)
{
    s_rx_len = 0U;
    HAL_UART_Init();
}

void UART_SendLine(const char *line)
{
    HAL_UART_Send((const uint8_t *)line, (uint16_t)strlen(line));
    HAL_UART_Send((const uint8_t *)"\r\n", 2);
}

void UART_Process(uint32_t now_ms)
{
    (void)now_ms;   /* 当前同步处理即可，无需节拍 */
    /* 若有完整行，则处理（见 UART_OnByteReceived 中拆行） */
}

void UART_OnByteReceived(uint8_t byte)
{
    if (byte == '\n') {
        /* 行结束（\r 已在 handle_line 中剥离） */
        s_rx_buf[s_rx_len] = '\0';
        handle_line(s_rx_buf);
        s_rx_len = 0U;
    } else if (s_rx_len < UART_RX_BUF_SIZE - 1) {
        s_rx_buf[s_rx_len++] = (char)byte;
    }
    /* 缓冲满：丢弃该行，重置 */
    else {
        s_rx_len = 0U;
    }
}
