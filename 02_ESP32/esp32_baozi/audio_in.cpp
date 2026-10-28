#include "audio_in.h"
#include "config.h"
#include "log.h"
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "esp_adc/adc_continuous.h"
#include "esp_heap_caps.h"

#if (ADC_OVERSAMPLE != 1) && (ADC_OVERSAMPLE != 2) && (ADC_OVERSAMPLE != 4)
#error "ADC_OVERSAMPLE 只能是 1 / 2 / 4"
#endif

// ---------------------------------------------------------------------
//  DMA 缓冲对齐要求（core 兼容层）
//
//  ESP_ARDUINO_DMA_BUF_ALIGN 是 arduino-esp32 3.3.0 才加进 cores/esp32/esp32-hal.h
//  的宏；本工程在 3.2.x 上编译时它不存在，直接报
//  "'ESP_ARDUINO_DMA_BUF_ALIGN' was not declared in this scope"。
//
//  这里按上游同一套逻辑补一份兜底：有 L2 缓存按 L2 行大小，否则 L1，再否则 4 字节。
//  与 core 3.3+ 的取值完全一致，所以在任何版本上行为都不变。
//  （core 升到 3.3+ 后本文件的 define 不生效，自动改用 core 里的定义。）
// ---------------------------------------------------------------------
#ifndef ESP_ARDUINO_DMA_BUF_ALIGN
#if defined(CONFIG_CACHE_L2_CACHE_LINE_SIZE)
#define ESP_ARDUINO_DMA_BUF_ALIGN CONFIG_CACHE_L2_CACHE_LINE_SIZE
#elif defined(CONFIG_CACHE_L1_CACHE_LINE_SIZE)
#define ESP_ARDUINO_DMA_BUF_ALIGN CONFIG_CACHE_L1_CACHE_LINE_SIZE
#else
#define ESP_ARDUINO_DMA_BUF_ALIGN 4
#endif
#endif

// =====================================================================
//  麦克风采集：MAX9814（模拟）-> ESP32-S3 ADC -> 16k/16bit 单声道 PCM
//
//  【为什么直接用底层 adc_continuous API，而不用 Arduino core 的
//    analogContinuous / analogContinuousRead 封装】
//
//  core 的 analogContinuousRead() 每次调用都会：
//      heap_caps_aligned_alloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) ... free()
//  在 16kHz 这种"每秒上万次"的使用方式下，会把内部 DMA 堆碎片化，
//  最终分配失败；而 core 只在失败时打一条 log_e（默认日志级别看不见），
//  于是函数开始永久返回 false —— 采集彻底停摆。
//  实机现象：read 成功数冻结在 1122，之后一直失败，样本数不再增长，
//            但 free heap 看着还剩 194KB（连续 DMA 块没了）。
//
//  【也不要用 analogRead() 逐点采】
//  oneshot 模式虽然不分配内存，但实测单次约 61us。16kHz 要求 62.5us/点，
//  再算上过采样就完全来不及（只能到 ~8kHz），而且靠 delayMicroseconds()
//  排节拍是纯忙等（core 里就是 NOP 循环，不 yield），会饿死同核上
//  优先级更低、负责 TCP 的主循环。
//
//  【所以：自己管 DMA】
//  一次性创建 handle、一次性分配缓冲，之后循环 adc_continuous_read() 复用，
//  全程零动态分配。DMA 由硬件搬运，CPU 开销和采样率都稳定。
// =====================================================================

// ---------------------------------------------------------------------
// ADC 硬件配置
// ---------------------------------------------------------------------
// 硬件采样率 = 输出采样率 * 过采样倍数。
// 注意：这里不是"每个点转换 N 次再平均"，而是硬件按 hw_rate 采样，
// 我们在软件里把连续的 N 个点平均成 1 个输出点（见 averageFrames）。
#define HW_RATE  ((uint32_t)SAMPLE_RATE * (uint32_t)ADC_OVERSAMPLE)

// 一次 adc_continuous_read 最多取多少字节。取 ~10ms 的量，兼顾延迟与开销。
#define DMA_READ_BYTES  (HW_RATE / 100 * SOC_ADC_DIGI_RESULT_BYTES)

static adc_continuous_handle_t s_handle = NULL;
static uint8_t*                s_dma_buf = NULL;   // 只分配一次，循环复用
static volatile bool           s_adc_ok  = false;
static TaskHandle_t            s_task    = NULL;

// 启动 ADC 前的就绪检查（通常是"WiFi 连上了没有"）。
// 不设置就立即启动。
static audioInReadyFn s_ready_fn = NULL;

// GPIO -> ADC 通道的映射结果（在 audioInBegin 里通过官方函数得到）
static uint8_t s_mic_channel = 0;
static uint8_t s_mic_unit    = 0;

// ---------------------------------------------------------------------
// 待发送的样本块：生产者只写、消费者取走
// ---------------------------------------------------------------------
// 交接方式（避免两边同时改计数器）：
//   - 生产者（micTask）写满一整块后置 s_block_ready=1，并把 s_pending_n 归零
//   - 消费者（audioInReadBlock）看到 ready 才取走，并清 0
//   - 生产者只在 ready==0 时写，所以两边不会同时动同一块内存
static int16_t  s_pending[MIC_BLOCK_SAMPLES];
static uint32_t s_pending_n = 0;
static volatile bool s_block_ready = false;

// 直流偏置：一阶低通慢慢跟踪。开机用第一个样本播种，省去收敛时间。
static float s_dc = 2048.0f;
static bool  s_dc_seeded = false;

static float s_gain = MIC_GAIN;
static int   s_peak = 0;

// 诊断计数器
static volatile uint32_t s_diag_read_ok   = 0;  // 成功取到数据的次数
static volatile uint32_t s_diag_read_fail = 0;  // 读取失败次数
static volatile uint32_t s_diag_samples   = 0;  // 累计产出样本数
static volatile int      s_diag_last_raw  = 0;
static volatile int      s_diag_min_raw   = 99999;
static volatile int      s_diag_max_raw   = 0;
static volatile int      s_diag_last_err  = 0;  // 最近一次失败的 esp_err_t
static volatile uint32_t s_diag_restarts  = 0;  // ADC 被自动重启的次数

// ---------------------------------------------------------------------
// 把一个 12bit ADC 原始值转成去偏置 + 加增益后的 int16 样本
// ---------------------------------------------------------------------
static inline void pushRaw(int raw) {
  if (!s_dc_seeded) {
    s_dc = (float)raw;
    s_dc_seeded = true;
  }

  // 一阶低通跟踪直流偏置（时间常数约 0.125s：比语音慢、比温漂快）
  s_dc += ((float)raw - s_dc) * MIC_DC_TRACK;

  float v = ((float)raw - s_dc) * s_gain;   // 去偏置 + 增益
  if (v >  32767.0f) v =  32767.0f;         // 削顶保护
  if (v < -32768.0f) v = -32768.0f;
  int16_t s = (int16_t)lrintf(v);

  int a = (s < 0) ? -(int)s : (int)s;
  if (a > s_peak) s_peak = a;

  // 上一块还没被取走就先丢掉，避免和消费者抢同一块内存
  if (s_block_ready) return;

  s_pending[s_pending_n] = s;
  s_pending_n++;
  if (s_pending_n >= MIC_BLOCK_SAMPLES) {
    s_pending_n = 0;
    // 内存屏障：确保 s_pending 的写入在 s_block_ready 之前对消费者可见
    portMEMORY_BARRIER();
    s_block_ready = true;
  }
}

// ---------------------------------------------------------------------
// 把一段 DMA 原始数据（每 4 字节一个采样点）转成 int16 样本。
//
// 采样点格式（ESP32-S3 用 TYPE2）：
//   低 12 位 = 转换结果，高 4 位 = 通道号
// 我们只配了一个通道，但仍然校验通道号，避免配错时静默产出垃圾。
// ---------------------------------------------------------------------
static void feedDmaBytes(const uint8_t* buf, uint32_t len) {
  // 用 audioInBegin 里通过 adc_continuous_io_to_channel() 映射出来的通道号。
  // 注意：不能假设 "通道号 == GPIO 号"，否则会过滤掉自己的数据（读数恒 0）。
  const uint8_t want_ch = s_mic_channel;

  for (uint32_t off = 0; off + SOC_ADC_DIGI_RESULT_BYTES <= len;
       off += SOC_ADC_DIGI_RESULT_BYTES) {
    const adc_digi_output_data_t* p = (const adc_digi_output_data_t*)(buf + off);

    if (p->type2.channel != want_ch) continue;      // 不是我们的通道，跳过
    int raw = (int)p->type2.data;                   // 低 12 位就是结果

    if (raw < s_diag_min_raw) s_diag_min_raw = raw;
    if (raw > s_diag_max_raw) s_diag_max_raw = raw;
    s_diag_last_raw = raw;
    s_diag_samples = s_diag_samples + 1;

#if ADC_OVERSAMPLE > 1
    // 软件过采样：累积 ADC_OVERSAMPLE 个点后取平均，再交给 pushRaw
    static int32_t acc = 0;
    static uint32_t acc_n = 0;
    acc += raw;
    if (++acc_n >= ADC_OVERSAMPLE) {
      pushRaw((int)(acc / ADC_OVERSAMPLE));
      acc = 0;
      acc_n = 0;
    }
#else
    pushRaw(raw);
#endif
  }
}

// ---------------------------------------------------------------------
// 采集任务
//
// 三件事：
//   1) 等"就绪"（通常是 WiFi 连上）—— 见文件顶部关于 IDFGH-11635 的说明；
//   2) 启动 ADC，然后循环读 DMA（缓冲只分配一次，全程零动态分配）；
//   3) 连续读不到数据就自动 stop/start 重启 ADC。
//
// 为什么 start/stop 都放在这个任务里：IDF 的 ADC 连续模式驱动里，如果
// 启动它的任务和停止它的任务不是同一个，（在部分版本上）会崩。放同一个
// 任务里最稳。
// ---------------------------------------------------------------------
static void micTask(void*) {
  // ---- 1) 等就绪（例如 WiFi 连上），最多等 MIC_WAIT_WIFI_MS ----
  if (s_ready_fn) {
    uint32_t waited = 0;
    while (!s_ready_fn() && waited < MIC_WAIT_WIFI_MS) {
      vTaskDelay(pdMS_TO_TICKS(100));
      waited += 100;
    }
    bool ready = s_ready_fn();
    if (waited > 0) {
      logPrintf("[mic] 等就绪 %lu ms（%s），现在启动 ADC\n",
                (unsigned long)waited,
                ready ? "已就绪" : "超时，仍然启动");
    }
  }

  // ---- 2) 启动 ADC ----
  if (adc_continuous_start(s_handle) != ESP_OK) {
    logLine("[mic] adc_continuous_start 失败");
    s_adc_ok = false;
    s_task = NULL;              // 免得 audioInEnd() 去删一个已经失效的任务
    vTaskDelete(NULL);
    return;
  }
  logLine("[mic] ADC 已启动，开始采集");

  // ---- 3) 读循环 ----
  uint32_t fail_ms = 0;            // 连续读不到数据的累计时长
  for (;;) {
    uint32_t got = 0;

    // 给它一个超时是有意的：没数据时它会阻塞等待，从而让出 CPU，
    // 不会饿死同核上优先级更低、负责 TCP 的主循环。
    esp_err_t err = adc_continuous_read(s_handle, s_dma_buf, DMA_READ_BYTES,
                                        &got, 100);

    if (err == ESP_OK && got > 0) {
      s_diag_read_ok = s_diag_read_ok + 1;
      fail_ms = 0;
      feedDmaBytes(s_dma_buf, got);
    } else {
      s_diag_read_fail = s_diag_read_fail + 1;
      s_diag_last_err  = (int)err;

      // 连续读不到 -> 自动重启 ADC。
      // 典型触发场景：WiFi 连接/重连等事件把 ADC 的连续 DMA 打停了
      //（ESP-IDF IDFGH-11635，详见 config.h 里的说明）。
      fail_ms += 100;              // 上面 read 的超时就是 100ms
      if (fail_ms >= MIC_FAIL_RESTART_MS) {
        s_diag_restarts = s_diag_restarts + 1;
        logPrintf("[mic] 连续 %u ms 读不到数据(err=0x%X)，重启 ADC（第 %lu 次）\n",
                      (unsigned)fail_ms, (unsigned)err,
                      (unsigned long)s_diag_restarts);
        adc_continuous_stop(s_handle);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (adc_continuous_start(s_handle) != ESP_OK) {
          logLine("[mic] ADC 重启失败，10 秒后再试");
          vTaskDelay(pdMS_TO_TICKS(10000));
        }
        fail_ms = 0;
      }

      // 兜底：无论什么原因没拿到数据，都明确让出 CPU
      vTaskDelay(1);
    }
  }
}

// ---------------------------------------------------------------------
bool audioInBegin(void) {
  s_pending_n = 0;
  s_block_ready = false;
  s_dc_seeded = false;
  s_peak = 0;
  s_diag_read_ok = 0;
  s_diag_read_fail = 0;
  s_diag_samples = 0;
  s_diag_last_raw = 0;
  s_diag_min_raw = 99999;
  s_diag_max_raw = 0;
  s_diag_last_err = 0;
  s_diag_restarts = 0;

  // ---- 1) 创建 handle ----
  // max_store_buf_size 是 DMA 环形缓冲（给 50ms 余量），
  // conv_frame_size 是每次中断搬运的量（取一个 DMA 对齐友好的大小）。
  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = HW_RATE / 20 * SOC_ADC_DIGI_RESULT_BYTES;
  hcfg.conv_frame_size    = 256;

  if (adc_continuous_new_handle(&hcfg, &s_handle) != ESP_OK) {
    logLine("[mic] adc_continuous_new_handle 失败");
    return false;
  }

  // ---- 2) 配置 ADC1 + 指定 GPIO ----
  // 关键：不要假设 "GPIO 号 == 通道号"。用官方函数把 GPIO 映射成
  // (unit, channel)，否则可能采到另一个悬空通道（现象：读数恒为 0/极小）。
  adc_unit_t     unit = ADC_UNIT_1;
  adc_channel_t  ch   = ADC_CHANNEL_0;
  esp_err_t ioe = adc_continuous_io_to_channel(PIN_MIC_ADC, &unit, &ch);
  if (ioe != ESP_OK) {
    logPrintf("[mic] GPIO%d 不是可用的 ADC 引脚 (err=%d)\n", (int)PIN_MIC_ADC, (int)ioe);
    adc_continuous_deinit(s_handle);
    s_handle = NULL;
    return false;
  }
  if (unit != ADC_UNIT_1) {
    logLine("[mic] 该引脚不是 ADC1（ADC2 与 WiFi 冲突，连续模式不可用）");
    adc_continuous_deinit(s_handle);
    s_handle = NULL;
    return false;
  }
  s_mic_channel = (uint8_t)ch;      // 给 feedDmaBytes 过滤用
  s_mic_unit    = (uint8_t)unit;

  adc_digi_pattern_config_t pattern = {};
  pattern.atten     = ADC_ATTEN_DB_12;              // 量程约 0~3.1V
  pattern.channel   = (uint8_t)ch;                  // ← 用映射出来的通道号
  pattern.unit      = unit;
  pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;    // 12

  adc_continuous_config_t ccfg = {};
  ccfg.sample_freq_hz = HW_RATE;
  ccfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
  ccfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;   // ESP32-S3
  ccfg.adc_pattern    = &pattern;
  ccfg.pattern_num    = 1;

  if (adc_continuous_config(s_handle, &ccfg) != ESP_OK) {
    logLine("[mic] adc_continuous_config 失败（PIN_MIC_ADC 要在 GPIO1~GPIO10）");
    adc_continuous_deinit(s_handle);
    s_handle = NULL;
    return false;
  }

  // ---- 3) 一次性分配 DMA 缓冲（关键：只分配这一次，之后循环复用）----
  s_dma_buf = (uint8_t*)heap_caps_aligned_alloc(
      ESP_ARDUINO_DMA_BUF_ALIGN, DMA_READ_BYTES,
      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (s_dma_buf == NULL) {
    logLine("[mic] DMA 缓冲分配失败");
    adc_continuous_deinit(s_handle);
    s_handle = NULL;
    return false;
  }

  // ---- 4) 采集任务 ----
  // 真正的 adc_continuous_start() 在任务里做：要先等 WiFi 就绪。
  // 原因见 config.h 里关于 IDFGH-11635 的说明 —— ESP32-S3 上 WiFi 的
  // 启动/连接事件会让 ADC 的连续 DMA 永久停止填充。
  //
  // 优先级 4、固定在 core 1：它大部分时间阻塞在 adc_continuous_read 的
  // 超时等待上，不会饿死主循环；同时比 WiFi 任务低，不抢它们的 CPU。
  if (!xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 4, &s_task, 1)) {
    logLine("[mic] 无法创建采集任务");
    heap_caps_free(s_dma_buf);
    s_dma_buf = NULL;
    adc_continuous_deinit(s_handle);
    s_handle = NULL;
    return false;
  }

  s_adc_ok = true;
  logPrintf("[mic] 采集就绪: GPIO%d -> ADC%d_CH%d, 硬件 %u Hz, 过采样 x%d -> 输出 %d Hz, DMA缓冲 %u 字节\n",
                (int)PIN_MIC_ADC, (int)s_mic_unit + 1, (int)s_mic_channel,
                (unsigned)HW_RATE, (int)ADC_OVERSAMPLE,
                (int)SAMPLE_RATE, (unsigned)DMA_READ_BYTES);
  return true;
}

void audioInEnd(void) {
  if (s_task) {
    vTaskDelete(s_task);
    s_task = NULL;
  }
  if (s_handle) {
    adc_continuous_stop(s_handle);
    adc_continuous_deinit(s_handle);
    s_handle = NULL;
  }
  if (s_dma_buf) {
    heap_caps_free(s_dma_buf);
    s_dma_buf = NULL;
  }
  s_adc_ok = false;
}

size_t audioInReadBlock(int16_t* out, uint32_t timeout_ms) {
  if (!out || !s_adc_ok) return 0;

  uint32_t deadline = millis() + timeout_ms;
  while (!s_block_ready && (int32_t)(millis() - deadline) < 0) {
    vTaskDelay(1);      // 让出 CPU，别忙等
  }

  if (!s_block_ready) return 0;

  memcpy(out, s_pending, MIC_BLOCK_SAMPLES * sizeof(int16_t));
  s_block_ready = false;   // 交还给生产者
  return MIC_BLOCK_SAMPLES;
}

void  audioInSetGain(float g) { s_gain = (g < 0.1f) ? 0.1f : (g > 500.0f ? 500.0f : g); }
float audioInGetGain(void)    { return s_gain; }

int audioInTakePeak(void) {
  int p = s_peak;
  s_peak = 0;
  return p;
}

float audioInGetDcLevel(void) { return s_dc; }

// ---- 诊断 ----
void audioInGetDiag(uint32_t* read_ok, uint32_t* read_fail,
                    uint32_t* samples, int* last_raw, int* min_raw, int* max_raw,
                    int* last_err, uint32_t* restarts) {
  if (read_ok)   *read_ok   = s_diag_read_ok;
  if (read_fail) *read_fail = s_diag_read_fail;
  if (samples)   *samples   = s_diag_samples;
  if (last_raw)  *last_raw  = s_diag_last_raw;
  if (min_raw)   *min_raw   = (s_diag_min_raw == 99999) ? 0 : s_diag_min_raw;
  if (max_raw)   *max_raw   = s_diag_max_raw;
  if (last_err)  *last_err  = s_diag_last_err;
  if (restarts)  *restarts  = s_diag_restarts;
}

void audioInSetReadyCheck(audioInReadyFn fn) { s_ready_fn = fn; }

void audioInTakeRawRange(int* lo, int* hi) {
  if (lo) *lo = (s_diag_min_raw == 99999) ? 0 : s_diag_min_raw;
  if (hi) *hi = s_diag_max_raw;
  s_diag_min_raw = 99999;
  s_diag_max_raw = 0;
}
