/*
 * SDL_video.c
 * LCD OSPI(QSPI) Çý¶¯ + DMA ´«Êä
 */
#include "video.h"
#include <drv/ospi.h>
#include <drv/pin.h>
#include <drv/pwm.h>
#include <drv/spi.h>
#include <dw_dma_ll.h>
#include <dw_ospi_ll.h>
#include <dw_spi_ll.h>
#include <string.h>

// LCD QSPI °æ±¾Ñ¡Ôñ
#define LCD_QSPI_TR230S 1
#define LCD_QSPI_TR220S 0
// µ÷ÊÔ¿ª¹Ø£º1-¿ªÆô´òÓ¡µ÷ÊÔ£¬0-¹Ø±ÕËùÓÐµ÷ÊÔÊä³ö
#define LCD_DEBUG          0
// Ê¹ÄÜ OSPI DMA ´«Êä
#define CONFIG_USE_OSPI_DMA 1
// DMA Í¨µÀºÅ
#define CH_IDX             2

// LCD Ó²¼þIO¶¨Òå
static csi_gpio_t lcd_cs;
static csi_gpio_t lcd_dc;
static csi_gpio_t lcd_rst;
static csi_gpio_t lcd_wait;
static csi_gpio_t lcd_bl;

// ¸´Î»Òý½Å¿ØÖÆ PB15£¨À©Õ¹°å CON2: LCD_RST£©
#define RST_LOW()    csi_gpio_write(&lcd_rst, PIN15, 0)
#define RST_HI()     csi_gpio_write(&lcd_rst, PIN15, 1)
// Æ¬Ñ¡Òý½Å¿ØÖÆ PA2£¨À©Õ¹°å CON2: SPI_CS£©
#define CLR_CS1()    csi_gpio_write(&lcd_cs, PIN2, 0)
#define SET_CS1()    csi_gpio_write(&lcd_cs, PIN2, 1)
// ±³¹â¿ØÖÆ PA31£¨À©Õ¹°å CON2 ¸½½ü LCD_BL£©£¬¸ßµçÆ½µãÁÁ
#define BLK_LOW()    csi_gpio_write(&lcd_bl, PIN31, 0)
#define BLK_HI()     csi_gpio_write(&lcd_bl, PIN31, 1)

// ÍâÉè»ùµØÖ·
#define DWOSPI       ((dw_ospi_regs_t *)DW_OSPI0_BASE)
#define DWDMA        ((dw_dma_regs_t *)DW_DMA_BASE)
#define DWDMA_CH     ((dw_dma_ch_regs_t *)(DW_DMA_BASE + (CH_IDX * 0x58)))

// OSPI & DMA ¾ä±ú
static csi_ospi_t        ospi1;
static csi_dma_ch_t      lcdtx_ch;
static csi_ospi_command_t cmd;

// ×´Ì¬±äÁ¿
static volatile uint32_t fill_count        = 0;
static volatile bool      ospi_is_transmitting = false;

extern int frame_count;

/**
 * @brief ±ê×¼SPIÄ£Ê½µ¥×Ö½Ú·¢ËÍ(8bit)
 * @param num ´ý·¢ËÍ×Ö½ÚÊý¾Ý
 */
#define TFT_SPI_Write_Byte(num)                                                \
do {                                                                           \
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));                                     \
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);                                       \
    dw_ospi_disable(DWOSPI);                                                   \
    dw_ospi_set_spi_frame_format_std_mode(DWOSPI);                             \
    dw_ospi_config_ctl_frame_len(DWOSPI, 8 - 1);                               \
    csi_ospi_baud(&ospi1, 120 * 100000);                                       \
    dw_ospi_enable(DWOSPI);                                                    \
    DWOSPI->DR = num;                                                          \
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));                                     \
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);                                       \
} while (0)

/**
 * @brief ¿ìËÙ16bitÊý¾Ý·¢ËÍ(QSPIÄ£Ê½×¨ÓÃ)
 * @param num ´ý·¢ËÍ16Î»ÑÕÉ«Êý¾Ý
 */
void LCD_WritemutileData16_fast(uint16_t num)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);
    DWOSPI->DR = num;
}

/**
 * @brief Æô¶¯ÅúÁ¿Êý¾Ý´«Êä£ºÇÐ»»QSPI 16bitÄ£Ê½
 */
void LCD_startWriteMutileData(void)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);

    // 先切换标准 SPI 发指令头
    dw_ospi_disable(DWOSPI);
    csi_ospi_baud(&ospi1, 500 * 100000);
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

    // ÇÐ»»ÎªËÄÏßQSPI¡¢16bitÖ¡¸ñÊ½ÓÃÓÚÑÕÉ«´«Êä
    dw_ospi_disable(DWOSPI);
    dw_ospi_set_spi_frame_format_quad_mode(DWOSPI);
    *(volatile uint32_t *)0x6000c0f4 = 0x302;
    dw_ospi_config_ctl_frame_len(DWOSPI, 16 - 1);
    dw_ospi_enable(DWOSPI);
}

/**
 * @brief ½áÊøÅúÁ¿Êý¾Ý´«Êä£¬»Ö¸´±ê×¼SPIÄ£Ê½
 */
/* 等 OSPI 真正空闲（TX FIFO 空 + 移位寄存器不忙）。
 *
 * 为什么需要单独一个函数：
 *   LCD_DmaSend() 末尾的 while (!DWDMA->RawBlock) 只保证【DMA 把数据搬进
 *   SPI FIFO】，不代表 SPI 已经把最后一个 bit 移出去。整屏刷新是 60 段循环，
 *   若不等就覆写源缓冲并发起下一次 DMA，正在移位的数据可能与新数据交错，
 *   屏幕上就是错位的白色块/白条（眨眼时背景大面积全黑，最刺眼）。
 *   本函数把“等 SPI 空闲”暴露出来，让渲染端在每段之间调用。
 */
void LCD_WaitIdle(void)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);
}

void LCD_endWriteMutileData(void)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);

    SET_CS1();
    dw_ospi_disable(DWOSPI);
    dw_ospi_set_spi_frame_format_std_mode(DWOSPI);
    dw_ospi_config_ctl_frame_len(DWOSPI, 8 - 1);
    dw_ospi_enable(DWOSPI);

    frame_count++;
}

/**
 * @brief OSPI ÊÂ¼þ»Øµ÷º¯Êý
 * @param ospi    OSPI¾ä±ú
 * @param event   ÖÐ¶ÏÊÂ¼þ
 * @param arg     ×Ô¶¨Òå²ÎÊý
 */
static void ospi_event_callback(csi_ospi_t *ospi, csi_ospi_event_t event, void *arg)
{
    switch (event)
    {
        case OSPI_EVENT_SEND_COMPLETE:
            LCD_endWriteMutileData();
            ospi_is_transmitting = false;
#if LCD_DEBUG
            printf("ospi send complete intr\r\n");
#endif
            break;
        case OSPI_EVENT_RECEIVE_COMPLETE:
        case OSPI_EVENT_SEND_RECEIVE_COMPLETE:
        default:
            break;
    }
}

/**
 * @brief OSPI/SPI ÍâÉè¼°DMA³õÊ¼»¯
 */
static void LCDSPIInit(void)
{
    // Òý½Å¸´ÓÃÅäÖÃ£¨ÊÊÅäÀ©Õ¹°å CON2 ×ßÏß£ºSCK=PA3£©
    csi_pin_set_mux(PORTA, PIN3,  PA3_SPI1_SCK);
    csi_pin_set_mux(PORTB, PIN4,  PB4_SPI1_D0);
    csi_pin_set_mux(PORTB, PIN5,  PB5_SPI1_D1);
    csi_pin_set_mux(PORTB, PIN6,  PB6_SPI1_D2);
    csi_pin_set_mux(PORTB, PIN7,  PB7_SPI1_D3);

    // ÉÏÀ­ÅäÖÃ
    csi_pin_mode(PORTA, PIN3,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN4,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN5,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN6,  GPIO_MODE_PULLUP);
    csi_pin_mode(PORTB, PIN7,  GPIO_MODE_PULLUP);

    // OSPI »ù´¡ÅäÖÃ
    csi_ospi_init(&ospi1, 0);
    csi_ospi_mode(&ospi1, OSPI_MASTER);
    csi_ospi_link_dma(&ospi1, &lcdtx_ch, NULL);
    csi_ospi_attach_callback(&ospi1, ospi_event_callback, NULL);
    csi_ospi_baud(&ospi1, 120 * 100000);
    csi_ospi_cp_format(&ospi1, SPI_FORMAT_CPOL0_CPHA0);

    // OSPI ´«Êä²ÎÊýÅäÖÃ
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
    csi_ospi_config(&ospi1, &cmd);

    // µ×²ãOSPI¼Ä´æÆ÷ÅäÖÃ
    dw_ospi_disable(DWOSPI);
    dw_ospi_set_tx_mode(DWOSPI);

#if CONFIG_USE_OSPI_DMA
    dw_ospi_config_dma_tx_data_level(DWOSPI, 0);
    dw_ospi_enable_tx_dma(DWOSPI);

    // DMAÍ¨µÀ¿ØÖÆ×ÖÅäÖÃ
    DWDMA_CH->CTL = (1 << 1) | (1 << 4) | (2 << 7) | (0 << 9) | (4 << 11) |
                    (4 << 14) | (1 << 20);

    dw_dma_set_dar(DWDMA_CH, (uint32_t)&(DWOSPI->DR));
    dw_dma_dst_hs_hw(DWDMA_CH);
    dw_dma_src_hs_sft(DWDMA_CH);
    dw_dma_set_dest_per(DWDMA_CH, 7);
    dw_dma_dis_ch_susp(DWDMA_CH);
#endif

    dw_ospi_enable(DWOSPI);
    csi_ospi_select_slave(&ospi1, 0);
}

/**
 * @brief LCD ¿ØÖÆGPIO³õÊ¼»¯(CS/RST/WAIT)
 */
static void LcdGPIOInit(void)
{
    // CS Æ¬Ñ¡ PA2£¨À©Õ¹°å CON2: SPI_CS£©
    csi_pin_set_mux(PORTA, PIN2, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_cs, 0);
    csi_gpio_write(&lcd_cs, PIN2, 1);
    csi_gpio_dir(&lcd_cs, PIN2, GPIO_DIRECTION_OUTPUT);

    // WAIT Ã¦ÐÅºÅ PB10 ÊäÈë£¨À©Õ¹°å CON2: WAIT£©
    csi_pin_set_mux(PORTB, PIN10, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_wait, 1);
    csi_gpio_dir(&lcd_wait, PIN10, GPIO_DIRECTION_INPUT);

    // RST ¸´Î» PB15 Êä³ö£¨À©Õ¹°å CON2: LCD_RST£©
    csi_pin_set_mux(PORTB, PIN15, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_rst, 1);
    csi_gpio_dir(&lcd_rst, PIN15, GPIO_DIRECTION_OUTPUT);

    // BLK ±³¹â PA31 Êä³ö£¨À©Õ¹°å LCD_BL£©£¬¸ßµçÆ½µãÁÁ
    csi_pin_set_mux(PORTA, PIN31, PIN_FUNC_GPIO);
    csi_gpio_init(&lcd_bl, 0);
    csi_gpio_write(&lcd_bl, PIN31, 0);
    csi_gpio_dir(&lcd_bl, PIN31, GPIO_DIRECTION_OUTPUT);

    // ³õÊ¼»¯OSPIÍâÉè
    LCDSPIInit();
}

/**
 * @brief LCD Ö¸Áî+Êý¾Ý×éºÏ·¢ËÍ
 * @param cmd    LCDÖ¸ÁîÂë
 * @param data   Ö¸Áî¶ÔÓ¦²ÎÊýÊý¾Ý
 * @param len    Êý¾Ý³¤¶È
 */
void my_write(uint8_t cmd, uint8_t *data, uint32_t len)
{
    uint8_t data_in[16] = {0};
    if (cmd == 0x2C)
    {
        data_in[0] = 0x12;
    }
    else
    {
        data_in[0] = 0x02;
    }
    data_in[1] = 0x0;
    data_in[2] = cmd;
    data_in[3] = 0x0;
    memcpy(&data_in[4], data, len);

    CLR_CS1();
    for (int i = 0; i < len + 4; i++)
    {
        TFT_SPI_Write_Byte(data_in[i]);
    }
    SET_CS1();
}

/**
 * @brief ÉèÖÃLCDÏÔÊ¾´°¿ÚÇøÓò
 * @param sx      ÆðÊ¼X×ø±ê
 * @param sy      ÆðÊ¼Y×ø±ê
 * @param width   ´°¿Ú¿í¶È
 * @param height  ´°¿Ú¸ß¶È
 */
void LCD_Set_Window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height)
{
    // µÈ´ýLCDÃ¦×´Ì¬½â³ý
    while (csi_gpio_read(&lcd_wait, (1 << 10)) == 0);

    uint8_t param_data[10] = {0};
    // ÉèÖÃXÖá´°¿Ú
    param_data[0] = (sx >> 8);
    param_data[1] = (sx & 0xff);
    param_data[2] = ((sx + width - 1) >> 8);
    param_data[3] = ((sx + width - 1) & 0xff);
    my_write(0x2a, param_data, 4);

    // ÉèÖÃYÖá´°¿Ú
    param_data[0] = (sy >> 8);
    param_data[1] = (sy & 0xff);
    param_data[2] = ((sy + height - 1) >> 8);
    param_data[3] = ((sy + height - 1) & 0xff);
    my_write(0x2B, param_data, 4);

#if LCD_DEBUG
    printf("sx:%d,sy:%d,width:%d,height:%d\r\n", sx, sy, width, height);
#endif

    while (csi_gpio_read(&lcd_wait, (1 << 10)) == 0);
}

/**
 * @brief ÇøÓòµ¥É«Ìî³ä(ÂÖÑ¯·½Ê½)
 * @param x,y     ÆðÊ¼×ø±ê
 * @param w,h     ¿í¸ß
 * @param color   Ìî³äÑÕÉ«(16bit RGB565)
 */
void my_fill_color(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint8_t param_data[10];
    // ÉèÖÃ´°¿Ú
    param_data[0] = (x >> 8);
    param_data[1] = (x & 0xff);
    param_data[2] = ((x + w - 1) >> 8);
    param_data[3] = ((x + w - 1) & 0xff);
    my_write(0x2a, param_data, 4);

    param_data[0] = (y >> 8);
    param_data[1] = (y & 0xff);
    param_data[2] = ((y + h - 1) >> 8);
    param_data[3] = ((y + h - 1) & 0xff);
    my_write(0x2B, param_data, 4);

    // ÅúÁ¿·¢ËÍÑÕÉ«Êý¾Ý
    LCD_startWriteMutileData();
    for (int i = 0; i < (w * h); i++)
    {
        LCD_WritemutileData16_fast(color);
    }
    LCD_endWriteMutileData();
}

/**
 * @brief OSPI DMA ÅúÁ¿Êý¾Ý·¢ËÍ
 * @param pdata   Êý¾ÝÔ´µØÖ·(16bitÑÕÉ«»º³åÇø)
 * @param len     Êý¾Ý¸öÊý
 */
void LCD_DmaSend(uint16_t *pdata, int len)
{
#if CONFIG_USE_OSPI_DMA
#if LCD_DEBUG
    printf("dma send %d\r\n", len);
#endif
    fill_count++;
    dw_dma_set_transfer_size(DWDMA_CH, len);
    dw_dma_set_sar(DWDMA_CH, (uint32_t)pdata);

    // Ë¢ÐÂDCache£¬±£Ö¤DMA¶ÁÈ¡×îÐÂÊý¾Ý
    csi_dcache_clean_invalid_range((uint32_t *)pdata, len * 2);
    // Ê¹ÄÜDMAÍ¨µÀ¿ªÊ¼´«Êä
    dw_dma_set_channel_en(DWDMA, 1 << CH_IDX);

    // µÈ´ý´«ÊäÍê³É²¢Çå³ýÖÐ¶Ï±êÖ¾
    while (!DWDMA->RawBlock);
    DWDMA->ClearBlock  = (1 << CH_IDX);
    DWDMA->ClearTfr    = (1 << CH_IDX);
    DWDMA->ClearSrcTran= (1 << CH_IDX);
    DWDMA->ClearDstTran= (1 << CH_IDX);
#else
    // ·ÇDMAÄ£Ê½£ºÂÖÑ¯·¢ËÍ
    for (int i = 0; i < (len); i++)
    {
        LCD_WritemutileData16_fast(*pdata);
        pdata++;
    }
#endif
}

/**
 * @brief 毫秒延时函数（STM32 HAL 风格）
 *
 * HAL_Delay 的【实现】在 src/main.c 中（内部用 udelay(1000) 实现，
 * 可带看门狗喂狗逻辑），本文件只使用、不定义。
 *
 * 为什么这里要 extern 声明：
 *   make 构建时每个 .c 文件是独立编译（gcc -c video.c），
 *   video.c 看不到 main.c 里定义的 HAL_Delay，若不声明会报
 *   "implicit declaration of function 'HAL_Delay'" 错误。
 *   此声明让本编译单元在编译期即可识别该函数。
 *
 * @param ms 延时毫秒数
 */
extern void HAL_Delay(uint32_t ms);

#define delay_ms    HAL_Delay
#define Delay       HAL_Delay

/**
 * @brief ÊÓÆµ/LCD Ó²¼þ³õÊ¼»¯Èë¿Ú
 * @return ³õÊ¼»¯½á¹û true-³É¹¦
 */
bool video_init(void)
{
#if LCD_DEBUG
    printf("video start \r\n");
#endif
    LcdGPIOInit();

    // µãÁÁ±³¹â£¨À©Õ¹°å CON2: LCD_BL£©
    BLK_HI();

    // LCD ¸´Î»Ê±Ðò
    SET_CS1();
    RST_HI();
    delay_ms(100);
    RST_LOW();
    delay_ms(100);
    RST_HI();
    delay_ms(2000);

    uint8_t param_data[5] = {0};
    param_data[0] = 0x64;
    my_write(0x20, param_data, 1);

#if LCD_DEBUG
    printf("LCD init success!\r\n");
#endif
    Delay(1000);

    // ÈýÉ«È«ÆÁ²âÊÔ
    my_fill_color(0, 0, 480, 480, 0xF800);
    delay_ms(100);
    my_fill_color(0, 0, 480, 480, 0x07e0);
    delay_ms(100);
    my_fill_color(0, 0, 480, 480, 0x001F);
    delay_ms(100);

    return true;
}

/**
 * @brief ·´³õÊ¼»¯(Ô¤Áô½Ó¿Ú)
 */
void video_deinit(void)
{

}

/**
 * @brief »­ÃæË¢ÐÂµÈ´ý(µÈ´ýOSPI¿ÕÏÐ+LCDÃ¦½â³ý)
 */
void video_flip(void)
{
    while (!(DWOSPI->SR & DW_SPI_SR_TFE));
    while (DWOSPI->SR & DW_OSPI_SR_BUSY);
    while (csi_gpio_read(&lcd_wait, (1 << 10)) == 0);
}