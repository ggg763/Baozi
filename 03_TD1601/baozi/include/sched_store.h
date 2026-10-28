#ifndef _SCHED_STORE_H_
#define _SCHED_STORE_H_

#include <stdint.h>

/* ============================================================================
 * sched_store —— TODAY 页日程缓存（对应小程序首页的日程列表）
 * ----------------------------------------------------------------------------
 * 数据来源：PC 桥接脚本把后端 /api/v1/events 翻译成 0x15 帧逐条下发
 * （0x14 BEGIN / 0x15 ITEM×N / 0x16 END，布局见 uart_protocol.h）。
 *
 * 渲染模仿小程序首页：按日期分组（组头 + 事件行），已完成=实心圆，
 * 未完成=空心圆。点事件行 = 切换完成状态（上行 0x93），真值仍以后端为准。
 *
 * 与 habit_store 相同的两条铁律：
 *   1) ui_add_text 只存指针 → 所有字符串必须 static；
 *   2) 接收是"原子"的：BEGIN 不清旧数据，END 才整体替换（防丢帧出空页）。
 * ========================================================================== */

#define SCHED_MAX_ITEMS        12   /* 板子最多缓存几条日程 */
/* 标题最多 6 字：16px 点阵经 2x 放大成 32px 大字后，6 字宽 192px，
 * 加时间(120px)和完成圆(32px)刚好占满 360px 卡片。帧 DATA 上限 255B
 * 也只装得下 6 字（10+6*2*16=202）。真值定义在 uart_protocol.h。 */
#ifndef SCHED_TITLE_MAX_CHARS
#define SCHED_TITLE_MAX_CHARS   6
#endif
#define SCHED_TITLE_BITMAP_MAX  (7 * 2 * 16)   /* 缓冲留 7 字余量，协议仍按 16px 点阵下发 */

typedef struct {
    uint8_t mon, day, weekday;      /* weekday: 1=周一 .. 7=周日 */
    uint8_t hh, mm;
    uint8_t status;                 /* SCHED_STATUS_xxx */
    uint8_t chars;                  /* 标题字数 0=不画标题 */
    uint8_t nw, nh;                 /* 点阵单字宽 / 高 */
    uint8_t bitmap[SCHED_TITLE_BITMAP_MAX];
} sched_item_t;

#define SCHED_STATUS_PENDING  0
#define SCHED_STATUS_DONE     1

/* ---- 数据层 ---- */
void sched_store_reset(void);                       /* 0x14 BEGIN：标记接收中 */
int  sched_store_put(const uint8_t *data, uint8_t len);  /* 0x15：落一条，0=成功 */
void sched_store_commit(void);                      /* 0x16 END：整体替换生效 */
int  sched_store_count(void);
const sched_item_t *sched_store_get(int idx);

/* ---- 交互（返回值同 habit_card_tap 的 HABIT_TAP_xxx）---- */
#define SCHED_TAP_NONE    0
#define SCHED_TAP_REPORT  1
#define SCHED_TAP_REDRAW  2
#define SCHED_TAP_SYNC    3   /* 空态点了卡片：调用方发一次催数据 */

int sched_card_tap(int x, int y, int *out_idx, int *out_action);

/* 本地乐观翻转（不动后端真值，PC 重推时校正） */
void sched_store_apply_local(int idx, int action);

/* ---- 渲染 / 滚动 ---- */
void sched_card_draw(void);
int  sched_card_contains(int x, int y);
void sched_store_scroll(int dir);

/* ---- 空态反馈窗（同 habit_store_tick 的机制）---- */
void sched_store_mark_sync_req(void);
int  sched_store_tick(uint32_t dt_ms);   /* 返回 1 = 提示到期需重绘 */

#endif /* _SCHED_STORE_H_ */
