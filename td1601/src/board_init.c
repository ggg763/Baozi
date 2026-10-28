

/******************************************************************************
 * @file     main.c
 * @brief    hello world
 * @version  V1.0
 * @date     17. Jan 2018
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "board_config.h"
#include "console/sys_console.h"

sys_console_t console;

extern void __ChipInitHandler(void);

void board_init(void)
{
    __ChipInitHandler();

    console.uart_id = (uint32_t)CONSOLE_IDX;
    console.baudrate = 115200;
    console.tx.port = CONSOLE_TXD_PORT;
    console.tx.pin = CONSOLE_TXD;
    console.tx.func = CONSOLE_TXD_FUNC;
    console.rx.port = CONSOLE_RXD_PORT;
    console.rx.pin = CONSOLE_RXD;
    console.rx.func = CONSOLE_RXD_FUNC;
    console.uart = NULL;

    console_init(&console);
}

