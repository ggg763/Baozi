/******************************************************************************
 * @file     qspi_lcd.c
 * @brief    轻量 QSPI LCD 驱动（480x480, TR230S）—— 移植自 LCD 模板
 *
 * 来源：QSPI_480/projects/LCD/src/video.c（已点亮验证，作模板基准）
 * 精简点：
 *   1. 去掉 OSPI DMA 与中断回调，像素全部轮询发送（足够轻量、简单）
 *   2. 去掉调试 printf 与帧计数
 * 保留点（与模板严格一致，勿随意改动，否则可能不显示）：
 *   - 命令写：标准 SPI 8bit + 命令前缀字节(0x02/0x12)协议
 *   - 像素写：切换 QSPI 16bit 模式（含 0x6000c0f4=0x302 配置）+ 轮询
 *   - WAIT(PB10) 忙信号等待、复位/背光时序、0x20/0x64 初始化命令
 *
 * 接线（TD1601 EVB 扩展板 CON2）：
 *   SCK=PA3  D0~D3=PB4~PB7(SPI1 mux)
 *   CS=PA2(低有效)  RST=PB15  WAIT=PB10(输入)  BL=PA31(背光,高亮)
 ******************************************************************************/

#include "qspi_lcd.h"
#include <stdio.h>
#include <drv/ospi.h>
#include <drv/pin.h>
#include <drv/common.h>     /* mdelay / udelay */
#include <dw_ospi_ll.h>
#include <dw_spi_ll.h>
#include <string.h>

/* ---------------- LCD 控制 GPIO ---------------- */
static csi_gpio_t lcd_cs;      /* 片选 PA2 */
static csi_gpio_t lcd_rst;     /* 复位 PB15 */
static csi_gpio_t lcd_wait;    /* 忙信号 PB10（输入） */
static csi_gpio_t lcd_bl;      /* 背光 PA31 */

/* 复位引脚（PB15） */
#define RST_LOW()    csi_gpio_write(&lcd_rst, PIN15, 0)
#define RST_HI()     csi_gpio_write(&lcd_rst, PIN15, 1)
/* 片选引脚（PA2）低有效 */
#define CLR_CS1()    csi_gpio_write(&lcd_cs, PIN2, 0)
#define SET_CS1()    csi_gpio_write(&lcd_cs, PIN2, 1)
/* 背光（PA31）高电平点亮 */
#define BLK_LOW()    csi_gpio_write(&lcd_bl, PIN31, 0)
#define BLK_HI()     csi_gpio_write(&lcd_bl, PIN31, 1)

/* ---------------- OSPI 外设 ---------------- */
#define DWOSPI   ((dw_ospi_regs_t *)DW_OSPI0_BASE)

static csi_ospi_t ospi;

/* 忙信号等待：WAIT(PB10) 为高才继续（与模板一致） */
static void lcd_wait_ready(void)
{
    while (csi_gpio_read(&lcd_wait, (1U << 10)) == 0);
}

/**
 * @brief 标准 SPI 模式单字节发送（8bit，命令/参数用），与模板一致
 * @param num 待发送字节
 */
#define TFT_SPI_Write_Byte(num)                                                \
do {                                                                           \
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));                                     \
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);                                      \
    dw_ospi_disable(DWOSPI);                                                   \
    dw_ospi_set_spi_frame_format_std_mode(DWOSPI);                             \
    dw_ospi_config_ctl_frame_len(DWOSPI, 8 - 1);                               \
    csi_ospi_baud(&ospi, 120 * 100000);                                        \
    dw_ospi_enable(DWOSPI);                                                    \
    DWOSPI->DR = num;                                                          \
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));                                     \
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);                                      \
} while (0)

/**
 * @brief 快速 16bit 数据发送（QSPI 模式，仅像素阶段用）
 * @param num 16bit RGB565 颜色
 */
static void lcd_write_pixel16_fast(uint16_t num)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);
    DWOSPI->DR = num;
}

/**
 * @brief 开始批量像素传输：发 RAMWR(0x2C) 命令前缀并切到 QSPI 16bit 模式
 */
static void lcd_start_write_data(void)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);

    /* 标准 SPI 发送 RAMWR 命令头（模板协议：0x12 0x00 0x2C 0x00） */
    dw_ospi_disable(DWOSPI);
    csi_ospi_baud(&ospi, 500 * 100000);
    dw_ospi_set_spi_frame_format_std_mode(DWOSPI);
    dw_ospi_config_ctl_frame_len(DWOSPI, 8 - 1);
    dw_ospi_enable(DWOSPI);

    CLR_CS1();
    DWOSPI->DR = 0x12;
    DWOSPI->DR = 0x00;
    DWOSPI->DR = 0x2C;
    DWOSPI->DR = 0x00;

    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);

    /* 切到四线 QSPI、16bit 帧格式，用于颜色数据 */
    dw_ospi_disable(DWOSPI);
    dw_ospi_set_spi_frame_format_quad_mode(DWOSPI);
    *(volatile uint32_t *)0x6000c0f4 = 0x302;
    dw_ospi_config_ctl_frame_len(DWOSPI, 16 - 1);
    dw_ospi_enable(DWOSPI);
}

/**
 * @brief 结束批量像素传输：拉高 CS，恢复标准 SPI 8bit 模式
 */
static void lcd_end_write_data(void)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);

    SET_CS1();
    dw_ospi_disable(DWOSPI);
    dw_ospi_set_spi_frame_format_std_mode(DWOSPI);
    dw_ospi_config_ctl_frame_len(DWOSPI, 8 - 1);
    dw_ospi_enable(DWOSPI);
}

/**
 * @brief 命令 + 参数发送（模板 my_write：带命令前缀字节的 8bit 写）
 * @param cmd  LCD 命令码
 * @param data 参数数据
 * @param len  参数长度
 */
static void lcd_cmd_write(uint8_t cmd, uint8_t *data, uint32_t len)
{
    uint8_t data_in[16] = {0};

    if (cmd == 0x2C) {
        data_in[0] = 0x12;      /* RAMWR 用 0x12 前缀 */
    } else {
        data_in[0] = 0x02;      /* 普通命令用 0x02 前缀 */
    }
    data_in[1] = 0x00;
    data_in[2] = cmd;
    data_in[3] = 0x00;

    CLR_CS1();
    for (uint32_t i = 0; i < len + 4; i++) {
        if (i < 4) {
            TFT_SPI_Write_Byte(data_in[i]);
        } else {
            TFT_SPI_Write_Byte(data[i - 4]);
        }
    }
    SET_CS1();
}

/**
 * @brief 设置显示窗口（列 x0..x1、行 y0..y1，含端点）
 */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t param[4];

    param[0] = (uint8_t)(x0 >> 8);
    param[1] = (uint8_t)(x0 & 0xff);
    param[2] = (uint8_t)(x1 >> 8);
    param[3] = (uint8_t)(x1 & 0xff);
    lcd_cmd_write(0x2a, param, 4);

    param[0] = (uint8_t)(y0 >> 8);
    param[1] = (uint8_t)(y0 & 0xff);
    param[2] = (uint8_t)(y1 >> 8);
    param[3] = (uint8_t)(y1 & 0xff);
    lcd_cmd_write(0x2B, param, 4);
}

/**
 * @brief 区域单色填充（模板 my_fill_color 的轮询版）
 * @param x,y    起点坐标
 * @param w,h    宽高
 * @param color  16bit RGB565
 */
void qspi_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    lcd_wait_ready();
    lcd_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));

    /* 批量发送颜色数据 */
    lcd_start_write_data();
    uint32_t total = (uint32_t)w * h;
    for (uint32_t i = 0; i < total; i++) {
        lcd_write_pixel16_fast(color);
    }
    lcd_end_write_data();
}

/**
 * @brief 实心圆填充：只发一次窗口命令，圆内填 fill、外接矩形其余填 out
 *
 * 相比"逐行小窗口画圆"，本函数每帧事务数少一个数量级，不会出现碎窗口
 * 导致的漏画；out 传该圆所在表面的底色即可安全盖掉旧画面。
 * @param cx,cy  圆心坐标
 * @param r      半径
 * @param fill   圆内颜色（RGB565）
 * @param out    圆外接矩形内、圆以外的颜色（表面底色）
 */
void qspi_fill_circle(int cx, int cy, int r, uint16_t fill, uint16_t out)
{
    int x0 = cx - r; if (x0 < 0)           x0 = 0;
    int y0 = cy - r; if (y0 < 0)           y0 = 0;
    int x1 = cx + r; if (x1 >= QSPI_LCD_W) x1 = QSPI_LCD_W - 1;
    int y1 = cy + r; if (y1 >= QSPI_LCD_H) y1 = QSPI_LCD_H - 1;

    lcd_wait_ready();
    lcd_set_window((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);

    lcd_start_write_data();
    int rr = r * r;
    for (int yy = y0; yy <= y1; yy++) {
        int dy2 = (yy - cy) * (yy - cy);
        for (int xx = x0; xx <= x1; xx++) {
            int dx = xx - cx;
            uint16_t c = (dx * dx + dy2 <= rr) ? fill : out;
            lcd_write_pixel16_fast(c);
        }
    }
    lcd_end_write_data();
}

/**
 * @brief 实心椭圆填充（单窗口双色），用于地面阴影等扁圆图形
 */
void qspi_fill_ellipse(int cx, int cy, int rx, int ry, uint16_t fill, uint16_t out)
{
    int x0 = cx - rx; if (x0 < 0)           x0 = 0;
    int y0 = cy - ry; if (y0 < 0)           y0 = 0;
    int x1 = cx + rx; if (x1 >= QSPI_LCD_W) x1 = QSPI_LCD_W - 1;
    int y1 = cy + ry; if (y1 >= QSPI_LCD_H) y1 = QSPI_LCD_H - 1;

    lcd_wait_ready();
    lcd_set_window((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);

    lcd_start_write_data();
    long long rx2 = (long long)rx * rx;
    long long ry2 = (long long)ry * ry;
    long long lim = rx2 * ry2;
    for (int yy = y0; yy <= y1; yy++) {
        long long dy = yy - cy;
        for (int xx = x0; xx <= x1; xx++) {
            long long dx = xx - cx;
            uint16_t c = (dx * dx * ry2 + dy * dy * rx2 <= lim) ? fill : out;
            lcd_write_pixel16_fast(c);
        }
    }
    lcd_end_write_data();
}

/**
 * @brief 实心三角形填充（单窗口双色，边函数光栅化）
 *
 * 内外判定用"重心符号法"：三角形重心必然在内部，取其三条边函数符号
 * 作基准，其余像素同号即内部——避免屏幕坐标 y 向下造成的绕向歧义。
 */
void qspi_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                        uint16_t fill, uint16_t out)
{
    /* 外接矩形 + 屏内裁剪 */
    int minx = x0, maxx = x0;
    if (x1 < minx) minx = x1;  if (x1 > maxx) maxx = x1;
    if (x2 < minx) minx = x2;  if (x2 > maxx) maxx = x2;
    int miny = y0, maxy = y0;
    if (y1 < miny) miny = y1;  if (y1 > maxy) maxy = y1;
    if (y2 < miny) miny = y2;  if (y2 > maxy) maxy = y2;
    if (minx < 0)         minx = 0;
    if (miny < 0)         miny = 0;
    if (maxx >= QSPI_LCD_W) maxx = QSPI_LCD_W - 1;
    if (maxy >= QSPI_LCD_H) maxy = QSPI_LCD_H - 1;
    if (minx > maxx || miny > maxy) return;

    /* 三条边隐式方程 f(x,y)=a*x+b*y+c（像素坐标差 <= 480，中间量 int 足够） */
    int a0 = y1 - y0, b0 = x0 - x1;
    int a1 = y2 - y1, b1 = x1 - x2;
    int a2 = y0 - y2, b2 = x2 - x0;
    int c0 = -(a0 * x0 + b0 * y0);
    int c1 = -(a1 * x1 + b1 * y1);
    int c2 = -(a2 * x2 + b2 * y2);

    /* 重心（顶点平均，非退化三角形必在内部）处各边函数符号 */
    int gx = (x0 + x1 + x2) / 3;
    int gy = (y0 + y1 + y2) / 3;
    int s0 = (a0 * gx + b0 * gy + c0) >= 0 ? 1 : -1;
    int s1 = (a1 * gx + b1 * gy + c1) >= 0 ? 1 : -1;
    int s2 = (a2 * gx + b2 * gy + c2) >= 0 ? 1 : -1;

    lcd_wait_ready();
    lcd_set_window((uint16_t)minx, (uint16_t)miny, (uint16_t)maxx, (uint16_t)maxy);

    lcd_start_write_data();
    for (int yy = miny; yy <= maxy; yy++) {
        for (int xx = minx; xx <= maxx; xx++) {
            int e0 = a0 * xx + b0 * yy + c0;
            int e1 = a1 * xx + b1 * yy + c1;
            int e2 = a2 * xx + b2 * yy + c2;
            uint16_t c = (e0 * s0 >= 0 && e1 * s1 >= 0 && e2 * s2 >= 0) ? fill : out;
            lcd_write_pixel16_fast(c);
        }
    }
    lcd_end_write_data();
}

/**
 * @brief OSPI/SPI 外设与引脚初始化（与模板 LCDSPIInit + LcdGPIOInit 一致）
 */
static void lcd_hw_init(void)
{
    /* 数据线引脚复用：SCK=PA3, D0~D3=PB4~PB7 */
    csi_pin_set_mux(PORTA, PIN3, PA3_SPI1_SCK);
    csi_pin_set_mux(PORTB, PIN4, PB4_SPI1_D0);
    csi_pin_set_mux(PORTB, PIN5, PB5_SPI1_D1);
    csi_pin_set_mux(PORTB, PIN6, PB6_SPI1_D2);
    csi_pin_set_mux(PORTB, PIN7, PB7_SPI1_D3);

    /* 上拉配置 */
    csi_pin_mode(PORTA, PIN3, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN4, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN5, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN6, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN7, GPIO_MODE_PULLUP);

    /* CS 片选 PA2 */
    csi_pin_set_mux(PORTA, PIN2, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_cs, 0);
    csi_gpio_write(&lcd_cs, PIN2, 1);
    csi_gpio_dir(&lcd_cs, PIN2, GPIO_DIRECTION_OUTPUT);

    /* WAIT 忙信号 PB10 输入 */
    csi_pin_set_mux(PORTB, PIN10, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_wait, 1);
    csi_gpio_dir(&lcd_wait, PIN10, GPIO_DIRECTION_INPUT);

    /* RST 复位 PB15 输出 */
    csi_pin_set_mux(PORTB, PIN15, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_rst, 1);
    csi_gpio_dir(&lcd_rst, PIN15, GPIO_DIRECTION_OUTPUT);

    /* BL 背光 PA31 输出（高电平点亮） */
    csi_pin_set_mux(PORTA, PIN31, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_bl, 0);
    csi_gpio_write(&lcd_bl, PIN31, 0);
    csi_gpio_dir(&lcd_bl, PIN31, GPIO_DIRECTION_OUTPUT);

    /* OSPI 基础配置（初始即标准 SPI 8bit 模式） */
    csi_ospi_init(&ospi, 0);
    csi_ospi_mode(&ospi, OSPI_MASTER);
    csi_ospi_baud(&ospi, 120 * 100000);
    csi_ospi_cp_format(&ospi, OSPI_FORMAT_CPOL0_CPHA0);

    /* 传输参数：无地址/无指令，数据 8bit 单线只发 */
    csi_ospi_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.address.bus_width  = OSPI_LINE_SINGLE;
    cmd.address.disabled   = 1;
    cmd.address.size       = OSPI_ADDRESS_24_BITS;
    cmd.alt.disabled       = true;
    cmd.data.bus_width     = OSPI_LINE_SINGLE;
    cmd.data.disabled      = 0;
    cmd.data.frame_len     = 8;
    cmd.data.transfer_mode = OSPI_TRANSFER_SEND_ONLY;
    cmd.ddr_enable         = 0;
    cmd.dummy_count        = 0;
    cmd.instruction.disabled  = 1;
    cmd.instruction.bus_width = OSPI_LINE_SINGLE;
    cmd.instruction.size     = OSPI_INSTRUCTION_8_BITS;
    csi_ospi_config(&ospi, &cmd);

    dw_ospi_disable(DWOSPI);
    dw_ospi_set_tx_mode(DWOSPI);
    dw_ospi_enable(DWOSPI);
    csi_ospi_select_slave(&ospi, 0);
}

/**
 * @brief LCD 初始化入口：硬件配置 -> 复位 -> 背光 -> 初始化命令
 *
 * 注：复位/等待时长取自模板原值，若觉得上电黑屏太久可自行缩短。
 */
void qspi_lcd_init(void)
{
    lcd_hw_init();

    /* 点亮背光 */
    BLK_HI();

    /* LCD 复位时序（模板验证值，勿缩短：实测缩短后上电不亮） */
    SET_CS1();
    RST_HI();
    mdelay(100);
    RST_LOW();
    mdelay(100);
    RST_HI();
    mdelay(2000);

    /* 初始化命令：0x20 / 0x64（模板验证可用） */
    uint8_t param_data[1];
    param_data[0] = 0x64;
    lcd_cmd_write(0x20, param_data, 1);
    mdelay(1000);

    printf("[qspi_lcd] init done, %dx%d\r\n", QSPI_LCD_W, QSPI_LCD_H);

    /* 三色全屏自检（可注释掉，不影响表情动画） */
    qspi_fill_rect(0, 0, QSPI_LCD_W, QSPI_LCD_H, 0xF800);   mdelay(100);
    qspi_fill_rect(0, 0, QSPI_LCD_W, QSPI_LCD_H, 0x07E0);   mdelay(100);
    qspi_fill_rect(0, 0, QSPI_LCD_W, QSPI_LCD_H, 0x001F);   mdelay(100);
}
