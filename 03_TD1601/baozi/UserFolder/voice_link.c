/******************************************************************************
 * @file     voice_link.c
 * @brief    语音触发上报：往 UART0 打一行 `VOICE:<NAME>:<LEVEL>`，由 PC 转给 ESP32
 *
 * 只做一件事：把"该说话了"翻译成一行 ASCII 打出去。不解析回执、不重传、
 * 不管 ESP32 到底播没播 —— 这条链路是尽力而为的通知。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/tick.h>       /* csi_tick_get：SysTick，1 tick = 1ms */
#include "voice_link.h"

/* 同一条语音的最小间隔：手势偶尔会连着触发两次（滑动达标 + 抬手前的最后一帧），
 * 600ms 足够把重复挡掉，又不会影响"故意再蹭一下"。 */
#define VOICE_MIN_GAP_MS    600U

static const char *k_voice_name[VOICE_ID_MAX] = {
    "LOVING", "ANGRY", "HAPPY", "SAD", "SURPRISED", "BORED", "SLEEP"
};

static uint32_t s_last_ms  = 0U;
static uint8_t  s_last_id  = 0xFFU;     /* 0xFF = 还没发过 */
static uint8_t  s_has_sent = 0U;

void voice_say(uint8_t voice, uint8_t level)
{
    uint32_t now;

    if (voice >= VOICE_ID_MAX) {
        return;
    }

    if (level < 1U) {
        level = 1U;
    } else if (level > 3U) {
        level = 3U;
    }

    now = csi_tick_get();
    if ((s_has_sent != 0U) && (voice == s_last_id) &&
        ((uint32_t)(now - s_last_ms) < VOICE_MIN_GAP_MS)) {
        return;                         /* 同一条语音太密：丢掉 */
    }
    s_last_id  = voice;
    s_last_ms  = now;
    s_has_sent = 1U;

    /* 这行会混在 printf 日志里出去，PC 侧按 "VOICE:" 前缀抓即可 */
    printf("VOICE:%s:%u\r\n", k_voice_name[voice], (unsigned)level);
}
