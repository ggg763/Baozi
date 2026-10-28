/******************************************************************************
 * @file     face_action.c
 * @brief    动作层实现（见 face_action.h）
 *
 * 每个动作 = 若干条正弦的叠加，参数直接照搬浏览器模拟器的 ACT 表
 * （_probe/face_sim_v9.html）。模拟器里的 time 单位是 ms，这里一致，
 * 所以两边的动作曲线是同一条，改一个数记得两边一起改。
 *
 * 正弦怎么算（没有 FPU）：
 *   fd_sin_q15[] 是 0..90°、65 点的 Q15 表（face_sup_tab.h，自动生成）。
 *   把相位归一化到 0..1023（= 0..2π），按象限折叠后查表 + 线性插值。
 *   一次 osc() 约 20 个周期，20ms 一次调用完全无压力。
 *
 * 为什么要"插值"而不是直接取最近点：表是 1.4° 一档，小振幅（例如 gy 只有 2）
 * 时最近的台阶会造成 1 个单位以上的跳变，看起来是抖而不是动。
 ******************************************************************************/

#include <stdint.h>

#include "face_action.h"
#include "face_sup_tab.h"           /* fd_sin_q15[] */

/* ---------------------------------------------------------------- 正弦 */
/* 相位 0..1023 -> 正弦 Q10（-1024..1024） */
static int32_t sin_q10(int32_t ph)
{
    int32_t q, r, idx, fr, v0, v1, v;
    int neg = 0;

    ph &= 1023;
    q = ph >> 8;                    /* 象限 0..3 */
    r = ph & 255;                   /* 象限内相位 0..255 */

    if (q & 1) {                    /* 第 2、4 象限：用余角 sin(90-x) */
        r = 256 - r;
        if (r == 256) {             /* 正好 90°：表尾 */
            return (q == 1) ? 1024 : -1024;
        }
    }
    if (q >= 2) {
        neg = 1;
    }

    /* r*64/256 -> 0..63，余数 fr 用于插值 */
    idx = (r * 64) >> 8;
    fr  = (r * 64) & 255;
    v0 = fd_sin_q15[idx];
    v1 = fd_sin_q15[idx + 1];
    v  = v0 + ((v1 - v0) * fr) / 256;
    v  = (v * 1024) / 32767;        /* Q15 -> Q10 */

    return neg ? -v : v;
}

/* 周期为 per 毫秒、振幅为 amp 的正弦（0 起点） */
static int32_t osc(uint32_t t, uint32_t per, int32_t amp)
{
    int32_t ph;

    if (per == 0 || amp == 0) {
        return 0;
    }
    ph = (int32_t)(((uint32_t)(t % per) * 1024u) / per);
    return (amp * sin_q10(ph)) / 1024;
}

/* 全波整流：|sin|，返回 0..amp。周期是 sin 的一半。
 * laugh 的眼/嘴用的就是 Math.abs(Math.sin(...)) —— 用半波整流（pos_osc）会
 * 变成"鼓一下、停一下"，节奏完全不对。 */
static int32_t abs_osc(uint32_t t, uint32_t per, int32_t amp)
{
    int32_t v = osc(t, per, amp);

    return (v < 0) ? -v : v;
}

/* 正半周：max(0, sin)，返回 0..amp。用于"只往一个方向鼓"的动作（心跳、灵光）。*/
static int32_t pos_osc(uint32_t t, uint32_t per, int32_t amp)
{
    int32_t v = osc(t, per, amp);

    return (v > 0) ? v : 0;
}

/* ---------------------------------------------------------------- 动作 */
/* 常用周期（= 2π / 模拟器里的角频率，保留整数毫秒）：
 *   0.00035 -> 17952   0.0004 -> 15708   0.0006 -> 10472   0.0007 -> 8976
 *   0.0008  -> 7854    0.0009 -> 6981    0.0011 -> 5712    0.0012 -> 5236
 *   0.0013  -> 4833    0.0017 -> 3696    0.0021 -> 2992    0.0026 -> 2417
 *   0.003   -> 2094    0.0038 -> 1653    0.0042 -> 1496    0.0053 -> 1185
 *   0.0065  -> 967     0.0071 -> 885     0.0075 -> 838     0.008  -> 785
 *   0.013   -> 483     0.02   -> 314     0.031  -> 203     0.05   -> 126
 */

/* 待机：缓慢巡视。这是"活着"的基线，所有姿势都能叠它 */
static void a_idle(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 5712, 26);
    e->gaze_dy = (int16_t)osc(t, 8976, 10);
}

static void a_happy(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 4833, 18);
    e->gaze_dy = (int16_t)(-4 + osc(t, 6981, 8));
    e->eye_scale = (int16_t)(100 + osc(t, 2992, 2));
}

static void a_laugh(uint32_t t, face_expr_t *e)
{
    int32_t b = osc(t, 483, 100);           /* sin(t*0.013) */
    int32_t m = abs_osc(t, 967, 100);       /* |sin(t*0.0065)| —— 嘴是双倍频率 */

    e->gaze_dy = (int16_t)(-6 + b * 3 / 100);
    e->eye_scale = (int16_t)(100 + ((b < 0) ? -b : b) * 3 / 100);
    e->mouth_open = (uint8_t)(128 + m * 127 / 100);
}

/* 被抚摸 / 害羞：眯眼由上层按抚摸强度另叠，这里只给基础的轻颤 */
static void a_shy(uint32_t t, face_expr_t *e)
{
    int32_t b = osc(t, 2094, 13);           /* 13*sin(t*0.003) */

    e->blink = (uint8_t)((b > 0) ? b : 0);
    e->gaze_dx = (int16_t)osc(t, 1571, 6);
    e->gaze_dy = 6;
}

static void a_sad(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = (int16_t)(26 + osc(t, 7854, 5));
    e->gaze_dx = (int16_t)osc(t, 10472, 8);
    e->blink = 20;
}

static void a_sad_anguish(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = 20;
    e->gaze_dx = (int16_t)osc(t, 8976, 6);
    e->blink = 31;
}

static void a_angry_low(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 314, 5);   /* 高频小幅：气得发抖 */
    e->gaze_dy = 4;
    e->blink = 25;
}

static void a_angry_high(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 203, 7);
    e->gaze_dy = 2;
    e->blink = 10;
    e->eye_scale = (int16_t)(100 + osc(t, 126, 1));
}

static void a_surprised(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = -8;
    e->eye_scale = (int16_t)(106 + osc(t, 785, 3));
}

static void a_loving(uint32_t t, face_expr_t *e)
{
    int32_t b = pos_osc(t, 838, 100);

    e->eye_scale = (int16_t)(100 + b * b * 9 / 10000);   /* 心跳两下的加速感 */
    e->gaze_dy = 2;
    e->mouth_open = (uint8_t)(102 + b * 153 / 100);
}

static void a_asleep(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = (int16_t)(8 + osc(t, 15708, 4));        /* 呼吸：很慢 */
    e->eye_scale = (int16_t)(100 + osc(t, 6981, 1));
}

static void a_think(uint32_t t, face_expr_t *e)
{
    /* 视线在右上画圈：sin 与 cos 各一路（cos 用相位前移 1/4 周期）*/
    e->gaze_dx = (int16_t)(16 + osc(t, 5200, 10));
    e->gaze_dy = (int16_t)(-16 + osc(t + 1300, 5200, 8));
    e->blink = 15;
}

static void a_listen(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = (int16_t)(-8 + osc(t, 6981, 6));
    e->gaze_dx = (int16_t)osc(t, 10472, 5);
    e->eye_scale = 103;
}

static void a_question(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 3600, 20);
    e->gaze_dy = (int16_t)(-14 + osc(t, 1800, 6));
}

static void a_look(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 5712, 54);             /* 大范围扫视 */
    e->gaze_dy = (int16_t)osc(t, 6981, 4);
}

static void a_glance(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 4200, 40);
    e->gaze_dy = 2;
}

static void a_pout(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = 18;
    e->gaze_dx = (int16_t)(-10 + osc(t, 7854, 5));
    e->blink = 15;
}

static void a_yawn(uint32_t t, face_expr_t *e)
{
    /* 6 秒一个哈欠：0~35% 张开、35~60% 保持、60~100% 收回 */
    uint32_t p = t % 6000u;
    int32_t k = 0;                      /* 0..100 */

    if (p < 2100u) {
        k = (int32_t)(p * 100u / 2100u);
    } else if (p < 3600u) {
        k = 100;
    } else if (p < 6000u) {
        k = (int32_t)((6000u - p) * 100u / 2400u);
    }
    e->gaze_dy = 6;
    e->blink = (uint8_t)(k * 230 / 100);
    e->mouth_open = (uint8_t)(k * 255 / 100);
}

static void a_wink(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 5236, 14);
    e->gaze_dy = -4;
}

static void a_cry(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = (int16_t)(16 + osc(t, 967, 4));        /* 抽泣的抖 */
    e->gaze_dx = (int16_t)osc(t, 2992, 6);
    e->blink = 15;
}

static void a_shock(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = -6;
    e->eye_scale = (int16_t)(100 + osc(t, 314, 2));
}

static void a_idea(uint32_t t, face_expr_t *e)
{
    int32_t b = pos_osc(t, 1653, 100);

    e->gaze_dy = -10;
    e->eye_scale = (int16_t)(100 + b * 7 / 100);
}

static void a_star_struck(uint32_t t, face_expr_t *e)
{
    int32_t b = pos_osc(t, 1496, 100);

    e->eye_scale = (int16_t)(100 + b * 8 / 100);
    e->gaze_dy = -4;
    e->mouth_open = (uint8_t)(76 + b * 127 / 100);
}

static void a_dizzy(uint32_t t, face_expr_t *e)
{
    /* JS: gx = cos, gy = sin —— 上一版把两者写反了，相位差 90° 完全对不上 */
    e->gaze_dx = (int16_t)osc(t + 604, 2417, 26);
    e->gaze_dy = (int16_t)osc(t, 2417, 16);
}

static void a_bored(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 17952, 30);            /* 极慢 */
    e->gaze_dy = (int16_t)(8 + osc(t, 10472, 4));
}

static void a_smug(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 6981, 12);
    e->gaze_dy = (int16_t)(-2 + osc(t, 4833, 3));
}

static void a_nervous(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)(osc(t, 885, 10) + osc(t, 2731, 6));
    e->gaze_dy = (int16_t)osc(t, 1185, 5);
}

static void a_sleepy(uint32_t t, face_expr_t *e)
{
    e->gaze_dy = (int16_t)(10 + osc(t, 10472, 5));
    e->eye_scale = (int16_t)(100 + osc(t, 5712, 1));
}

static void a_mischief(uint32_t t, face_expr_t *e)
{
    e->gaze_dx = (int16_t)osc(t, 5712, 20);
    e->gaze_dy = (int16_t)(2 + osc(t, 3696, 4));
}

/* ---------------------------------------------------------------- 表 */
static const face_act_t g_acts[] = {
    { "idle",        a_idle },
    { "happy",       a_happy },
    { "laugh",       a_laugh },
    { "shy",         a_shy },
    { "pet",         a_shy },
    { "sad",         a_sad },
    { "sad_anguish", a_sad_anguish },
    { "angry_low",   a_angry_low },
    { "angry_high",  a_angry_high },
    { "surprised",   a_surprised },
    { "loving",      a_loving },
    { "asleep",      a_asleep },
    { "think",       a_think },
    { "listen",      a_listen },
    { "question",    a_question },
    { "look",        a_look },
    { "glance",      a_glance },
    { "pout",        a_pout },
    { "yawn",        a_yawn },
    { "wink",        a_wink },
    { "cry",         a_cry },
    { "shock",       a_shock },
    { "idea",        a_idea },
    { "star_struck", a_star_struck },
    { "dizzy",       a_dizzy },
    { "bored",       a_bored },
    { "smug",        a_smug },
    { "nervous",     a_nervous },
    { "sleepy",      a_sleepy },
    { "mischief",    a_mischief }
};

#define ACT_COUNT ((int)(sizeof(g_acts) / sizeof(g_acts[0])))

int face_act_count(void)
{
    return ACT_COUNT;
}

const face_act_t *face_act_get(int idx)
{
    if (idx < 0 || idx >= ACT_COUNT) {
        return 0;
    }
    return &g_acts[idx];
}

static char act_lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

const face_act_t *face_act_find(const char *name)
{
    int i;

    if (name == 0) {
        return 0;
    }
    for (i = 0; i < ACT_COUNT; i++) {
        const char *a = g_acts[i].name;
        const char *b = name;
        while ((*a != '\0') && (*b != '\0') && (act_lc(*a) == act_lc(*b))) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return &g_acts[i];
        }
    }
    return 0;
}

void face_act_sample(const face_act_t *a, uint32_t t_ms, face_expr_t *out)
{
    if (out == 0) {
        return;
    }
    face_expr_reset(out);
    if (a != 0 && a->fn != 0) {
        a->fn(t_ms, out);
    }
}
