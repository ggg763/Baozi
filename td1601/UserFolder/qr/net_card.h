/******************************************************************************
 * @file     net_card.h
 * @brief    配网卡片（第 5 页 NET）—— 二维码 + 连接状态
 *
 * 链路（详见 UserFolder/doc/qr_provision.md）：
 *
 *   PC ──局域网(UDP/HTTP)──► ESP32 ──UART3(PB18, 单向)──► TD1601 ──► 本屏
 *     共享 WiFi 凭证            转成一行文本              自己编码二维码
 *
 * 为什么二维码在 TD1601 上现算而不是下传点阵：
 *   · UART3 是单向的、115200，传 480×480 点阵要 28 KB，传字符串只要一百来字节；
 *   · 内容随时会变（热点名/密码/IP），现算不用改协议；
 *   · qrgen 已在 PC 上与成熟实现逐字节比对过（见 _probe/qr/qr_verify.py）。
 *
 * UI 的两档：
 *   · 卡片页内（160×160 左右的码 + 状态文字）—— 从卡片层滑到 NET 就能看见；
 *   · 全屏（点一下放大）—— 真扫码用这个，模块更大、对焦更快。
 ******************************************************************************/
#ifndef _NET_CARD_H_
#define _NET_CARD_H_

#include <stdint.h>

#define NET_SSID_MAX     32     /* 802.11 SSID 上限 */
#define NET_IP_MAX       15     /* "255.255.255.255" + NUL */
#define NET_PAYLOAD_MAX  128    /* QR: 后面能跟的最长内容 */

typedef enum {
    NET_ST_IDLE = 0,     /* 还没收到任何配网信息 */
    NET_ST_AP,           /* 热点已开：二维码就绪，等手机扫 */
    NET_ST_CONNECTING,   /* 手机已提交，ESP32 正在连路由器 */
    NET_ST_OK,           /* 连上了：显示 SSID + IP */
    NET_ST_FAIL          /* 连不上：让用户重来 */
} net_state_t;

/* 清空（上电/收到 QR:OFF）。 */
void net_card_reset(void);

/* 设置二维码内容并立即编码。payload 为 ASCII（WIFI: 串或 URL）。
 * 返回 0 = 成功；-1 = 空/超长；-2 = 编码器装不下（版本 10 都放不进去）。 */
int net_card_set_qr(const char *payload);

/* 设置状态与附加信息（ssid / ip 可为 0）。返回 0 = 状态有效。 */
int net_card_set_state(int st, const char *ssid, const char *ip);

int net_card_state(void);
int net_card_has_qr(void);

/* 当前二维码的编码参数（排障用：版本突然变大 = 内容太长，屏上模块会变小）。
 * 没有码时返回 -1。任一指针可为 0。 */
int net_card_qr_meta(int *ver, int *size, int *mask, int *ecc);

int net_card_full(void);            /* 1 = 当前是全屏码 */
void net_card_set_full(int on);

/* 内容变过就置脏：ui_app_tick 里取走并重绘（避免在 UART 中断窗口刷屏）。 */
int  net_card_take_dirty(void);

/* 点击命中：全屏码点一下退出，卡片内点一下放大。返回 1 = 已处理。 */
int  net_card_tap(int x, int y);

/* 往当前绘制列表里画（调用方已经铺好背景/卡片框）。 */
void net_card_draw_card(void);

/* 全屏：自己铺背景 + 大码 + 底部状态。 */
void net_card_draw_full(void);

#endif /* _NET_CARD_H_ */
