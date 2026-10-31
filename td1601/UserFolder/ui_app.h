/******************************************************************************
 * @file     ui_app.h
 * @brief    包子屏幕 UI 状态机（表情态 ↔ 卡片层）
 *
 * 手势语义：
 *   表情态：**同时按住两只眼睛**约 0.55s 进卡片层（两只手各压一只眼，误触近零；
 *           单点触摸面板会自动退化成"单指长按任一只眼睛"）
 *           脸颊区（眼心连线以下、未绘制区）左右来回滑 = 被抚摸 -> loving
 *           额头区（连线以上、未绘制区）连点三下 = 惹恼 -> angry
 *   卡片层：左右滑切七页（今日日程 / 计时 / 习惯 / 动作 / 表情 / 配网 / 演示）
 *           上下滑滚动列表（今日日程页、习惯页、表情页）
 *           点右上角 X 或从屏幕顶部往下拖 = 立刻退出卡片层
 *           （2026-10-31 起取消了"15 秒无操作自动退出"：看列表时会被自己收走，
 *             而且回落时手指若还压在眼睛上会立刻又被拉回卡片层，屏幕来回闪）
 *           串口/ESP32 的情绪命令在卡片层里只记录、不抢屏，退出后再显示
 ******************************************************************************/

#ifndef _UI_APP_H_
#define _UI_APP_H_

#include <stdint.h>

void ui_app_init(void);                                   /* 开机：显示表情态 */
void ui_app_handle_gesture(int g, uint16_t x, uint16_t y);/* 喂入触摸手势 */
void ui_app_tick(uint32_t dt_ms);                         /* 每 20ms 调一次 */
void ui_app_force_emotion(const char *emo);               /* 串口/上层要求表情（表情态下立即生效）*/

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

/* 卡片页公共骨架：渐变底 + 顶部时钟 + 页码圆点 + 右上角关闭按钮 + 圆角卡片框。
 * 单独导出是因为 pomo 的 MODE 页（第 2 页）也要走同一套骨架 —— 它原先自己画
 * 一套（3 个圆点、没有时钟、没有卡片框），整页看起来像另一个 App。 */
void ui_cards_chrome(int page);

/* 只画顶栏：当前时间 + 六个页码圆点（with_close=1 时再加关闭按钮），不清屏。
 * 番茄钟的深层子页用它 —— 那些页的内容是整屏的，套不进圆角卡片框，
 * 但"第 2 页"这个身份必须和其余页一样（6 个圆点，不是旧的 3 个）。
 * with_close 传 0：深层子页手势锁死，退出走各自的 EXIT / GIVE UP。 */
void ui_cards_header(int page, int with_close);

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
