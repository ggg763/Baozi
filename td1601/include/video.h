/*
 * video.h
 *
 *  Created on: Nov 29, 2020
 *      Author: ZKSWE Develop Team
 */

#ifndef _VIDEO_H_
#define _VIDEO_H_

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* 毫秒延时：实现见 src/main.c（HAL_Delay 兼容层） */
extern void HAL_Delay(uint32_t ms);

typedef struct {
	uint16_t width;
	uint16_t height;
	uint8_t bpp;
	uint32_t *disp_buff;
} video_info_t;

bool video_init();
void video_deinit();
void video_flip();

/* 等 OSPI 真正把数据发完（TX FIFO 空 + 不忙）。
 * LCD_DmaSend() 只等 DMA 搬完，不等 SPI 移位寄存器发完；连续发多段时
 * 若不等，上一段可能还在移位，屏幕会出现错位的白块/白条。
 * 实现在 src/video.c。 */
void LCD_WaitIdle(void);

#endif /* _GRAPHICS_VIDEO_H_ */
