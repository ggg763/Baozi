/******************************************************************************
 * @file     mood_fsm.c
 * @brief    心情状态机实现（PC emotion/state_machine.py 的 C 移植）
 *
 * 迁移对照（PC → C）：
 *   last_interaction_time = time.time()        → s_last_ms（touch 时取当前时基）
 *   idle_seconds = time.time() - last_...      → idle = s_now_ms - s_last_ms
 *   if (idle > 20 && idle <= 30) → BORED       → 同（毫秒化：60000 / 180000，已放宽）
 *   elif (idle > 30) → SLEEPING（只触发一次）  → 同（靠 s_mood 判重）
 *   update_interaction() → ACTIVE               → mood_fsm_touch()
 *   wake_up()                                   → mood_fsm_wake()
 *
 * 时基来源：now_ms 由 emotion_fsm.c 用 csi_tick_get()（SysTick，1 tick = 1ms）
 * 喂入，是【真实毫秒】；主循环每 10ms 调一次，动作播放期间动作库的 tick 钩子
 * 也会把它刷成当前 tick。所以 idle 计时与实际墙钟一致，不会因为一拍多长而漂。
 *
 * ⚠️ 硬件动作不在这里做 —— PC 的 state_machine.py 内部直接调
 *    hardware.set_expression/set_led/set_servo；本工程把这些【统一收敛到
 *    emotion_fsm.c】按 (mood, emotion) 决定动作（见该文件顶部的架构图），
 *    状态机只负责"心情是什么、什么时候变"，保持单一职责、便于单独测试。
 ******************************************************************************/

#include <stdio.h>
#include "emotion/mood_fsm.h"

static const char *const s_mood_names[MOOD_MAX] = {
    "active",
    "idle",
    "bored",
    "sleeping",
};

static mood_id_t s_mood = MOOD_IDLE;
static uint32_t  s_now_ms = 0;          /* 时基（由 tick 推进）*/
static uint32_t  s_last_ms = 0;         /* 上次交互时刻 */
static uint32_t  s_last_check_ms = 0;   /* 上次心跳检查时刻 */

/* 空闲降级阈值（默认 60s/180s，可由 mood_fsm_set_thresholds 临时改）*/
static uint32_t  s_bored_ms = MOOD_BORED_MS;
static uint32_t  s_sleep_ms = MOOD_SLEEP_MS;

const char *mood_name(mood_id_t m)
{
    if ((int)m >= (int)MOOD_MAX) {
        return "?";
    }
    return s_mood_names[(int)m];
}

void mood_fsm_init(void)
{
    s_mood = MOOD_IDLE;     /* = PC：current_mood = BaoZiMood.IDLE */
    s_now_ms = 0;
    s_last_ms = 0;
    s_last_check_ms = 0;
    s_bored_ms = MOOD_BORED_MS;     /* 上电回到默认阈值（测试里 set 过的不要带过来）*/
    s_sleep_ms = MOOD_SLEEP_MS;
    printf("[mood] init: %s (bored>%ums, sleep>%ums)\r\n",
           mood_name(s_mood), s_bored_ms, s_sleep_ms);
}

void mood_fsm_touch(void)
{
    /* = PC update_interaction() 第 1 步：重置时间戳 + 打断无聊/睡眠 */
    s_last_ms = s_now_ms;
    s_last_check_ms = s_now_ms;
    s_mood = MOOD_ACTIVE;
}

mood_id_t mood_fsm_tick(uint32_t now_ms)
{
    uint32_t idle;

    s_now_ms = now_ms;

    /* ---- 心跳节拍：与 PC 的 time.sleep(5) 等价 ---- */
    if ((now_ms - s_last_check_ms) < MOOD_HEARTBEAT_MS) {
        return s_mood;
    }
    s_last_check_ms = now_ms;

    idle = mood_fsm_idle_ms();

    /* ---- 状态迁移（逐条对应 PC _heartbeat_loop）---- */
    if ((idle > s_bored_ms) && (idle <= s_sleep_ms)) {
        if (s_mood != MOOD_BORED) {
            s_mood = MOOD_BORED;        /* 1 分钟没理我，有点无聊了... */
        }
    } else if (idle > s_sleep_ms) {
        if (s_mood != MOOD_SLEEPING) {
            s_mood = MOOD_SLEEPING;     /* 3 分钟没理我，我要睡觉了... */
        }
    }

    return s_mood;
}

void mood_fsm_force(mood_id_t m)
{
    if ((int)m >= (int)MOOD_MAX) {
        return;
    }

    s_mood = m;

    if (m == MOOD_SLEEPING) {
        /* 显式睡觉：不重置 idle 计时（保持"确实很久没互动"的语义），
         * 但把心跳节拍推到现在，避免下一拍立刻又走一遍迁移判断。 */
        s_last_check_ms = s_now_ms;
        return;
    }

    /* 显式置位 ACTIVE / IDLE / BORED：按一次"设定"处理，从现在起重新计算空闲时间
     * （所以 EMO:BORED 之后会保持无聊 120s，然后按规则自动降级到 sleeping）。
     * SLEEPING 例外：不重置计时，保持"确实很久没互动"的语义。 */
    s_last_ms = s_now_ms;
    s_last_check_ms = s_now_ms;
}

void mood_fsm_wake(void)
{
    /* = PC wake_up()：只有睡着时才有"被唤醒"这回事，否则就是一次普通交互 */
    if (s_mood == MOOD_SLEEPING) {
        printf("[mood] wake up\r\n");
    }
    mood_fsm_touch();
}

mood_id_t mood_fsm_mood(void)
{
    return s_mood;
}

uint32_t mood_fsm_idle_ms(void)
{
    return s_now_ms - s_last_ms;
}

/* ==================== 阈值运行时调整（给测试用）==================== */

void mood_fsm_set_thresholds(uint32_t bored_ms, uint32_t sleep_ms)
{
    /* 传 0 表示该档保持/恢复默认 */
    s_bored_ms = (bored_ms != 0U) ? bored_ms : MOOD_BORED_MS;
    s_sleep_ms = (sleep_ms != 0U) ? sleep_ms : MOOD_SLEEP_MS;

    /* 睡觉阈值必须大于无聊阈值，否则 BORED 那档会被架空 */
    if (s_sleep_ms <= s_bored_ms) {
        s_sleep_ms = s_bored_ms + 1000U;
    }

    printf("[mood] thresholds: bored>%ums, sleep>%ums\r\n", s_bored_ms, s_sleep_ms);
}

void mood_fsm_reset_thresholds(void)
{
    mood_fsm_set_thresholds(MOOD_BORED_MS, MOOD_SLEEP_MS);
}

uint32_t mood_fsm_bored_ms(void)
{
    return s_bored_ms;
}

uint32_t mood_fsm_sleep_ms(void)
{
    return s_sleep_ms;
}
