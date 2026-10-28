/******************************************************************************
 * @file     moves_demo.c
 * @brief    动作库演示入口：循环播放【待机（IDLE）】动作，用来确认动作效果
 *
 * 后续把情感状态机接上动作库之后，这个 demo 就可以退役
 * （状态机直接调 moves_play_once(MOVE_xxx)）。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "board_config.h"
#include "drv/common.h"     /* mdelay */
#include "moves.h"

/* 演示哪个动作：改这一行即可（MOVE_IDLE / MOVE_HAPPY / MOVE_EIGHT ...）*/
#define DEMO_MOVE           MOVE_EIGHT

/*****************************************************************************
 * moves_demo: 入口（被 src/main.c 调用，内部死循环不返回）
 *****************************************************************************/
void moves_demo(void)
{
    printf("\r\n[moves] demo start: %s\r\n", moves_name(DEMO_MOVE));

    if (moves_init() != 0) {
        return;
    }
    printf("[moves] playing \"%s\" repeatedly...\r\n", moves_name(DEMO_MOVE));

    for (;;) {
        moves_play_once(DEMO_MOVE);

        if (moves_interrupted() != 0) {
            printf("[moves] interrupted\r\n");
        }
        mdelay(300);        /* 两个周期之间留一点间隙 */
    }
}
