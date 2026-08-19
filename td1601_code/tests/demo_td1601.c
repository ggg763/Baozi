/**
 * demo_td1601.c - TD1601 行为层演示程序（PC 上运行看效果）
 *
 * 演示内容（自动播放）：
 *   1. 启动初始化（BOOT READY）
 *   2. PC 下发情绪序列：HAPPY -> SAD -> ANGRY -> SURPRISED -> LOVING
 *      （观察 TD1601 每次的表情/LED/舵机行为输出）
 *   3. 意图消息 INTENT:CHAT 不影响情绪（验证语义）
 *   4. 回 IDLE 后静置 20s -> BORED（无聊），再静置 10s -> SLEEPING（睡眠）
 *   5. 下发 EMO:HAPPY 唤醒
 *
 * 编译（在 td1601_code/ 目录）：
 *   gcc -std=c99 -Wall -Wextra -Iinc -I. tests/demo_td1601.c \
 *       src/uart_comm.c src/emotion_state.c src/behavior.c \
 *       src/motion.c src/servo_control.c src/screen_display.c src/led_control.c \
 *       hal/hal_pwm.c hal/hal_lcd.c hal/hal_led.c hal/hal_gpio.c hal/hal_uart.c \
 *       -o demo_td1601
 */

#include <stdio.h>
#include <string.h>

#include "robot_types.h"
#include "uart_comm.h"
#include "emotion_state.h"
#include "behavior.h"

/* ---------- 演示工具 ---------- */

static uint32_t s_now_ms = 0U;      /* 演示用虚拟时钟 */

static const char *emo_name(EmotionState e)
{
    switch (e) {
    case EMO_IDLE:      return "IDLE";
    case EMO_HAPPY:     return "HAPPY";
    case EMO_SAD:       return "SAD";
    case EMO_ANGRY:     return "ANGRY";
    case EMO_SURPRISED: return "SURPRISED";
    case EMO_LOVING:    return "LOVING";
    case EMO_BORED:     return "BORED";
    case EMO_SLEEPING:  return "SLEEPING";
    default:            return "?";
    }
}

static void feed_line(const char *line)
{
    size_t n = strlen(line);
    size_t i;
    printf("\n=== PC 下发: %s", line);
    for (i = 0; i < n; i++) {
        UART_OnByteReceived((uint8_t)line[i]);
    }
    /* 互动后推进一小段时间并驱动 FSM 节拍 */
    s_now_ms += 500U;
    Emotion_Tick(s_now_ms);
    printf(">>> 当前情绪: %s\n", emo_name(Emotion_Get()));
}

static void advance_ms(uint32_t ms, const char *note)
{
    printf("\n=== %s（推进 %u ms）===\n", note, (unsigned)ms);
    s_now_ms += ms;
    Emotion_Tick(s_now_ms);
    printf(">>> 当前情绪: %s\n", emo_name(Emotion_Get()));
}

/* ---------- 主演示 ---------- */
int main(void)
{
    printf("========== BaoZi TD1601 行为层演示 ==========\n\n");

    /* 初始化：行为层内部初始化 Screen/LED/Servo 并复位姿态 */
    Emotion_Init(Behavior_OnEmotionChanged);
    Behavior_Init();
    UART_Init();
    Emotion_Touch(s_now_ms);
    printf(">>> 启动完成，当前情绪: %s\n\n", emo_name(Emotion_Get()));

    /* ---- 1. 情绪序列（模拟 PC 下发） ---- */
    feed_line("EMO:HAPPY\n");
    feed_line("EMO:SAD\n");
    feed_line("EMO:ANGRY\n");
    feed_line("EMO:SURPRISED\n");
    feed_line("EMO:LOVING\n");

    /* ---- 2. 意图消息不应改变情绪 ---- */
    feed_line("INTENT:CHAT\n");

    /* ---- 3. 回待机 ---- */
    feed_line("EMO:IDLE\n");

    /* ---- 4. 空闲降级：20s -> BORED，再 10s -> SLEEPING ---- */
    advance_ms(20000U, "静置 20 秒（应进入 BORED）");
    advance_ms(10000U, "再静置 10 秒（应进入 SLEEPING）");

    /* ---- 5. 唤醒 ---- */
    feed_line("EMO:HAPPY\n");

    printf("\n========== 演示结束 ==========\n");
    return 0;
}
