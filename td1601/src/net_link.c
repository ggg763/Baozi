/******************************************************************************
 * @file     net_link.c
 * @brief    配网文本命令解析实现（见 net_link.h）
 *
 * 三个约束，每个都是从这套代码库别处学来的：
 *   1. **不在中断里干活**：uart_esp.c 的中断只把整行存进邮箱，真正执行在
 *      uart_esp_poll()（主循环上下文）。本文件因此只被 poll 路径调用，
 *      可以放心 printf、可以置"脏"标记触发重绘。
 *   2. **不自己刷屏**：只调 ui_app_show_net() / 置脏，让 ui_app 在
 *      "串口静默 150ms" 的窗口里统一刷 —— 直接刷是实机花屏的高发窗口。
 *   3. **字符串一律静态**：ui_add_text 只存指针，字形在 ui_flush 时才取。
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_link.h"
#include "ui_app.h"          /* ui_app_show_net */
#include "qr/net_card.h"

#define NET_LINK_PREFIX_QR    "QR:"
#define NET_LINK_PREFIX_NET   "NET:"

void net_link_init(void)
{
    net_card_reset();
}

/* 从 p 开始截到 sep（或行尾），写进 dst。返回下一个字段的起点（0 = 没有下一个）。 */
static const char *take_field(const char *p, char sep, char *dst, int cap)
{
    int n = 0;

    while (*p != '\0' && *p != sep) {
        if (n < cap - 1) {
            dst[n++] = *p;
        }
        p++;
    }
    dst[n] = '\0';
    if (*p == sep) {
        return p + 1;
    }
    return 0;
}

/* 状态推进到"要让用户看见"时，把屏幕拉过去。
 * 已连接(3) 反而不拉 —— 那时候人可能正在跟包子说话，弹一张网卡很突兀。 */
static void maybe_jump(int st, int qr_just_set)
{
    if (qr_just_set || st == NET_ST_AP || st == NET_ST_CONNECTING || st == NET_ST_FAIL) {
        ui_app_show_net();
    }
}

static int handle_qr(const char *arg)
{
    int rc;

    if (arg[0] == '\0' || strcmp(arg, "OFF") == 0) {
        net_card_reset();
        printf("[NET] qr cleared\r\n");
        return 1;
    }

    rc = net_card_set_qr(arg);
    if (rc != 0) {
        printf("[NET] qr rejected rc=%d len=%u\r\n", rc, (unsigned)strlen(arg));
        return 1;
    }
    {
        int ver = 0, size = 0, mask = 0;
        (void)net_card_qr_meta(&ver, &size, &mask, 0);
        printf("[NET] qr set: v%d %dx%d mask=%d len=%u\r\n",
               ver, size, size, mask, (unsigned)strlen(arg));
    }
    /* 有码 = 等着被扫。若 ESP32 没单独发 NET:1（省一帧），这里补上，
     * 免得界面显示"no network yet"却配着一张让人扫的码。 */
    if (net_card_state() == NET_ST_IDLE) {
        (void)net_card_set_state(NET_ST_AP, 0, 0);
    }
    maybe_jump(net_card_state(), 1);
    return 1;
}

static int handle_net(const char *arg)
{
    char ssid[NET_SSID_MAX + 1];
    char ip[NET_IP_MAX + 1];
    const char *next;
    int st;

    if (arg[0] < '0' || arg[0] > '9') {
        return 0;
    }
    st = arg[0] - '0';

    ssid[0] = '\0';
    ip[0] = '\0';
    next = take_field(arg + 1, ';', ssid, (int)sizeof(ssid));
    if (next != 0) {
        (void)take_field(next, ';', ip, (int)sizeof(ip));
    }

    if (net_card_set_state(st, ssid[0] ? ssid : 0, ip[0] ? ip : 0) != 0) {
        printf("[NET] bad state %c\r\n", arg[0]);
        return 1;
    }
    printf("[NET] state=%d ssid=\"%s\" ip=\"%s\"\r\n", st, ssid, ip);
    maybe_jump(st, 0);
    return 1;
}

int net_link_handle_line(const char *line)
{
    if (line == 0 || line[0] == '\0') {
        return 0;
    }
    if (strncmp(line, NET_LINK_PREFIX_QR, 3) == 0) {
        return handle_qr(line + 3);
    }
    if (strncmp(line, NET_LINK_PREFIX_NET, 4) == 0) {
        return handle_net(line + 4);
    }
    return 0;
}
