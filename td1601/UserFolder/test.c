/******************************************************************************
 * test.c - Lightweight ST7701S screen test (OSPI 8-bit)
 *
 * HW   : TD1601 EVB + TL040WVS14CT (4" square, ST7701S driver, 480x480)
 * IF   : OSPI 8-line data + GPIO control (from board schematic / pin map)
 *        D0~D7 = PB4~PB11, CLK = PA28 (SPI1 mux)
 *        CS = PA15, DC = PA6, RST = PA7, BL = PA8 (GPIO)
 *
 * This is a self-contained minimal driver/test: OSPI init, ST7701S init
 * sequence, then loops solid colors -> color bars -> simple pattern.
 *
 * NOTE: ST7701S register values below are a common reference set.
 *       If the panel shows white/garbled, adjust panel-specific regs
 *       (gamma/power) against the panel vendor demo.
 *
 * Entry: void test_lcd(void)  -- called from src/main.c
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

/* ---- LCD control pins (GPIO) ---- */
#define LCD_PIN_CS   15      /* PA15 */
#define LCD_PIN_DC   6       /* PA6  */
#define LCD_PIN_RST  7       /* PA7  */
#define LCD_PIN_BL   8       /* PA8  */

/* ---- OSPI handles ---- */
static csi_ospi_t ospi;
static csi_ospi_command_t cmd;
static csi_gpio_t gpio;

/* ---- GPIO helpers ---- */
static void lcd_cs(int v)  { csi_gpio_write(&gpio, 1U << LCD_PIN_CS,  v); }
static void lcd_dc(int v)  { csi_gpio_write(&gpio, 1U << LCD_PIN_DC,  v); }
static void lcd_rst(int v) { csi_gpio_write(&gpio, 1U << LCD_PIN_RST, v); }
static void lcd_bl(int v)  { csi_gpio_write(&gpio, 1U << LCD_PIN_BL,  v); }

/* ---- OSPI low-level send (one 8-bit item) ---- */
static void ospi_byte(uint16_t data)
{
    cmd.instruction.value = data;
    csi_ospi_send(&ospi, &data, 0, 1000);
}

/* ---- Write command / data (DC distinguishes) ---- */
static void lcd_cmd(uint8_t c)
{
    lcd_dc(0);
    lcd_cs(0);
    ospi_byte(c);
    lcd_cs(1);
}

static void lcd_data8(uint8_t d)
{
    lcd_dc(1);
    lcd_cs(0);
    ospi_byte(d);
    lcd_cs(1);
}

static void lcd_cmd_params(uint8_t c, const uint8_t *p, uint8_t n)
{
    uint8_t i;
    lcd_cmd(c);
    for (i = 0; i < n; i++) {
        lcd_data8(p[i]);
    }
}

/* ---- OSPI data lines pinmux (QSPI 4-wire: D0~D3 = PB4~PB7, CLK = PA28) ---- */
static void lcd_pin_init(void)
{
    csi_pin_set_mux(PORTB, PIN4,  PB4_SPI1_D0);
    csi_pin_set_mux(PORTB, PIN5,  PB5_SPI1_D1);
    csi_pin_set_mux(PORTB, PIN6,  PB6_SPI1_D2);
    csi_pin_set_mux(PORTB, PIN7,  PB7_SPI1_D3);
    csi_pin_set_mux(PORTA, PIN28, PA28_SPI1_SCK);

    /* pullup on data lines (align with official example) */
    csi_pin_mode(PORTB, PIN4, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN5, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN6, GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN7, GPIO_MODE_PULLUP);

    /* control pins as GPIO */
    csi_pin_set_mux(PORTA, PIN15, PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTA, PIN6,  PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTA, PIN7,  PIN_FUNC_GPIO);
    csi_pin_set_mux(PORTA, PIN8,  PIN_FUNC_GPIO);

    csi_gpio_init(&gpio, PORTA);
    csi_gpio_dir(&gpio, (1U << LCD_PIN_CS) | (1U << LCD_PIN_DC) |
                        (1U << LCD_PIN_RST) | (1U << LCD_PIN_BL),
                 GPIO_DIRECTION_OUTPUT);
    lcd_cs(1);
    lcd_bl(1);
}

static void ospi_lcd_init(void)
{
    lcd_pin_init();

    if (csi_ospi_init(&ospi, 0) != CSI_OK) {
        printf("OSPI init failed\n");
        return;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.data.transfer_mode = OSPI_TRANSFER_SEND_ONLY;
    cmd.data.frame_len = 8;
    cmd.data.bus_width = OSPI_LINE_QUAD;    /* QSPI 4-wire data */
    cmd.data.disabled = false;
    cmd.address.disabled = true;
    cmd.address.bus_width = OSPI_LINE_QUAD;
    cmd.address.size = OSPI_ADDRESS_0_BITS;
    cmd.alt.disabled = true;
    cmd.ddr_enable = false;
    cmd.instruction.disabled = false;
    cmd.instruction.bus_width = OSPI_LINE_SINGLE;  /* command on 1 wire */
    cmd.instruction.size = OSPI_INSTRUCTION_8_BITS;
    cmd.dummy_count = 0;
    csi_ospi_config(&ospi, &cmd);

    csi_ospi_cp_format(&ospi, OSPI_FORMAT_CPOL1_CPHA1);
    csi_ospi_baud(&ospi, 25U * 1000U * 1000U);
    csi_ospi_select_slave(&ospi, 0);
}

/* ---- ST7701S init sequence ----
 * Reference values from ST7701S 480x480 production panels (M5Stack class).
 * Panel-specific gamma/power may need tuning; core wake-up is 0x11/0x29.
 */
static void st7701s_init(void)
{
    uint8_t p[16];

    /* hardware reset + backlight */
    lcd_rst(1); mdelay(10);
    lcd_rst(0); mdelay(10);
    lcd_rst(1); mdelay(120);
    lcd_bl(1);

    /* ---- unlock vendor area BK0 ---- */
    { uint8_t b[] = {0x77,0x01,0x00,0x00,0x10}; lcd_cmd_params(0xFF, b, 5); }

    /* panel setting (480x480) */
    { uint8_t b[] = {0x3B,0x00}; lcd_cmd_params(0xC0, b, 2); }   /* LNSET */
    { uint8_t b[] = {0x0D,0x02}; lcd_cmd_params(0xC1, b, 2); }
    { uint8_t b[] = {0x31,0x05}; lcd_cmd_params(0xC2, b, 2); }
    { uint8_t b[] = {0x08};      lcd_cmd_params(0xCD, b, 1); }

    /* gamma BK0 (reference, tune if color off) */
    { uint8_t b[] = {0x00,0x0D,0x17,0x2C,0x4A,0x53,0x6A,0x78,
                     0x86,0x86,0x83,0x8C,0x75,0x68,0x6C,0x0C};
      lcd_cmd_params(0xB0, b, 16); lcd_cmd_params(0xB1, b, 16); }

    /* ---- power BK1 ---- */
    { uint8_t b[] = {0x77,0x01,0x00,0x00,0x11}; lcd_cmd_params(0xFF, b, 5); }
    { uint8_t b[] = {0x60}; lcd_cmd_params(0xB0, b, 1); }
    { uint8_t b[] = {0x32}; lcd_cmd_params(0xB1, b, 1); }
    { uint8_t b[] = {0x07}; lcd_cmd_params(0xB2, b, 1); }
    { uint8_t b[] = {0x80}; lcd_cmd_params(0xB3, b, 1); }
    { uint8_t b[] = {0x49}; lcd_cmd_params(0xB5, b, 1); }
    { uint8_t b[] = {0x85}; lcd_cmd_params(0xB7, b, 1); }
    { uint8_t b[] = {0x21}; lcd_cmd_params(0xB8, b, 1); }
    { uint8_t b[] = {0x78}; lcd_cmd_params(0xC1, b, 1); }
    { uint8_t b[] = {0x78}; lcd_cmd_params(0xC2, b, 1); }

    /* timing E0~E8 (reference) */
    { uint8_t b[] = {0x00,0x00,0x02}; lcd_cmd_params(0xE0, b, 3); }
    { uint8_t b[] = {0x0B,0x00,0x0A,0x00,0x07,0x00,0x09,0x00,0x00,0x00,0x00,0x00};
      lcd_cmd_params(0xE1, b, 12); }
    { uint8_t b[] = {0x00,0x00,0x0D,0x00,0x0B,0x00,0x09,0x00,0x00,0x00,0x00,0x00};
      lcd_cmd_params(0xE2, b, 12); }
    { uint8_t b[] = {0x00,0x00,0x02}; lcd_cmd_params(0xE3, b, 3); }
    { uint8_t b[] = {0x0B,0x00,0x0A,0x00,0x07,0x00,0x09,0x00,0x00,0x00,0x00,0x00};
      lcd_cmd_params(0xE4, b, 12); }
    { uint8_t b[] = {0x00,0x00,0x0D,0x00,0x0B,0x00,0x09,0x00,0x00,0x00,0x00,0x00};
      lcd_cmd_params(0xE5, b, 12); }
    { uint8_t b[] = {0x00,0x00,0x02}; lcd_cmd_params(0xE6, b, 3); }
    { uint8_t b[] = {0x0B,0x00,0x0A,0x00,0x07,0x00,0x09,0x00,0x00,0x00,0x00,0x00};
      lcd_cmd_params(0xE7, b, 12); }
    { uint8_t b[] = {0x00,0x00,0x0D,0x00,0x0B,0x00,0x09,0x00,0x00,0x00,0x00,0x00};
      lcd_cmd_params(0xE8, b, 12); }

    /* ---- back to standard command area ---- */
    { uint8_t b[] = {0x77,0x01,0x00,0x00,0x00}; lcd_cmd_params(0xFF, b, 5); }

    lcd_cmd(0x21);                       /* INVON (IPS) */
    { uint8_t b[] = {0x60}; lcd_cmd_params(0x3A, b, 1); }   /* RGB666 */
    { uint8_t b[] = {0x00}; lcd_cmd_params(0x36, b, 1); }   /* MADCTL */

    lcd_cmd(0x11); mdelay(120);          /* SLPOUT */
    lcd_cmd(0x29);                       /* DISPON */
}

/* ---- drawing primitives (RGB666, 3 bytes/pixel) ---- */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t b[4];
    b[0] = x0 >> 8; b[1] = x0 & 0xFF; b[2] = x1 >> 8; b[3] = x1 & 0xFF;
    lcd_cmd_params(0x2A, b, 4);          /* CASET */
    b[0] = y0 >> 8; b[1] = y0 & 0xFF; b[2] = y1 >> 8; b[3] = y1 & 0xFF;
    lcd_cmd_params(0x2B, b, 4);          /* RASET */
    lcd_cmd(0x2C);                       /* RAMWR */
}

/* fill window with one color (RGB666: 3 bytes per pixel) */
static void lcd_fill_color(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                           uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixels = (uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1);
    uint8_t buf[256];
    uint32_t n = pixels * 3;
    uint32_t chunk = sizeof(buf);
    uint32_t i;

    lcd_set_window(x0, y0, x1, y1);

    /* fill buffer with one color, then stream it */
    for (i = 0; i + 2 < chunk; i += 3) {
        buf[i] = r; buf[i+1] = g; buf[i+2] = b;
    }
    while (n > 0) {
        uint32_t send = (n < chunk) ? n : chunk;
        lcd_dc(1); lcd_cs(0);
        csi_ospi_send(&ospi, buf, send, 1000);
        lcd_cs(1);
        n -= send;
    }
}

/* color bars: 8 vertical strips */
static void lcd_color_bars(void)
{
    static const uint8_t bars[8][3] = {
        {255,0,0},{0,255,0},{0,0,255},{255,255,0},
        {0,255,255},{255,0,255},{255,255,255},{0,0,0}
    };
    uint16_t w = 480, h = 480, i;
    for (i = 0; i < 8; i++) {
        lcd_fill_color(i * 60, 0, i * 60 + 59, h - 1, bars[i][0], bars[i][1], bars[i][2]);
    }
}

/* simple pattern: two squares + border (quick visual check) */
static void lcd_pattern(void)
{
    lcd_fill_color(0, 0, 479, 479, 255, 255, 255);   /* white bg */
    lcd_fill_color(40, 40, 199, 199, 255, 0, 0);     /* red square */
    lcd_fill_color(280, 40, 439, 199, 0, 255, 0);    /* green square */
    lcd_fill_color(40, 280, 199, 439, 0, 0, 255);    /* blue square */
    lcd_fill_color(280, 280, 439, 439, 255, 255, 0); /* yellow square */
}

/* ==================================================================
 * Test entry (called from src/main.c)
 * ================================================================== */
void test_lcd(void)
{
    ospi_lcd_init();
    st7701s_init();

    printf("ST7701S test running\n");

    while (1) {
        lcd_fill_color(0, 0, 479, 479, 255, 0, 0);   mdelay(800);
        lcd_fill_color(0, 0, 479, 479, 0, 255, 0);   mdelay(800);
        lcd_fill_color(0, 0, 479, 479, 0, 0, 255);   mdelay(800);
        lcd_fill_color(0, 0, 479, 479, 255, 255, 255); mdelay(800);
        lcd_color_bars();                            mdelay(1500);
        lcd_pattern();                               mdelay(2000);
    }
}
