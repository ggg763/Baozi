// baozi_hooks.h — 协议层与"包子"功能层的接口（TD1601 版）
// ============================================================================
// PC 仿真版里 uart_protocol.c 直接调 emotion_fsm / personality / script_engine。
// 这些模块还没移植进 SDK，所以在板上改用这套钩子函数隔开：
//   uart_protocol.c 只认钩子 → 谁实现钩子，谁就是功能层。
// 以后情感系统移植完，把 baozi_app.c 里的实现换成真函数即可，协议层不动。
// ============================================================================
#ifndef _BAOZI_HOOKS_H_
#define _BAOZI_HOOKS_H_

#include <stdint.h>

// ---- 命令钩子（uart_protocol.c 的 UART_HandleFrame 调用）----

// 设置情绪（0x00-0x05，对应 uart_protocol.h 的 EMOTION_xxx）
void BAOZI_SetEmotion(uint8_t emo);

// 发送事件给状态机（0x01-0x06，对应 EVT_CODE_xxx）
void BAOZI_SendEvent(uint8_t evt);

// 切换性格。返回 1=成功，0=没找到（协议层会回 ERR_PERSON_NOT_FOUND）
int  BAOZI_SwitchPerson(const char *name);

// 播放脚本。返回 1=成功，0=没找到（协议层会回 ERR_SCRIPT_NOT_FOUND）
int  BAOZI_PlayScript(const char *name);

// 停止脚本
void BAOZI_StopScript(void);

// 设置舵机（id + 角度）
void BAOZI_SetServo(uint8_t id, uint8_t angle);

// 复位（回到待机）
void BAOZI_Reset(void);

// 跳舞（播放内置舞蹈节拍表）
void BAOZI_Dance(void);

// 心跳：时间推进 seconds 秒
void BAOZI_Tick(uint32_t seconds);

// ---- v1.1 习惯看板钩子（实现见 src/habit_store.c 的调用方 baozi_proto_app.c）----

// 对时：mon 1-12 / day 1-31 / weekday 1-7(周一=1) / hh 0-23 / mm 0-59
// ss 0-59；传 0xFF = 旧格式没带秒（保持板子当前秒不动）
void BAOZI_SyncTime(uint8_t mon, uint8_t day, uint8_t weekday,
                    uint8_t hh, uint8_t mm, uint8_t ss);

// 习惯列表开始：清空旧缓存，准备收 count 条
void BAOZI_HabitBegin(uint8_t count);

// 下发一个习惯（data/len 即 CMD_HABIT_ITEM 的 DATA）
// 返回 0=成功，非 0=失败（协议层据此回错误码）
int  BAOZI_HabitItem(const uint8_t *data, uint8_t len);

// 习惯列表结束：应用并重绘 HABIT 卡片（本函数内回 RSP_HABIT_SYNCED）
void BAOZI_HabitEnd(uint8_t count);

// 用户在屏幕上点了某一格习惯：上报 PC + 本地乐观翻转
// （由 UI 层调用，免得 ui_app.c 直接依赖协议层）
void BAOZI_ReportHabitTap(uint8_t idx, uint8_t slot, uint8_t action);

// 用户在"还没数据的空卡片"上点了一下：请 PC 立刻把习惯重发一遍
// （板子比 PC 先开机、或 PC 端脚本重启过时用得上；见 uart_protocol.h 的 RSP_HABIT_REQ）
void BAOZI_RequestHabits(void);

// ---- v1.5 TODAY 页日程钩子（实现见 baozi_proto_app.c，数据在 sched_store）----

// 日程列表开始：标记接收中（不清旧数据，END 时整体替换）
void BAOZI_SchedBegin(uint8_t count);

// 下发一条日程（data/len 即 CMD_SCHED_ITEM 的 DATA）
// 返回 0=成功，非 0=失败（协议层据此回错误码）
int  BAOZI_SchedItem(const uint8_t *data, uint8_t len);

// 日程列表结束：整体替换生效 + 请求重绘 TODAY 卡片（本函数内回 0x91）
void BAOZI_SchedEnd(uint8_t count);

// 用户在 TODAY 页点了某条日程：上报 PC + 本地乐观翻转
void BAOZI_ReportSchedTap(uint8_t idx, uint8_t action);

// 用户在空 TODAY 卡上点了一下：请 PC 立刻把日程重发一遍（RSP_SCHED_REQ 0x94）
void BAOZI_RequestSched(void);

// ---- v1.8 提醒弹卡钩子（时机判定在 PC 端，板子只负责展示）----

// PC 判定"该提醒了"（习惯到点 / 日程将开始）：弹"即将开始"卡片。
// bmp/chars/w/h 为 16px 标题点阵（chars 可为 0 = 只显示时间）。
void BAOZI_Remind(uint8_t type, uint8_t hh, uint8_t mm,
                  const uint8_t *bmp, uint8_t chars, uint8_t w, uint8_t h);

// 用户点掉了（reason=0）或 60 秒超时（reason=1）提醒卡片：上报 PC（0x95）
// （由 UI 层调用，免得 ui_app.c 直接依赖协议层）
void BAOZI_ReportRemindClosed(uint8_t type, uint8_t reason);

// ---- 状态查询钩子（UART_SendStatus 上报用）----

uint8_t  BAOZI_GetEmotion(void);     // 当前情绪 0x00-0x05
uint8_t  BAOZI_GetEnergy(void);      // 能量 0-100
uint16_t BAOZI_GetIdleTime(void);    // 无互动秒数
uint16_t BAOZI_GetEmotionTime(void); // 当前情绪持续秒数

#endif
