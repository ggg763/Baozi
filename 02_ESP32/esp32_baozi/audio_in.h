#pragma once
#include <stdint.h>
#include <stddef.h>

// =====================================================================
//  麦克风输入：MAX9814（模拟输出）-> ESP32-S3 的 ADC
//
//  ESP32-S3 的 I2S 没有内置 ADC 模式（那是老 ESP32 才有的），所以麦克风
//  必须走 SAR ADC。这里用的是 esp32 core 3.x 提供的 ADC 连续(continuous)
//  模式：硬件 + DMA 按采样率搬运，并且 core 内部已经对每个采样点做了
//  多次转换取平均（analogContinuous 的 conversions_per_pin 参数）。
//
//  注意：ADC 连续模式只支持 ADC1（GPIO1 ~ GPIO10）。ADC2 与 WiFi 互斥。
// =====================================================================

// 启动采集。返回 false 表示 ADC 初始化失败（引脚/通道配错时会这样）。
//
// 注意：返回 true 只代表驱动配置成功，真正的 adc_continuous_start() 是在
// 采集任务里做的（要等 WiFi 就绪，见下）。这样设计是因为 ESP32-S3 上
// WiFi 的启动会打断 ADC 连续 DMA（ESP-IDF IDFGH-11635）。
bool audioInBegin();
void audioInEnd();

// 可选钩子：采集任务启动 ADC 之前会轮询它，返回 true 才启动。
// 用途：让 WiFi 先起来（ESP32-S3 上 WiFi 启动/连接事件会让 ADC 的连续
// DMA 永久停止）。不设置的话就立即启动。
typedef bool (*audioInReadyFn)(void);
void audioInSetReadyCheck(audioInReadyFn fn);

// 诊断用：读采集链路的内部计数，用来定位"采集不到样本"类问题。
//   read_ok / read_fail : adc_continuous_read() 的成功/失败次数
//   samples             : 实际产出的样本数（关键，不涨就是采集没工作）
//   last/min/max_raw    : 最近/最小/最大 ADC 原始码值（说话时应该大幅变化）
//   last_err            : 最近一次失败的 esp_err_t 错误码
//   restarts            : ADC 被自动重启了几次（>0 说明踩到了 WiFi 冲突）
void audioInGetDiag(uint32_t* read_ok, uint32_t* read_fail,
                    uint32_t* samples, int* last_raw, int* min_raw, int* max_raw,
                    int* last_err, uint32_t* restarts);

// 取走"自上次调用以来"的原始读数范围并复位
void audioInTakeRawRange(int* lo, int* hi);

// 取一块数据，得到 MIC_BLOCK_SAMPLES 个 int16 样本（16bit 有符号，小端含义与 PCM 一致）。
// 阻塞等待，最多 timeout_ms 毫秒。返回实际写进 out 的样本数。
size_t audioInReadBlock(int16_t* out, uint32_t timeout_ms);

// ---- 运行时调参（串口里也能改）----
void  audioInSetGain(float g);
float audioInGetGain(void);

// 取"自上次调用以来"的最大绝对值样本（0~32767），用来判断增益是否合适。
int   audioInTakePeak(void);

// 当前直流偏置的估计值（12bit 原始码值，正常应该在 1500~2600 之间）
float audioInGetDcLevel(void);
