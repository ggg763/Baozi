#pragma once

/* =====================================================================
 *  UDP 自动发现（ESP32 侧）
 *
 *  ⚠️ 2026-11：ESP32 与 PC 之间改成 USB 串口直连后，本模块在默认配置
 *  （config.h 的 USE_USB_LINK=1）下是【空实现】—— 不走网络自然不需要发现，
 *  但接口保持可调用，link.cpp / esp32_baozi.ino 的调用点不用改。
 *  只有切回 USE_USB_LINK=0（WiFi + TCP）时，下面的发现逻辑才生效：
 *
 *    ESP32 每 2 秒 -> 255.255.255.255:8766  "ESP32:<本机IP>:<HTTP端口>"
 *    PC    每 2 秒 -> 255.255.255.255:8766  "PC:<PC的IP>:<TCP端口>"
 *
 *  ESP32 收到 "PC:..." 就更新要连的服务器地址（link.cpp 用 discoveryServerHost()），
 *  PC 收到 "ESP32:..." 就更新取图的地址。换任何网络都不用改代码。
 *
 *  发现失败时 discoveryServerHost() 返回 config.h 的 SERVER_HOST（占位符 0.0.0.0）。
 *  ⚠️ 占位符不是真地址，别直接拿去 connect()——先用 discoveryServerFound() 判断。
 * ===================================================================== */

/* 启动：绑定 UDP 发现端口（在 WiFi 连上后调用） */
void discoveryBegin(void);

/* 主循环里反复调用：收 PC 广播 + 每 2 秒广播自己 */
void discoveryLoop(void);

/* 当前应连接的 PC 地址（发现到的，或 config.h 的 SERVER_HOST 兜底） */
const char *discoveryServerHost(void);

/* 是否真的收到过 PC 的广播（来自组员 app 版固件）。
 *   false = discoveryServerHost() 返回的只是 config.h 的兜底占位地址，
 *           这时不要拿它去连（必然失败），应该直接提示"PC 端没开机"。
 *   true  = 是真地址，可以连。 */
bool discoveryServerFound(void);

/* 本机 IP 点分十进制字符串（未连上时返回 "0.0.0.0"） */
const char *discoveryMyIp(void);
