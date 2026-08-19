/* 临时测试：TD1601 端 UART 协议解析 -> Emotion FSM 联动 */
#include <stdio.h>
#include <string.h>
#include "robot_types.h"
#include "uart_comm.h"
#include "emotion_state.h"

static EmotionState g_emo = EMO_COUNT;
static int g_changes = 0;

static void on_change(EmotionState s)
{
    g_emo = s;
    g_changes++;
    printf("emotion -> %d\n", (int)s);
}

static void feed_line(const char *line)
{
    size_t n = strlen(line);
    size_t i;
    for (i = 0; i < n; i++) {
        UART_OnByteReceived((uint8_t)line[i]);
    }
}

int main(void)
{
    Emotion_Init(on_change);
    UART_Init();

    /* 1. 轻量协议 */
    feed_line("EMO:HAPPY\n");
    /* 2. 轻量协议切换 */
    feed_line("EMO:SAD\r\n");
    /* 3. JSON 协议 */
    feed_line("{\"type\":\"emotion\",\"value\":\"loving\"}\n");
    /* 4. 意图消息不应改变情绪 */
    feed_line("INTENT:CHAT\n");
    /* 5. 未知情绪应忽略 */
    feed_line("EMO:UNKNOWN\n");

    printf("final emo=%d changes=%d\n", (int)g_emo, g_changes);

    if (g_emo == EMO_LOVING && g_changes == 3) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
