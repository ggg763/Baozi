/* =====================================================================
 *  UDP 自动发现（ESP32 侧）—— 实现，说明见 discovery.h
 * ===================================================================== */
#include "discovery.h"
#include "config.h"
#include "log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>

// USB 模式（USE_USB_LINK=1）下 ESP32 不开 WiFi，UDP 发现整体编译成空实现，
// 只为保持接口可调用（esp32_baozi.ino / link.cpp 里的调用点就不用再包 #if）。
#if USE_USB_LINK

void discoveryBegin(void) {}
void discoveryLoop(void) {}
const char *discoveryServerHost(void) { return "127.0.0.1"; }
bool discoveryServerFound(void) { return false; }
const char *discoveryMyIp(void) { return "0.0.0.0"; }

#else

static WiFiUDP  s_udp;
static char     s_server[24];     /* 当前要连的 PC 地址，默认取 config */
static char     s_myip[24] = "0.0.0.0";
static bool     s_begun = false;
static uint32_t s_last_beacon = 0;

/* 有没有真的收到过 PC 的广播（来自组员 app 版固件）。
 * 用来说明 s_server 里现在装的是"真地址"还是 config.h 的兜底占位：
 *   false -> linkLoop 会跳过 TCP 连接尝试，直接提示"PC 端没开机"
 *   true  -> 正常连
 * 早先没有这个标志，兜底地址会被当成真地址去连，日志里就是
 * "连接 192.168.1.100 ... 连接失败" 无限刷，看不出真正原因。 */
static bool     s_server_real = false;

/* 把当前本机地址刷进 s_myip。
 * 早先只在 discoveryLoop() 每 2 秒广播时才更新，而 setup() 里
 * discoveryBegin() 之后立刻就打印 discoveryMyIp()，那时主循环还没跑过，
 * 读到的永远是初始值 "0.0.0.0"（日志里的假故障）。
 * 这里抽成函数，初始化时就填一次，广播时再持续刷新。 */
static void refresh_myip(void)
{
#if USE_AP_MODE
    /* 热点模式：地址固定 192.168.4.1（softAPIP），不需要等"连上" */
    IPAddress ip = WiFi.softAPIP();
#else
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    IPAddress ip = WiFi.localIP();
#endif
    snprintf(s_myip, sizeof(s_myip), "%u.%u.%u.%u",
             (unsigned)ip[0], (unsigned)ip[1],
             (unsigned)ip[2], (unsigned)ip[3]);
}

void discoveryBegin(void)
{
    if (s_begun) {
        return;
    }

    /* 兜底：先用 config.h 里的占位值，发现到 PC 后再覆盖 */
    strncpy(s_server, SERVER_HOST, sizeof(s_server) - 1);
    s_server[sizeof(s_server) - 1] = '\0';

    refresh_myip();          /* 先把本机 IP 填上，别让调用方读到 0.0.0.0 */

    if (s_udp.begin(DISCOVERY_PORT)) {
        s_begun = true;
        logPrintf("[disc] UDP 发现已启动: 监听 %d, 兜底地址 %s\n",
                  DISCOVERY_PORT, s_server);
        logLine("[disc] 等待 PC 广播... 若一直搜不到，请确认：");
        logLine("[disc]   1) PC 上 pc_baozi 的 main.py 正在运行（它会广播）");
        logLine("[disc]   2) PC 和板子连同一个 2.4G 热点，同一网段");
        logLine("[disc]   3) Windows 防火墙放行 UDP 8766 / TCP 8765（专用+公用）");
    } else {
        logLine("[disc] UDP 发现启动失败（PC 广播收不到，只能用兜底地址）");
    }
}

/* 解析 PC 的广播："PC:<ip>:<port>" */
static void on_pc_beacon(const char *msg)
{
    if (strncmp(msg, "PC:", 3) != 0) {
        return;
    }

    const char *ip = msg + 3;
    const char *colon = strchr(ip, ':');
    size_t len = colon ? (size_t)(colon - ip) : strlen(ip);

    if (len == 0 || len >= sizeof(s_server)) {
        return;
    }

    char buf[24];
    memcpy(buf, ip, len);
    buf[len] = '\0';

    if (!s_server_real) {
        /* 第一次收到真地址：不管和兜底值是否相同，都标成"真的" */
        s_server_real = true;
        logPrintf("[disc] >>> 已发现 PC: %s（UDP 发现成功）\n", buf);
        strncpy(s_server, buf, sizeof(s_server) - 1);
        s_server[sizeof(s_server) - 1] = '\0';
        return;
    }

    if (strcmp(buf, s_server) != 0) {
        strncpy(s_server, buf, sizeof(s_server) - 1);
        s_server[sizeof(s_server) - 1] = '\0';
        logPrintf("[disc] 发现 PC: %s\n", s_server);
    }
}

void discoveryLoop(void)
{
    /* ---- 1) 收：解析 PC 的广播 ---- */
    if (s_begun) {
        int sz = s_udp.parsePacket();
        while (sz > 0) {
            char buf[64];
            int n = s_udp.read(buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                on_pc_beacon(buf);
            }
            sz = s_udp.parsePacket();
        }

        /* ---- 2) 发：每 2 秒广播自己的地址（供 PC 更新取图地址）---- */
        if (millis() - s_last_beacon > 2000U) {
            s_last_beacon = millis();
            bool can_beacon;
#if USE_AP_MODE
            /* AP 模式：PC 连进来之前广播也没人收，省点电只等有客户端时再发。
             * 原来这里判断 WiFi.status()==WL_CONNECTED —— 那是 STA 才有的状态，
             * 热点模式下永远不成立，结果 s_myip 一直是 "0.0.0.0"
             * （实测日志里那行 `[disc] 本机 IP=0.0.0.0` 就是这么来的）。 */
            can_beacon = (WiFi.softAPgetStationNum() > 0);
#else
            can_beacon = (WiFi.status() == WL_CONNECTED);
#endif
            if (can_beacon) {
                refresh_myip();

                char pkt[64];
                int len = snprintf(pkt, sizeof(pkt), "ESP32:%s:%d", s_myip, HTTP_PORT);
                s_udp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
                s_udp.write((const uint8_t *)pkt, (size_t)len);
                s_udp.endPacket();
            }
        }
    } else if (WiFi.status() == WL_CONNECTED
#if USE_AP_MODE
               || WiFi.softAPgetStationNum() > 0
#endif
              ) {
        /* WiFi 之前没起来（比如启动早于连接）——连上后补启动一次 */
        discoveryBegin();
    }
}

const char *discoveryServerHost(void)
{
    return s_server;
}

bool discoveryServerFound(void)
{
    return s_server_real;
}

const char *discoveryMyIp(void)
{
    return s_myip;
}

#endif  // USE_USB_LINK
