/******************************************************************************
 * @file     habit_store.h
 * @brief    包子屏幕「习惯看板」：设备端缓存 + 时钟 + HABIT 卡片渲染 + 点击命中
 *
 * 职责边界（重要）：
 *   本模块 **不做任何打卡判定**。谁是 done / pending / missed，全部由 PC 端
 *   下发（源头是小程序/后端）。设备端只做三件事：
 *     1. 把 PC 发来的一批习惯缓存起来
 *     2. 按缓存画出来，并把用户点的格子编码成一条上报帧
 *     3. 本地"乐观"翻转那一格，让屏幕立刻有反馈（真值等 PC 下次下发覆盖）
 *
 * 为什么不做天数本地加减：用户的铁律是"不要假天数"。连续天数只会由用户
 * 真正确认产生，设备端点一下只是**上报**，加不加由后端算。所以这里的
 * apply_local() 只动 mask/done/state，绝不碰 streak。
 *
 * 数据流：
 *   PC ──0x11 BEGIN──► habit_store_reset()
 *      ──0x12 ITEM ×N─► habit_store_put()
 *      ──0x13 END─────► habit_store_commit() → 重绘 + 回 0x91
 *      ──0x10 对时────► habit_clock_set()
 *   用户点格子 ──► habit_card_tap() → 上报 0x90 + apply_local() → 重绘
 *
 * 依赖：ui.h（绘图层）。本文件放 include/，实现放 src/（工程按 src 目录通配收源码）。
 ******************************************************************************/

#ifndef _HABIT_STORE_H_
#define _HABIT_STORE_H_

#include <stdint.h>
#include "uart_protocol.h"   /* HABIT_MAX_COUNT / HABIT_STATE_xxx / HABIT_SLOT_NONE */

/* 名字点阵容量：HABIT_NAME_MAX_CHARS 个字，每字最大 16x16 = 32 字节 */
#define HABIT_BITMAP_MAX   (HABIT_NAME_MAX_CHARS * 32)
/* 卡片上最多画几行（行高 52，卡片内容区 260 高）*/
#define HABIT_DRAW_ROWS    5
/* 一行里最多画几个字的名字（再长就截断，屏宽不够）*/
#define HABIT_DRAW_NAME_CHARS 4

/* 一个习惯（设备端视图）*/
typedef struct {
    uint8_t  mode;        /* HABIT_MODE_SINGLE / HABIT_MODE_MULTI */
    uint8_t  total;       /* 格子总数 1..HABIT_MAX_SLOT */
    uint8_t  done;        /* 已确认格子数（PC 下发值，本地乐观增减）*/
    uint8_t  state;       /* HABIT_STATE_xxx（决定整行配色）*/
    uint8_t  cur_slot;    /* 当前窗口对应的格子序号；HABIT_SLOT_NONE = 无 */
    uint8_t  open_max;    /* 已开窗的最大格子序号（含）：<= 它能点，> 它是"还没到时间"
                           * HABIT_OPEN_NONE = 一格都没开窗（全 future）*/
    uint16_t streak;      /* 连续天数（只读显示，设备端不修改）*/
    uint32_t mask;        /* 已完成格子位掩码，bit i = 第 i 格 */
    uint8_t  name_chars;  /* 名字字数 0..HABIT_NAME_MAX_CHARS */
    uint8_t  name_w;      /* 点阵字宽（像素，须为 8 的倍数）*/
    uint8_t  name_h;      /* 点阵字高（像素）*/
    uint8_t  bitmap[HABIT_BITMAP_MAX];  /* 整串名字点阵，逐行扫描，MSB=最左 */
} habit_item_t;

/* ---------------- 数据层（协议钩子调用）---------------- */

/* 0x11：清空旧缓存，准备接收（count 仅用于打印/校验，不预分配）*/
void habit_store_reset(void);

/* 0x12：写入一个习惯。data/len 即 CMD_HABIT_ITEM 的 DATA
 * 返回 0=成功，-1=格式/长度错误，-2=缓存已满 */
int  habit_store_put(const uint8_t *data, uint8_t len);

/* 0x13：整批结束（调用方随后重绘并回 RSP_HABIT_SYNCED）*/
void habit_store_commit(void);

int  habit_store_count(void);
const habit_item_t *habit_store_get(int idx);

/* ---------------- 时钟（0x10 对时）---------------- */

/* 对时：mon 1-12 / day 1-31 / weekday 1-7(周一=1) / hh 0-23 / mm 0-59 / ss 0-59
 *
 * 为什么带秒（v1.9 加的）：0x10 原来只有时分，板子收到就把秒清零。于是
 * "对时"本身会让时间往回跳 1~2 秒，肉眼看见的正是"时间怎么变了"。
 * PC 现在每 10 秒发一次带秒的对时，板子被钉在 PC 时间上，误差 <1 秒，
 * hh:mm 永远不会倒退。ss 传 0xFF 表示"旧格式没带秒"，此时保持当前秒不动。*/
void habit_clock_set(uint8_t mon, uint8_t day, uint8_t weekday,
                     uint8_t hh, uint8_t mm, uint8_t ss);

/* 每 20ms 喂一次，内部按秒进位（板子自己走分，PC 定期再对一次即可）*/
void habit_clock_tick(uint32_t dt_ms);

/* 取 "HH:MM"（写 6 字节，含结束符）*/
void habit_clock_hhmm(char *out6);

/* 取 "MM/DD"（写 6 字节，含结束符）*/
void habit_clock_mmdd(char *out6);

/* ---------------- 渲染 ---------------- */

/* 画 HABIT 卡片内容。必须在 ui_clear() 之后、ui_flush() 之前调用，
 * 且卡片外框已由 ui_app.c 画好 —— 本函数只填卡片内部。 */
void habit_card_draw(void);

/* ---------------- 交互（列表页 + 详情页统一入口）----------------
 *
 * 【为什么改成两极界面】旧版"点整行 = 自动挑一格切换"，对一天 12 次的
 * 喝水这种习惯是灾难：想撤销只能撤最后一格，永远到不了 0，用户不知道
 * 它在操作哪一格。现在：列表页点多次型 → 进详情页；详情页里**一圈一格**，
 * 点哪个圈就切哪一格（对齐小程序的打卡界面）。
 *
 * habit_card_tap() 返回值：
 *   HABIT_TAP_NONE   (0) 没命中任何可点区域
 *   HABIT_TAP_REPORT (1) 命中一格：写回 idx/slot/action，调用方上报 0x90
 *   HABIT_TAP_REDRAW (2) 只是界面状态变了（进/出详情页），调用方重绘即可
 */
#define HABIT_TAP_NONE    0
#define HABIT_TAP_REPORT  1
#define HABIT_TAP_REDRAW  2

int habit_card_tap(int x, int y, int *out_idx, int *out_slot, int *out_action);

/* 详情页状态（左右滑翻页时要先把详情退掉，避免停在看不见的页上）*/
int  habit_detail_active(void);
void habit_detail_close(void);

/* ---------------- 列表滚动（习惯多于 HABIT_DRAW_ROWS 个时上下滑）--------
 * 上滑(手指向上) = 向下翻 = dir+1；下滑 = 向上翻 = dir-1。
 * 偏移会在 draw/tap 里自动夹到 [0, max]，越界不报错。 */
void habit_store_scroll(int dir);
int  habit_store_scroll_offset(void);
int  habit_store_scroll_max(void);

/* 本地乐观更新：让屏幕立即反映用户刚点的那一下。
 * 只改 mask/done/state/cur_slot —— **不动 streak**（天数必须由后端产生）。*/
void habit_store_apply_local(int idx, int slot, int action);

/* ---------------- 空态（还没收到任何习惯数据）---------------- */

/* 点是否落在 HABIT 卡片外框内。空态时屏幕只有提示文字、没有行可命中，
 * 用户点卡片任何位置都该算"催一下"，所以要先判断在不在卡片里。*/
int  habit_card_contains(int x, int y);

/* 用户刚催过数据：屏幕上把 "waiting data..." 换成 "requesting..." 给个反馈，
 * 免得点了没动静让人以为触摸坏了。超时后自己变回来（见 habit_store_tick）。*/
void habit_store_mark_sync_req(void);

/* 每 tick 喂一次。同步提示到期时返回 1 → 调用方重绘一次把文字换回来。*/
int  habit_store_tick(uint32_t dt_ms);

#endif /* _HABIT_STORE_H_ */
