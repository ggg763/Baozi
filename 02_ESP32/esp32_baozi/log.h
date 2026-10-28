#pragma once
#include <stdint.h>
#include <stddef.h>

// =====================================================================
//  日志输出（与传输方式无关的一层）
//
//  ★ 为什么不能直接 Serial.print：
//    USB 模式下，串口这根线同时承载【二进制协议帧】（音频 / 图像 / 控制）。
//    往里面塞裸文本会把帧流冲乱 —— PC 端只看到一堆对不上 magic 的字节，
//    日志看不到、音频也跟着丢。所以日志必须也走帧：PKT_LOG 文本帧。
//
//    USE_USB_LINK=1：logPrintf -> PKT_LOG 帧 -> PC 打印（前缀 [esp]）
//    USE_USB_LINK=0：logPrintf -> 直写串口（WiFi 模式下串口是自由日志口）
//
//  行为约定：
//    · logPrintf() 与 Serial.printf() 语义一致（换行要自己写在 fmt 里）；
//    · logLine()   等于 printf + 自动换行；
//    · logWrite()  对应 Serial.write()（原样字节，可能含换行）。
// =====================================================================

void logPrintf(const char* fmt, ...);
void logLine(const char* text);
void logWrite(const uint8_t* data, size_t len);
