/******************************************************************************
 * @file     net_card.c
 * @brief    配网卡片实现（见 net_card.h）
 *
 * 三层：
 *   ① 数据：payload / 状态 / ssid / ip（全 static，128 字节级，不占堆）
 *   ② 编码：qrgen → 模块矩阵 → 打包成 1bpp（ui_add_bitmap2 要的格式）
 *   ③ 绘制：卡片内小码 / 全屏大码，两套布局共用同一个点阵缓冲
 *
 * ⚠️ 板载字库只有 ASCII（FONT.h 的 asc2_2412）。中文 SSID 只能显示成
 *    "ssid: (non-ascii)" —— 二维码里仍然是完整 UTF-8 字节，手机扫得出来。
 *    想在屏上也看得见中文，得走 0x12/0x15 那种"PC 渲染点阵"的路子，本页暂不做。
 *
 * ⚠️ 一 tick 只重绘一次（HANDOVER §6-14）：本文件只往绘制列表里加元素，
 *    真正刷屏由 ui_app 的 ui_flush_begin/step 统一安排，不要在这里自己刷。
 ******************************************************************************/

#include <stdint.h>
#include <string.h>

#include "net_card.h"
#include "qrgen.h"
#include "ui.h"          /* ui_add_* / ui_text_width / 配色 */

/* 二维码是黑白的，别掺主题色：手机对焦靠的是极值对比 */
#define NET_COL_INK     0x0000
#define NET_COL_PAPER   0xFFFF

/* 卡片页内的二维码区域：卡片框是 (60,122)-(420,412)。
 * 上边留 24px 给标题，下边留两行文字（状态 + SSID/IP），中间全给二维码。 */
#define CARD_QR_TOP     164
#define CARD_QR_MAX     200
#define CARD_QR_CX      240

/* 全屏：上留 20、下留 56 给页脚 */
#define FULL_QR_TOP     20
#define FULL_QR_MAX     400

static net_state_t s_state = NET_ST_IDLE;
static int  s_full = 0;
static int  s_dirty = 0;
static int  s_has_qr = 0;

static char s_payload[NET_PAYLOAD_MAX];
static char s_ssid[NET_SSID_MAX + 1];
static char s_ip[NET_IP_MAX + 1];

/* ⚠️ 这行必须 static：ui_add_text 只存指针，字形是在 ui_flush() 时才取的。
 * 用栈上的临时 buf（曾经就是这么写的）会刷出一行随机字符 —— 已在 ui_sim 里复现。 */
static char s_info[48];

/* 模块矩阵打包后的 1bpp 缓冲：最大 57×57 → 57 行 × 8 字节 */
static uint8_t s_pack[QRGEN_MAX_SIZE * ((QRGEN_MAX_SIZE + 7) / 8)];
static int     s_size = 0;      /* 模块边长 */
static int     s_stride = 0;    /* 每行字节数 */

static qrgen_result_t s_qr;

/* ---------------------------------------------------------------- 小工具 */

/* 只保留可打印 ASCII；有中文就整体标成 (non-ascii)，避免显示成乱码方块 */
static void copy_ascii(char *dst, int cap, const char *src)
{
    int i = 0;

    if (cap <= 0) {
        return;
    }
    dst[0] = '\0';
    if (src == 0) {
        return;
    }
    for (i = 0; src[i] != '\0' && i < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c > 0x7E) {
            const char *tag = "(non-ascii)";
            int k = 0;
            while (tag[k] != '\0' && k < cap - 1) {
                dst[k] = tag[k];
                k++;
            }
            dst[k] = '\0';
            return;
        }
        dst[i] = (char)c;
    }
    dst[i] = '\0';
}

static void draw_text_center(int cy, const char *s, int scale, uint16_t color)
{
    int w;

    if (s == 0 || s[0] == '\0') {
        return;
    }
    w = ui_text_width(s, scale);
    ui_add_text((int16_t)(240 - w / 2), (int16_t)cy, s, scale, color);
}

/* ---------------------------------------------------------------- 数据层 */

void net_card_reset(void)
{
    s_state  = NET_ST_IDLE;
    s_full   = 0;
    s_has_qr = 0;
    s_size   = 0;
    s_stride = 0;
    s_payload[0] = '\0';
    s_ssid[0] = '\0';
    s_ip[0] = '\0';
    s_dirty  = 1;
}

int net_card_set_qr(const char *payload)
{
    int rc;

    if (payload == 0 || payload[0] == '\0') {
        return -1;
    }
    if (strlen(payload) >= NET_PAYLOAD_MAX) {
        return -1;
    }

    rc = qrgen_encode(payload, QRGEN_ECC_M, &s_qr);
    if (rc != 0) {
        return -2;
    }

    s_size   = s_qr.size;
    s_stride = (s_size + 7) / 8;
    memset(s_pack, 0, sizeof(s_pack));
    {
        int x, y;
        for (y = 0; y < s_size; y++) {
            for (x = 0; x < s_size; x++) {
                if (s_qr.modules[y * s_qr.stride + x]) {
                    s_pack[y * s_stride + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
                }
            }
        }
    }
    strcpy(s_payload, payload);
    s_has_qr = 1;
    s_dirty  = 1;
    return 0;
}

int net_card_set_state(int st, const char *ssid, const char *ip)
{
    if (st < NET_ST_IDLE || st > NET_ST_FAIL) {
        return -1;
    }
    s_state = (net_state_t)st;
    if (ssid != 0) {
        copy_ascii(s_ssid, (int)sizeof(s_ssid), ssid);
    }
    if (ip != 0) {
        copy_ascii(s_ip, (int)sizeof(s_ip), ip);
    }
    s_dirty = 1;
    return 0;
}

int net_card_state(void)      { return (int)s_state; }
int net_card_has_qr(void)     { return s_has_qr; }
int net_card_full(void)       { return s_full; }
void net_card_set_full(int on) { s_full = (on != 0); s_dirty = 1; }

int net_card_qr_meta(int *ver, int *size, int *mask, int *ecc)
{
    if (!s_has_qr) {
        return -1;
    }
    if (ver)  { *ver  = s_qr.version; }
    if (size) { *size = s_qr.size; }
    if (mask) { *mask = s_qr.mask; }
    if (ecc)  { *ecc  = s_qr.ecc; }
    return 0;
}

int net_card_take_dirty(void)
{
    int d = s_dirty;
    s_dirty = 0;
    return d;
}

/* ---------------------------------------------------------------- 绘制层 */

static const char *state_line(void)
{
    switch (s_state) {
    case NET_ST_AP:         return "waiting for scan";
    case NET_ST_CONNECTING: return "connecting...";
    case NET_ST_OK:         return "connected";
    case NET_ST_FAIL:       return "connect failed";
    default:                return "no network yet";
    }
}

/* 一行"SSID: xxx"或"IP: x.x.x.x"（谁有用显示谁）*/
static const char *info_line(char *buf, int cap)
{
    if (s_state == NET_ST_OK) {
        if (s_ip[0] != '\0') {
            int n = 0;
            const char *p = "IP: ";
            while (*p && n < cap - 1) {
                buf[n++] = *p++;
            }
            {
                int k = 0;
                while (s_ip[k] && n < cap - 1) {
                    buf[n++] = s_ip[k++];
                }
            }
            buf[n] = '\0';
            return buf;
        }
        if (s_ssid[0] != '\0') {
            int n = 0;
            const char *p = "SSID: ";
            while (*p && n < cap - 1) {
                buf[n++] = *p++;
            }
            {
                int k = 0;
                while (s_ssid[k] && n < cap - 1) {
                    buf[n++] = s_ssid[k++];
                }
            }
            buf[n] = '\0';
            return buf;
        }
    }
    if (s_ssid[0] != '\0' && s_state != NET_ST_OK) {
        int n = 0;
        const char *p = "SSID: ";
        while (*p && n < cap - 1) {
            buf[n++] = *p++;
        }
        {
            int k = 0;
            while (s_ssid[k] && n < cap - 1) {
                buf[n++] = s_ssid[k++];
            }
        }
        buf[n] = '\0';
        return buf;
    }
    (void)cap;
    return 0;
}

/* 在 (x0,y0) 画 size*scale 的二维码，底下垫白纸（含 2 像素白边）*/
static void draw_qr_block(int x0, int y0, int scale)
{
    int w = s_size * scale;

    ui_add_round_fill((int16_t)(x0 - 2), (int16_t)(y0 - 2),
                      (int16_t)(w + 4), (int16_t)(w + 4), 0, NET_COL_PAPER);
    ui_add_bitmap2((int16_t)x0, (int16_t)y0,
                   (int16_t)s_size, (int16_t)s_size,
                   s_pack, (int16_t)s_stride, NET_COL_INK, (int16_t)scale);
}

/* 给定可用边长，选最大的整数缩放（模块必须整像素，否则边缘发毛）*/
static int pick_scale(int box)
{
    int s;

    if (s_size <= 0 || box <= 0) {
        return 0;
    }
    s = box / s_size;
    if (s < 1) {
        s = 1;
    }
    return s;
}

void net_card_draw_card(void)
{
    const char *info;
    int scale, w, y;

    draw_text_center(132, "SCAN TO JOIN", 1, C_TITLE);

    if (!s_has_qr) {
        /* 还没收到二维码：给出"在等什么"，别让界面看起来是坏的 */
        draw_text_center(240, "waiting for network", 1, C_TITLE);
        draw_text_center(268, "from ESP32 over UART3", 1, UI_COL_SUB);
        return;
    }

    scale = pick_scale(CARD_QR_MAX);
    w     = s_size * scale;
    y     = CARD_QR_TOP;
    draw_qr_block(CARD_QR_CX - w / 2, y, scale);

    draw_text_center(y + w + 12, state_line(), 1, C_TITLE);

    info = info_line(s_info, (int)sizeof(s_info));
    if (info != 0) {
        draw_text_center(y + w + 34, info, 1, UI_COL_SUB);
    }
}

void net_card_draw_full(void)
{
    const char *info;
    int scale, w, y;

    /* 全屏：整块白底，模块四周留够静区（标准要求 ≥4 模块，
     * 这里靠白色背景 + 20px 边距自然满足，比"画个白框"更省事也更稳）*/
    ui_add_round_fill(0, 0, 480, 480, 0, NET_COL_PAPER);

    if (!s_has_qr) {
        draw_text_center(220, "no network code", 1, C_TITLE);
        draw_text_center(248, "tap to go back", 1, UI_COL_SUB);
        return;
    }

    scale = pick_scale(FULL_QR_MAX);
    w     = s_size * scale;
    y     = FULL_QR_TOP;
    draw_qr_block((480 - w) / 2, y, scale);

    info = info_line(s_info, (int)sizeof(s_info));
    {
        int ty = y + w + 12;
        if (ty > 480 - 58) {
            ty = 480 - 58;
        }
        draw_text_center(ty, state_line(), 1, C_TITLE);
        if (info != 0) {
            draw_text_center(ty + 22, info, 1, UI_COL_SUB);
        }
    }
}

int net_card_tap(int x, int y)
{
    (void)x;
    (void)y;

    if (!s_has_qr) {
        return 0;                    /* 没码：点了也没意义，交回上层 */
    }
    s_full  = (s_full ? 0 : 1);
    s_dirty = 1;
    return 1;
}
