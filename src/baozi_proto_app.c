// baozi_proto_app.c — 协议钩子的 TD1601 实现（baozi_LCD 版）
// ============================================================================
// 实现 baozi_hooks.h 里的钩子，把串口命令接到本工程的功能层：
//   情绪 → ui_app_force_emotion()（回到表情态并变脸，参数化脸 7 种名字）
//   舵机 → 【2026-10-17 起停用，见下】
//   复位 → 回待机脸 + （舵机回中位已停用）
//
// ⚠️ 舵机归属变更（2026-10-17）：
//   本工程的 servo.c 是 PA0/PA1 方案，但这两个引脚 csi_ch 都是 0、共享 load
//   寄存器 → 两路波形必然同步，做不到独立两轴。现改为由第三位同学的
//   servo_core（PA0 + PA27，见 baozi_src/UserFolder/servo_test/servo_pins.h）
//   统一驱动。所以本文件里 servo_move()/servo_center_all() 的调用**改为空操作**，
//   只保留日志。等 servo_core 移植进来后，把下面两处换成 servo_core 的接口即可。
//   旧文件已备份在 _bak_servo_20261017/。
//
// 与旧工程 baozi_app.c 的区别（重要，不能直接拷）：
//   旧版调 LCD_ShowEmotion(uint8_t)        —— 旧 emotions.c 位图表情体系
//   新版调 face_app_set_emotion(const char*) —— 参数化脸，按名字选模型
//   中间隔一层 ui_app_force_emotion()，保证"串口来的情绪"能正确落到屏幕上
//   （不管此刻在表情态还是卡片层）。
//
// 尚未实现（接口保留，先只打日志）：
//   性格切换 / 脚本播放 / 跳舞 —— 对应模块还没移植进本工程
//   （旧工程有 dance.c，但它调旧表情接口，需要时再移入并改写）
// ============================================================================
#include <stdio.h>
#include <stdint.h>
#include "uart_protocol.h"
#include "baozi_hooks.h"
/* #include "servo.h"   —— 舵机已移交 servo_core，本工程不再直接依赖 */
#include "ui_app.h"
#include "habit_store.h"
#include "sched_store.h"
#include "emotion/emotion_fsm.h"    /* emotion_fsm_command_emotion：情绪层同步 */

/* 情绪编号 → 参数化脸的名字。
 * face_model.c 里只有 7 个模型：neutral / happy / sad / angry / surprised /
 * loving / asleep（用 face_model_find(name) 精确匹配）。 */
static const char *k_emo_name[7] = {
    "neutral",    /* 0x00 EMOTION_IDLE      */
    "happy",      /* 0x01 EMOTION_HAPPY     */
    "sad",        /* 0x02 EMOTION_SAD       */
    "surprised",  /* 0x03 EMOTION_SURPRISED */
    "asleep",     /* 0x04 EMOTION_SLEEPY    */
    "neutral",    /* 0x05 EMOTION_THOUGHTFUL（暂无对应模型，退回 neutral）*/
    "angry"       /* 0x06 EMOTION_ANGRY     */
};

/* 同一批编号 → 【情绪层】的名字（emotion_fsm_command_emotion 的入参）。
 * 和 k_emo_name 只差一项：SLEEPY 在情绪层是"心情"（mood = sleeping），
 * 而在屏幕上是姿势名 asleep。喂错了的后果是：心情层下一拍用
 * face_ui_mood() 把屏幕重新拉回"醒着的脸"。 */
static const char *k_emo_state[7] = {
    "neutral",    /* 0x00 */
    "happy",      /* 0x01 */
    "sad",        /* 0x02 */
    "surprised",  /* 0x03 */
    "sleeping",   /* 0x04 */
    "neutral",    /* 0x05 */
    "angry"       /* 0x06 */
};

void BAOZI_SetEmotion(uint8_t emo)
{
    if (emo > EMOTION_ANGRY) {
        return;   /* 协议层已挡一次，这里再兜一层 */
    }
    printf("[BaoZi] emotion -> 0x%02X (%s)\r\n", emo, k_emo_name[emo]);

    /* ★ 两件事，顺序不能省（2026-11 实机问题）：
     *   ① emotion_fsm_command_emotion() 改【情绪层】—— 情绪/心情状态机跟着变；
     *   ② ui_app_force_emotion()       改【屏幕】—— 无论当前在卡片层还是表情态，
     *      都立刻回到表情态并把这张脸画上去。
     * 原来只有 ②，屏幕是变了、情绪层没变，而 emotion_fsm_step() 每拍都用
     * emo_current_name() 重新同步屏幕 —— 于是 PC/ESP32 发来的表情只闪一下就被
     * 拉回原来的情绪（触摸手势用 emo_force 定过的 loving/angry 最明显）。 */
    emotion_fsm_command_emotion(k_emo_state[emo]);
    ui_app_force_emotion(k_emo_name[emo]);
}

void BAOZI_SendEvent(uint8_t evt)
{
    /* 情绪状态机（emotion_fsm）未移植，事件先只打日志 */
    printf("[BaoZi] event 0x%02X (FSM 未移植，暂不处理)\r\n", evt);
}

int BAOZI_SwitchPerson(const char *name)
{
    printf("[BaoZi] switch person '%s' (未实现)\r\n", name);
    return 0;   /* 0 = 未找到，协议层会回 ERR_PERSON_NOT_FOUND */
}

int BAOZI_PlayScript(const char *name)
{
    printf("[BaoZi] play script '%s' (未实现)\r\n", name);
    return 0;   /* 0 = 未找到，协议层会回 ERR_SCRIPT_NOT_FOUND */
}

void BAOZI_StopScript(void)
{
    /* 脚本引擎未移植，暂空 */
}

void BAOZI_SetServo(uint8_t id, uint8_t angle)
{
    /* 舵机已移交 servo_core（PA0 + PA27）。移植进来后这里换成：
     *   servo_core 的"目标角"接口（具体函数名见 servo_core.h）
     * 目前只打日志，保证协议层不报错。 */
    printf("[BaoZi] servo id=%u angle=%u (servo disabled, owned by servo_core)\r\n",
           id, angle);
}

void BAOZI_Reset(void)
{
    printf("[BaoZi] reset\r\n");
    /* 复位同样要动情绪层：只把脸画成 neutral 的话，下一拍就会被情绪同步拉回去
     * （理由见 BAOZI_SetEmotion）。 */
    emotion_fsm_command_emotion("neutral");
    ui_app_force_emotion("neutral");   /* 回到待机脸 */
    /* 舵机回中位：已移交 servo_core，接进来后在这里调它的 center 接口 */
}

void BAOZI_Dance(void)
{
    /* dance.c 未移植进本工程（旧版调的是旧表情接口），先只打日志 */
    printf("[BaoZi] dance (dance.c 未移植)\r\n");
}

void BAOZI_Tick(uint32_t seconds)
{
    (void)seconds;   /* 状态机未移植，时间先不推 */
}

/* ---- 状态查询（上报用，先给默认值）---- */
uint8_t  BAOZI_GetEmotion(void)     { return EMOTION_IDLE; }
uint8_t  BAOZI_GetEnergy(void)      { return 100; }
uint16_t BAOZI_GetIdleTime(void)    { return 0; }
uint16_t BAOZI_GetEmotionTime(void) { return 0; }

/* ============================================================================
 * v1.1 习惯看板钩子
 * ----------------------------------------------------------------------------
 * 对时后刻意**不主动重绘**：实现在卡片层时，最多少显示 1 分钟的旧时间，
 * 而每次对时都刷屏会在卡片层闪一下。PC 端按分钟级对时即可。
 * ==========================================================================*/

void BAOZI_SyncTime(uint8_t mon, uint8_t day, uint8_t weekday,
                    uint8_t hh, uint8_t mm, uint8_t ss)
{
    habit_clock_set(mon, day, weekday, hh, mm, ss);
}

void BAOZI_HabitBegin(uint8_t count)
{
    if (count > HABIT_MAX_COUNT) {
        printf("[BaoZi] habit begin: %u > max %u, extras will be rejected\r\n",
               count, (unsigned)HABIT_MAX_COUNT);
    }
    habit_store_reset();
}

int BAOZI_HabitItem(const uint8_t *data, uint8_t len)
{
    int rc = habit_store_put(data, len);
    if (rc != 0) {
        printf("[BaoZi] habit item rejected (rc=%d, len=%u)\r\n", rc, len);
    }
    return rc;
}

void BAOZI_HabitEnd(uint8_t count)
{
    int n;

    habit_store_commit();
    n = habit_store_count();
    printf("[BaoZi] habit end: declared=%u stored=%d\r\n", count, n);

    /* 只标脏、不在 UART 帧流里直接刷屏：整批 0x11~0x13 刚发完，
     * 此时立刻整屏刷新（100ms+）会跟串口中断挤在一起，
     * 实机上表现为花屏概率飙升。ui_app_tick 会在串口静默 150ms 后补刷。 */
    ui_app_cards_dirty();
    UART_SendHabitSynced((uint8_t)n);
}

void BAOZI_ReportHabitTap(uint8_t idx, uint8_t slot, uint8_t action)
{
    printf("[BaoZi] habit tap idx=%u slot=%u action=%u\r\n", idx, slot, action);
    UART_SendHabitCheckin(idx, slot, action);          /* 上报 PC → 桥接脚本写后端 */
    habit_store_apply_local((int)idx, (int)slot, (int)action);  /* 屏幕立刻翻转 */
}

void BAOZI_RequestHabits(void)
{
    printf("[BaoZi] habit sync requested (cache empty, asking PC)\r\n");
    UART_SendHabitReq();
}

/* ============================================================================
 * v1.5 TODAY 页日程钩子（数据层在 sched_store.c）
 * ==========================================================================*/

void BAOZI_SchedBegin(uint8_t count)
{
    if (count > SCHED_MAX_ITEMS) {
        printf("[BaoZi] sched begin: %u > max %u, extras will be rejected\r\n",
               count, (unsigned)SCHED_MAX_ITEMS);
    }
    sched_store_reset();
}

int BAOZI_SchedItem(const uint8_t *data, uint8_t len)
{
    int rc = sched_store_put(data, len);
    if (rc != 0) {
        printf("[BaoZi] sched item rejected (rc=%d, len=%u)\r\n", rc, len);
    }
    return rc;
}

void BAOZI_SchedEnd(uint8_t count)
{
    int n;

    sched_store_commit();
    n = sched_store_count();
    printf("[BaoZi] sched end: declared=%u stored=%d\r\n", count, n);

    ui_app_cards_dirty();      /* 串口静默后由 tick 补刷（同 habit end）*/
    UART_SendSchedSynced((uint8_t)n);
}

void BAOZI_ReportSchedTap(uint8_t idx, uint8_t action)
{
    printf("[BaoZi] sched tap idx=%u action=%u\r\n", idx, action);
    UART_SendSchedCheck(idx, action);
    sched_store_apply_local((int)idx, (int)action);
}

void BAOZI_RequestSched(void)
{
    printf("[BaoZi] sched sync requested (cache empty, asking PC)\r\n");
    UART_SendSchedReq();
}

/* ============================================================================
 * v1.8 提醒弹卡钩子（时机判定在 PC 端：习惯到点 / 日程提前 10 分钟）
 * 声音由 PC 的 EdgeTTS 出（板子无喇叭）；板子只负责把卡片弹上屏。
 * ==========================================================================*/

void BAOZI_Remind(uint8_t type, uint8_t hh, uint8_t mm,
                  const uint8_t *bmp, uint8_t chars, uint8_t w, uint8_t h)
{
    printf("[BaoZi] remind type=%u %02u:%02u chars=%u\r\n", type, hh, mm, chars);
    ui_app_show_remind(type, hh, mm, bmp, chars, w, h);
}

void BAOZI_ReportRemindClosed(uint8_t type, uint8_t reason)
{
    printf("[BaoZi] remind closed type=%u reason=%u\r\n", type, reason);
    UART_SendRemindClosed(type, reason);
}
