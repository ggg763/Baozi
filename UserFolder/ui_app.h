/******************************************************************************
 * @file     ui_app.h
 * @brief    包子屏幕 UI 状态机（表情态 ↔ 卡片层）
 *
 * 手势语义：
 *   长按（按住 >800ms 未滑动）在表情态↔卡片层之间切换
 *   卡片层内：左右滑切换三张卡片（今日日程 / 番茄钟 / 习惯）
 ******************************************************************************/

#ifndef _UI_APP_H_
#define _UI_APP_H_

#include <stdint.h>

void ui_app_init(void);                                   /* 开机：显示表情态 */
void ui_app_handle_gesture(int g, uint16_t x, uint16_t y);/* 喂入触摸手势 */
void ui_app_tick(uint32_t dt_ms);                         /* 每 20ms 调一次 */
void ui_app_force_emotion(const char *emo);               /* 串口/上层要求表情（可抢屏）*/

/* 只更新"表情态该显示什么"，不抢屏（emotion_fsm 的 face_ui_* 走这里）：
 * 当前就在表情态 → 立刻换脸；在卡片层/番茄钟里 → 只记住，回表情态时再显示。
 * emo 必须传常量串（face_model_find 按名字查模型，指针要长期有效）。 */
void ui_app_set_emotion(const char *emo);

/* ui_app_init() 是否已跑过（face_ui 桥接做惰性初始化用）*/
int  ui_app_ready(void);

/* 把屏幕拉到第 5 页 NET（配网二维码）。
 * 由 src/net_link.c 在"热点已开、码已就绪"时调用 —— 用户不必自己长按进卡片层再滑过去。
 * 番茄钟计时中 / 提醒弹卡中 / 手指按着时**不抢屏**（直接返回）。 */
void ui_app_show_net(void);

void ui_cards_show(int page);                             /* 重绘卡片页（pomo 退出时回调）*/

/* 数据变了想刷新卡片层：只在"当前正好停在卡片层"时重绘，
 * 否则会把正在看表情/番茄钟的用户拽走。习惯下发完成后由协议层调用。 */
void ui_app_refresh_cards(void);

/* 数据落板后的"脏"标记（习惯/日程 END 时调用）：不在 UART 帧流里直接刷屏，
 * 由 ui_app_tick 等串口静默 150ms 后补刷 —— 直接刷是实机花屏的高发窗口。 */
void ui_app_cards_dirty(void);

/* 提醒弹卡（v1.8，PC 端 0x17 下发）：全屏"即将开始"卡片，
 * 点一下或 60 秒自动关闭并回到原界面。番茄钟计时中先扣下、结束再弹。
 * bmp 为 16px 标题点阵（chars=0 表示只显示时间）。 */
void ui_app_show_remind(uint8_t type, uint8_t hh, uint8_t mm,
                        const uint8_t *bmp, uint8_t chars, uint8_t w, uint8_t h);

#endif /* _UI_APP_H_ */
