/******************************************************************************
 * @file     emotion_manager.c
 * @brief    情绪管理实现（PC emotion/emotion_manager.py 的 C 移植）
 *
 * 连续状态映射表与 PC 端 _update_state() 的 mapping【逐项一致】（×1000 取整）：
 *
 *     情绪        happiness  energy  curiosity
 *     happy          900      800      700
 *     sad            200      300      400
 *     angry          200      900      300
 *     surprised      600     1000      900
 *     loving         800      600      700
 *     neutral        500      500      500
 ******************************************************************************/

#include <string.h>
#include "emotion/emotion_manager.h"

/* 情绪名表（顺序与 emo_id_t 严格一致）*/
static const char *const s_emo_names[EMO_MAX] = {
    "neutral",
    "happy",
    "sad",
    "angry",
    "surprised",
    "loving",
};

/* 连续状态映射表（顺序与 emo_id_t 严格一致；= PC _update_state 的 mapping）*/
static const emo_state_t s_emo_states[EMO_MAX] = {
    { 500,  500,  500 },    /* neutral   */
    { 900,  800,  700 },    /* happy     */
    { 200,  300,  400 },    /* sad       */
    { 200,  900,  300 },    /* angry     */
    { 600, 1000,  900 },    /* surprised */
    { 800,  600,  700 },    /* loving    */
};

/* 当前情绪 + 当前连续状态 */
static emo_id_t   s_current = EMO_NEUTRAL;
static emo_state_t s_state   = { 500, 500, 500 };

/* 前置声明：emo_force 在 emo_reset 之后就要用到，而实现在下面 */
static int16_t clamp1000(int16_t v);

const char *emo_name(emo_id_t e)
{
    if ((int)e >= (int)EMO_MAX) {
        return "?";
    }
    return s_emo_names[(int)e];
}

/* 大小写不敏感比较（不用 strcasecmp：minilibc 里不保证有）*/
static int name_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0')) {
        char ca = *a;
        char cb = *b;

        if ((ca >= 'A') && (ca <= 'Z')) { ca = (char)(ca - 'A' + 'a'); }
        if ((cb >= 'A') && (cb <= 'Z')) { cb = (char)(cb - 'A' + 'a'); }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return ((*a == '\0') && (*b == '\0')) ? 1 : 0;
}

emo_id_t emo_parse(const char *name)
{
    int i;

    if (name == NULL) {
        return EMO_MAX;
    }

    for (i = 0; i < (int)EMO_MAX; i++) {
        if (name_eq(name, s_emo_names[i]) != 0) {
            return (emo_id_t)i;
        }
    }

    /* 兼容 PC 协议里 neutral 的别名 "IDLE"（见 communication.py EMOTION_MAP）*/
    if (name_eq(name, "idle") != 0) {
        return EMO_NEUTRAL;
    }

    return EMO_MAX;     /* 不认识（调用方按 neutral 处理，= PC normalize()）*/
}

void emo_apply(emo_id_t e)
{
    if ((int)e >= (int)EMO_MAX) {
        e = EMO_NEUTRAL;                    /* = PC：非法值归一化为 neutral */
    }

    s_current = e;
    s_state   = s_emo_states[(int)e];       /* = PC _update_state() */
}

emo_id_t emo_current(void)
{
    return s_current;
}

const char *emo_current_name(void)
{
    return emo_name(s_current);
}

emo_state_t emo_state(void)
{
    return s_state;
}

void emo_reset(void)
{
    s_current = EMO_NEUTRAL;
    s_state   = s_emo_states[(int)EMO_NEUTRAL];
}

/* 直接设定情绪 + 连续状态（脸部手势分级用，见头文件说明）。
 * 注意这里【不查表】：调用方给的强度就是要生效的强度。 */
void emo_force(emo_id_t e, int16_t happiness, int16_t energy, int16_t curiosity)
{
    if ((int)e >= (int)EMO_MAX) {
        e = EMO_NEUTRAL;
    }
    s_current = e;
    s_state.happiness = clamp1000(happiness);
    s_state.energy    = clamp1000(energy);
    s_state.curiosity = clamp1000(curiosity);
}

/* ==================== 刺激 → 连续状态 → 漂移 ====================
 *
 * 只动连续状态（千分制），情绪只在跨阈值时才漂移；漂移时**不调 emo_apply**
 * （那会用查表值把刚攒的增量覆盖掉）。 */
static int16_t clamp1000(int16_t v)
{
    if (v < 0) { return 0; }
    if (v > EMO_SCALE) { return (int16_t)EMO_SCALE; }
    return v;
}

emo_id_t emo_stimulus(uint8_t kind, uint8_t amount)
{
    int a = (amount > 100) ? 100 : (int)amount;
    int dh = 0, de = 0, dc = 0;

    if (a <= 0) {
        return s_current;
    }

    /* 每种刺激的"满强度增量"（千分制）*/
    switch (kind) {
    case EMO_STIM_TOUCH:  dh =  12; de =   0; dc =   6; break;
    case EMO_STIM_PET:    dh =  45; de =  -8; dc =  10; break;
    case EMO_STIM_PRAISE: dh =  70; de =  30; dc =  10; break;
    case EMO_STIM_SCOLD:  dh = -120; de =  25; dc = -20; break;
    case EMO_STIM_PLAY:   dh =  30; de = -60; dc =  85; break;
    default:              return s_current;
    }

    s_state.happiness = clamp1000((int16_t)(s_state.happiness + (int16_t)(dh * a / 100)));
    s_state.energy    = clamp1000((int16_t)(s_state.energy    + (int16_t)(de * a / 100)));
    s_state.curiosity = clamp1000((int16_t)(s_state.curiosity + (int16_t)(dc * a / 100)));

    /* 漂移（只改"标签"，不动连续状态）*/
    if (s_state.happiness <= 250) {
        s_current = EMO_SAD;
    } else if ((kind == EMO_STIM_PET) && (s_state.happiness >= 760)) {
        s_current = EMO_LOVING;         /* 被摸够了 → 撒娇脸 */
    } else if (s_state.happiness >= 820) {
        s_current = EMO_HAPPY;
    } else if ((kind == EMO_STIM_SCOLD) && (s_state.happiness < 450)) {
        s_current = EMO_ANGRY;
    } else if (s_current == EMO_SAD && s_state.happiness >= 500) {
        s_current = EMO_NEUTRAL;        /* 缓过来了 */
    }

    return s_current;
}
