/* =====================================================================
 *  baozi 机器人 · ESP32-S3-CAM 整合固件
 *
 *  把两件事合到一块板子上：
 *    【耳朵+嘴】MAX9814 麦克风 -> ADC -> 【USB 串口】-> PC(VAD/ASR/LLM/TTS)
 *               PC -> 【USB 串口】-> I2S 功放 -> 喇叭
 *    【眼睛】   摄像头：PC 发 CTRL_CAM_REQ -> 抓一帧 -> PKT_CAM_JPEG 分片回传
 *    【下行】   PC 发的情绪/意图控制帧 -> UART2(GPIO14) -> TD1601（RISC-V 主控）
 *
 *  ★ 2026-11：ESP32 与 PC 之间改走【一根 USB 线】（USB CDC），不再需要 WiFi：
 *      · PC 的网络只用来访问外部服务，不必和 ESP32 在同一个局域网；
 *      · 不用再配 IP、不用 UDP 发现、不用开 HTTP 服务；
 *      · 调试日志也封装成 PKT_LOG 帧走这根线（见 log.h），由 PC 打印。
 *    旧的 WiFi + TCP + HTTP 实现保留在 config.h 的 USE_USB_LINK=0 后面。
 *
 *  协议：见 protocol.h（语音 / 图像 / 日志 / 控制帧）。
 *  引脚、传输方式、参数全部在 config.h 里改。
 *
 *  编译：Arduino IDE + esp32 core >= 3.0.0
 *        开发板 ESP32S3 Dev Module
 *        PSRAM    = OPI PSRAM（必须）
 *        USB CDC On Boot = Enabled（★ 必须：否则 Serial 落到 UART 口，
 *                           PC 那头连不上这个 "COM"）
 * ===================================================================== */

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "esp_camera.h"

// ★★ config.h 必须最先包含 ★★
//   下面几个 #if 用的是 config.h 里的 USE_USB_LINK / WEB_CONSOLE_ENABLE。
//   如果 config.h 排在它们后面，那两个宏在判断时还是"未定义"：
//     #if !USE_USB_LINK    -> !0 = 1，碰巧成立（看起来没事）
//     #if WEB_CONSOLE_ENABLE -> #if 0，整块被跳过
//   后果就是 web_console.h 根本没被 include，编译报
//     'webConsoleBegin' was not declared in this scope
//   所以这两行【不能】往下挪。
#include "config.h"

#if !USE_USB_LINK
// ---- 以下只在 WiFi 模式（USE_USB_LINK=0）下需要 ----
#include <WiFi.h>
#include <WebServer.h>
#include "discovery.h"
#if WEB_CONSOLE_ENABLE
#include "web_console.h"     /* 手机浏览器网页控制台（GET / 与 POST /api/emotion）*/
#endif
#endif

#include "protocol.h"
#include "audio_in.h"
#include "audio_out.h"
#include "link.h"
#include "log.h"

// ============================ 语音：全局状态 ============================
static int16_t  s_mic[MIC_BLOCK_SAMPLES];
static uint32_t s_stat_ms = 0;
static uint32_t s_mic_blocks = 0;
static uint32_t s_mic_timeouts = 0;

// ============================ 眼睛：全局状态 ============================
camera_fb_t* latest_fb = NULL;
volatile bool camera_ok = false;

#if !USE_USB_LINK
// ============================ WiFi 模式：HTTP 服务器 ============================
WebServer server(HTTP_PORT);
// 定时抓的"最新一帧"，只在 WiFi 模式（/capture 用）下维护。
// USB 模式不需要：PC 发一次 CTRL_CAM_REQ 就现抓一次（见 captureAndSend）。
// HTTP 服务器是否已启动。提到全局是因为 setup() 和 loop() 两边都要判断 ——
// 原来它是 loop() 里的 static 局部变量，setup() 已经启动后 loop() 还会再
// 注册一遍路由、再 begin() 一次（重复注册同一组 handler）。
static bool s_http_started = false;

// 路由是否已注册。WebServer::on() 是【追加】而不是替换，
// 所以 WiFi 重连后重开 HTTP 时只能 stop()+begin()，不能再注册一遍 ——
// 否则每断一次网就多一组 handler，最终把链表撑爆。
static bool s_routes_registered = false;

// =====================================================================
//  网络就绪 / 本机 IP —— 兼容 STA（连路由器）与 AP（开热点）两种模式
// =====================================================================
// 为什么要包一层：WiFi.status() == WL_CONNECTED 是【STA 专有】的状态，
// 热点模式下它永远不成立。原来代码到处直接用它，一开热点就会出现
// "HTTP 服务器不启动 / 日志里本机 IP=0.0.0.0" 这类莫名问题。
static bool netReady() {
#if USE_AP_MODE
  return true;                       // 热点开了就是就绪（没有"连不上"这回事）
#else
  return WiFi.status() == WL_CONNECTED;
#endif
}

static String myIpString() {
#if USE_AP_MODE
  return WiFi.softAPIP().toString(); // 固定 192.168.4.1
#else
  return WiFi.localIP().toString();
#endif
}

// 最近 STREAM_ACTIVE_MS 内有没有人来拉过 /stream（用来暂停 loop() 里的定时抓帧）
static uint32_t s_stream_last_ms = 0;

static bool streamActive() {
  return (s_stream_last_ms != 0) &&
         ((uint32_t)(millis() - s_stream_last_ms) < 5000U);
}
#endif  // !USE_USB_LINK

// =====================================================================
//  图像：把一帧 JPEG 按协议分片发给 PC
//
//  为什么要分片：PROTO_MAX_PAYLOAD = 2048，而 VGA 一帧 JPEG 通常 20~60KB。
//  做法：连续发 N 个 PKT_CAM_JPEG 帧，最后一个 CTRL_CAM_DONE 收尾，
//  payload 里带"总字节数"，PC 端据此校验有没有丢片。
// =====================================================================
static bool sendJpegOverLink(const uint8_t* buf, size_t len, uint8_t req_id) {
  size_t off = 0;
  size_t sent = 0;
  while (off < len) {
    uint16_t n = (uint16_t)((len - off > (size_t)PROTO_MAX_PAYLOAD)
                                ? (size_t)PROTO_MAX_PAYLOAD
                                : (len - off));
    if (!linkSendFrame(PKT_CAM_JPEG, buf + off, n)) {
      break;                       // 链路断了：别再空转，让 PC 端超时重试
    }
    off += n;
    sent += n;
  }

  bool ok = (sent == len);
  uint8_t extra[6];
  extra[0] = ok ? 0 : 1;
  uint32_t total = ok ? (uint32_t)sent : 0;
  extra[1] = (uint8_t)(total & 0xFF);
  extra[2] = (uint8_t)((total >> 8) & 0xFF);
  extra[3] = (uint8_t)((total >> 16) & 0xFF);
  extra[4] = (uint8_t)((total >> 24) & 0xFF);
  extra[5] = req_id;
  linkSendCtrl(CTRL_CAM_DONE, extra, sizeof(extra));

  return ok;
}

// 抓一帧并发给 PC。返回 true = 完整发出去了。
// ⚠️ 只能在 loop() 里调用（不能在串口回调里）——见 onFrame() 里 CTRL_CAM_REQ 的说明。
static bool captureAndSend(uint8_t req_id) {
  if (!camera_ok) {
    logLine("[cam] 抓图请求被拒：摄像头未就绪");
    uint8_t extra[6] = {1, 0, 0, 0, 0, req_id};
    linkSendCtrl(CTRL_CAM_DONE, extra, sizeof(extra));
    return false;
  }

  uint32_t t0 = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    logLine("[cam] 抓帧失败（fb_get 返回空）");
    uint8_t extra[6] = {1, 0, 0, 0, 0, req_id};
    linkSendCtrl(CTRL_CAM_DONE, extra, sizeof(extra));
    return false;
  }

  bool ok = sendJpegOverLink(fb->buf, fb->len, req_id);
  size_t len = fb->len;
  esp_camera_fb_return(fb);

  logPrintf("[cam] 已发一帧 %u 字节（%lu ms）%s\n", (unsigned)len,
            (unsigned long)(millis() - t0), ok ? "" : " —— 中途断开");
  return ok;
}

// ---------------------------------------------------------------------
// 图像：待处理的抓图请求（由 onFrame 登记，由 loop() 执行）
// ---------------------------------------------------------------------
static bool     s_cam_req_pending = false;
static uint8_t  s_cam_req_id      = 0;
static uint32_t s_cam_req_ms      = 0;      // 请求登记时刻（超时兜底用）
static bool     s_cam_in_flight   = false;  // 正在抓/发（防止重入时叠请求）

// 一次抓图最多允许占用主循环多久（毫秒）。超过就认定卡住，回报失败。
// 依据：VGA JPEG 抓帧+分片发送实测 100~600ms，8 秒已经非常宽松。
#define CAM_REQ_BUDGET_MS 8000

// loop() 里调用：有请求就抓一帧发出去。
//
// 两个兜底，目的都是"绝不让 PC 干等到超时还不明原因"：
//   1) 摄像头没就绪 / fb_get 返回空 -> 立刻回失败的 CAM_DONE（captureAndSend 内部已处理）
//   2) 请求排队太久（主循环被别的东西占住）-> 丢掉并回失败帧
static void serviceCameraRequest() {
  if (!s_cam_req_pending) return;
  if (s_cam_in_flight) return;          // 上一帧还在发，别叠

  if ((uint32_t)(millis() - s_cam_req_ms) > CAM_REQ_BUDGET_MS) {
    uint8_t req_id = s_cam_req_id;
    s_cam_req_pending = false;
    logPrintf("[cam] 抓图请求排队 %lu ms 未处理，回报失败\n",
              (unsigned long)(millis() - s_cam_req_ms));
    uint8_t extra[6] = {1, 0, 0, 0, 0, req_id};
    linkSendCtrl(CTRL_CAM_DONE, extra, sizeof(extra));
    return;
  }

  s_cam_req_pending = false;
  s_cam_in_flight   = true;
  captureAndSend(s_cam_req_id);
  s_cam_in_flight   = false;
}

void applyDebugCommand(const char* line, uint16_t len);

#if !USE_USB_LINK
void httpStartOnce();
#endif

// =====================================================================
//  握手：开机时主动发一次 HELLO（带设备信息）
//
//  ⚠️ 握手必须是【单向发起 + 单向应答】，绝不能两边都"有问必答"：
//       ESP32 --HELLO--> PC --HELLO_ACK--> ESP32（到此为止，不再回 HELLO）
//     PC 端收到 HELLO 只回一次 HELLO_ACK，收到 HELLO_ACK 什么也不做。
//     两边都回就会变成 HELLO ↔ HELLO_ACK 无限回声，把串口带宽吃满
//     （2026-11 实测踩过：PC 终端"设备上线"疯狂刷屏）。
//
//  为什么不在 loop() 里"没收到 ACK 就定时重发"：
//    USB 打开常常比板子上电晚（先上电、再跑 PC 程序），那时发出去的
//    HELLO 没人应答。所以改成"PC 一联系我们就补发一次"——
//    PC 的 CTRL_PING 保活帧就是天然的联络信号。
// =====================================================================
static bool     s_hello_pending  = true;   // 还没得到 PC 的应答
static uint32_t s_hello_last_ms  = 0;      // 上次发 HELLO 的时刻（限频用）

static const char* kHelloText =
    "esp32s3cam/baozi-voice/16k-mono [+cam] fw/2026-11-05a";

void sendHelloOnce() {
  s_hello_last_ms = millis();
  linkSendCtrl(CTRL_HELLO, (const uint8_t*)kHelloText,
               (uint16_t)strlen(kHelloText));
}

// 收到 PC 的联络（PING）时调用：PC 显然在线，但我们还没被它认出来 -> 补发
static void maybeResendHello() {
  if (!s_hello_pending) return;
  // 限频：PC 每 2 秒一个 PING，别每个都发，20 秒最多重试一次
  if (s_hello_last_ms != 0 &&
      (uint32_t)(millis() - s_hello_last_ms) < 20000) {
    return;
  }
  sendHelloOnce();
}

// =====================================================================
//  语音/图像：处理 PC 发来的帧
//  注意：.ino 里的函数【不要】加 static（Arduino 会自动生成原型，冲突）
// =====================================================================
void onFrame(uint8_t type, uint16_t seq, const uint8_t* payload, uint16_t len) {
  switch (type) {
    case PKT_TTS_PCM: {
      size_t off = 0;
      uint32_t t0 = millis();
      while (off < len) {
        size_t w = playWrite(payload + off, len - off);
        off += w;
        if (off >= len) break;
        if (millis() - t0 > PLAY_WRITE_TIMEOUT_MS) {
          logPrintf("[spk] 播放缓冲满，丢弃剩余 %u 字节\n", (unsigned)(len - off));
          break;
        }
        delay(1);
      }
      break;
    }

    case PKT_CTRL: {
      if (len < 1) break;
      switch (payload[0]) {
        case CTRL_PLAY_BEGIN:
          logLine("[pc] >>> 开始播放 TTS");
          break;
        case CTRL_PLAY_END:
          logLine("[pc] >>> TTS 推送结束");
          break;
        case CTRL_PLAY_ABORT:
          playFlush();
          logLine("[pc] >>> 打断，已请求清空播放缓冲");
          break;
        case CTRL_PING:
          linkSendCtrl(CTRL_PONG);
          maybeResendHello();      // PC 在线但还没见过我们？补发一次 HELLO
          break;

        /* ---- 握手应答：PC 收到了我们的 HELLO ----
         * ⚠️ 这里【只能清标记，绝不能回发 CTRL_HELLO】。
         *    设备信息由下面的 sendHelloOnce() 在开机时主动发一次；
         *    如果这里再回一个 HELLO，PC 收到又回 HELLO_ACK，就成死循环了：
         *      HELLO → HELLO_ACK → HELLO → ……
         *    表现是 PC 终端"设备上线"疯狂刷屏、把串口带宽吃满。
         *    （2026-11 实测踩过这个坑，别改回去。） */
        case CTRL_HELLO_ACK:
          s_hello_pending = false;
          break;

        /* ---- 图像：PC 请求抓一帧（USB 模式唯一的取图方式）----
         * ⚠️ 这里【只登记请求，不现场抓图】。
         *    esp_camera_fb_get() 会阻塞等帧缓冲；而 onFrame() 是在
         *    linkLoop() 的串口读回调里同步执行的，一旦在这里阻塞，
         *    整个 loop() 就停转：麦克风不再上传、心跳不回、PC 端等不到
         *    CAM_DONE 直接 8 秒超时（2026-11 实测：问"你看到了什么"永远失败）。
         *    改成置标志，由 loop() 在安全的地方抓。 */
        case CTRL_CAM_REQ:
          s_cam_req_pending = true;
          s_cam_req_id      = (len >= 2) ? payload[1] : 0;
          s_cam_req_ms      = millis();     // 登记时刻，供 serviceCameraRequest 判超时
          // ★ 登记时就打一条：这样 PC 端一旦超时，日志能立刻区分
          //   "请求根本没到固件"（没有这行）与"到了但没抓成"（有这行、没后续）。
          logPrintf("[cam] 收到抓图请求 #%u\n", (unsigned)s_cam_req_id);
          break;

        /* ---- 情感链路：PC 发来的情绪/意图 -> 经 UART2 转发给 TD1601 ----
         * payload[1..] 是已经拼好的整行（如 "EMO:HAPPY" / "INTENT:CHAT"），
         * 这里原样发出去，TD1601 的 esp_link/emotion_fsm 会解析。
         * 注意：必须带行结束符，TD1601 按 CR/LF 分行。 */
        case CTRL_EMOTION:
        case CTRL_INTENT:
        case CTRL_INTENSITY: {
          if (len > 1) {
            Serial2.write(payload + 1, len - 1);
            Serial2.write('\n');
            Serial2.flush();
            logPrintf("[td] 转发: %.*s\n", (int)(len - 1), (const char*)(payload + 1));
          }
          break;
        }

        // ---- 任意一行文本：原样转发给 TD1601（如 TRACK:ON）----
        // ⚠️ 例外：以 '+' '-' '=' '_' 'p' 'P' 开头的行是本机调试命令
        //    （调麦克风增益），不往外发 —— USB 模式下这根线在跑二进制帧，
        //    没法再在串口里敲字符调参，只能借这条通道。
        case CTRL_TD_TEXT: {
          if (len > 1) {
            char c0 = (char)payload[1];
            if (c0 == '+' || c0 == '-' || c0 == '=' || c0 == '_' ||
                c0 == 'p' || c0 == 'P') {
              applyDebugCommand((const char*)(payload + 1), (uint16_t)(len - 1));
              break;
            }
            Serial2.write(payload + 1, len - 1);
            Serial2.write('\n');
            Serial2.flush();
            logPrintf("[td] 转发: %.*s\n", (int)(len - 1), (const char*)(payload + 1));
          }
          break;
        }

        // ---- 状态字节帧：PC 把 情绪+意图+强度 打包成 1 个字节，原样透传 ----
        // 发出去的是 0xAA + code 两个字节（不含换行）。TD1601 的 esp_link 认帧头。
        // 与上面的文本命令并列存在：文本用于调试/兼容，字节帧用于正式链路。
        case CTRL_STATE: {
          if (len >= 2) {
            Serial2.write((uint8_t)0xAA);
            Serial2.write(payload[1]);
            Serial2.flush();
            logPrintf("[td] state code 0x%02X\n", (unsigned)payload[1]);
          }
          break;
        }
        default:
          break;
      }
      break;
    }

    case PKT_TEXT: {
      // PC 发来的调试文本（比如识别出的句子）
      logPrintf("[pc-text] %.*s\n", (int)len, (const char*)payload);
      break;
    }

    default:
      break;
  }
}

// =====================================================================
//  USB 模式下没有"串口敲字符"这件事（那根线在跑二进制帧），
//  调试命令改为 PC 发一行文本过来：见 onFrame -> CTRL_TD_TEXT -> applyDebugCommand。
//  支持的命令：+ / =（增益 ×1.25）、- / _（增益 ÷1.25）、p / P（清峰值统计）
// =====================================================================
void applyDebugCommand(const char* line, uint16_t len) {
  if (len == 0) return;
  if (line[0] == '+' || line[0] == '=') {
    audioInSetGain(audioInGetGain() * 1.25f);
    logPrintf("[mic] 增益 = %.2f\n", audioInGetGain());
  } else if (line[0] == '-' || line[0] == '_') {
    audioInSetGain(audioInGetGain() / 1.25f);
    logPrintf("[mic] 增益 = %.2f\n", audioInGetGain());
  } else if (line[0] == 'p' || line[0] == 'P') {
    logPrintf("[mic] 峰值已清零(之前的峰值 = %d / 32767)\n", audioInTakePeak());
  }
}

#if !USE_USB_LINK
// WiFi 模式下串口是自由日志口，保留原来的"直接敲字符"调参
void pollSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    char ch = (char)c;
    applyDebugCommand(&ch, 1);
  }
}
#endif

// =====================================================================
//  眼睛：摄像头初始化（返回 true 表示可用）
// =====================================================================
bool cameraInit() {
  if (!psramFound()) {
    logLine("!! 未找到 PSRAM！请在 Arduino IDE 里设 工具->PSRAM = 'OPI PSRAM'");
  } else {
    logPrintf("PSRAM 已找到：%u MB，空闲 %u kB\n",
              ESP.getPsramSize()/1024/1024, ESP.getFreePsram()/1024);
  }

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = CAM_D0_GPIO;  config.pin_d1 = CAM_D1_GPIO;
  config.pin_d2 = CAM_D2_GPIO;  config.pin_d3 = CAM_D3_GPIO;
  config.pin_d4 = CAM_D4_GPIO;  config.pin_d5 = CAM_D5_GPIO;
  config.pin_d6 = CAM_D6_GPIO;  config.pin_d7 = CAM_D7_GPIO;
  config.pin_xclk = CAM_XCLK_GPIO; config.pin_pclk = CAM_PCLK_GPIO;
  config.pin_vsync = CAM_VSYNC_GPIO; config.pin_href = CAM_HREF_GPIO;
  config.pin_sscb_sda = CAM_SIOD_GPIO; config.pin_sscb_scl = CAM_SIOC_GPIO;
  config.pin_pwdn = CAM_PWDN_GPIO; config.pin_reset = CAM_RESET_GPIO;
  config.xclk_freq_hz = CAM_XCLK_HZ;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = CAM_FRAME_SIZE;      // VGA 640x480
  config.jpeg_quality = CAM_JPEG_QUALITY;    // 12
  config.fb_count = CAM_FB_COUNT;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    logPrintf("[失败] 摄像头初始化错误 err=0x%x\n", (unsigned)err);
    if (!psramFound()) {
      logLine("原因：IDE 里 PSRAM 被禁用。修复：工具->PSRAM = 'OPI PSRAM' 后重烧。");
    } else {
      logLine("提示：把 xclk 降到 10MHz、fb_count 改为 1、或调低 frame_size。");
    }
    return false;
  }

  // ---- 应用图像调优参数 ----
  // ⚠️ 必须在 esp_camera_init() 之后设置：init 会重跑整张传感器寄存器表，
  //    把之前设的值全部打回默认（参考实现 README 里记过这个坑）。
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    // 这三个宏在 config.h 里可调，0 表示不动（保持传感器默认）
    if (CAM_BRIGHTNESS != 0) s->set_brightness(s, CAM_BRIGHTNESS);
    if (CAM_SATURATION != 0) s->set_saturation(s, CAM_SATURATION);
    if (CAM_CONTRAST   != 0) s->set_contrast(s,   CAM_CONTRAST);

    // ⚠️ camera_status_t 里【没有】width/height 字段，只有 framesize 枚举
    //    （见 esp32-camera 的 sensor.h）。要显示实际像素得自己把枚举换算成宽高。
    framesize_t fs = s->status.framesize;
    uint16_t w = 0, h = 0;
    switch (fs) {
      case FRAMESIZE_QQVGA:  w = 160;  h = 120;  break;
      case FRAMESIZE_QVGA:   w = 320;  h = 240;  break;
      case FRAMESIZE_VGA:    w = 640;  h = 480;  break;
      case FRAMESIZE_SVGA:   w = 800;  h = 600;  break;
      case FRAMESIZE_XGA:    w = 1024; h = 768;  break;
      case FRAMESIZE_HD:     w = 1280; h = 720;  break;
      case FRAMESIZE_UXGA:   w = 1600; h = 1200; break;
      default:               w = 0;    h = 0;     break;
    }
    logPrintf("[cam] 分辨率=%ux%u (framesize=%d)  quality=%d  PID=0x%02X\n",
              (unsigned)w, (unsigned)h, (int)fs,
              (int)s->status.quality, (unsigned)s->id.PID);
  }

  logPrintf("摄像头 OK。空闲堆=%uKB 空闲psram=%uKB\n",
            (unsigned)(ESP.getFreeHeap()/1024),
            (unsigned)(ESP.getFreePsram()/1024));
  return true;
}

// =====================================================================
void setup() {
  // ★ 顺序很重要：先把串口（USB CDC）起来并挂上帧回调，
  //   否则后面所有 logXxx() 都发不出去（PC 也就看不到启动日志）。
  linkSetHandler(onFrame);
  linkBegin();                       // USB：初始化串口，等 PC 打开（最多 3 秒）

  logLine("");
  logLine("==================================================");
  logLine(" baozi 机器人 · ESP32-S3-CAM 整合固件");
  // ★ 版本号：排查时第一眼看这个，能立刻确认板子上跑的是不是最新固件。
  //   改过通信 / 抓图 / 握手逻辑就要动它，否则"到底烧没烧进去"说不清。
  //   例：问"你看到了什么"没反应时，先看这行是不是当前版本。
  logLine("  固件: 2026-11-06  双模链路 + 组员 app 特性（多热点/手机控制台）");
#if USE_USB_LINK
  logLine("  链路: USB 串口（PC 只管外网，不需要 WiFi/IP）");
  logLine("  耳朵: MAX9814 -> ADC -> USB -> PC");
  logLine("  嘴  : PC -> USB -> I2S 功放 -> 喇叭");
  logLine("  眼睛: 摄像头 -> USB（PC 发抓图请求）");
#else
  logLine("  链路: WiFi + TCP（USE_USB_LINK=0 的旧模式）");
  logLine("  耳朵: MAX9814 -> ADC -> TCP -> PC");
  logLine("  嘴  : PC -> TCP -> I2S 功放 -> 喇叭");
  logLine("  眼睛: 摄像头 -> HTTP /capture -> PC");
#endif
  logLine("  下行: PC 情绪/意图 -> UART2(GPIO14) -> TD1601");
  logLine("==================================================");
  logPrintf("空闲堆=%uKB psram=%uKB 空闲psram=%uKB\n",
            (unsigned)(ESP.getFreeHeap()/1024),
            (unsigned)(ESP.getPsramSize()/1024),
            (unsigned)(ESP.getFreePsram()/1024));

  if (PIN_STATUS_LED >= 0) {
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);
  }

  // ---- 眼睛：摄像头（不依赖网络；失败也让语音照常工作）----
  camera_ok = cameraInit();

  // ---- 嘴：I2S 功放（不依赖网络）----
  bool spk_ok = playBegin();

  // ---- 发给 TD1601 的串口（只发不收）----
  Serial2.begin(TD_UART_BAUD, SERIAL_8N1, TD_UART_RX_GPIO, TD_UART_TX_GPIO);
  logPrintf("[td] UART2 就绪：TX=GPIO%d -> TD1601 PB18\n", TD_UART_TX_GPIO);

#if !USE_USB_LINK
  // ---- WiFi 模式额外的东西：WiFi + UDP 发现 + HTTP 服务器 ----
  discoveryBegin();
  logPrintf("[disc] 本机 IP=%s  目标 PC=%s（UDP %d，每 2 秒互播）\n",
            discoveryMyIp(),
            discoveryServerFound() ? discoveryServerHost() : "尚未发现PC",
            DISCOVERY_PORT);
  if (netReady()) {
    httpStartOnce();
  } else {
    logLine("网络未就绪，HTTP 服务器暂未启动（会在就绪后由主循环补启）");
  }
#endif

  // ---- 耳朵：ADC 采集 ----
  // 采集任务启动前会轮询这个钩子：等链路就绪（USB：等 PC 开口）再开 ADC。
  audioInSetReadyCheck(linkConnected);
  bool mic_ok = audioInBegin();

  logPrintf("[init] 麦克风=%s  喇叭=%s  摄像头=%s\n",
            mic_ok ? "OK" : "失败", spk_ok ? "OK" : "失败",
            camera_ok ? "OK" : "失败");
  if (!mic_ok) {
    logLine("[init] 提示:PIN_MIC_ADC 必须落在 GPIO1~GPIO10(ADC1)。");
  }

  s_stat_ms = millis();
}

// =====================================================================
void loop() {
  // ---- 链路收发（USB：读串口切帧；WiFi：重连 + 收帧）----
  linkLoop();

  // ---- 握手：PC 一上线就立刻把设备信息报过去（只发一次，等它应答）----
  // USB 线拔了重插、或 PC 重启后都会走到这里重新握手。
  if (linkConnected() && s_hello_last_ms == 0) {
    sendHelloOnce();
  }

  // ---- 图像：PC 有抓图请求就在这里处理 ----
  // 必须放在 loop() 而不是串口回调里：esp_camera_fb_get() 会阻塞等帧缓冲，
  // 在回调里阻塞会让整个 loop()（含麦克风上传）停转。见 onFrame() 的说明。
  serviceCameraRequest();

  // ---- 采集一个 20ms 的块并推给 PC ----
  {
    size_t n = audioInReadBlock(s_mic, MIC_BLOCK_MS * 2);
    if (n >= MIC_BLOCK_SAMPLES) {
      s_mic_blocks++;
      if (linkConnected()) {
        // 发不出去不重试：宁可丢这 20ms 也别阻塞采集节奏
        linkSendFrame(PKT_MIC_PCM, (const uint8_t*)s_mic,
                      (uint16_t)(n * sizeof(int16_t)));
      }
    } else {
      s_mic_timeouts++;
    }
  }

#if !USE_USB_LINK
  // ---- WiFi 模式：HTTP 服务器（PC 取图 / 收描述 / 手机控制台）----
  // WiFi 重连后 IP 可能变了，旧 server 绑的地址已失效，必须关掉重开。
  // link.cpp 的 linkLoop() 里置这个标志（见 s_http_need_restart 说明）。
  if (s_http_need_restart) {
    s_http_need_restart = false;
    server.stop();
    s_http_started = false;        // 让 httpStartOnce() 重新 begin()（路由不重复注册）
    logLine("[net] WiFi 重连后 IP 已变，正在重开 HTTP 服务器 ...");
  }
  httpStartOnce();               // 自己带判断：已启动就立刻返回
  if (s_http_started) server.handleClient();

  // ---- WiFi 模式：周期性刷新一帧供 /capture 抓取 ----
  // ⚠️ 有 /stream 客户端在拉流时跳过：两边都调 esp_camera_fb_get() 会抢
  //    同一套双缓冲（fb_count=2），出现"其中一路拿不到帧"的随机失败。
  if (camera_ok && !streamActive()) {
    static uint32_t tLast = 0;
    if (millis() - tLast >= CAM_REFRESH_MS) {
      if (latest_fb) { esp_camera_fb_return(latest_fb); latest_fb = NULL; }
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) latest_fb = fb;
      tLast = millis();
    }
  }

  // ---- 把 TD1601 回传的数据转发给 PC（USB 模式下也走帧）----
  while (Serial2.available()) {
    uint8_t b = (uint8_t)Serial2.read();
    logWrite(&b, 1);
  }
#endif

  // ---- 状态灯 ----
  if (PIN_STATUS_LED >= 0) {
    static uint32_t blink = 0;
    if (millis() - blink > 200) {
      blink = millis();
      digitalWrite(PIN_STATUS_LED, (linkConnected() && playIsActive()) ? HIGH : LOW);
    }
  }

  // ---- 播放状态上报（每 PLAY_STATE_MS）----
  // PC 需要 ESP32 的【真实】播放进度才能闭环（协议里 CTRL_AUDIO_STATE 早就定义，
  // 但一直没人发，PC 只能按时间推算水位 —— 那是"断续/幻听/吃话"的共同根因）。
  // payload：active(1字节) + buffered(4字节小端，还没播完的字节数)
  if (linkConnected()) {
    static uint32_t t_state = 0;
    if (millis() - t_state >= PLAY_STATE_MS) {
      t_state = millis();
      uint8_t extra[5];
      uint32_t n = (uint32_t)playBuffered();
      extra[0] = playIsActive() ? 1 : 0;
      extra[1] = (uint8_t)(n & 0xFF);
      extra[2] = (uint8_t)((n >> 8) & 0xFF);
      extra[3] = (uint8_t)((n >> 16) & 0xFF);
      extra[4] = (uint8_t)((n >> 24) & 0xFF);
      linkSendCtrl(CTRL_AUDIO_STATE, extra, sizeof(extra));
    }
  }

  // ---- 每 5 秒打印一次状态 ----
  if (millis() - s_stat_ms >= 5000) {
    int peak = audioInTakePeak();
    float rms_dbfs = (peak > 0) ? (20.0f * log10f((float)peak / 32768.0f)) : -99.0f;
    logPrintf("[stat] 链路=%s 采集块=%lu 超时=%lu 峰值=%d(%.1f dBFS) 增益=%.2f "
              "DC=%.0f 播放缓冲=%u 播放中=%s 摄像头=%s 空闲堆=%u\n",
              linkConnected() ? "OK" : "断开",
              (unsigned long)s_mic_blocks, (unsigned long)s_mic_timeouts,
              peak, rms_dbfs, (double)audioInGetGain(), (double)audioInGetDcLevel(),
              (unsigned)playBuffered(), playIsActive() ? "是" : "否",
              camera_ok ? "OK" : "无", (unsigned)ESP.getFreeHeap());
    s_stat_ms = millis();
  }
}

#if !USE_USB_LINK
// =====================================================================
//  眼睛：WiFi 模式下的 HTTP 传输 JPEG 用的内存流
// =====================================================================
class MemoryStream : public Stream {
public:
  MemoryStream(const uint8_t* data, size_t len) : _data(data), _len(len), _pos(0) {}
  size_t write(uint8_t) override { return 0; }
  int available() override { return _len - _pos; }
  int read() override { return (_pos < _len) ? _data[_pos++] : -1; }
  int peek() override { return (_pos < _len) ? _data[_pos] : -1; }
  void flush() override {}
private:
  const uint8_t* _data; size_t _len; size_t _pos;
};

void handleCapture() {
  if (!camera_ok || !latest_fb) { server.send(503, "text/plain", "no image"); return; }
  MemoryStream stream(latest_fb->buf, latest_fb->len);
  server.send(200, "image/jpeg", stream, latest_fb->len);
}

// =====================================================================
//  MJPEG 实时流（/stream）—— 非阻塞分帧：每次只发一帧就返回
// =====================================================================
void handleStream() {
  WiFiClient client = server.client();
  s_stream_last_ms = millis();      // 标记"有人在拉流"，让 loop() 暂停定时抓帧

  if (!camera_ok) {
    client.print("HTTP/1.1 503 Service Unavailable\r\n"
                 "Content-Type: text/plain\r\n"
                 "Connection: close\r\n\r\n"
                 "camera not ready");
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    client.print("HTTP/1.1 503 Service Unavailable\r\n"
                 "Content-Type: text/plain\r\n"
                 "Connection: close\r\n\r\n"
                 "frame grab failed");
    return;
  }

  client.print("HTTP/1.1 200 OK\r\n"
               "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
               "Cache-Control: no-store\r\n"
               "Connection: close\r\n"
               "\r\n");
  client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                (unsigned)fb->len);
  client.write(fb->buf, fb->len);
  client.print("\r\n");

  esp_camera_fb_return(fb);
  client.stop();     // 主动关连接，让 PC 立刻发起下一次请求
}

void handleSendText() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Missing body"); return; }
  String text = server.arg("plain");

  // 经 UART2(GPIO14) 转发给 TD1601（PB18 = UART3 RX），按行发送。
  Serial2.print(text);
  Serial2.write('\n');
  Serial2.flush();

  logPrintf(">> 已转发给 TD1601（%u 字节）：%s\n",
            (unsigned)text.length(), text.c_str());

  server.send(200, "text/plain", "OK");
}

void httpStartOnce() {
  if (s_http_started) return;
  if (!netReady()) return;          // WiFi 还没连上：等主循环下次再来补启
  if (!s_routes_registered) {       // 路由只注册一次（见 s_routes_registered 说明）
    server.on("/capture", handleCapture);
    server.on("/stream", handleStream);      // MJPEG 实时流
    server.on("/send_text", HTTP_POST, handleSendText);
#if WEB_CONSOLE_ENABLE
    webConsoleBegin(server);        // 手机网页控制台：GET / 与 POST /api/emotion
#endif
    s_routes_registered = true;
  }
  server.begin();
  s_http_started = true;
  logPrintf("HTTP 已启动：取图 http://%s/capture ，流 http://%s/stream\n",
            myIpString().c_str(), myIpString().c_str());
#if WEB_CONSOLE_ENABLE
  logPrintf("手机控制台：http://%s/  （手机需连同一 WiFi）\n", myIpString().c_str());
#endif
}
#endif  // !USE_USB_LINK
