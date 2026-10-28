#pragma once

// =====================================================================
//  baozi 机器人 · ESP32-S3-CAM 整合固件 配置
//  把"耳朵+嘴"(voice) 和 "眼睛"(camera) 合到同一个固件里。
//  需要按你的实际情况修改的东西，全部集中在这个文件里。
//  要求：Arduino IDE + esp32 core >= 3.0.0（本项目用了 ESP_I2S 与
//        adc_continuous，core 2.x 编译不过）。
// =====================================================================

// =====================================================================
//                    传输方式：USB 串口（当前默认）
// =====================================================================
// ★ 2026-11：ESP32 与 PC 之间的通信【全部改走 USB 线】，不再依赖局域网。
//
//   为什么改：
//     · PC 只该用【外部网络】（edge-tts / 天气 / Ollama 等），
//       不该再为了接 ESP32 去连某个 WiFi、也不需要知道 ESP32 的 IP；
//     · STA/AP 两套 WiFi 方案都踩过坑（AP 让 PC 上不了外网；STA 时
//       视频流把音频挤掉）。一根 USB 线同时解决供电 + 数据，最省事。
//
//   现在这根线（USB CDC）上跑三条链路：
//     1) 语音 PCM 双向（PKT_MIC_PCM / PKT_TTS_PCM）
//     2) 图像：PC 发 CTRL_CAM_REQ -> 本机抓一帧 -> PKT_CAM_JPEG 分片回传
//     3) 情感/意图：PC 发 CTRL_STATE 等控制帧 -> UART2(GPIO14) -> TD1601
//   调试日志也封装成 PKT_LOG 帧走同一根线（见 log.h），PC 端打 [esp] 前缀。
//
//   1 = 用 USB 串口（备用）
//   0 = 用 WiFi + TCP（当前用这个）
#define USE_USB_LINK    0

// USB CDC 实际忽略波特率（USB 全速 12Mbps）；填这个值只是为了兼容
// "板子用的是 CH340/CP2102 这类 USB-UART 桥"的情况 —— 那时 Serial 就是
// UART0，真实波特率必须两端一致，且 115200 带不动 64KB/s 的音频+图像。
#define USB_LINK_BAUD       921600

// 开机后最多等 PC 打开串口多久（毫秒）。USB CDC 没被打开时 Serial 收不到东西，
// 但也不必死等 —— 超时后照常往下跑，日志先丢弃，PC 一连上就恢复正常。
#define USB_WAIT_SERIAL_MS  3000

// 这么久没收到 PC 的任何帧，就认为"PC 没在跑"（不往串口塞音频）。
// PC 端每 2 秒发一次 CTRL_PING 保活，所以 15 秒足够宽松。
#define USB_LINK_IDLE_MS    15000

// =====================================================================
//                    以下 WiFi 参数：仅 USE_USB_LINK=0 时使用
// =====================================================================
// ★ 工作模式（2026-10）——
//   0 = STA：ESP32 连路由器
//   1 = AP ：ESP32 自己开热点，PC 直连它
//
//   2026-10 曾决定改回 STA（只用外网）：
//     AP 模式虽然让摄像头更流畅，但 PC 连了热点就上不了外网，
//     必须再插一根网线。实际使用太折腾，而且 edge-tts（唯一依赖
//     外网的环节）会因此哑掉。所以统一走路由器，PC 和 ESP32 在同一个
//     局域网里，外网、语音、图像一条网络搞定。
//
//   2026-11 进一步改成 USB 串口（见上面的 USE_USB_LINK），WiFi 段整体
//   保留在 #if !USE_USB_LINK 后面，放着不用。
//
//   ⚠️ 遗留代价：视频流和音频共用同一信道，VAD 可能被挤（已通过
//      "按需拉帧"缓解 —— 平时不拉流，只在拍照/看图时临时拉 5 帧）。
#define USE_AP_MODE     0               // ← 0 = 连路由器

// ---- AP 模式的参数（USE_AP_MODE=1 时才用；保留以备切换）----
// 故意不用常见名字，避免和邻居的 "ESP32" 撞名。
#define AP_SSID         "baozi-cam"
#define AP_PASSWORD     "baozi12345"    // ≥8 位，WPA2
#define AP_CHANNEL      6               // 尽量避开你路由器在用的信道，减少干扰
#define AP_HIDDEN       0               // 1 = 隐藏 SSID（要手输名字才连得上）
#define AP_MAX_CLIENTS  2               // 只服务 PC + 调试用手机
// 注意：AP 模式下 ESP32 的 IP 固定是 192.168.4.1（ESP-IDF 的默认网关地址）

// ---- STA 模式的参数（USE_AP_MODE=0 时生效）----
// ⚠️ 这里要填【PC 当前所连的那个 WiFi】的名字和密码。
//
// ★ 多热点自动切换（来自组员 app 版固件）：按顺序挨个试，连上就停，
//   换环境不用重新烧固件。想加第 3 个：下面两个数组【同一位置】各加一项，
//   并把 WIFI_NET_COUNT 改成 3。两个数组长度必须严格一致，
//   否则会读到脏内存（现象是连不上或乱连）。
//
//   顺序建议：把现场要用的放前面，省开机等待时间（每个热点最多等
//   WIFI_PER_NET_TIMEOUT_MS，2 个热点最坏多等 24 秒）。
//
// ⚠️ 三个硬性约束，违反任何一条都连不上：
//   · 必须是 2.4GHz（ESP32-S3 硬件不支持 5GHz；iPhone  hotspots 要开"最大兼容性"）
//   · 必须是 WPA2（WPA3 兼容性差，会报 status=6）
//   · SSID 用纯英文（中文 SSID 实测报 status=6 被对方断开）
#define WIFI_NET_COUNT  2
static const char* const WIFI_SSID_LIST[WIFI_NET_COUNT] = {
  "shubei",      // [0] 比赛现场
  "lanlll",      // [1] 开发用
};
static const char* const WIFI_PASSWORD_LIST[WIFI_NET_COUNT] = {
  "wrz006611221199",
  "scfh86mesrfmia2",
};

// 兼容旧代码：WIFI_SSID / WIFI_PASSWORD 指向列表第 0 个（主用热点）
#define WIFI_SSID       (WIFI_SSID_LIST[0])
#define WIFI_PASSWORD   (WIFI_PASSWORD_LIST[0])

// 每个热点连接时最多等多久（毫秒）。2 个热点 -> 最坏开机多等 2× 这个值。
// 12 秒够用：正常路由器 2~4 秒完成 DHCP。
#define WIFI_PER_NET_TIMEOUT_MS  12000

// ---- 地址自动发现（推荐）----
// 你和 ESP32 的 IP 会随网络变化（DHCP/换 WiFi）。本工程用【UDP 双向广播】自动发现：
//   ESP32 每 2 秒广播自己的地址；PC 也每 2 秒广播自己的地址；
//   双方互相监听 8766 端口，收到就更新对方地址 —— 换任何网络都不用改代码。
// 因此这里【不再写死 IP】，下面的 SERVER_HOST 只作为"发现失败时的兜底"。
#define USE_STATIC_IP   0               // 0 = DHCP（自动适配当前网络，推荐）
#define STATIC_IP       "10.16.117.204" // 仅当 USE_STATIC_IP=1 时有效
#define STATIC_GW       "10.16.117.104"
#define STATIC_MASK     "255.255.255.0"
#define STATIC_DNS      "10.16.117.104"

// UDP 发现端口（两端必须一致）
#define DISCOVERY_PORT  8766

// PC 端服务器：正常情况下由 discovery 自动发现，这里只是【兜底占位值】。
//
// ⚠️ 2026-10-17（组员 app 版踩的坑，合并沿用）：原来这里写死某个真实 IP，
//    一旦 UDP 发现失败，板子就拿那个死地址猛连，日志里无限刷
//    "连接 10.16.117.221 ... 连接失败"，看上去像 bug，实际只是 PC 程序没开。
//    换成 0.0.0.0（保留地址，永远不可能是真实主机）后，link.cpp 会先问
//    discoveryServerFound()：没发现过 PC 就根本不尝试连接，只打印排查提示。
#define SERVER_HOST     "0.0.0.0"
#define SERVER_PORT     8765

// 调试开关：1 = 即使还没收到 PC 的 UDP 广播，也直接拿 SERVER_HOST 去连。
//
// 0 是组员那版固件的行为（避免拿"死地址"无限刷连接失败）。
// 但排查"板子明明在线、HTTP 也通，就是不连 PC"时，这个保护会让人分不清
// 到底是【UDP 发现不通】还是【TCP 连不上】。临时改成 1，并把 SERVER_HOST
// 写成 PC 的实际 IP（如 "10.16.117.221"），就能一刀切开：
//   · 改完能连上   -> 问题在 UDP 发现（广播被拦 / 热点客户端隔离），TCP 是好的
//   · 改完还连不上 -> 问题在 TCP（防火墙拦入站 / 板子还连着上一台 PC）
// ⚠️ 排查完记得改回 0 与 "0.0.0.0"。
#define ALLOW_CONNECT_WITHOUT_DISCOVERY 0

// 断线后的重连间隔（毫秒）
#define RECONNECT_MS    3000

// ============================ 引脚分配 ============================
// 本板 GOOGUU / AI-Thinker ESP32-S3-CAM 用的是 OPI (Octal) PSRAM，
// 模块内部占用 GPIO33~37，一律避开；摄像头占用 GPIO4~18（板内已接好）。
//
// 语音(voice) 实际使用： 1(麦克风ADC) 2(状态灯) 38/39/40/41(I2S功放)
// 眼睛(camera) 板内固定：4/5(SIOD/SIOC) 6 7 13 15 8 9 10 11 12 16 17 18
// 发给 TD1601：         14(UART2 TX)  21(UART2 RX, 本工程未用)
// 备用：                42 / 47 / 48
// -------------------------------------------------------------------

// ---- 耳朵：MAX9814 模拟麦克风 -> ADC1（必须 GPIO1~GPIO10，ADC2 与 WiFi 冲突）
#define PIN_MIC_ADC     1

// ---- 嘴：I2S 数字功放（MAX98357A / PCM5102 / UDA1334）
#define PIN_I2S_BCLK    38
#define PIN_I2S_LRC     39      // 有的板子丝印写 WS / LRCLK
#define PIN_I2S_DIN     40

// MAX98357A 的 SD（shutdown）：高=工作。若直接接 3V3，填 -1。
#define PIN_AMP_EN      41

// 状态指示灯。填 -1 关闭。
#define PIN_STATUS_LED  2

// ---- 眼睛：摄像头引脚（板内走线已固定，一般不用改）
#define CAM_PWDN_GPIO   -1
#define CAM_RESET_GPIO  -1
#define CAM_XCLK_GPIO   15
#define CAM_SIOD_GPIO   4
#define CAM_SIOC_GPIO   5
#define CAM_D7_GPIO     16
#define CAM_D6_GPIO     17
#define CAM_D5_GPIO     18
#define CAM_D4_GPIO     12
#define CAM_D3_GPIO     10
#define CAM_D2_GPIO     8
#define CAM_D1_GPIO     9
#define CAM_D0_GPIO     11
#define CAM_VSYNC_GPIO  6
#define CAM_HREF_GPIO   7
#define CAM_PCLK_GPIO   13

// ---- 发给 TD1601（RISC-V 主控）的串口
//   TD1601 侧：PB18 = UART3 RX。ESP32 只发不收，所以只用 TX 一根。
//   GPIO14 是干净空闲脚（GPIO4/5 是摄像头 I2C，做 UART 会失真，已实测排除）。
#define TD_UART_TX_GPIO 14
#define TD_UART_RX_GPIO 21      // 本工程未使用，保持一个合法值即可
#define TD_UART_BAUD    115200

// HTTP 服务器端口（PC 通过它取图 / 送文字描述；手机网页控制台也挂在这里）
#define HTTP_PORT       80

// 手机网页控制台（来自组员 app 版）：ESP32 自己开 HTTP 服务，
// 手机浏览器打开 http://<板子IP>/ 点按钮 -> /api/emotion -> UART2 -> TD1601 变脸。
// 不经过 PC、不需要外网，比赛现场演示很稳。
// ⚠️ 只挂在 WiFi 分支（USE_USB_LINK=0）下：USB 模式下没有 HTTP 服务，
//    而且那根线在跑二进制帧，网页日志不能直接写串口。
#define WEB_CONSOLE_ENABLE 1

// ============================ 音频参数 ============================
#define SAMPLE_RATE     16000
#define AUDIO_BITS      16
#define MIC_BLOCK_MS    20
#define MIC_BLOCK_SAMPLES  (SAMPLE_RATE / 1000 * MIC_BLOCK_MS)
#define MIC_BLOCK_BYTES    (MIC_BLOCK_SAMPLES * AUDIO_BITS / 8)

#define ADC_OVERSAMPLE  2

#define MIC_GAIN        20.0f
#define MIC_DC_TRACK    0.0005f

#define MIC_WAIT_WIFI_MS      8000
#define MIC_FAIL_RESTART_MS   2000

// ============================ 播放参数 ============================
#define PLAY_CHUNK_MS     20
#define PLAY_IDLE_OFF_MS  200

// 攒够多少毫秒音频才开始播（首字延迟 vs 抖动容忍的取舍）。
// 60 -> 200ms（来自组员 app 版实机结论）：原来只攒 3 片(60ms)就开播，
// WiFi 一抖就 underrun，听感是"说到一半断一下、再续上"。
// 200ms(10 片) 对首字延迟几乎无感，抗抖动明显变好。
// ⚠️ USB 链路下这个值只影响首字延迟，不影响稳定性。
#define PLAY_PREBUFFER_MS 200

// 环形缓冲深度。
// 我们保持 1500ms（PC 端 voice_server 有 AUDIO_STATE 闭环限速推流，
// 不会把缓冲灌爆，所以不需要更深的缓冲）。
// 组员 app 版曾因为 PC 端开环推流吃到教训：1500ms 实测只响开头 0.5s，
// 改成 4000ms(128KB 内部 RAM) 后播放任务创建失败、喇叭完全不响，
// 折中取 2500ms(80000B)/RAM 约 43%。
// 👉 如果换回"PC 端不读 AUDIO_STATE、按时间盲推"的旧链路，把这里调到 2500。
#define PLAY_RING_MS      1500

// 缓冲满时的等待上限。
// 我们的 onFrame 收到 TTS_PCM 后会按这个上限重试写入，超时才丢弃剩余数据；
// 所以【不能】学组员 app 版改成 2000ms —— 那会把 loop() 卡住 2 秒，
// 麦克风掉块、心跳超时。50ms 足够让播放任务排空一片(20ms)。
#define PLAY_WRITE_TIMEOUT_MS 50

// 播放状态上报间隔（ms）。PC 靠这个闭环判断"缓冲还剩多少 / 还在不在放"：
//   · 推流限速：原来是 PC 按时间【推算】水位（开环），pace 偏慢时 ESP32
//     缓冲会饿死 -> 声音断续；偏快时 TCP 背压。
//   · 播完判定：PC 原来推算"播完了"就切回收音，但 ESP32 可能还在响 ->
//     它把自己的尾音当用户说话（幻听），或把用户的话当回环丢掉。
// 100ms 上报一次，每帧 13 字节 -> 约 130 B/s，可忽略。
#define PLAY_STATE_MS     100

// ============================ 摄像头参数 ==========================
#define CAM_XCLK_HZ       10000000     // 10MHz 稳定默认值

// ★ 分辨率（2026-10：从 HQVGA 240x176 提到 VGA 640x480）
//   原来用的是 FRAMESIZE_HQVGA = 240x176，像素只有 VGA 的 1/7，
//   画面明显糊 —— 参考实现（D:\Camera.test）实测用的就是 VGA 640x480。
//   本板是 OV2640（200 万像素），支持到 UXGA 1600x1200，VGA 远远没到它的上限。
//
//   可选：
//     FRAMESIZE_VGA    640x480   ← 默认，清晰且带宽可控（约 40KB/帧）
//     FRAMESIZE_SVGA   800x600   更清晰，帧率略降
//     FRAMESIZE_XGA    1024x768  更清晰，约 80KB/帧
//     FRAMESIZE_HD     1280x720
//     FRAMESIZE_UXGA   1600x1200 最高，但帧率会很低、PSRAM 也吃紧
//
//   ⚠️ 分辨率调高后记得同步看 CAM_FB_COUNT 和 CAM_STREAM_INTERVAL_MS：
//      大图编码更慢，间隔太小会出现 FB-OVF 溢出（串口会刷）。
#define CAM_FRAME_SIZE    FRAMESIZE_VGA

// ★ JPEG 质量：数值【越小越清晰】、体积越大（OV2640 的约定）。
//   30 → 12：原值压得太狠，细部和边缘全糊成一团。
//   常用区间 10~15；太小时帧会变大、编码变慢。
#define CAM_JPEG_QUALITY  12

// 帧缓冲数量。双缓冲（需要 PSRAM）意味着摄像头手里始终有一帧在备着，
// 所以 esp_camera_fb_get() 一般能立刻返回。
// ⚠️ 但它【不是】一定不阻塞：USB 模式下按需抓图时，如果上一帧还没被
//    归还（或 WiFi 模式的定时抓帧正占着），fb_get() 会等驱动出下一帧。
//    这就是为什么抓图必须放在 loop() 里做，而不是串口回调里（见 .ino）。
#define CAM_FB_COUNT      2            // 双缓冲（需要 PSRAM）
#define CAM_REFRESH_MS    1000         // 多久刷新一帧给 PC 抓（仅 WiFi 模式 /capture）

// 摄像头图像调优（0 = 保持传感器默认）。想更"通透"可以试：
//     CAM_SATURATION  -1 / 0 / 1        （-2~2）
//     CAM_BRIGHTNESS   0 / 1 / 2        （-2~2）
//     CAM_CONTRAST     0 / 1 / 2        （-2~2）
#define CAM_BRIGHTNESS    0
#define CAM_SATURATION    0
#define CAM_CONTRAST      0

// ---- 实时拉流（/stream）----
// ESP32 侧是【逐帧模式】：每次请求只发一帧就立刻返回（不占用 loop()），
// 帧率完全由 PC 拉取速度决定。所以不再需要"帧间隔"参数 ——
// CAM_STREAM_INTERVAL_MS 已移除（旧代码若引用会编译报错，方便及时发现）。

// ============================ 其他 ================================
#define DEBUG_SERIAL    1
#define HEARTBEAT_MS    5000
