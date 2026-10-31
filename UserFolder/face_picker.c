/******************************************************************************
 * @file     face_picker.c
 * @brief    表情选择页实现（见 face_picker.h）
 ******************************************************************************/

#include <stdint.h>
#include "face_picker.h"
#include "ui.h"
#include "anim/face_model.h"        /* face_model_count / face_model_get */

/* 卡片内框（与 ui_app.c 的 UI_CARD_* 一致）*/
#define FP_X        (60 + 20)        /* 列表左边距 */
#define FP_W        (360 - 40 - 16)  /* 列表宽度（右侧留 16px 给滚动条）*/
#define FP_LIST_Y   168              /* 列表起始 y（标题之下）*/
#define FP_ROW_H    26               /* 每行高（12×24 字 + 2px 间距）*/
#define FP_ROWS     8                /* 一屏可见行数 */
#define FP_LIST_BOT (122 + 290 - 8)  /* 列表底界（卡片内 8px 边距）*/

static int g_scroll = 0;             /* 已滚动的行数 */

static int count(void)
{
    return face_model_count();
}

/* 跳过别名姿势（pet 是 shy 的别名，视觉完全一样，列表里不重复出现）*/
static int skip_alias(const char *name)
{
    return (name != 0 && name[0] == 'p' && name[1] == 'e' &&
            name[2] == 't' && name[3] == '\0');
}

void face_picker_reset(void)
{
    g_scroll = 0;
}

void face_picker_draw(void)
{
    int n = count();
    int maxscroll = n - FP_ROWS;
    int i, w;

    if (maxscroll < 0) {
        maxscroll = 0;
    }
    if (g_scroll > maxscroll) {
        g_scroll = maxscroll;
    }

    /* 标题 */
    w = ui_text_width("FACES", 1);
    ui_add_text((int16_t)(240 - w / 2), 126, "FACES", 1, C_TITLE);

    /* 副标题：点名字即播放 */
    w = ui_text_width("tap a name to play", 1);
    ui_add_text((int16_t)(240 - w / 2), 146, "tap a name to play", 1, UI_COL_SUB);

    /* 列表：只画可见窗口里的行 */
    for (i = 0; i < FP_ROWS; i++) {
        int idx = g_scroll + i;
        const char *name;
        int y;

        if (idx >= n) {
            break;
        }
        name = face_model_get(idx)->name;
        if (skip_alias(name)) {
            continue;               /* pet 跳过，行留白 */
        }
        y = FP_LIST_Y + i * FP_ROW_H;
        ui_add_text((int16_t)FP_X, (int16_t)y, name, 1, UI_COL_TEXT);
    }

    /* 右侧滚动条：画一个小圆点指示当前位置。
     * 卡片右边界 420（60+360），滚动条放 408（卡内右侧，与列表 x 范围 80..400 不重叠）*/
    if (n > FP_ROWS) {
        int track_h = FP_ROWS * FP_ROW_H;
        int thumb_h = track_h * FP_ROWS / n;
        int thumb_y = FP_LIST_Y + g_scroll * (track_h - thumb_h) / maxscroll;
        if (thumb_h < 12) {
            thumb_h = 12;
        }
        ui_add_round_fill(408, (int16_t)thumb_y, 8, (int16_t)thumb_h, 4, C_DOT_ON);
    }
}

/* 把点击坐标换算成姿势下标；-1 = 没点中列表 */
static int hit_index(int x, int y)
{
    int n = count();
    int i;

    if (x < FP_X || x > FP_X + FP_W) {
        return -1;
    }
    for (i = 0; i < FP_ROWS; i++) {
        int idx = g_scroll + i;
        int y0 = FP_LIST_Y + i * FP_ROW_H;

        if (idx >= n) {
            break;
        }
        if (skip_alias(face_model_get(idx)->name)) {
            continue;
        }
        if (y >= y0 && y < y0 + FP_ROW_H) {
            return idx;
        }
    }
    return -1;
}

int face_picker_tap(int x, int y)
{
    return hit_index(x, y);         /* -1 = 没点中；否则是姿势下标 */
}

void face_picker_scroll(int dir)
{
    int n = count();
    int maxscroll = n - FP_ROWS;

    if (maxscroll < 0) {
        maxscroll = 0;
    }
    g_scroll += dir;
    if (g_scroll < 0) {
        g_scroll = 0;
    }
    if (g_scroll > maxscroll) {
        g_scroll = maxscroll;
    }
}
