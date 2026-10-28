/******************************************************************************
 * @file     face_model.c
 * @brief    姿势数据库 + 几何变换实现（见 face_model.h）
 *
 * 姿势数据在 face_poses_v9.h（自动生成，来源是模拟器导出的 faces_v9.json）。
 * 本文件只做三件事：
 *   1. 按名字查姿势
 *   2. 两帧之间插值（切换表情的过渡）
 *   3. 把眨眼/视线/张嘴叠到几何上
 *
 * 全部整数、无浮点、无外设依赖 —— 可以在 PC 上编译跑出图。
 ******************************************************************************/

#include <stdint.h>

#include "face_model.h"
#include "face_poses_v9.h"          /* fd_poses[] / FD_POSE_COUNT（自动生成） */

/* ---------------------------------------------------------------- 查表 */
int face_model_count(void)
{
    return FD_POSE_COUNT;
}

const fd_pose_t *face_model_get(int idx)
{
    if (idx < 0 || idx >= FD_POSE_COUNT) {
        return 0;
    }
    return &fd_poses[idx];
}

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_eq(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    while ((*a != '\0') && (*b != '\0')) {
        if (lc(*a) != lc(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return ((*a == '\0') && (*b == '\0')) ? 1 : 0;
}

int face_model_find(const char *name)
{
    int i;

    if (name == 0) {
        return -1;
    }
    for (i = 0; i < FD_POSE_COUNT; i++) {
        if (ci_eq(fd_poses[i].name, name) != 0) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- 构建 */
int face_model_build(const fd_pose_t *ps, fd_part_t *out, int max)
{
    int i, n;

    if (ps == 0 || out == 0 || max <= 0) {
        return 0;
    }
    n = (ps->n < max) ? ps->n : max;
    for (i = 0; i < n; i++) {
        out[i] = ps->parts[i];
    }
    return n;
}

/* ---------------------------------------------------------------- 插值 */
/* 配对规则：**先按角色分组，组内按 x 升序，再顺序配对**。
 *
 * 为什么必须按角色分组（踩过的坑）：
 *   laugh 有 2 个部件（眼+嘴），shy 有 4 个（眼+3 道腮红）。只按顺序配对会让
 *   "嘴"和"第一道腮红"配成一对，插值时嘴被拉成一条竖直细弧 —— 屏幕上就是
 *   一道莫名其妙的斜线。角色不同的部件之间不存在"同一个东西在变形"的关系，
 *   只能淡出 + 淡入。
 *
 * 为什么组内要按 x 排序：切换表情时位置变化大，贪心最近邻会把左眼配到右眼，
 *   插值过程中两只眼交叉飞过 —— 看起来像眼睛瞬移。 */
static void gather_role(const fd_part_t *p, int n, uint8_t role,
                        fd_part_t *out, int *cnt)
{
    int i, j;

    *cnt = 0;
    for (i = 0; i < n; i++) {
        if (p[i].role == role && *cnt < FD_MAX_PARTS) {
            out[(*cnt)++] = p[i];
        }
    }
    for (i = 1; i < *cnt; i++) {
        fd_part_t t = out[i];
        for (j = i - 1; j >= 0 && out[j].x > t.x; j--) {
            out[j + 1] = out[j];
        }
        out[j + 1] = t;
    }
}

static int16_t lerp_i16(int16_t a, int16_t b, int t)
{
    return (int16_t)(a + ((b - a) * t) / 255);
}

static void part_lerp(const fd_part_t *a, const fd_part_t *b, int t, fd_part_t *o)
{
    o->x    = lerp_i16(a->x, b->x, t);
    o->y    = lerp_i16(a->y, b->y, t);
    o->rx   = (uint16_t)(a->rx + ((int32_t)b->rx - (int32_t)a->rx) * t / 255);
    o->ry   = (uint16_t)(a->ry + ((int32_t)b->ry - (int32_t)a->ry) * t / 255);
    o->rot  = lerp_i16(a->rot, b->rot, t);
    o->a    = lerp_i16(a->a, b->a, t);
    o->b    = lerp_i16(a->b, b->b, t);
    o->c    = lerp_i16(a->c, b->c, t);
    o->n10  = (uint8_t)(a->n10 + ((int)b->n10 - (int)a->n10) * t / 255);
    o->alpha = (uint8_t)(a->alpha + ((int)b->alpha - (int)a->alpha) * t / 255);
    /* 形状/角色/镜像在过渡中点切换：这几项是"离散"属性，插值没有物理意义 */
    if (t >= 128) {
        o->kind   = b->kind;
        o->role   = b->role;
        o->mirror = b->mirror;
    } else {
        o->kind   = a->kind;
        o->role   = a->role;
        o->mirror = a->mirror;
    }
}

/* 未配对部件的淡出/淡入：只改不透明度，几何原样。
 *
 * ⚠ 曾经的错误做法是"把半径缩到 0"：对胶囊（腮红斜纹、泪）来说端点坐标
 *   (a,b) 也是相对屏幕中心的绝对值，按同一比例缩就会把它们拉向原点 ——
 *   过渡时屏幕中央会窜出一条斜线。改用 alpha 后不再有任何几何畸变。 */
static void part_fade(const fd_part_t *a, int t, int appear, fd_part_t *o)
{
    int k = appear ? t : (255 - t);

    *o = *a;
    o->alpha = (uint8_t)((int32_t)a->alpha * k / 255);
}

static int pair_group(const fd_part_t *a, int na, const fd_part_t *b, int nb,
                      int t, fd_part_t *out, int n, int max)
{
    int i;

    for (i = 0; i < na && n < max; i++) {
        if (i < nb) {
            part_lerp(&a[i], &b[i], t, &out[n]);
        } else {
            part_fade(&a[i], t, 0, &out[n]);
        }
        n++;
    }
    for (; i < nb && n < max; i++) {
        part_fade(&b[i], t, 1, &out[n]);
        n++;
    }
    return n;
}

int face_model_mix(const fd_part_t *a, int na, const fd_part_t *b, int nb,
                   uint8_t t, fd_part_t *out, int max)
{
    /* 顺序有讲究：眼睛在最前，保证"面积前 2 = 眼睛"这类假设成立；
     * 其余按"嘴 -> 装饰 -> 腮红 -> 气泡"排，视觉上叠放顺序稳定。 */
    static const uint8_t order[] = { FR_EYE, FR_MOUTH, FR_DECO, FR_BLUSH, FR_BUBBLE };
    fd_part_t ga[FD_MAX_PARTS], gb[FD_MAX_PARTS];
    int n = 0;
    unsigned r;

    if (out == 0 || max <= 0) {
        return 0;
    }
    for (r = 0; r < sizeof(order) / sizeof(order[0]) && n < max; r++) {
        int na2, nb2;
        gather_role(a, na, order[r], ga, &na2);
        gather_role(b, nb, order[r], gb, &nb2);
        n = pair_group(ga, na2, gb, nb2, t, out, n, max);
    }
    return n;
}

/* ---------------------------------------------------------------- 参数叠加 */
void face_expr_reset(face_expr_t *e)
{
    if (e == 0) {
        return;
    }
    e->gaze_dx = 0;
    e->gaze_dy = 0;
    e->blink = 0;
    e->eye_scale = 100;
    e->mouth_open = 0;
}

void face_model_apply(fd_part_t *p, int n, const face_expr_t *e)
{
    int i;

    if (p == 0 || e == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        fd_part_t *q = &p[i];

        if (q->role == FR_EYE) {
            if (e->eye_scale > 0 && e->eye_scale != 100) {
                int32_t s = e->eye_scale;
                q->rx = (uint16_t)((int32_t)q->rx * s / 100);
                q->ry = (uint16_t)((int32_t)q->ry * s / 100);
                /* 月牙的纵向偏移决定"厚度"：不跟着缩放的话，眼睛变大但月牙不变厚 */
                if (q->kind == FD_CRES) {
                    q->a = (int16_t)((int32_t)q->a * s / 100);
                }
            }
            q->x = (int16_t)(q->x + e->gaze_dx);
            q->y = (int16_t)(q->y + e->gaze_dy);

            if (e->blink != 0) {
                int32_t k = 255 - (int32_t)e->blink;   /* 0..255 = 睁眼程度 */
                int32_t ry = (int32_t)q->ry * k / 255;
                if (ry < 6) {
                    ry = 6;         /* 闭合时留一条 3px 细线：完全消失会像没眼睛 */
                }
                q->ry = (uint16_t)ry;
                if (q->kind == FD_CRES) {
                    q->a = (int16_t)((int32_t)q->a * k / 255);
                }
            }
        } else if (q->role == FR_MOUTH) {
            if (e->mouth_open != 0 && q->kind != FD_CAP) {
                /* 张嘴：纵向最多放大到 1.55 倍（与模拟器 mouth*0.55 同口径）*/
                int32_t g = 255 + (int32_t)e->mouth_open * 140 / 255;
                q->ry = (uint16_t)((int32_t)q->ry * g / 255);
            }
        }
    }
}

/* ---------------------------------------------------------------- 颜色 */
uint16_t face_model_lerp_color(uint16_t a, uint16_t b, uint8_t t)
{
    int wa = 255 - t;
    int wb = t;
    int r, g, bl;

    if (t == 0) {
        return a;
    }
    if (t == 255) {
        return b;
    }
    r  = ((((a >> 11) & 0x1F) * wa) + (((b >> 11) & 0x1F) * wb)) / 255;
    g  = ((((a >> 5) & 0x3F) * wa) + (((b >> 5) & 0x3F) * wb)) / 255;
    bl = (((a & 0x1F) * wa) + ((b & 0x1F) * wb)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
