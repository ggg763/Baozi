/******************************************************************************
 * @file     voice_link.h
 * @brief    语音触发上报（TD1601 → UART0 → PC → ESP32 → 喇叭）
 *
 * 板子没有喇叭，也没有直连 ESP32 的上行通道，所以语音走这条接力：
 *
 *     触控手势 ──> 本模块 ──printf──> UART0 ──> PC ──(转发)──> ESP32 ──> 喇叭
 *
 * 为什么用【文本行】而不是 A5 二进制帧：
 *   ① PC 端现有 communication.py 就是文本协议（EMO:xxx / INTENT:xxx），
 *      加一个前缀比重写一套帧解析便宜得多；
 *   ② 串口助手里人能直接看到，联调时不用抓包；
 *   ③ 这行是"事件通知"而不是控制流 —— 丢了最多少一句语音，不影响状态一致性，
 *      不值得为它引入确认/重传。
 *
 * 帧格式（纯 ASCII，\r\n 结尾；和 printf 日志同一根线，PC 侧按前缀过滤）：
 *
 *     VOICE:<NAME>:<LEVEL>\r\n
 *
 *     NAME  : LOVING / ANGRY / HAPPY / SAD / SURPRISED / BORED / SLEEP
 *     LEVEL : 1~3（手势强度等级，30s 内重复触发会升一级）
 *
 * 例：`VOICE:LOVING:2` —— 猫爪蹭两下，该撒娇得更起劲
 ******************************************************************************/
#ifndef _VOICE_LINK_H_
#define _VOICE_LINK_H_

#include <stdint.h>

/* 语音编号（独立于情绪枚举，避免和 emotion/ 的 EMO_* 抢命名空间）*/
#define VOICE_LOVING        0U
#define VOICE_ANGRY         1U
#define VOICE_HAPPY         2U
#define VOICE_SAD           3U
#define VOICE_SURPRISED     4U
#define VOICE_BORED         5U
#define VOICE_SLEEP         6U
#define VOICE_ID_MAX        7U

/* 触发一次语音。level 会被夹到 1~3；同一条语音 MIN_GAP 内不重复发。
 * 未知 voice 直接忽略（不打印，避免脏日志）。 */
void voice_say(uint8_t voice, uint8_t level);

#endif /* _VOICE_LINK_H_ */
