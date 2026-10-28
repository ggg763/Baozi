/******************************************************************************
 * lcd_test.c - Screen test: basic facial expression animations
 *
 * HW: TD1601 EVB + bundled display (ST7789, OSPI 8-bit, X7A header)
 * DRV: UserFolder/LCD.c (ported from official example)
 *
 * Wiring (from official ospi_8line_polling example):
 *   D0~D7 = PB4~PB11, SCK = PA28 (OSPI0)
 *   CS = PA15, DC = PA6, LED = PA8, RST = PA7
 *
 * Usage: after flashing, the screen loops 6 expressions (1 s each):
 *   happy -> sad -> angry -> surprised -> loving -> neutral
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <soc.h>
#include <drv/ospi.h>
#include <drv/pin.h>
#include <drv/gpio.h>
#include <drv/common.h>     /* mdelay */
#include "board_config.h"
#include "board_init.h"
#include "LCD.h"

/* ==================================================================
 * OSPI + LCD control pins (ported from example_ospi_8line_polling.c)
 * ================================================================== */
static csi_ospi_t ospi;
static csi_ospi_command_t cmd;
static csi_gpio_t gpio_cs;
static csi_gpio_t gpio_dc;
static csi_gpio_t gpio_led;
static csi_gpio_t gpio_rst;

/* Pin-control callbacks declared in LCD.h (CS/DC/LED/RST) */
int ioctl(uint32_t value)     { csi_gpio_write(&gpio_cs,  (1U << 15), value); return 0; }
int dc_ioctl(uint32_t value)  { csi_gpio_write(&gpio_dc,  (1U << 6),  value); return 0; }
int led_ioctl(uint32_t value) { csi_gpio_write(&gpio_led, (1U << 8),  value); return 0; }
int rst_ioctl(uint32_t value) { csi_gpio_write(&gpio_rst, (1U << 7),  value); return 0; }

/* OSPI write register/data (bound to LCD_WR_REG / LCD_WR_DATA) */
static void LCD_WR_REG_OSPI(uint16_t data)
{
    LCD_RS_CLR;
    LCD_CS_CLR;
    cmd.instruction.value = data;
    csi_ospi_send(&ospi, &data, 0, 1000);
    LCD_CS_SET;
}

static void LCD_WR_DATA_OSPI(uint16_t data)
{
    LCD_RS_SET;
    LCD_CS_CLR;
    cmd.instruction.value = data;
    csi_ospi_send(&ospi, &data, 0, 1000);
    LCD_CS_SET;
}

/* OSPI data lines pinmux (D0~D7 = PB4~PB11, SCK = PA28) */
static void lcd_pin_init(void)
{
    csi_pin_set_mux(PORTB, PIN4,  PB4_SPI1_D0);
    csi_pin_set_mux(PORTB, PIN5,  PB5_SPI1_D1);
    csi_pin_set_mux(PORTB, PIN6,  PB6_SPI1_D2);
    csi_pin_set_mux(PORTB, PIN7,  PB7_SPI1_D3);
    csi_pin_set_mux(PORTB, PIN8,  PB8_SPI1_D4);
    csi_pin_set_mux(PORTB, PIN9,  PB9_SPI1_D5);
    csi_pin_set_mux(PORTB, PIN10, PB10_SPI1_D6);
    csi_pin_set_mux(PORTB, PIN11, PB11_SPI1_D7);
    csi_pin_set_mux(PORTA, PIN28, PA28_SPI1_SCK);

    csi_pin_mode(PORTB, PIN4,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN5,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN6,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN7,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN8,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN9,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN10, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN11, GPIO_MODE_PULLUP);
}

/* Control pins as GPIO output */
static void soft_ctrl_pin_init(void)
{
    csi_pin_set_mux(PORTA, PIN15, PIN_FUNC_GPIO);
    csi_gpio_init(&gpio_cs, 0);
    csi_gpio_write(&gpio_cs, (1U << 15), 1);
    csi_gpio_dir(&gpio_cs, (1U << 15), GPIO_DIRECTION_OUTPUT);

    csi_pin_set_mux(PORTA, PIN6, PIN_FUNC_GPIO);
    csi_gpio_init(&gpio_dc, 0);
    csi_gpio_dir(&gpio_dc, (1U << 6), GPIO_DIRECTION_OUTPUT);

    csi_pin_set_mux(PORTA, PIN8, PIN_FUNC_GPIO);
    csi_gpio_init(&gpio_led, 0);
    csi_gpio_dir(&gpio_led, (1U << 8), GPIO_DIRECTION_OUTPUT);

    csi_pin_set_mux(PORTA, PIN7, PIN_FUNC_GPIO);
    csi_gpio_init(&gpio_rst, 0);
    csi_gpio_dir(&gpio_rst, (1U << 7), GPIO_DIRECTION_OUTPUT);
}

/* OSPI init (25 MHz, octal send) */
static int ospi_lcd_init(void)
{
    lcd_pin_init();
    soft_ctrl_pin_init();

    if (csi_ospi_init(&ospi, 0) != CSI_OK) {
        return -1;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.data.transfer_mode = OSPI_TRANSFER_SEND_ONLY;
    cmd.data.frame_len = 8;
    cmd.data.bus_width = OSPI_LINE_OCTAL;
    cmd.data.disabled = false;
    cmd.address.disabled = true;
    cmd.address.bus_width = OSPI_LINE_OCTAL;
    cmd.address.size = OSPI_ADDRESS_0_BITS;
    cmd.alt.disabled = true;
    cmd.ddr_enable = false;
    cmd.instruction.disabled = false;
    cmd.instruction.bus_width = OSPI_LINE_OCTAL;
    cmd.instruction.size = OSPI_INSTRUCTION_8_BITS;
    cmd.dummy_count = 0;
    csi_ospi_config(&ospi, &cmd);

    csi_ospi_cp_format(&ospi, OSPI_FORMAT_CPOL1_CPHA1);
    csi_ospi_baud(&ospi, 25U * 1000U * 1000U);
    csi_ospi_select_slave(&ospi, 0);

    /* Bind LCD low-level write functions to the OSPI implementation */
    LCD_WR_REG  = LCD_WR_REG_OSPI;
    LCD_WR_DATA = LCD_WR_DATA_OSPI;

    LCD_Init();               /* ST7789 init sequence */
    LCD_Display_Dir(0);       /* portrait */
    LCD_Clear(WHITE);

    return 0;
}

/* ==================================================================
 * Expression drawing (basic primitives: filled circle / lines)
 * ================================================================== */

/* Filled circle */
static void fill_circle(int cx, int cy, int r, uint16_t color)
{
    int x, y;
    for (y = cy - r; y <= cy + r; y++) {
        for (x = cx - r; x <= cx + r; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                LCD_Fast_DrawPoint((uint16_t)x, (uint16_t)y, color);
            }
        }
    }
}

/* Base face: skin circle + two eyes + optional angry brows */
static void draw_face_base(uint16_t w, uint16_t h, int cx, int cy, int r,
                           int eye_dx, int eye_cy, int eye_r, int angry_brow)
{
    /* face (skin color) */
    fill_circle(cx, cy, r, 0xFCA0);

    /* eyes */
    fill_circle(cx - eye_dx, eye_cy, eye_r, BLACK);
    fill_circle(cx + eye_dx, eye_cy, eye_r, BLACK);

    /* angry/surprised brows (slanted lines, color from global POINT_COLOR) */
    if (angry_brow) {
        POINT_COLOR = BLACK;
        LCD_DrawLine((uint16_t)(cx - eye_dx - eye_r), (uint16_t)(eye_cy - eye_r * 2),
                     (uint16_t)(cx - eye_dx + eye_r), (uint16_t)(eye_cy - eye_r));
        LCD_DrawLine((uint16_t)(cx + eye_dx - eye_r), (uint16_t)(eye_cy - eye_r),
                     (uint16_t)(cx + eye_dx + eye_r), (uint16_t)(eye_cy - eye_r * 2));
    }
}

/* Mouth: 3-segment polyline (up=1 frown, up=0 smile, flat=1 straight) */
static void draw_mouth(int cx, int cy, int r, int up, int flat)
{
    int x0 = cx - r / 2;
    int x1 = cx + r / 2;
    int mid = (up ? cy - r / 4 : cy + r / 4);

    POINT_COLOR = BLACK;
    if (flat) {
        LCD_DrawLine((uint16_t)x0, (uint16_t)(cy + r / 4),
                     (uint16_t)x1, (uint16_t)(cy + r / 4));
    } else {
        LCD_DrawLine((uint16_t)x0, (uint16_t)(cy + r / 4),
                     (uint16_t)cx, (uint16_t)mid);
        LCD_DrawLine((uint16_t)cx, (uint16_t)mid,
                     (uint16_t)x1, (uint16_t)(cy + r / 4));
    }
}

/* ---- 6 expressions ---- */
static void draw_happy(uint16_t w, uint16_t h)
{
    int cx = w / 2, cy = h / 2, r = h / 5;
    draw_face_base(w, h, cx, cy, r, r / 2, cy - r / 4, r / 6, 0);
    draw_mouth(cx, cy + r / 4, r, 0, 0);      /* smile */
}

static void draw_sad(uint16_t w, uint16_t h)
{
    int cx = w / 2, cy = h / 2, r = h / 5;
    draw_face_base(w, h, cx, cy, r, r / 2, cy - r / 4, r / 6, 0);
    draw_mouth(cx, cy + r / 4, r, 1, 0);      /* frown */
}

static void draw_angry(uint16_t w, uint16_t h)
{
    int cx = w / 2, cy = h / 2, r = h / 5;
    draw_face_base(w, h, cx, cy, r, r / 2, cy - r / 4, r / 6, 1);  /* with brows */
    draw_mouth(cx, cy + r / 4, r, 1, 1);      /* flat mouth */
}

static void draw_surprised(uint16_t w, uint16_t h)
{
    int cx = w / 2, cy = h / 2, r = h / 5;
    draw_face_base(w, h, cx, cy, r, r / 2, cy - r / 4, r / 8, 0);  /* bigger eyes */
    fill_circle(cx, cy + r / 3, r / 5, BLACK);  /* O-shaped mouth */
}

static void draw_loving(uint16_t w, uint16_t h)
{
    int cx = w / 2, cy = h / 2, r = h / 5;
    int eye_r = r / 5, eye_dx = r / 2, eye_cy = cy - r / 4;

    /* face */
    fill_circle(cx, cy, r, 0xFCA0);

    /* heart-shaped eyes (two circles + tip) */
    fill_circle(cx - eye_dx, eye_cy, eye_r, RED);
    fill_circle(cx - eye_dx + eye_r / 2, eye_cy + eye_r / 3, eye_r, RED);
    fill_circle(cx + eye_dx, eye_cy, eye_r, RED);
    fill_circle(cx + eye_dx - eye_r / 2, eye_cy + eye_r / 3, eye_r, RED);

    draw_mouth(cx, cy + r / 4, r, 0, 0);      /* smile */
}

static void draw_neutral(uint16_t w, uint16_t h)
{
    int cx = w / 2, cy = h / 2, r = h / 5;
    draw_face_base(w, h, cx, cy, r, r / 2, cy - r / 4, r / 6, 0);
    draw_mouth(cx, cy + r / 4, r, 0, 1);      /* straight mouth */
}

/* ==================================================================
 * Test entry (called from src/main.c)
 * ================================================================== */
void lcd_test(void)
{
    if (ospi_lcd_init() != 0) {
        printf("LCD init failed\n");
        return;
    }

    /* Solid-color self test: white/black/red/green/blue */
    LCD_Clear(WHITE);   mdelay(500);
    LCD_Clear(BLACK);   mdelay(500);
    LCD_Clear(RED);     mdelay(500);
    LCD_Clear(GREEN);   mdelay(500);
    LCD_Clear(BLUE);    mdelay(500);

    /* Expression animation loop */
    while (1) {
        draw_happy(lcddev.width, lcddev.height);
        mdelay(1000);
        draw_sad(lcddev.width, lcddev.height);
        mdelay(1000);
        draw_angry(lcddev.width, lcddev.height);
        mdelay(1000);
        draw_surprised(lcddev.width, lcddev.height);
        mdelay(1000);
        draw_loving(lcddev.width, lcddev.height);
        mdelay(1000);
        draw_neutral(lcddev.width, lcddev.height);
        mdelay(1000);
    }
}
