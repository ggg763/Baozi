/******************************************************************************
 * @file     pomo.h
 * @brief    计时功能模块（卡片层第 2 页）：番茄钟 / 正计时 / 倒计时
 *
 * MODE（三入口，即卡片层第 2 页）
 *   ├─ POMODORO：选个数(1-10) → FOCUS 25min → REST 5min 循环 → ALL DONE
 *   │            运行中 GIVE UP → 确认页（YES 放弃 / NO 继续）
 *   ├─ COUNT UP ：正计时递增，STOP 直接返回
 *   └─ COUNTDOWN：选时长(1/3/5/10/15/20/30/60 分钟) → 倒计时 → TIME UP
 * 深层界面（非 MODE）锁定：滑动/长按/15s 回落全部失效，只响应点击。
 ******************************************************************************/

#ifndef _POMO_H_
#define _POMO_H_

#include <stdint.h>

void pomo_reset(void);              /* 彻底退出计时流程 */
int  pomo_active(void);             /* 是否处于计时流程中（含 MODE 页）*/
int  pomo_at_mode(void);            /* 是否在三入口页（此时卡片层行为有效）*/
void pomo_enter_mode(void);         /* 渲染三入口页（卡片层第 2 页调用）*/
void pomo_on_tap(int x, int y);     /* 点击处理（区域判定）*/
void pomo_tick(uint32_t dt_ms);     /* 每 20ms 调一次：计时推进 */

#endif /* _POMO_H_ */
