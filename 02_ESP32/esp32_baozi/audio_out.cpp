#include "audio_out.h"
#include "config.h"
#include "log.h"
#include <Arduino.h>
#include <string.h>
#include "ESP_I2S.h"

// =====================================================================
//  用 esp32 core 3.x 的 I2SClass 播放。
//
//  格式：16k / 16bit / 单声道。
//
//  单声道要注意：I2SClass::write() 是把字节原样丢给 DMA 的，
//  如果我们按 STEREO 打开总线却只喂单声道数据，DMA 会把相邻两个样本
//  当成 L/R 播出去 —— 结果是音高翻倍、每个声道各丢一半样本。
//  所以要按 MONO slot 打开，并由硬件负责把它送到两个声道。
// =====================================================================

static I2SClass s_i2s;

// ---------------------------------------------------------------------
// 播放环形缓冲
// ---------------------------------------------------------------------
#define RING_BYTES   ((size_t)(SAMPLE_RATE * (AUDIO_BITS / 8) * PLAY_RING_MS / 1000))
#define CHUNK_BYTES  ((size_t)(SAMPLE_RATE * (AUDIO_BITS / 8) * PLAY_CHUNK_MS / 1000))

static uint8_t  s_ring[RING_BYTES];
// 环形缓冲的读写指针：s_head 只由生产者（TCP 线程）改，s_tail 只由消费者
// （播放任务）改，"还有多少字节" 由两个指针相减得出，不用共享计数器。
// 之前用一个两边都 +=/-= 的 s_buffered，volatile 并不保证原子性，
// 丢更新会让计数漂移，最终 size_t 下溢把整个缓冲搞乱。
static volatile size_t s_head = 0;      // 写入位置（生产者独占）
static volatile size_t s_tail = 0;      // 读出位置（消费者独占）
static volatile bool s_flush_req = false;  // 生产者请求清空缓冲
static volatile uint32_t s_last_play_ms = 0;
static volatile bool s_started = false; // 本轮是否已经开始出声
static volatile bool s_active = false;

static TaskHandle_t s_task = NULL;
static uint8_t s_chunk[CHUNK_BYTES];

// 未播字节数 = head - tail（模 RING_BYTES）
static inline size_t ringUsed(void) {
  size_t h = s_head, t = s_tail;
  return (h >= t) ? (h - t) : (RING_BYTES - t + h);
}

// 保留 2 字节（而不是 1 字节）不写：这样可用容量永远是偶数，
// 生产者写入的字节数也就永远是偶数，不会把某个 16bit 样本从中间切开
// 交给 I2S（切开会产生"咔"的一声）。这也是"满"和"空"的区分点。
static inline size_t ringFree(void) { return RING_BYTES - ringUsed() - 2; }

// ---------------------------------------------------------------------
// 生产者：把 PCM 写进环形缓冲。只有本函数会推进 s_head。
size_t playWrite(const uint8_t* data, size_t len) {
  if (!data || len == 0) return 0;

  // 消费端可能刚请求过 flush，等它处理完再动这块内存
  if (s_flush_req) return 0;

  size_t written = 0;
  size_t head = s_head;
  size_t free_bytes = ringFree();
  while (written < len && free_bytes > 0) {
    size_t to_end = RING_BYTES - head;
    size_t n = len - written;
    if (n > free_bytes) n = free_bytes;
    if (n > to_end)     n = to_end;
    memcpy(&s_ring[head], data + written, n);
    head = (head + n) % RING_BYTES;
    written += n;
    free_bytes -= n;
  }
  s_head = head;   // 一次性发布，消费者只会看到完整的写入
  return written;
}

// 消费者侧执行真正的清空，避免和生产者的 memcpy 抢同一块内存
static void playTaskApplyFlush(void) {
  if (!s_flush_req) return;
  s_tail = s_head;
  s_started = false;
  s_active = false;
  // 必须顺手把功放关掉：下面的"静音关断"判断只在 s_started==true 时才会走到，
  // 这里不清的话打断之后功放会一直挂在使能状态（白耗电 + 底噪）。
  playAmpEnable(false);
  s_flush_req = false;
}

// 生产者：请求清空。真正清空由播放任务在安全点执行。
void playFlush(void) {
  s_flush_req = true;
}

size_t playBuffered(void) { return ringUsed(); }
bool   playIsActive(void) { return s_active; }

// ---------------------------------------------------------------------
// 后台任务：把环形缓冲里的数据喂给 I2S DMA
// ---------------------------------------------------------------------
static void playTask(void*) {
  // 开播前先攒一点数据，避免第一包就 underrun 出"咔"的一声
  const size_t prebuffer =
      CHUNK_BYTES * ((size_t)PLAY_PREBUFFER_MS / PLAY_CHUNK_MS + 1);

  uint32_t prev_ms = millis();
  uint32_t prev_buf = 0;

  for (;;) {
    // 清空请求在这里执行：这是唯一的消费者安全点，
    // 生产者（playWrite）看到 s_flush_req 时会直接返回 0，
    // 所以这里的 memcpy 不会和它抢同一块内存。
    playTaskApplyFlush();

    size_t used = ringUsed();

    if (!s_started) {
      if (used >= prebuffer) {
        s_started = true;
        s_active = true;
        playAmpEnable(true);
        s_last_play_ms = millis();
        prev_ms = s_last_play_ms;
        prev_buf = used;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
    }

    size_t n = used;
    if (n > CHUNK_BYTES) n = CHUNK_BYTES;

    if (n > 0) {
      // 可能要绕环，先拼到临时 chunk 里
      size_t tail = s_tail;
      size_t to_end = RING_BYTES - tail;
      size_t first = (n > to_end) ? to_end : n;
      memcpy(s_chunk, &s_ring[tail], first);
      if (n > first) memcpy(s_chunk + first, s_ring, n - first);
      s_tail = (s_tail + n) % RING_BYTES;   // 先交还空间再写，避免白等

      // I2SClass::write 内部会重试直到写完（或出错），这里不用自己循环。
      // 它是阻塞的，正好起到"按播放速率节流"的作用。
      size_t wrote = s_i2s.write(s_chunk, n);
#if DEBUG_SERIAL
      if (wrote != n) {
        logPrintf("[spk] I2S 写入不完整: %u/%u (err=%d)\n",
                      (unsigned)wrote, (unsigned)n, s_i2s.lastError());
      }
#endif
      s_last_play_ms = millis();
      prev_ms = s_last_play_ms;
      prev_buf = ringUsed();
    } else {
      uint32_t now = millis();

      // 缓冲一直见底 = TTS 推完了：留一小段余量后关功放，
      // 免得把下一轮的开头也吃掉。
      if ((int32_t)(now - s_last_play_ms) >= PLAY_IDLE_OFF_MS) {
        playAmpEnable(false);
        s_started = false;
        s_active = false;
      }

#if DEBUG_SERIAL
      // 兜底提示：写入速度 < 播放速度，I2S 迟早会出现空隙
      if ((int32_t)(now - prev_ms) >= 1000) {
        if (prev_buf < CHUNK_BYTES) {
          logPrintf("[spk] 注意：播放缓冲见底(%u 字节)，PC 推流可能跟不上\n",
                        (unsigned)prev_buf);
        }
        prev_ms = now;
        prev_buf = ringUsed();
      }
#endif
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

// ---------------------------------------------------------------------
bool playBegin() {
  playFlush();

  if (PIN_AMP_EN >= 0) {
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW);   // 先静音，等真的有数据再开
  }

  s_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
  // 单声道：DMA 会把每个样本同时送到 L/R。若用 STEREO 打开就必须自己
  // 把样本交错成 L/R，否则左右声道会各播一半样本（音高翻倍）。
  if (!s_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_MONO)) {
    logPrintf("[spk] I2S 初始化失败 (err=%d)，检查 PIN_I2S_* 是否和接线一致\n",
                  s_i2s.lastError());
    return false;
  }

  logPrintf("[spk] I2S 播放就绪: BCLK=%d LRC=%d DIN=%d AMP_EN=%d @ %d Hz\n",
                (int)PIN_I2S_BCLK, (int)PIN_I2S_LRC, (int)PIN_I2S_DIN,
                (int)PIN_AMP_EN, (int)SAMPLE_RATE);

  // ⚠️ 必须检查返回值（组员 app 版踩过的坑）：RAM 紧张时 xTaskCreate 会静默
  //    失败（返回 pdFAIL），此时 s_task 保持 NULL，没有任何任务喂 I2S ->
  //    现象是"喇叭完全不响"，而且没有任何报错，极难排查。
  //    组员实测：PLAY_RING_MS 调到 4000(128KB 内部 RAM) 时就是这样。
  BaseType_t ok = xTaskCreatePinnedToCore(playTask, "spk", 4096, NULL, 4, &s_task, 1);
  if (ok != pdPASS) {
    logPrintf("[spk] !! 播放任务创建失败(ok=%d)，空闲堆=%u 字节。"
              "RAM 不够，请调小 config.h 的 PLAY_RING_MS\n",
              (int)ok, (unsigned)ESP.getFreeHeap());
    s_task = NULL;
    return false;
  }
  return true;
}

void playEnd() {
  if (s_task) {
    vTaskDelete(s_task);
    s_task = NULL;
  }
  playAmpEnable(false);
  s_i2s.end();
}

void playAmpEnable(bool on) {
  if (PIN_AMP_EN < 0) return;
  digitalWrite(PIN_AMP_EN, on ? HIGH : LOW);
}
