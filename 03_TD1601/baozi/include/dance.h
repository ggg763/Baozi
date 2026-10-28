// dance.h — 包子"跳舞"动作脚本（TD1601）
// ============================================================================
// 收到 CMD_DANCE(0x0A) 后按节拍表连续播放一串舵机动作+表情：
//   喊"跳舞" → ASRPRO 发 A5 00 0A AF → TD1601 → dance_start() → dance_poll()
//   每 10ms 推进一次，按 s_steps 表逐拍动作，跳完自动回中位+待机脸。
//
// 期间收到任何其他命令（情绪/舵机/复位）会自动打断舞蹈（baozi_app.c 里调 dance_stop）。
//
// 用法（main.c）：
//   dance_start();          // 收到跳舞命令时调（baozi_app.c 的 BAOZI_Dance 里）
//   while(1){ dance_poll(); } // 主循环里调，推进舞步
// ============================================================================
#ifndef _DANCE_H_
#define _DANCE_H_

#include <stdint.h>

/* 开始跳舞（从头播放节拍表） */
void dance_start(void);

/* 立即停止跳舞（被打断时用，舵机停在哪就是哪） */
void dance_stop(void);

/* 1 = 正在跳舞 */
int  dance_is_active(void);

/* 主循环里周期调用（约 10ms 一次）：推进舞步。
 * 不调的话舞蹈不会往前走。 */
void dance_poll(void);

#endif /* _DANCE_H_ */
