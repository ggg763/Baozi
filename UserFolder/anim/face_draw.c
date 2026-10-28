/******************************************************************************
 * @file     face_draw.c
 * @brief    v9 表情图元光栅器实现（见 face_draw.h）
 *
 * 纯计算：不碰屏幕、不碰 DMA、不依赖任何外设头文件 —— 因此可以在 PC 上
 * 用 mingw gcc 直接编译运行，把输出与浏览器模拟器逐像素对比。
 *
 * 定点约定：
 *   所有几何量进来是【1/2 像素】，进来立刻换成【1/256 像素】定点：
 *       屏幕 X = (240 + x/2) * 256 = 61440 + x*128
 *       半径   = rx * 128
 *   行中心 = y*256 + 128；两条子扫描线 = y*256 + 64 / +192。
 *
 * 为什么不用浮点：TD1601 没有 FPU，一次 float 运算要软仿上百周期。
 * 这里全部是 int32 乘除 + 一次 isqrt，一行一个部件约 30 个周期。
 ******************************************************************************/

#include "face_draw.h"
#include "face_sup_tab.h"

#define FX          256
#define X_FX(v)     (61440L + (int32_t)(v) * 128L)      /* 中心 x（1/2px）→ 1/256px */
#define Y_FX(v)     (61440L + (int32_t)(v) * 128L)      /* 中心 y 同理 */
#define R_FX(v)     ((int32_t)(v) * 128L)               /* 半径 */
#define MAXSPAN     3

/* 多边形暂存（星形最多 2*6 点，超椭圆旋转 2*33 点）*/
#define POLY_MAX    70
static int32_t s_poly[POLY_MAX * 2];

/* ---------------------------------------------------------------- 基础 */
static int32_t isqrt32(uint32_t n)
{
    uint32_t x = n, y = 0, bit = 1u << 30;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= y + bit) {
            x -= y + bit;
            y = (y >> 1) + bit;
        } else {
            y >>= 1;
        }
        bit >>= 2;
    }
    return (int32_t)y;
}

static int32_t iabs32(int32_t v)
{
    return (v < 0) ? -v : v;
}

/* 线段 a..b 是否跨过 y —— 必须比较符号，不能写 (a-y)*(b-y)<=0：
   a 与 y 都是 1/256px，量级到 ±246k，乘积 6e10 会溢出 int32，
   判据随即变成随机数（这是第一版扫描线出现横条伪影的根因）。*/
static int crosses(int32_t a, int32_t b, int32_t y)
{
    if (a == b) {
        return 0;
    }
    return ((a < y) != (b < y)) || (a == y) || (b == y);
}

/* Q8 正弦/余弦，角度用度（任意范围，内部取模）*/
static int32_t sin_q8(int deg)
{
    int sgn = 1;
    deg %= 360;
    if (deg < 0) {
        deg += 360;
    }
    if (deg >= 180) {
        deg -= 180;
        sgn = -1;
    }
    if (deg > 90) {
        deg = 180 - deg;
    }
    {
        int32_t v = (int32_t)fd_sin_q15[deg * 128 / 180];
        return sgn * ((v + (v >> 7)) >> 7);      /* Q15 -> Q8，四舍五入到 256 */
    }
}

static int32_t cos_q8(int deg)
{
    return sin_q8(deg + 90);
}

/* 椭圆在 dy 处的半宽（1/256 px）；<0 = 该行无覆盖 */
static int32_t ell_half(int32_t dy, int32_t rx, int32_t ry)
{
    int32_t v, rem;

    if (rx <= 0 || ry <= 0) {
        return -1;
    }
    v = (dy * FX) / ry;                 /* |v| <= 256 */
    if (v >= FX || v <= -FX) {
        return -1;
    }
    rem = FX * FX - v * v;
    return (rx * isqrt32((uint32_t)rem)) / FX;
}

/* 超椭圆半宽：n=2 走解析，其余查表 + 线性插值 */
static int32_t sup_half(int32_t dy, int32_t rx, int32_t ry, uint8_t n10)
{
    int32_t tt, idx, fr, v, t0, t1, prof;

    if (n10 == 0) {
        n10 = 20;
    }
    if (n10 <= 20) {
        return ell_half(dy, rx, ry);
    }
    if (ry <= 0) {
        return -1;
    }
    tt = (iabs32(dy) * FX) / ry;        /* 0..256 */
    if (tt >= FX) {
        return -1;
    }
    idx = tt >> 3;                      /* 0..31 */
    fr = tt & 7;

    prof = 0;
    {
        int i, bestd = 999, d;
        for (i = 0; i < FD_SUP_PROFILES; i++) {
            d = (int)fd_sup_n10[i] - (int)n10;
            if (d < 0) {
                d = -d;
            }
            if (d < bestd) {
                bestd = d;
                prof = i;
            }
        }
    }
    t0 = fd_sup_tab[prof][idx];
    t1 = fd_sup_tab[prof][idx + 1];
    v = t0 + ((t1 - t0) * fr) / 8;
    if (v <= 0) {
        return -1;
    }
    return (rx * v) / 255;
}

/* ---------------------------------------------------------------- 区间 */
typedef struct {
    int32_t x0[MAXSPAN], x1[MAXSPAN];
    int n;
} span_t;

static void sp_reset(span_t *s)
{
    s->n = 0;
}

static void sp_push(span_t *s, int32_t a, int32_t b)
{
    if (s->n >= MAXSPAN || b <= a) {
        return;
    }
    if (a < 0) {
        a = 0;
    }
    if (b > FD_W * FX) {
        b = FD_W * FX;
    }
    if (b <= a) {
        return;
    }
    s->x0[s->n] = a;
    s->x1[s->n] = b;
    s->n++;
}

/* ---------------------------------------------------------------- 各图元 */

/* 超椭圆（含带旋转的情况：旋转时切成多边形，用奇偶规则求区间）*/
static void poly_spans(const int32_t *pts, int npts, int32_t y2, span_t *s)
{
    int32_t xs[24];
    int n = 0, i;

    for (i = 0; i < npts; i++) {
        int32_t ax = pts[i * 2], ay = pts[i * 2 + 1];
        int32_t bx = pts[((i + 1) % npts) * 2], by = pts[((i + 1) % npts) * 2 + 1];
        if (!crosses(ay, by, y2)) {
            continue;
        }
        {
            /* 用 64 位保护一下乘法：坐标最大 ±123k，差 ±246k，乘积 6e10 超 int32 */
            int64_t t;
            t = (int64_t)(y2 - ay) * (bx - ax);
            t = t / (by - ay);
            if (n < 24) {
                xs[n++] = ax + (int32_t)t;
            }
        }
    }
    if (n < 2) {
        return;
    }
    /* 插入排序 */
    for (i = 1; i < n; i++) {
        int32_t key = xs[i];
        int j = i - 1;
        while (j >= 0 && xs[j] > key) {
            xs[j + 1] = xs[j];
            j--;
        }
        xs[j + 1] = key;
    }
    for (i = 0; i + 1 < n; i += 2) {
        sp_push(s, xs[i], xs[i + 1]);
    }
}

static int build_sup_poly(const fd_part_t *p, int32_t cx, int32_t cy,
                          int32_t rx, int32_t ry, uint8_t n10)
{
    /* 按 t = v/ry 从 -1 走到 +1 求出右半轮廓，再走左半轮廓 */
    int i, n = 0;
    int32_t sn = sin_q8(p->rot), cs = cos_q8(p->rot);

    for (i = 0; i < 33; i++) {
        int32_t t = -FX + (i * 2 * FX) / 32;         /* -256..256 */
        int32_t dy = (ry * t) / FX;
        int32_t h = sup_half(dy, rx, ry, n10);
        if (h < 0) {
            h = 0;
        }
        {
            int32_t ux = h, uy = dy;
            /* 旋转 */
            int32_t rxp = (ux * cs - uy * sn) / FX;
            int32_t ryp = (ux * sn + uy * cs) / FX;
            if (n < POLY_MAX) {
                s_poly[n * 2] = cx + rxp;
                s_poly[n * 2 + 1] = cy + ryp;
                n++;
            }
        }
    }
    for (i = 32; i >= 0; i--) {
        int32_t t = -FX + (i * 2 * FX) / 32;
        int32_t dy = (ry * t) / FX;
        int32_t h = sup_half(dy, rx, ry, n10);
        if (h < 0) {
            h = 0;
        }
        {
            int32_t ux = -h, uy = dy;
            int32_t rxp = (ux * cs - uy * sn) / FX;
            int32_t ryp = (ux * sn + uy * cs) / FX;
            if (n < POLY_MAX) {
                s_poly[n * 2] = cx + rxp;
                s_poly[n * 2 + 1] = cy + ryp;
                n++;
            }
        }
    }
    return n;
}

static int build_star_poly(const fd_part_t *p, int32_t cx, int32_t cy,
                           int32_t rx, int32_t ry, int tips)
{
    int i, n = 0;
    int32_t inner = (p->c > 0) ? p->c : 45;

    if (tips < 3) {
        tips = 5;
    }
    for (i = 0; i < tips * 2; i++) {
        /* 角步长 = 360/(tips*2)，从正上方开始 */
        int deg = -90 + (i * 360) / (tips * 2);
        int32_t s = sin_q8(deg), c = cos_q8(deg);
        int32_t rr = (i & 1) ? inner : 100;
        if (n < POLY_MAX) {
            s_poly[n * 2] = cx + (rx * rr / 100) * c / FX;
            s_poly[n * 2 + 1] = cy + (ry * rr / 100) * s / FX;
            n++;
        }
    }
    return n;
}

static void spans_of(const fd_part_t *p, int32_t y2, span_t *s)
{
    int32_t cx = X_FX(p->x), cy = Y_FX(p->y);
    int32_t rx = R_FX(p->rx), ry = R_FX(p->ry);
    int32_t dy = y2 - cy;
    int32_t h;

    sp_reset(s);

    switch (p->kind) {
        case FD_SUP: {
            if (p->rot != 0) {
                int n = build_sup_poly(p, cx, cy, rx, ry, p->n10);
                poly_spans(s_poly, n, y2, s);
            } else {
                h = sup_half(dy, rx, ry, p->n10);
                if (h >= 0) {
                    sp_push(s, cx - h, cx + h);
                }
            }
            break;
        }

        case FD_DISC: {
            h = ell_half(dy, rx, rx);
            if (h >= 0) {
                sp_push(s, cx - h, cx + h);
            }
            break;
        }

        case FD_CUT: {
            int32_t nx = cos_q8(p->a), ny = sin_q8(p->a);
            int32_t d = R_FX(p->b);       /* 偏移：1/2px → 1/256px */
            h = ell_half(dy, rx, ry);
            if (h < 0) {
                break;
            }
            if (nx == 0) {
                /* 水平切：n = (0, ±1) */
                if ((ny * dy) / FX >= d) {
                    sp_push(s, cx - h, cx + h);
                }
            } else {
                int32_t k = d - (ny * dy) / FX;       /* (x-cx)*nx >= k */
                int32_t lim;
                if (nx > 2 || nx < -2) {
                    lim = cx + (k * FX) / nx;
                } else {
                    lim = (nx > 0) ? (cx - h) : (cx + h);   /* 近水平切：整行都算保留 */
                }
                /* 必须夹回椭圆范围内，否则切弦在弧外时区间会溢出到别处 */
                if (lim < cx - h) lim = cx - h;
                if (lim > cx + h) lim = cx + h;
                if (nx > 0) {
                    sp_push(s, lim, cx + h);
                } else {
                    sp_push(s, cx - h, lim);
                }
            }
            break;
        }

        case FD_CRES: {
            int32_t a, b;
            int32_t d = R_FX(p->a);
            a = ell_half(dy, rx, ry);
            if (a < 0) {
                break;
            }
            b = ell_half(dy - d, rx, ry);
            if (b < 0) {
                sp_push(s, cx - a, cx + a);
            } else if (b < a) {
                sp_push(s, cx - a, cx - b);
                sp_push(s, cx + b, cx + a);
            }
            break;
        }

        case FD_CAP: {
            int32_t x0 = X_FX(p->x), y0 = Y_FX(p->y);
            int32_t x1 = X_FX(p->a), y1 = Y_FX(p->b);
            int32_t r = R_FX(p->rx);
            int32_t dx = x1 - x0, dy2 = y1 - y0;
            int32_t lo = 0x7FFFFFFF, hi = -0x7FFFFFFF;
            int32_t hh;

            /* 两端圆 */
            hh = ell_half(y2 - y0, r, r);
            if (hh >= 0) {
                if (x0 - hh < lo) lo = x0 - hh;
                if (x0 + hh > hi) hi = x0 + hh;
            }
            hh = ell_half(y2 - y1, r, r);
            if (hh >= 0) {
                if (x1 - hh < lo) lo = x1 - hh;
                if (x1 + hh > hi) hi = x1 + hh;
            }
            /* 中间矩形：把法向偏移算出来，再求四条边与扫描线的交点 */
            if (dx != 0 || dy2 != 0) {
                int32_t ax = iabs32(dx), ay = iabs32(dy2);
                int32_t big = (ax > ay) ? ax : ay;
                int sh = 0;
                int32_t sx, sy, L, nx, ny;
                int32_t qx[4], qy[4];
                int i;
                while (big > 4096) {
                    big >>= 1;
                    sh++;
                }
                sx = dx >> sh;
                sy = dy2 >> sh;
                L = isqrt32((uint32_t)(sx * sx + sy * sy));
                if (L > 0) {
                    nx = (-sy * r) / L;
                    ny = (sx * r) / L;
                    qx[0] = x0 + nx; qy[0] = y0 + ny;
                    qx[1] = x1 + nx; qy[1] = y1 + ny;
                    qx[2] = x1 - nx; qy[2] = y1 - ny;
                    qx[3] = x0 - nx; qy[3] = y0 - ny;
                    for (i = 0; i < 4; i++) {
                        int j = (i + 1) & 3;
                        if (!crosses(qy[i], qy[j], y2)) {
                            continue;
                        }
                        {
                            int64_t t = (int64_t)(y2 - qy[i]) * (qx[j] - qx[i]);
                            int32_t xc = qx[i] + (int32_t)(t / (qy[j] - qy[i]));
                            if (xc < lo) lo = xc;
                            if (xc > hi) hi = xc;
                        }
                    }
                }
            }
            if (hi > lo) {
                sp_push(s, lo, hi);
            }
            break;
        }

        case FD_RING: {
            int32_t R2 = rx;
            int32_t r2 = rx - R_FX(p->c);
            int32_t ho, hi2;
            ho = ell_half(dy, R2, R2);
            if (ho < 0) {
                break;
            }
            hi2 = ell_half(dy, r2, r2);
            if (hi2 < 0) {
                sp_push(s, cx - ho, cx + ho);
            } else if (hi2 < ho) {
                sp_push(s, cx - ho, cx - hi2);
                sp_push(s, cx + hi2, cx + ho);
            }
            break;
        }

        case FD_HEART: {
            /* 左右两圆（半径 = 全宽/4）+ 下方三角 */
            int32_t W = rx, H = ry;            /* 1/256 px 的全宽/全高 */
            int32_t R = W / 4;
            int32_t yl = cy - H / 2 + R;

            h = ell_half(y2 - yl, R, R);
            if (h >= 0) {
                sp_push(s, cx - R - h, cx - R + h);
                sp_push(s, cx + R - h, cx + R + h);
            }
            {
                int32_t ytip = cy + H / 2;
                if (y2 >= yl && y2 <= ytip) {
                    int32_t t  = y2 - yl;
                    int32_t tt = ytip - yl;
                    if (tt > 0) {
                        int32_t w = (W / 2) - (W / 2) * t / tt;
                        sp_push(s, cx - w, cx + w);
                    }
                }
            }
            break;
        }

        case FD_STAR: {
            int n = build_star_poly(p, cx, cy, rx, ry, (p->n10 > 0) ? p->n10 : 5);
            poly_spans(s_poly, n, y2, s);
            break;
        }

        default:
            break;
    }
}

/* ---------------------------------------------------------------- 覆盖累加 */
static void cov_add(uint8_t *cov, int32_t a, int32_t b, int32_t w)
{
    int32_t ia = a >> 8;            /* 1/256px → px */
    int32_t ib = b >> 8;
    int32_t fa = a & 255;
    int32_t fb = b & 255;
    int32_t i;
    int32_t v;

    if (ia < 0) {
        ia = 0;
        fa = 0;
    }
    if (ib >= FD_W) {
        ib = FD_W - 1;
        fb = 0;
    }
    if (ia > ib) {
        return;
    }
    if (ia == ib) {
        v = cov[ia] + (w * (b - a)) / FX;
        cov[ia] = (v > 255) ? 255 : (uint8_t)v;
        return;
    }
    v = cov[ia] + (w * (FX - fa)) / FX;
    cov[ia] = (v > 255) ? 255 : (uint8_t)v;
    for (i = ia + 1; i < ib; i++) {
        v = cov[i] + w;
        cov[i] = (v > 255) ? 255 : (uint8_t)v;
    }
    v = cov[ib] + (w * fb) / FX;
    cov[ib] = (v > 255) ? 255 : (uint8_t)v;
}

void fd_cov_clear(uint8_t *cov)
{
    int i;
    for (i = 0; i < FD_W; i++) {
        cov[i] = 0;
    }
}

void fd_cov_part(uint8_t *cov, const fd_part_t *p, int y, int flip, uint8_t alpha)
{
    fd_part_t q;
    span_t s;
    int sub;
    int32_t w;

    if (p == 0 || alpha == 0 || p->alpha == 0) {
        return;
    }
    q = *p;
    if (flip) {
        q.x = (int16_t)(-q.x);
        if (q.kind == FD_CAP) {
            q.a = (int16_t)(-q.a);      /* 终点 x 也要镜像；两端各取反，不要交换 */
        }
        q.rot = (int16_t)(-q.rot);
        if (q.kind == FD_CUT) {
            q.a = (int16_t)(180 - q.a);
        }
    }
    /* 两条子扫描线各 1/2；再乘部件自身的不透明度 */
    w = (128L * alpha * (int32_t)q.alpha) / (255L * 255L);

    for (sub = 0; sub < 2; sub++) {
        int32_t y2 = (int32_t)y * FX + 64 + sub * 128;
        int i;
        spans_of(&q, y2, &s);
        for (i = 0; i < s.n; i++) {
            cov_add(cov, s.x0[i], s.x1[i], w);
        }
    }
}

/* ---------------------------------------------------------------- 调色 */
static uint16_t mix565(uint16_t a, uint16_t b, int wa, int wb)
{
    int r  = (((a >> 11) & 0x1F) * wa + ((b >> 11) & 0x1F) * wb) / (wa + wb);
    int g  = (((a >> 5) & 0x3F) * wa + ((b >> 5) & 0x3F) * wb) / (wa + wb);
    int bl = ((a & 0x1F) * wa + (b & 0x1F) * wb) / (wa + wb);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

void fd_blend_row(uint16_t *line, const uint8_t *cov, uint16_t fg, uint16_t bg)
{
    int i;
    for (i = 0; i < FD_W; i++) {
        int c = cov[i];
        if (c <= 0) {
            line[i] = bg;
        } else if (c >= 255) {
            line[i] = fg;
        } else {
            line[i] = mix565(fg, bg, c, 255 - c);
        }
    }
}

/* ---------------------------------------------------------------- 自检 */
int fd_selftest(void)
{
    uint8_t cov[FD_W];
    fd_part_t p;
    int i, lit;

    /* 圆心在屏幕正中、半径 100px 的圆，第 240 行应该有约 200px 宽 */
    p.kind = FD_DISC; p.role = FR_EYE; p.mirror = 0; p.n10 = 0; p.alpha = 255;
    p.x = 0; p.y = 0; p.rx = 200; p.ry = 200; p.rot = 0; p.a = 0; p.b = 0; p.c = 0;
    fd_cov_clear(cov);
    fd_cov_part(cov, &p, 240, 0, 255);
    lit = 0;
    for (i = 0; i < FD_W; i++) {
        if (cov[i] > 128) {
            lit++;
        }
    }
    if (lit < 195 || lit > 205) {
        return 1;                   /* 直径应为 200px（±5px 容差） */
    }
    /* 空行不应有覆盖 */
    fd_cov_clear(cov);
    fd_cov_part(cov, &p, 10, 0, 255);
    for (i = 0; i < FD_W; i++) {
        if (cov[i] != 0) {
            return 2;
        }
    }
    return 0;
}
