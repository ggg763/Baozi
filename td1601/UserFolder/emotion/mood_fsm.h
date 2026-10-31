/******************************************************************************
 * @file     mood_fsm.h
 * @brief    心情状态机（TD1601 行为层）—— PC 端 emotion/state_machine.py 的 C 移植
 *
 * 对齐 PC 架构：PC 里 MoodStateMachine 用【后台线程 + time.sleep(5)】做心跳，
 * 每 5s 检查"多久没人理我"，据此在 ACTIVE / IDLE / BORED / SLEEPING 之间迁移。
 * 本工程是裸机（KERNEL_NONE、无 RTOS），因此改成【非阻塞 tick】：
 *
 *     PC  : threading.Thread(target=_heartbeat_loop) 每 5s 跑一次
 *     C   : mood_fsm_tick(now_ms) 由状态机主循环每 10ms 调一次，
 *           内部自己按 MOOD_HEARTBEAT_MS(5s) 累加节拍 —— 判断逻辑与 PC 一致。
 *
 * 时间阈值（★ 2026-11 按用户要求从 PC 的 20s/30s 放宽）：
 *     0 ~ 60s      : ACTIVE   （刚互动完，精神好）
 *     60s ~ 180s   : BORED    （等久了，无聊）
 *     > 180s       : SLEEPING （3 分钟没人理才睡）
 * 起始态是 IDLE（= PC 构造时 current_mood = BaoZiMood.IDLE + 上电待机表情）。
 *
 * ⚠️ 时基必须是【真实毫秒】。emotion_fsm.c 用 csi_tick_get()（SysTick）喂 now_ms，
 *    历史上这里喂的是"主循环每拍自己 +10ms"的虚拟时基，与实际节拍（刷屏/动作
 *    阻塞播放时一拍 5~20ms 不等）对不上，30s 的阈值在实机上会漂成 20~60s。
 ******************************************************************************/
#ifndef _MOOD_FSM_H_
#define _MOOD_FSM_H_

#include <stdint.h>

/* 心情状态（= PC BaoZiMood，顺序一致）*/
typedef enum {
    MOOD_ACTIVE = 0,    /* 刚互动完，精神好 */
    MOOD_IDLE,          /* 启动初始态 / 待机基准态 */
    MOOD_BORED,         /* 等久了，无聊 */
    MOOD_SLEEPING,      /* 彻底待机 */
    MOOD_MAX
} mood_id_t;

/* ---- 阈值：本机放宽版（PC 是 20s/30s，太短；运行时可临时改，见 set_thresholds）---- */
#define MOOD_BORED_MS       60000U      /* > 60s 没互动 → 无聊 */
#define MOOD_SLEEP_MS      180000U      /* > 180s（3min）没互动 → 睡觉 */
#define MOOD_HEARTBEAT_MS    5000U      /* 心跳节拍（PC 的 time.sleep(5)）*/

const char *mood_name(mood_id_t m);

/* 初始化：current = IDLE，last_interaction = 0（now_ms 基准）*/
void mood_fsm_init(void);

/* 记录一次交互（= PC update_interaction 里的 last_interaction_time 重置）：
 * 打断无聊/睡眠，回到 ACTIVE。串口命令、触摸、IMU 敲击都该调它。 */
void mood_fsm_touch(void);

/* 推进状态机（非阻塞）：由主循环按 10ms 调一次；now_ms 是自增的毫秒计时。
 * 返回当前心情。内部按 MOOD_HEARTBEAT_MS 节拍做降级判断（与 PC 同频）。 */
mood_id_t mood_fsm_tick(uint32_t now_ms);

/* 直接置位（PC 协议 EMO:BORED / EMO:SLEEPING 这类显式心情下发）*/
void mood_fsm_force(mood_id_t m);

/* 唤醒（= PC wake_up()）：睡眠中调用则回到 ACTIVE 并重置计时；否则等同 touch() */
void mood_fsm_wake(void);

mood_id_t mood_fsm_mood(void);

/* 距上次交互的毫秒数 */
uint32_t mood_fsm_idle_ms(void);

/* ==================== 阈值运行时调整（给测试用）====================
 * 正式运行时阈值固定为 60s/180s；但上机测试不想干等 3 分钟，
 * 所以开放一个 setter：mood_fsm_set_thresholds(3000, 6000) 就能在几秒内
 * 看到 ACTIVE → BORED → SLEEPING 的完整降级链路。传 0 表示该档恢复默认。 */
void     mood_fsm_set_thresholds(uint32_t bored_ms, uint32_t sleep_ms);
void     mood_fsm_reset_thresholds(void);
uint32_t mood_fsm_bored_ms(void);
uint32_t mood_fsm_sleep_ms(void);

#endif /* _MOOD_FSM_H_ */
