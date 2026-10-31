/******************************************************************************
 * @file     face_picker.h
 * @brief    卡片层第 5 页：表情选择（"FACES"）—— 浏览并播放全部表情
 *
 * 动机：v9 姿势表有 30 个表情，但状态机能触发的只有 27 个，而且 glance /
 * dizzy / idea 这几个根本没有事件源。没有这一页的话，这些表情永远上不了屏。
 * 这一页把 face_model 的姿势表整个摊开成一个可滚动的名字列表：
 *   点名字 -> 立刻回表情态播放该表情（face_app_play，3s 后自动回落）。
 *
 * 为什么是"点名字就播、然后收卡片"而不是留在列表里预览：
 *   表情渲染归 face_app（表情态）管，卡片层画的是 ui.c 的卡片，两者抢同一块
 *   屏。留在列表里就看不到脸，所以点完立刻回表情态看效果，是最省事也最直觉的。
 ******************************************************************************/

#ifndef _FACE_PICKER_H_
#define _FACE_PICKER_H_

/* 画第 5 页内容。卡片外框由 ui_app.c 画好（60,122,360,290）。 */
void face_picker_draw(void);

/* 点击命中：返回命中的姿势下标（0..n-1，用 face_model_get(idx)->name 取名字）；
 * -1 = 没点中列表。 */
int  face_picker_tap(int x, int y);

/* 上/下滑滚动（dir: +1 下滚 / -1 上滚，语义同 habit_store_scroll）*/
void face_picker_scroll(int dir);

/* 回到本页时重置滚动 */
void face_picker_reset(void);

#endif /* _FACE_PICKER_H_ */
