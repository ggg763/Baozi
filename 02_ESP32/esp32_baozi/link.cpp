#include "link.h"
#include "config.h"
#include "log.h"

#include <Arduino.h>
#include <string.h>

#if !USE_USB_LINK
#include "discovery.h"          /* 自动发现：动态获取 PC 地址（仅 WiFi 模式） */
#include <WiFi.h>
#include <WiFiClient.h>
#endif

// =====================================================================
//  两种实现共用的部分：发送序号、收帧缓冲、切帧状态机
// =====================================================================
static FrameHandler s_handler = NULL;
static uint16_t s_tx_seq = 0;

static uint8_t s_rx[PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD];
static size_t  s_rx_len = 0;

// WiFi 掉线重连后 IP 可能变了 -> HTTP 服务器要重开。
// linkLoop() 里置 true，.ino 主循环看到后关掉旧 server 再 begin()。
// （USB 模式下永远不会置 true，但符号照样存在，免得两种模式引用不一致。）
volatile bool s_http_need_restart = false;

void linkSetHandler(FrameHandler h) { s_handler = h; }

bool linkSendCtrl(uint8_t ctrl_cmd, const uint8_t* extra, uint16_t extra_len) {
  uint8_t buf[64];
  if (extra_len > sizeof(buf) - 1) extra_len = sizeof(buf) - 1;
  buf[0] = ctrl_cmd;
  if (extra_len && extra) memcpy(buf + 1, extra, extra_len);
  return linkSendFrame(PKT_CTRL, buf, (uint16_t)(extra_len + 1));
}

// ---------------------------------------------------------------------
// 把收到的字节流切成帧。
//
// 串口/TCP 都是字节流：一次 read 可能拿到半帧或好几帧，所以这里用
// 一个累积缓冲 + 状态机。magic 对不上就逐字节重新同步 —— 这样即使
// 中途丢过一次字节（比如 USB 暂时写不进去），也能自己恢复。
// ---------------------------------------------------------------------
static void feed(const uint8_t* data, size_t n) {
  while (n > 0) {
    size_t space = sizeof(s_rx) - s_rx_len;
    size_t take = (n < space) ? n : space;
    if (take == 0) {
      // 缓冲满了说明看到的帧头是假的，丢掉一个字节重新同步
      memmove(s_rx, s_rx + 1, s_rx_len - 1);
      s_rx_len--;
      continue;
    }
    memcpy(s_rx + s_rx_len, data, take);
    s_rx_len += take;
    data += take;
    n -= take;

    // 尽量把缓冲里能解析的帧都解析掉
    bool progress = true;
    while (progress) {
      progress = false;

      if (s_rx_len < PROTO_HEADER_LEN) break;

      if (s_rx[0] != PROTO_MAGIC0 || s_rx[1] != PROTO_MAGIC1) {
        memmove(s_rx, s_rx + 1, s_rx_len - 1);   // 逐字节重新同步
        s_rx_len--;
        progress = true;
        continue;
      }

      uint16_t plen = (uint16_t)s_rx[6] | ((uint16_t)s_rx[7] << 8);
      if (plen > PROTO_MAX_PAYLOAD) {            // 长度非法 = 流错位
        memmove(s_rx, s_rx + 1, s_rx_len - 1);
        s_rx_len--;
        progress = true;
        continue;
      }

      if (s_rx_len < (size_t)(PROTO_HEADER_LEN + plen)) break;   // 还没收全

      uint8_t  type = s_rx[2];
      uint16_t seq  = (uint16_t)s_rx[4] | ((uint16_t)s_rx[5] << 8);
      if (s_handler) s_handler(type, seq, s_rx + PROTO_HEADER_LEN, plen);

      size_t consumed = PROTO_HEADER_LEN + plen;
      memmove(s_rx, s_rx + consumed, s_rx_len - consumed);
      s_rx_len -= consumed;
      progress = true;
    }
  }
}

// =====================================================================
//  ★ 默认实现：USB 串口（USB CDC）直连 PC
//
//  和 TCP 版最大的区别是"连接"这个概念：
//    · 没有 IP、没有端口、没有套接字，只有一根线；
//    · 判断"PC 在不在"不用 DTR（不同 core / 驱动表现不一致），
//      而是看【PC 最近有没有跟我们说话】：
//        PC 打开串口后立刻发 CTRL_HELLO_ACK，之后每 2 秒发一次 CTRL_PING，
//        超过 USB_LINK_IDLE_MS 没收到任何帧 = PC 没在跑。
//    · 因此 PC 那边一关程序，这边就自动停止推音频（不空转、不灌数据）。
// =====================================================================
#if USE_USB_LINK

static bool     s_begun      = false;
static uint32_t s_last_rx_ms = 0;      // 最近一次收到 PC 帧的时刻；0 = 还没说过话

// USB 写不进去时最多重试多久（毫秒）。一般瞬间就完成；真卡住就放弃这一帧，
// 让对端靠 magic 重新同步 —— 音频/图像丢一帧无所谓，卡住 loop() 才是灾难。
static const uint32_t kWriteTimeoutMs = 1000;

static bool usbWriteAll(const uint8_t* data, size_t len) {
  uint32_t t0 = millis();
  while (len > 0) {
    size_t n = Serial.write(data, len);
    if (n == 0) {
      if ((uint32_t)(millis() - t0) > kWriteTimeoutMs) return false;
      delay(1);
      continue;
    }
    data += n;
    len  -= n;
  }
  return true;
}

bool linkBegin() {
  // 波特率对 USB CDC 无意义（USB 全速），只有在用 CH340/CP2102 这类
  // USB-UART 桥、Serial 落到 UART0 时才是真的（必须与 PC 端一致）。
  Serial.begin(USB_LINK_BAUD);

  // 等 PC 打开串口（最多 USB_WAIT_SERIAL_MS）。等不到也照常往下跑：
  // 日志先丢，PC 一打开就恢复，不影响采集/播放初始化。
  uint32_t t0 = millis();
  while (!Serial && (uint32_t)(millis() - t0) < USB_WAIT_SERIAL_MS) {
    delay(50);
  }

  s_begun      = true;
  s_rx_len     = 0;
  s_last_rx_ms = 0;                    // 等 PC 先开口
  return true;
}

bool linkConnected() {
  if (!s_begun || s_last_rx_ms == 0) return false;
  return (uint32_t)(millis() - s_last_rx_ms) < USB_LINK_IDLE_MS;
}

bool linkSendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
  if (!s_begun) return false;
  if (len > PROTO_MAX_PAYLOAD) return false;
  // PC 没在跑就别往串口塞东西：USB CDC 未连接时的写入行为在不同 core
  // 版本上不一致（有的返回 0、有的会等），统一在这里挡掉最省心。
  if (!linkConnected()) return false;

  uint8_t hdr[PROTO_HEADER_LEN];
  hdr[0] = PROTO_MAGIC0;
  hdr[1] = PROTO_MAGIC1;
  hdr[2] = type;
  hdr[3] = 0;
  hdr[4] = (uint8_t)(s_tx_seq & 0xFF);
  hdr[5] = (uint8_t)(s_tx_seq >> 8);
  hdr[6] = (uint8_t)(len & 0xFF);
  hdr[7] = (uint8_t)(len >> 8);
  s_tx_seq++;

  // 头和数据必须连着写出去：中间不能插别的帧（日志帧也会走这里，
  // 但都在同一个 loop 线程里顺序调用，所以天然不会交错）。
  if (!usbWriteAll(hdr, PROTO_HEADER_LEN)) return false;
  if (len && !usbWriteAll(payload, len)) return false;
  return true;
}

void linkLoop() {
  if (!s_begun) return;

  // 把串口缓冲里的字节全部读出来喂给切帧器。
  // 分小块读：一次读完可能几百 KB（图像/音频混在一起），
  // 而 feed() 里的回调需要及时返回，别长时间霸占 loop()。
  for (;;) {
    int avail = Serial.available();
    if (avail <= 0) return;

    uint8_t buf[256];
    size_t want = (avail > (int)sizeof(buf)) ? sizeof(buf) : (size_t)avail;
    int n = Serial.readBytes(buf, want);
    if (n <= 0) return;

    s_last_rx_ms = millis();          // PC 还在说话 -> 链路活着
    feed(buf, (size_t)n);
  }
}

// =====================================================================
//  备用实现：WiFi + TCP（USE_USB_LINK=0 时才编译）
//  保留原样，方便哪天需要无线时切回去。
// =====================================================================
#else

static WiFiClient s_sock;
static uint32_t s_last_reconnect_ms = 0;
static uint32_t s_last_rx_ms = 0;

// 当前用的是第几个热点（0 = WIFI_SSID_LIST[0]），用于日志和重连优先顺序
static int s_cur_net = 0;

// 重连防重入：wifiConnectAny() 本身是阻塞的（最坏 N × WIFI_PER_NET_TIMEOUT_MS），
// 不能在自己内部被再次触发。
static bool s_reconnecting = false;

// 两轮重连之间等多久。热点扫描 + 逐个尝试本身就要 10~20 秒，所以给 10 秒。
#define WIFI_RETRY_INTERVAL_MS  10000

#if !USE_AP_MODE
// ---------------------------------------------------------------------
// 连一个指定热点，最多等 timeout_ms
// （多热点自动切换，来自组员 app 版固件：换环境不用重烧固件）
// ---------------------------------------------------------------------
static bool wifiTryOne(int idx, uint32_t timeout_ms) {
  const char* ssid = WIFI_SSID_LIST[idx];
  const char* pass = WIFI_PASSWORD_LIST[idx];

  Serial.printf("[net] 尝试 \"%s\" (第 %d/%d 个热点)...\n",
                ssid, idx + 1, WIFI_NET_COUNT);

  WiFi.disconnect(true, false);      // 清掉上一个的残留（wifiOff=false 不关射频）
  delay(100);
  WiFi.begin(ssid, pass);

  uint32_t t0 = millis();
  uint8_t dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
    delay(250);
    Serial.print('.');
    if (++dots == 40) {              // 每 10 秒报一次状态码
      Serial.printf(" [status=%d]", (int)WiFi.status());
      dots = 0;
    }
  }
  Serial.println();

  int st = (int)WiFi.status();
  if (st == WL_CONNECTED) {
    s_cur_net = idx;
    Serial.printf("[net] OK \"%s\" IP=%s RSSI=%d dBm\n",
                  ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  // status 码含义：1=SSID未找到 4=握手失败(密码错) 6=被踢 255=无
  Serial.printf("[net] 失败 \"%s\" status=%d", ssid, st);
  if (st == 1)      Serial.println("（找不到该 SSID：热点没开 / 5GHz / 太远）");
  else if (st == 4) Serial.println("（握手失败：密码错或加密方式不是 WPA2）");
  else if (st == 6) Serial.println("（被对方断开：多半是 5GHz 或路由器限流）");
  else              Serial.println();
  return false;
}

// ---------------------------------------------------------------------
// 遍历所有热点，连上就停。
// 先试上次连成功的那个（省时间），再依次试其余。
// ---------------------------------------------------------------------
static bool wifiConnectAny() {
  for (int k = 0; k < WIFI_NET_COUNT; k++) {
    int idx = (s_cur_net + k) % WIFI_NET_COUNT;
    if (wifiTryOne(idx, WIFI_PER_NET_TIMEOUT_MS)) return true;
  }
  Serial.println("[net] !!! 所有热点都连不上。检查：");
  Serial.println("[net]   ① 热点开了吗？是不是 2.4GHz？");
  Serial.println("[net]   ② config.h 里的 SSID / 密码和手机里的一致吗？");
  Serial.println("[net]   ③ ESP32 和热点距离别超过 10 米");
  return false;
}

// ---------------------------------------------------------------------
// 诊断：把扫描到的热点全打出来，方便核对 SSID 是否对得上、频段是否支持
// 只影响开机那几秒，正常用不上，出问题一眼定位
// ⚠️ 必须在 WiFi.mode(WIFI_STA) 之后调，否则 radio 没起来，scanNetworks 会返回 -1
// ---------------------------------------------------------------------
static void scanAndDump() {
  Serial.println("[scan] >>> 进入扫描诊断 <<<");
  Serial.printf("[scan] WiFi.status()=%d\n", (int)WiFi.status());

  /* 关键：ESP32-S3 默认只扫 2.4GHz(1~13)。若热点跑在 5GHz(36~165)，
   * 默认扫描根本看不到它 —— 表现为"热点明明开着但扫不到"。
   * 这里显式把两个频段都扫一遍，并把信道号打出来，一看就能判定。 */
  Serial.println("[scan] ---- 2.4GHz 扫描 ----");
  int n24 = WiFi.scanNetworks(false, true, false, 300, 0);
  Serial.printf("[scan] 2.4G 返回 n=%d\n", n24);
  Serial.println("[scan] #  RSSI  CH  BSSID              SSID");
  bool f24[WIFI_NET_COUNT];
  bool f5[WIFI_NET_COUNT];
  for (int k = 0; k < WIFI_NET_COUNT; k++) { f24[k] = false; f5[k] = false; }
  for (int i = 0; i < n24; i++) {
    String ssid = WiFi.SSID(i);
    for (int k = 0; k < WIFI_NET_COUNT; k++) {
      if (ssid == WIFI_SSID_LIST[k]) f24[k] = true;
    }
    Serial.printf("[scan] %2d  %4d  %2d  %s  \"%s\" (%u bytes)\n",
                  i, WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.BSSIDstr(i).c_str(),
                  ssid.c_str(), (unsigned)ssid.length());
  }
  WiFi.scanDelete();

  Serial.println("[scan] ---- 5GHz 扫描（ESP32 只能看，不能连）----");
  int n5 = WiFi.scanNetworks(false, true, false, 300, 1);
  Serial.printf("[scan] 5G 返回 n=%d\n", n5);
  for (int i = 0; i < n5; i++) {
    String ssid = WiFi.SSID(i);
    for (int k = 0; k < WIFI_NET_COUNT; k++) {
      if (ssid == WIFI_SSID_LIST[k]) f5[k] = true;
    }
    Serial.printf("[scan] %2d  %4d  %2d  %s  \"%s\" (%u bytes)\n",
                  i, WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.BSSIDstr(i).c_str(),
                  ssid.c_str(), (unsigned)ssid.length());
  }
  WiFi.scanDelete();

  Serial.println("[scan] ===== 结论 =====");
  for (int k = 0; k < WIFI_NET_COUNT; k++) {
    Serial.printf("[scan] 目标 \"%s\"：2.4G=%s  5G=%s  -> %s\n",
                  WIFI_SSID_LIST[k],
                  f24[k] ? "有" : "无", f5[k] ? "有" : "无",
                  f24[k] ? "可以连" : (f5[k] ? "只在5GHz，连不了！" : "没扫到"));
  }
  Serial.println("[scan] >>> 扫描诊断结束 <<<");
}
#endif  // !USE_AP_MODE

// ---------------------------------------------------------------------
bool linkBegin() {
#if USE_AP_MODE
  // ============ AP 模式：ESP32 自己开热点，PC 直连 ============
  // 为什么（2026-10 实测）：PC↔ESP32 之间是三条【同时】跑的实流
  //   · 麦克风 PCM 上行  32 KB/s
  //   · TTS PCM    下行  32 KB/s
  //   · MJPEG 视频流下行 约 100~200 KB/s
  // 走路由器时这三路和全网其他设备抢同一信道，摄像头明显不流畅；
  // 直连热点只有 PC 一个客户端，没有中间跳数，帧率和延迟都最稳。
  //
  // ⚠️ 代价：PC 连了热点就连不上原路由器 → 必须用【网线】接外网，
  //    否则 edge-tts（唯一依赖外网的环节）会哑，包子说不出话。
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);              // 关掉省电，否则 TCP 延迟会忽高忽低

  IPAddress ap_ip(192, 168, 4, 1);
  IPAddress gw(192, 168, 4, 1);
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(ap_ip, gw, mask);

  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CLIENTS);
  if (!ok) {
    Serial.println("[net] 开热点失败");
    return false;
  }
  Serial.printf("[net] 热点已开启: SSID=\"%s\"  密码=%s  信道=%d\n",
                AP_SSID, AP_PASSWORD, (int)AP_CHANNEL);
  Serial.printf("[net] ESP32 地址固定为 %s —— PC 连上热点后 /stream 就在这个地址\n",
                WiFi.softAPIP().toString().c_str());
  Serial.println("[net] 提示：PC 请用【网线】接外网，否则 edge-tts 会哑（包子说不出话）");

  // 等 PC 连进来（不等也能往下走；TCP 会在 linkLoop 里重试）
  uint32_t t0 = millis();
  while (WiFi.softAPgetStationNum() == 0 && millis() - t0 < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.softAPgetStationNum() == 0) {
    Serial.println("[net] 还没有设备接入热点（PC 连上后会自动重连）");
  } else {
    Serial.printf("[net] 已有 %d 台设备接入热点\n", (int)WiFi.softAPgetStationNum());
  }
  return true;

#else
  // ============ STA 模式：ESP32 连路由器（多热点自动切换）============
  WiFi.mode(WIFI_STA);               // radio 必须先起来，否则扫不到任何东西
  WiFi.setSleep(false);              // 关掉省电，否则 TCP 延迟会忽高忽低

  scanAndDump();                     // radio 已就绪，现在扫描才有结果

#if USE_STATIC_IP
  // 固定 IP：避免每次上电 DHCP 换地址（PC / ESP32 都不用再改配置）
  {
    IPAddress ip, gw, mask, dns;
    ip.fromString(STATIC_IP);
    gw.fromString(STATIC_GW);
    mask.fromString(STATIC_MASK);
    dns.fromString(STATIC_DNS);
    if (!WiFi.config(ip, gw, mask, dns)) {
      Serial.println("[net] 静态 IP 配置失败，回退 DHCP");
    } else {
      Serial.printf("[net] 使用静态 IP: %s\n", STATIC_IP);
    }
  }
#endif

  // 挨个试 config.h 里列的热点，连上就停
  bool ok = wifiConnectAny();

  // 记一下时间戳：刚连完/刚失败，都不要立刻再触发 linkLoop 里的重连
  s_last_reconnect_ms = millis();
  if (!ok) return false;

  logPrintf("[net] WiFi 已连接 \"%s\", IP=%s, RSSI=%d dBm\n",
            WiFi.SSID().c_str(),
            WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
#endif
}

bool linkConnected() { return s_sock.connected(); }

// ---------------------------------------------------------------------
bool linkSendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
  if (!s_sock.connected()) return false;
  if (len > PROTO_MAX_PAYLOAD) return false;

  uint8_t hdr[PROTO_HEADER_LEN];
  hdr[0] = PROTO_MAGIC0;
  hdr[1] = PROTO_MAGIC1;
  hdr[2] = type;
  hdr[3] = 0;
  hdr[4] = (uint8_t)(s_tx_seq & 0xFF);
  hdr[5] = (uint8_t)(s_tx_seq >> 8);
  hdr[6] = (uint8_t)(len & 0xFF);
  hdr[7] = (uint8_t)(len >> 8);
  s_tx_seq++;

  // ESP32 默认 TCP_NODELAY 是开着的，两段 write 之间不会因为 Nagle 被攒在一起
  if (s_sock.write(hdr, PROTO_HEADER_LEN) != PROTO_HEADER_LEN) return false;
  if (len && s_sock.write(payload, len) != len) return false;
  return true;
}

// ---------------------------------------------------------------------
void linkLoop() {
#if USE_AP_MODE
  // AP 模式：热点是一直开着的，"WiFi 断开"这个概念不存在。
  // 只需要看有没有客户端连进来 —— 没客户端时不必反复尝试 TCP 连接。
  if (WiFi.softAPgetStationNum() == 0) {
    return;
  }
#else
  // 1) WiFi 掉线：重连（优先上次连成功的那个，失败再依次试其余）
  //    wifiConnectAny() 本身是阻塞的（最坏 N × WIFI_PER_NET_TIMEOUT_MS），
  //    但 WiFi 都断了、TCP 必然作废，阻塞这段不影响什么。
  if (WiFi.status() != WL_CONNECTED) {
    if (!s_reconnecting &&
        millis() - s_last_reconnect_ms > WIFI_RETRY_INTERVAL_MS) {
      s_reconnecting = true;
      s_last_reconnect_ms = millis();
      Serial.println("[net] WiFi 断开，开始重连...");
      s_sock.stop();                 // 旧 TCP 一定作废，别留半开连接
#if USE_STATIC_IP
      // 重连后 DHCP 会重新分配，静态 IP 得重新配一遍
      {
        IPAddress ip, gw, mask, dns;
        ip.fromString(STATIC_IP); gw.fromString(STATIC_GW);
        mask.fromString(STATIC_MASK); dns.fromString(STATIC_DNS);
        WiFi.config(ip, gw, mask, dns);
      }
#endif
      if (wifiConnectAny()) {
        s_http_need_restart = true;  // 换了 IP，HTTP 服务器要重开
      } else {
        logPrintf("[net] 重连失败，%d 秒后再试\n",
                  (int)(WIFI_RETRY_INTERVAL_MS / 1000));
      }
      s_reconnecting = false;
    }
    return;
  }
#endif

  // 2) TCP 掉线：重连（服务器地址由 UDP 自动发现）
  if (!s_sock.connected()) {
    // ---- 还没发现 PC：别拿 config.h 的兜底占位地址去连 ----
    // 旧行为是直接连兜底值，于是日志里无限刷
    //   "连接 10.16.117.221 ... 连接失败"
    // 这个地址往往是别人的旧 IP，跟当前网段根本不通，刷屏反而掩盖真正原因。
    // 现在改成每 5 秒说一次人话，指出到底该查什么。
    if (!discoveryServerFound()) {
#if ALLOW_CONNECT_WITHOUT_DISCOVERY
      // 调试模式：跳过保护，直接拿 SERVER_HOST 去连（见 config.h 的说明）。
#else
      static uint32_t t_hint = 0;
      if (millis() - t_hint > 5000) {
        t_hint = millis();
        logPrintf("[net] 还没收到 PC 广播（当前占用兜底地址 %s，不代表真实地址）\n",
                  discoveryServerHost());
        logLine("[net]   -> PC 上 pc_baozi 的 main.py 跑起来了吗？它一启动就会广播");
        logLine("[net]   -> PC 和板子是不是连的同一个 2.4G 热点？");
        logLine("[net]   -> 防火墙放行 UDP 8766 / TCP 8765（专用+公用）？");
      }
      s_last_reconnect_ms = millis();   // 压住计时，别让它同时跑 TCP 重连
      return;
#endif
    }

    if (millis() - s_last_reconnect_ms > RECONNECT_MS) {
      s_last_reconnect_ms = millis();
      s_rx_len = 0;
      const char *host = discoveryServerHost();
      Serial.printf("[net] 连接 %s:%d ...\n", host, (int)SERVER_PORT);
      if (s_sock.connect(host, SERVER_PORT, 3000)) {
        s_sock.setNoDelay(true);
        s_last_rx_ms = millis();
        logLine("[net] 已连接 PC 服务器");
        // 打个招呼，顺便把设备信息带过去，方便 PC 端日志区分
        const char* hello = "esp32s3cam/baozi-voice/16k-mono [+cam]";
        linkSendCtrl(CTRL_HELLO, (const uint8_t*)hello, (uint16_t)strlen(hello));
      } else {
        logPrintf("[net] 连接 %s 失败。地址是 UDP 发现的，说明 PC 端在广播但\n", host);
        logLine("[net]   TCP 8765 不通 -> 查 Windows 防火墙（Python 有没有被放行）");
      }
    }
    return;
  }

  // 3) 收数据
  // 一次读满一帧（最大 = PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD = 2056 字节），
  // 取 4096 留余量。原来 1024 太小：一帧要 read 两次以上，多出来的轮次会拖慢
  // 主循环，播放缓冲被拖饿 -> 长句只响开头就被截断（组员 app 版实测）。
  uint8_t buf[4096];
  int n = s_sock.read(buf, sizeof(buf));
  if (n > 0) {
    s_last_rx_ms = millis();
    feed(buf, (size_t)n);
  } else if (n < 0) {
    Serial.println("[net] socket 读失败，准备重连");
    s_sock.stop();
    return;
  }

  // 4) 保活：长时间没收到东西就主动 PING，早点发现半开连接
  if (millis() - s_last_rx_ms > HEARTBEAT_MS) {
    s_last_rx_ms = millis();
    linkSendCtrl(CTRL_PING);
  }
}

#endif  // USE_USB_LINK
