/******************************************************************************
 * @file     play_card.c
 * @brief    卡片层第 4 页：动作 / 情绪（见 play_card.h）
 *
 * 布局（卡片外框 60,122,360,290；内容区 y=137..398）：
 *   2 列 × 3 行胶囊按钮，每个 152×72：
 *     ┌───────────────┬───────────────┐
 *     │ NOD    (开心) │ SHAKE  (生气) │
 *     ├───────────────┼───────────────┤
 *     │ WOW  (惊讶)   │ CUDDLE (爱意) │
 *     ├───────────────┼───────────────┤
 *     │ PRAISE (夸它) │ SCOLD  (凶它) │
 *     └───────────────┴───────────────┘
 * 前四个 = 动作请求（move_id），后两个 = 情绪刺激（EMO_STIM_*）。
 *
 * 为什么按钮要分级：动作按钮走 emotion_fsm_request_action()，情绪按钮走
 * emotion_fsm_stimulus()（只调连续状态）。后者连点几下也不会让情绪瞬间跳变，
 * 而是"夸多了才越来越开心"，符合用户"UI 只能在一定程度上影响情绪"的要求。
 ******************************************************************************/

#include <stdint.h>
#include "play_card.h"
#include "ui.h"
#include "behavior.h"
#include "emotion/emotion_manager.h"    /* EMO_STIM_PRAISE / EMO_STIM_SCOLD */
#include "movements/moves.h"            /* move_id_t */

/* 与 ui_app.c 的卡片外框保持一致 */
#define CARD_X        60
#define CARD_Y        122
#define CARD_W        360
#define CARD_H        290

#define BTN_W         152
#define BTN_H         72
#define BTN_X0        (CARD_X + 16)             /* 76  */
#define BTN_X1        (BTN_X0 + BTN_W + 24)     /* 252 */
#define BTN_Y0        (CARD_Y + 23)             /* 145 */
#define BTN_DY        (BTN_H + 15)              /* 87  */

typedef enum {
    ACT_NOD = 0,        /* 点头晃脑（开心动作）*/
    ACT_SHAKE,          /* 快速摇头（生气动作）*/
    ACT_WOW,            /* 猛抬头（惊讶动作）*/
    ACT_CUDDLE,         /* 缓慢歪头（爱意动作）*/
    ACT_PRAISE,         /* 情绪刺激：夸它 */
    ACT_SCOLD           /* 情绪刺激：凶它 */
} play_act_t;

typedef struct {
    const char *label;
    const char *hint;
    play_act_t  act;
} play_btn_t;

/* 顺序与布局一致：行优先（左、右）× 3 行 */
static const play_btn_t k_btns[6] = {
    { "NOD",    "happy",   ACT_NOD    },
    { "SHAKE",  "angry",   ACT_SHAKE  },
    { "WOW",    "surprise",ACT_WOW    },
    { "CUDDLE", "loving",  ACT_CUDDLE },
    { "PRAISE", "cheer up",ACT_PRAISE },
    { "SCOLD",  "easy...", ACT_SCOLD  }
};

/* 按钮矩形（x, y）*/
static void btn_rect(int i, int *x, int *y)
{
    int col = i % 2;
    int row = i / 2;

    *x = col ? BTN_X1 : BTN_X0;
    *y = BTN_Y0 + row * BTN_DY;
}

static void draw_btn(int i)
{
    int x, y, tw;
    const play_btn_t *b = &k_btns[i];

    btn_rect(i, &x, &y);

    /* 立体胶囊：主体 + 厚度阴影 + 描边（与 pomo.c 的 btn() 同款皮肤）*/
    ui_add_btn3d((int16_t)x, (int16_t)y, BTN_W, BTN_H, (int16_t)(BTN_H / 2),
                 C_CARD_TOP, C_CARD_BOT, C_CARD_SHAD);
    ui_add_round_rect((int16_t)x, (int16_t)y, BTN_W, BTN_H, (int16_t)(BTN_H / 2),
                      2, C_CARD_LINE);

    tw = ui_text_width(b->label, 1);
    ui_add_text((int16_t)(x + (BTN_W - tw) / 2), (int16_t)(y + 12),
                b->label, 1, C_TITLE);

    tw = ui_text_width(b->hint, 1);
    ui_add_text((int16_t)(x + (BTN_W - tw) / 2), (int16_t)(y + 40),
                b->hint, 1, UI_COL_SUB);
}

void play_card_draw(void)
{
    int i;
    int w;
    const char *title = "PLAY WITH ME";

    /* 卡内标题 */
    w = ui_text_width(title, 1);
    ui_add_text((int16_t)(240 - w / 2), (int16_t)(CARD_Y + 4), title, 1, C_TITLE);

    for (i = 0; i < 6; i++) {
        draw_btn(i);
    }
}

int play_card_tap(int x, int y)
{
    int i, bx, by;

    for (i = 0; i < 6; i++) {
        btn_rect(i, &bx, &by);
        if ((x >= bx) && (x < bx + BTN_W) && (y >= by) && (y < by + BTN_H)) {
            switch (k_btns[i].act) {
            case ACT_NOD:    behavior_request_move((uint8_t)MOVE_HAPPY);     break;
            case ACT_SHAKE:  behavior_request_move((uint8_t)MOVE_ANGRY);     break;
            case ACT_WOW:    behavior_request_move((uint8_t)MOVE_SURPRISED); break;
            case ACT_CUDDLE: behavior_request_move((uint8_t)MOVE_LOVING);    break;
            case ACT_PRAISE: behavior_stimulus(EMO_STIM_PRAISE, 100);        break;
            case ACT_SCOLD:  behavior_stimulus(EMO_STIM_SCOLD, 100);         break;
            default:         break;
            }
            return 1;
        }
    }
    return 0;
}
