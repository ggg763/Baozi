#pragma once
#include <stdint.h>
#include <stddef.h>

// =====================================================================
//  播放输出：ESP32 -> I2S 数字功放（MAX98357A / PCM5102 / UDA1334）-> 喇叭
//
//  数据流：TCP 收到的 TTS PCM 直接塞进播放环形缓冲，
//          一个独立任务负责把环形缓冲里的数据喂给 I2S DMA。
//  这样网络读取和 I2S 播放互不阻塞，也不会因为一次 TCP 抖动就卡顿。
// =====================================================================

bool playBegin();
void playEnd();

// 往播放缓冲里塞 PCM（16bit 小端，单声道，SAMPLE_RATE）。返回真正写进去的字节数。
// 返回 0 说明缓冲满了（PC 推得太快），调用方应当稍后再试。
size_t playWrite(const uint8_t* data, size_t len);

// 清空播放缓冲（用户打断时用）
void playFlush(void);

// 缓冲里还剩多少字节没播完
size_t playBuffered(void);

// 是否正在出声（开播后 + 缓冲排空前，或还没到 PLAY_IDLE_OFF_MS 的静默期）
bool playIsActive(void);

// 把功放使能脚 (PIN_AMP_EN) 拉高/拉低
void playAmpEnable(bool on);
