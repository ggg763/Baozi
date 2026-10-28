#include "log.h"
#include "config.h"
#include "protocol.h"
#include "link.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------
// 往"日志通道"写一段文本。
//
// USB 模式：切成 <= PROTO_MAX_PAYLOAD 的段，各自封成 PKT_LOG 帧。
//          发不出去（PC 没在跑 / 缓冲满）就【直接丢弃】——
//          日志绝不能阻塞音频采集，丢几行日志远比掉音频帧好。
// WiFi 模式：串口是空闲的，直接写。
// ---------------------------------------------------------------------
static void logEmit(const char* text, size_t len) {
  if (text == NULL || len == 0) return;

#if USE_USB_LINK
  while (len > 0) {
    uint16_t n = (len > (size_t)PROTO_MAX_PAYLOAD)
                     ? (uint16_t)PROTO_MAX_PAYLOAD
                     : (uint16_t)len;
    if (!linkSendFrame(PKT_LOG, (const uint8_t*)text, n)) return;
    text += n;
    len -= n;
  }
#else
  Serial.write((const uint8_t*)text, len);
#endif
}

void logWrite(const uint8_t* data, size_t len) {
  logEmit((const char*)data, len);
}

void logPrintf(const char* fmt, ...) {
  char buf[224];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n <= 0) return;
  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;   // 截断过
  logEmit(buf, (size_t)n);
}

void logLine(const char* text) {
  if (text == NULL) return;
  logPrintf("%s\n", text);
}
