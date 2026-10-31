/******************************************************************************
 * @file     ui.h
 * @brief    包子屏幕 UI 绘图层（米黄配色 + 矩形边框 + ASCII 文字）
 *
 * 与 face_render 分工：
 *   face_render = 画"包子脸"（椭圆/弧线几何部件）
 *   ui          = 画"卡片 UI"（背景/方框/文字），用于长按后唤出的卡片层
 *
 * 渲染方式：整屏刷新（480x480），每 8 行一段 DMA（与 face_render 同构）。
 * 用法：ui_clear() -> ui_add_rect()/ui_add_text() -> ui_flush(bg)
 ******************************************************************************/

#ifndef _UI_H_
#define _UI_H_

#include <stdint.h>

/* 包子 UI 米黄配色（RGB565，与《包子屏幕UI规格书》一致）*/
#define UI_COL_BG     0xFFBC   /* 主背景  米黄 #FDF6E3 */
#define UI_COL_LINE   0xBBA2   /* 描边    焦糖 #BA7517（方框/页码/圆点） */
#define UI_COL_TEXT   0x4120   /* 主文字  深棕 #412402 */
#define UI_COL_SUB    0x8261   /* 次文字  中棕 #854F0B */

/* ---- 拟物可爱风扩展配色（RGB565，已用工具换算核对）---- */
#define C_BG_TOP      0xFFFE   /* 顶部背景 奶白 #FFFCF4 */
#define C_BG_MID      0xFF9B   /* 中部背景 浅蜜 #FDF3DE */
#define C_BG_BOT      0xFF59   /* 底部背景 #F9E9CC */
#define C_CARD_TOP    0xFFFE   /* 卡片填充上 #FFFDF6 */
#define C_CARD_BOT    0xFF5A   /* 卡片填充下 #FAEBD0 */
#define C_CARD_LINE   0xE5F0   /* 卡片描边 #E0BE86 */
#define C_CARD_SHAD   0xDE12   /* 卡片厚度阴影 #DFC296 */
#define C_BTN_TOP     0xD487   /* 主按钮上 #D4903A */
#define C_BTN_BOT     0xBBA2   /* 主按钮下 #BA7517 */
#define C_BTN_SHAD    0x8AC1   /* 主按钮阴影 #8F5A0F */
#define C_DOT_ON      0xBBA2   /* 页码圆点·选中 #BA7517 */
#define C_DOT_OFF     0xE675   /* 页码圆点·未选 #E5CFA8 */
#define C_TITLE       0x59E3   /* 标题深棕 #5A3E1E */
#define C_HILITE      0xBBA2   /* 强调色 #BA7517 */
#define C_TRACK       0xF719   /* 进度底槽 #F0E3C8 */
#define C_ICON_RED    0xF4CF   /* 番茄钟图标 #F0997B */
#define C_ICON_GRN    0x962B   /* 正计时图标 #97C459 */
#define C_ICON_BLU    0x85BD   /* 倒计时图标 #85B7EB */

/* 清空绘制列表（每次画新界面先调一次）*/
void ui_clear(void);

/* 画圆角实心矩形（拟物风卡片/按钮主体）。r=圆角半径 */
void ui_add_round_fill(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, uint16_t color);

/* 画圆角矩形边框（t=线宽）*/
void ui_add_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, int16_t t, uint16_t color);

/* 画"立体按钮"：主体 + 下方 dy 像素的厚度色（模拟 box-shadow）*/
void ui_add_btn3d(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                  uint16_t top, uint16_t bot, uint16_t shade);

/* 画进度圆弧：从 12 点方向顺时针 sweep_deg 度（0..360），t=环宽 */
void ui_add_arc(int16_t cx, int16_t cy, int16_t r, int16_t t,
                int sweep_deg, uint16_t color);

/* 纵向三段渐变背景（近似 HTML linear-gradient），一次刷屏用 */
void ui_set_bg_gradient(uint16_t top, uint16_t mid, uint16_t bot);

/* 画一个矩形边框：x,y 左上角，w,h 宽高，t 线宽，color 颜色 */
void ui_add_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                 int16_t t, uint16_t color);

/* 画一行 ASCII 文字：x,y 左上角，scale 放大倍数，color 颜色 */
void ui_add_text(int16_t x, int16_t y, const char *s, int scale, uint16_t color);

/* 画一个圆：cx,cy 圆心，r 半径；filled=0 画圆环（t 线宽），filled=1 实心 */
void ui_add_circle(int16_t cx, int16_t cy, int16_t r, int16_t t,
                   int filled, uint16_t color);

/* 画一块 1bpp 点阵（用于显示 PC 下发的汉字点阵，如习惯名）。
 * 格式：逐行扫描，每行 stride 字节（stride >= (w+7)/8），MSB = 该行最左像素。
 * 注意：data 必须活到 ui_flush() 之后（一般指向静态缓冲区）。 */
void ui_add_bitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                   const uint8_t *data, int16_t stride, uint16_t color);

/* 同上，但整块放大 scale 倍（scale<=1 等价 ui_add_bitmap） */
void ui_add_bitmap2(int16_t x, int16_t y, int16_t w, int16_t h,
                    const uint8_t *data, int16_t stride, uint16_t color,
                    int16_t scale);

/* 计算文字渲染宽度（像素），用于居中 */
int ui_text_width(const char *s, int scale);

/* ---------------- 圆角数码（时钟 / 倒计时专用） ----------------
 *
 * 为什么不直接把 ui_add_text 放大：板载 ASCII 字库是 12×24 的 1bpp 点阵，
 * 放大 2 倍就是 2×2 的硬方块，48px 高的数字上每个台阶都看得见（用户反馈
 * "像素块太明显"）。放大 5 倍时更糟。
 *
 * 这里用**圆角矩形拼笔画**（七段数码，笔画两端是半圆），边缘天然是圆的，
 * 48px 和 120px 都不会出现台阶。代价只是绘制列表里多几十个圆角块。
 *
 * 支持字符：'0'..'9'、':'、'-'、' '（其它字符跳过并按空格占位）。
 * h = 数字高度（像素），笔画粗细/宽度/字距全部按比例算。
 */
void ui_add_clock(int16_t x, int16_t y, const char *s, int16_t h, uint16_t color);

/* 上面那串字符占多宽（像素），用于居中 */
int  ui_clock_width(const char *s, int16_t h);

/* 把绘制列表刷上屏（整屏刷新，bg 为背景色）。
 * 同步版：内部一直刷到完成为止（表情切换这类不能半张脸的场景用它）。*/
void ui_flush(uint16_t bg);

/* 分片刷新（v1.9）：ui_flush_begin() 之后由主循环反复调 ui_flush_step()，
 * 每次只刷 8 行（≈3~8ms），两片之间主循环能回触摸轮询。
 * ——卡片页交互一律走这个，否则一次点击会把主循环卡 100~200ms，
 * 连点时触摸事件被合并，看起来像"点了没反应 / 最后一起变色"。*/
void ui_flush_begin(uint16_t bg);
int  ui_flush_step(void);      /* 返回 1 = 已刷完；0 = 还有下一片 */
int  ui_flush_busy(void);      /* 1 = 上一次还没刷完 */

/* ★ 总线仲裁回调（与 face_render.h 的 face_set_bus_callback 同语义）。
 *
 * 为什么要它：卡片层刷屏同样是"分段 DMA"，DMA 持续从 SRAM 取数喂 SPI FIFO（走 AHB）。
 * 若此刻舵机在写 PWM 寄存器（每 20ms 一次读-改-写，也走 AHB），DMA 取数被延迟 →
 * SPI FIFO 被抽空 → 那段像素时序断 → 屏幕上出现错位白块。
 * 表情层（face_render.c）早已接了这个钩子，卡片层（本文件）也必须接，否则
 * "一进卡片页、舵机一动就闪白块"。
 *
 *   cb(1) = 即将发一段 DMA（外部应挂起会走 AHB 的寄存器写）
 *   cb(0) = 一段刚发完、DMA 空闲（外部可安全补写）
 */
typedef void (*ui_bus_cb_t)(int busy);
void ui_set_bus_callback(ui_bus_cb_t cb);

#endif /* _UI_H_ */
