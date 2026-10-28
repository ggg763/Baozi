/******************************************************************************
 * @file     anim_player.h
 * @brief    ANM3 动画资源播放器（240x240 资源 -> 480x480 面板，2x 放大）
 *
 * 资源由 PC 端 gif2res.py 生成（默认 240x240 / 16 色 / 帧间差分 / raw deflate）：
 *   "ANM3" | srcW | srcH | scale | frames | delay_ms | 调色板 256xRGB565
 *          | 帧记录（x,y,w,h,nblk + 块长度表） | 帧数据（每块 8 行 raw deflate）
 *
 * 解码：third_party/puff.c（zlib 自带最小 inflate，已打补丁去掉 setjmp/longjmp）
 * 送屏：索引查表 + 水平放大 -> 8 资源行一段 -> LCD_DmaSend（≤3840 像素）
 *       垂直放大 = 同一段重复发 scale 次；窗口坐标同步乘 scale
 *
 * RAM：g_idx 3840B + g_rgb 7680B + g_pal 512B ≈ 12 KB，无整帧缓冲
 *
 * 接入方式（见 anim_player.c 顶部 ANIM_USE_BIN）：
 *   0 = C 数组（anim_res.c，默认，最省事）
 *   1 = objcopy 生成的 libanimres.a（改代码不重烧资源，见 run_gif2res_lib.bat）
 *
 * 入口：anim_demo() —— 由 src/main.c 调用
 ******************************************************************************/

#ifndef _ANIM_PLAYER_H_
#define _ANIM_PLAYER_H_

/* 资源头由 PC 端脚本生成；若找不到，通常意味着还没跑 run_gif2res.bat */
#if defined(__has_include)
#  if !__has_include("anim_res.h")
#    error "anim_res.h not found: run run_gif2res.bat (or gif2res.py) first - it generates anim_res.h/.c into UserFolder/anim/"
#  endif
#endif
#include "anim_res.h"

/* 屏幕初始化（官方 video_init，含三色自检），调用一次；内部会初始化条目表 */
void anim_init(void);

/* 资源条目表（共 ANIM_COUNT 项，宏见 anim_res.h）。
 * 用来屏蔽“C 数组 / objcopy 库”两种接入模式，必须在 anim_init() 之后调用。 */
const anim_item_t *anim_items(void);

/* 非阻塞逐帧播放（供状态机 / 串口测试驱动）：
 *   anim_play_open(item)  选片段并复位游标，0 = 成功
 *   anim_play_step()      渲染下一帧并推进；返回 1 = 刚刚回卷到开头（上一轮播完）
 * 调用方自己按 ANIM_FRAME_DELAY_MS 的节奏调 anim_play_step()。
 * 注意：它们与阻塞版 anim_play() 共用行缓冲/调色板，不要混用。 */
int anim_play_open(const anim_item_t *item);
int anim_play_step(void);

/* 播放一个片段 loops 轮（阻塞；每帧间隔取资源头里的 delay_ms） */
int anim_play(const anim_item_t *item, int loops);

/* 演示入口：循环依次播放资源里的全部片段 */
void anim_demo(void);

#endif /* _ANIM_PLAYER_H_ */
