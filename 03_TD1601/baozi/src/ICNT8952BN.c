#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <csi_core.h>
#include <drv/tick.h>
#include <drv/gpio.h>
#include <drv/pin.h>
#include <drv/iic.h>
#include <stdio.h>

extern void mdelay(uint32_t ms);
extern int abs(int);

// 调试开关：1-开启打印输出，0-关闭所有调试信息
#define TP_DEBUG        0

// ==================== 硬件引脚定义 ====================
#define TP_PORT         PORTA
#define TP_RST_PORT     PORTB
#define TP_INT_PORT     PORTB
#define TP_RST_PIN      PIN1
#define TP_INT          PIN2
#define TP_IO_IDX       0
#define TP_IIC_IDX      0

#define TP_IIC_SDA      PIN22
#define TP_IIC_SCL      PIN21
#define TP_IIC_SDA_FUNC PA22_IIC0_SDA
#define TP_IIC_SCL_FUNC PA21_IIC0_SCL

// ==================== 触摸IC I2C 参数 ====================
#define IIC_SLAVE_ADDR      0x48
#define ICNT8952_GESTURE    0x1000

// ==================== 复位GPIO操作宏 ====================
static csi_gpio_t tp_rst;
static csi_gpio_t tp_int_gpio;

#define RST_LOW()   csi_gpio_write(&tp_rst, TP_RST_PIN, 0)
#define RST_HI()    csi_gpio_write(&tp_rst, TP_RST_PIN, 1)

// ==================== 触摸原始数据结构体 ====================
typedef struct {
    uint8_t gesture;
    uint8_t Point_num;
    uint8_t Point_ID;
    uint8_t XL;
    uint8_t XH;
    uint8_t YL;
    uint8_t YH;
    uint8_t pressure;
    uint8_t Event_ID;
    uint8_t reserve;
} __attribute__((packed)) TouchData_icnt;

// ==================== 双触点报文 ====================
/* 芯片报文格式：2 字节头（gesture + Point_num）+ 每点 8 字节
 * （Point_ID / XL / XH / YL / YH / pressure / Event_ID / reserve）。
 * 原来一次只读 10 字节 = 只拿到第一个点，于是上层永远不知道"还有第二根手指"，
 * 做不出双指同按。这里改读 18 字节，第二个点跟在后面。
 *
 * 兼容：万一面板/芯片固件只上报单点，Point_num 恒为 1，下面按 Point_num
 * 取值，缓存里永远只有 1 个点 —— 上层会自动退化成"单指长按"，不会卡死。 */
#define TP_MAX_POINTS   2
#define TP_PT_BYTES     8
#define TP_FRAME_BYTES  (2 + TP_PT_BYTES * TP_MAX_POINTS)

// ==================== 手势类型枚举 ====================
typedef enum {
    GESTURE_NONE        = 0,
    GESTURE_DOWN,          /* 1 = 首次按下（一次）*/
    GESTURE_UP,            /* 2 = 抬起且位移很小 = 点击 */
    GESTURE_MOVE_LEFT,     /* 3 = 左滑（越过阈值，一次）*/
    GESTURE_MOVE_RIGHT,    /* 4 = 右滑 */
    GESTURE_MOVE_UP,       /* 5 = 上滑 */
    GESTURE_MOVE_DOWN,     /* 6 = 下滑 */
    GESTURE_MOVE           /* 7 = 按住中的位置更新（带实时坐标，供抚摸/拖动手势）*/
} gesture_t;

// ==================== 全局状态变量 ====================
static csi_iic_t master_iic;
static volatile bool tp_irq_flag = false;

static bool finger_down     = false;        // 手指按下标记
static uint16_t start_x     = 0, start_y = 0; // 触摸起始坐标
static uint16_t last_x      = 0, last_y  = 0; // 上一次触摸坐标
static bool swipe_fired     = false;        // 滑动事件触发标记

/* 最近一次"确实读到手指按着"的时刻（ms）。tp_idle_check() 用它判 IRQ 丢失：
 * 触摸是中断驱动的，主循环忙的时候（整屏刷屏）一次中断可能被合并掉，
 * finger_down 就永远停在 true —— 之后每一次抬起都走不进 UP 分支，
 * 用户看到的就是"点了没反应"。600ms 没再读到按下就认为状态已失效。 */
static uint32_t tp_last_active_ms = 0;
#define TP_IDLE_RESET_MS    600

#define SWIPE_THRESHOLD     30              // 滑动判定像素阈值

/* ---- 对外查询（app_core / 动作抑制用）----
 * 为什么需要：合并后"用户手指在屏上时机器人必须停手"是一条硬需求
 * （防误触、防夹手）。上层不该去猜手势码，直接问驱动两个状态即可。 */
int tp_is_down(void)
{
    return finger_down ? 1 : 0;
}

uint32_t tp_last_touch_ms(void)
{
    return tp_last_active_ms;
}

/* ---- 触点缓存（最近一帧读到的所有触点）----
 * 只在这里被 tp_get_gesture() 更新：I2C 读数被人为限制成"每次 IRQ 读一帧"，
 * 上层反复调 tp_get_points() 拿到的都是同一帧，不会多占 I2C 带宽。
 * 手指静止时芯片仍会持续上报（否则 GESTURE_MOVE 做不出抚摸），
 * 所以按住期间缓存是活的；抬手那一帧读出来就是 0 个点。 */
static uint8_t  s_npt = 0;
static uint16_t s_px[TP_MAX_POINTS];
static uint16_t s_py[TP_MAX_POINTS];

/* 读最近一帧的触点。返回触点数（0 / 1 / 2），坐标写进 xs/ys（按点位序）。
 * 只关心"几个手指、各在哪"，手势码仍然由 tp_get_gesture() 给。 */
int tp_get_points(uint16_t *xs, uint16_t *ys)
{
    int i;
    for (i = 0; i < s_npt; i++)
    {
        xs[i] = s_px[i];
        ys[i] = s_py[i];
    }
    return s_npt;
}

// ==================== I2C 读写接口 ====================
/**
 * @brief I2C 寄存器读取
 * @param cmd 寄存器地址
 * @param buf 接收缓冲区
 * @param len 读取长度
 * @return 0-成功，-1-失败
 */
int tp_iic_read(uint16_t cmd, uint8_t *buf, uint32_t len)
{
    int32_t num = csi_iic_mem_receive(&master_iic, IIC_SLAVE_ADDR,
                                      cmd, IIC_MEM_ADDR_SIZE_16BIT,
                                      buf, len, 5);
    return (num == len) ? 0 : -1;
}

/**
 * @brief I2C 寄存器写入
 * @param cmd 寄存器地址
 * @param buf 发送数据缓冲区
 * @param len 写入长度
 * @return 0-成功，-1-失败
 */
int tp_iic_write(uint16_t cmd, uint8_t *buf, uint32_t len)
{
    int32_t num = csi_iic_mem_send(&master_iic, IIC_SLAVE_ADDR,
                                    cmd, IIC_MEM_ADDR_SIZE_16BIT,
                                    buf, len, 50);
    return (num == len) ? 0 : -1;
}

// ==================== 触摸中断服务函数 ====================
static void gpio_interrupt_handler(csi_gpio_t *gpio, uint32_t pin_mask, void *arg)
{
    tp_irq_flag = true;
}

// ==================== 手势识别主处理 ====================
/**
 * @brief 获取触摸手势与坐标
 * @param x 输出X坐标
 * @param y 输出Y坐标
 * @return 识别到的手势类型
 */
gesture_t tp_get_gesture(uint16_t *x, uint16_t *y)
{
    if (!tp_irq_flag)
    {
        return GESTURE_NONE;
    }
    tp_irq_flag = false;

#if TP_DEBUG
    uint32_t t0 = csi_tick_get();       /* 只在调试打印时用，关掉 TP_DEBUG 就不占用 */
#endif
    TouchData_icnt data;
    uint8_t frame[TP_FRAME_BYTES];
    if (tp_iic_read(ICNT8952_GESTURE, frame, sizeof(frame)) != 0)
    {
        /* 18 字节读失败 -> 退回原来的 10 字节单点报文。
         * 为什么要有这条退路：有的面板固件只吐得出一个点的长度，多读会 NAK。
         * 不能因为"想拿第二根手指"把整个触摸搞挂 —— 退回单点后手势照常，
         * 只是双指判定永远不成立，上层会自动走单点降级通道。 */
        if (tp_iic_read(ICNT8952_GESTURE, frame, sizeof(TouchData_icnt)) != 0)
        {
#if TP_DEBUG
            printf("[ERR] I2C read failed\r\n");
#endif
            s_npt = 0;
            return GESTURE_NONE;
        }
        memset(frame + sizeof(TouchData_icnt), 0,
               sizeof(frame) - sizeof(TouchData_icnt));
    }
    memcpy(&data, frame, sizeof(data));
#if TP_DEBUG
    uint32_t t1 = csi_tick_get();

    if (t1 - t0 > 2)
    {
        printf("I2C read cost: %d ms\r\n", t1 - t0);
    }
#endif

    // 拼接16位坐标
    uint16_t cx = ((uint16_t)data.XH << 8) | data.XL;
    uint16_t cy = ((uint16_t)data.YH << 8) | data.YL;
    *x = cx;
    *y = cy;

    // 触摸状态判定：压力非0 且 非抬起事件
    bool currently_pressed = (data.pressure > 0) && (data.Event_ID != 4);

    if (currently_pressed)
    {
        tp_last_active_ms = csi_tick_get();   // 手表还按着：喂狗给 tp_idle_check
    }

    /* ---- 触点缓存：每帧刷新一次，供 tp_get_points() 查询 ---- */
    s_npt = 0;
    if (currently_pressed)
    {
        s_px[0] = cx;
        s_py[0] = cy;
        s_npt   = 1;

        /* 第二个点：header 说 Point_num >= 2 才去解析，避免单点面板上
         * 把后面 8 字节的残值当成一根手指。坐标/压力/事件三项都校验，
         * 任何一项不合理就退回单点 —— 宁可少认一根手指，不可认错。 */
        if (data.Point_num >= 2)
        {
            const uint8_t *p1 = &frame[2 + TP_PT_BYTES];
            uint16_t x1 = (uint16_t)(((uint16_t)p1[2] << 8) | p1[1]);
            uint16_t y1 = (uint16_t)(((uint16_t)p1[4] << 8) | p1[3]);

            if ((p1[5] > 0) && (p1[6] != 4) && (x1 < 480) && (y1 < 480))
            {
                s_px[1] = x1;
                s_py[1] = y1;
                s_npt   = 2;
            }
        }
    }

#if TP_DEBUG
    printf("[TP] P=%d X=%d Y=%d prs=%d evt=%d pressed=%d npt=%d",
           data.Point_num, cx, cy, data.pressure, data.Event_ID,
           currently_pressed, s_npt);
    if (s_npt >= 2)
    {
        printf(" p1=(%d,%d)", s_px[1], s_py[1]);
    }
    printf("\r\n");
#endif

    // 1. 手指首次按下
    if (currently_pressed && !finger_down)
    {
        finger_down = true;
        start_x     = cx;
        start_y     = cy;
        last_x      = cx;
        last_y      = cy;
        swipe_fired = false;
#if TP_DEBUG
        printf("[GEST] DOWN @(%d,%d)\r\n", cx, cy);
#endif
        return GESTURE_DOWN;
    }

    // 2. 手指按住持续滑动检测
    if (currently_pressed && finger_down)
    {
        int16_t dx = (int16_t)cx - (int16_t)start_x;
        int16_t dy = (int16_t)cy - (int16_t)start_y;

        if (!swipe_fired && (abs(dx) > SWIPE_THRESHOLD || abs(dy) > SWIPE_THRESHOLD))
        {
            swipe_fired = true;
            gesture_t g;

            if (abs(dx) >= abs(dy))
            {
                g = (dx > 0) ? GESTURE_MOVE_RIGHT : GESTURE_MOVE_LEFT;
            }
            else
            {
                g = (dy > 0) ? GESTURE_MOVE_DOWN : GESTURE_MOVE_UP;
            }
#if TP_DEBUG
            printf("[GEST] SWIPE from(%d,%d) to(%d,%d) dx=%d dy=%d\r\n",
                   start_x, start_y, cx, cy, dx, dy);
#endif
            return g;
        }

        last_x = cx;
        last_y = cy;
        /* ★ 2026-11 合并版：按住但还没越过滑动阈值 → 报 GESTURE_MOVE（带实时坐标）。
         *   为什么要区分：卡片 UI 原来拿不到"按住期间的手指轨迹"，只能看到
         *   DOWN 一次 + 最终 UP，所以做不出"在脸上来回摸"这种连续手势。
         *   MOVE 的语义是"手指还在动/还在按，坐标持续有效"，上层可以据此
         *   统计方向反转次数（见 ui_app.c 的抚摸手势识别）。
         *   注意：长按计时只认 DOWN（一次性），不会被 MOVE 重复触发。 */
        return GESTURE_MOVE;
    }

    // 3. 手指抬起
    if (!currently_pressed && finger_down)
    {
        finger_down = false;
        int16_t total_dx = (int16_t)cx - (int16_t)start_x;
        int16_t total_dy = (int16_t)cy - (int16_t)start_y;

#if TP_DEBUG
        printf("[GEST] UP end(%d,%d) start(%d,%d) dx=%d dy=%d\r\n",
               cx, cy, start_x, start_y, total_dx, total_dy);
#endif

        // 过程中已触发滑动，直接返回无手势
        if (swipe_fired)
        {
            swipe_fired = false;
            return GESTURE_NONE;
        }

        // 抬起时二次判定滑动
        if (abs(total_dx) > SWIPE_THRESHOLD || abs(total_dy) > SWIPE_THRESHOLD)
        {
            swipe_fired = false;
            gesture_t g;
            if (abs(total_dx) >= abs(total_dy))
            {
                g = (total_dx > 0) ? GESTURE_MOVE_RIGHT : GESTURE_MOVE_LEFT;
            }
            else
            {
                g = (total_dy > 0) ? GESTURE_MOVE_DOWN : GESTURE_MOVE_UP;
            }
            *x = start_x;
            *y = start_y;
#if TP_DEBUG
            printf("[GEST] SWIPE(on UP) dx=%d dy=%d\r\n", total_dx, total_dy);
#endif
            return g;
        }

        // 位移很小判定为点击
        *x = start_x;
        *y = start_y;
#if TP_DEBUG
        printf("[GEST] TAP @(%d,%d)\r\n", start_x, start_y);
#endif
        return GESTURE_UP;
    }

    return GESTURE_NONE;
}

/**
 * @brief 触摸状态自愈（主循环每轮调用，只在没有 IRQ 时起兜底作用）
 *
 * 触摸走中断：IRQ 置 tp_irq_flag，主循环下一轮读一次芯片。主循环忙的时候
 * （以前一次整屏刷屏 100~200ms）多次 IRQ 会被合并成一次，读到的可能只是
 * "手指已经抬起"。于是 finger_down 卡在 true，之后每次真实按下都走不进
 * DOWN 分支，直到某次位移超过阈值才被当成滑动 —— 实机表现就是
 * "点一下没反应，连点几下忽然一起变色"。
 *
 * 现在刷屏已改成分片，这个概率大幅下降；这层兜底仍然留着：
 * 超过 600ms 没再读到"按着"，就把内部状态清零。
 * 长按不会被误伤 —— 只要触点还在，tp_get_gesture() 每次都会刷新时间戳。
 */
void tp_idle_check(uint32_t now_ms)
{
    if (!finger_down)
    {
        return;
    }
    if ((uint32_t)(now_ms - tp_last_active_ms) > TP_IDLE_RESET_MS)
    {
        finger_down = false;
        swipe_fired = false;
        s_npt       = 0;          /* 触点缓存一起作废：否则会留下一帧假的"还按着" */
    }
}

// ==================== 触摸芯片复位时序 ====================
void tp_icnt8952bn_chip_init(void)
{
    csi_pin_set_mux(TP_RST_PORT, TP_RST_PIN, PIN_FUNC_GPIO);
    csi_gpio_init(&tp_rst, TP_RST_PORT);
    csi_gpio_dir(&tp_rst, TP_RST_PIN, GPIO_DIRECTION_OUTPUT);

    RST_HI();
    mdelay(100);
    RST_LOW();
    mdelay(10);
    RST_HI();
    mdelay(100);
}

// ==================== I2C 引脚复用配置 ====================
static void example_pin_iic_init(void)
{
    csi_pin_set_mux(TP_PORT, TP_IIC_SDA, TP_IIC_SDA_FUNC);
    csi_pin_set_mux(TP_PORT, TP_IIC_SCL, TP_IIC_SCL_FUNC);
    csi_pin_mode(TP_PORT, TP_IIC_SDA, GPIO_MODE_PULLUP);
    csi_pin_mode(TP_PORT, TP_IIC_SCL, GPIO_MODE_PULLUP);
}

// ==================== I2C 外设初始化 ====================
void tp_iic_init(int32_t iic_idx)
{
    example_pin_iic_init();
    csi_iic_init(&master_iic, iic_idx);
    csi_iic_mode(&master_iic, IIC_MODE_MASTER);
    csi_iic_addr_mode(&master_iic, IIC_ADDRESS_7BIT);
    csi_iic_speed(&master_iic, IIC_BUS_SPEED_FAST);
}

// ==================== 触摸中断引脚初始化(下降沿触发) ====================
void tp_gpio_falling_edge_interrupt(void)
{
    csi_pin_set_mux(TP_INT_PORT, TP_INT, PIN_FUNC_GPIO);
    csi_gpio_init(&tp_int_gpio, TP_INT_PORT);
    csi_gpio_mode(&tp_int_gpio, TP_INT, GPIO_MODE_PULLUP);
    csi_gpio_attach_callback(&tp_int_gpio, gpio_interrupt_handler, NULL);
    csi_gpio_dir(&tp_int_gpio, TP_INT, GPIO_DIRECTION_INPUT);
    csi_gpio_irq_mode(&tp_int_gpio, TP_INT, GPIO_IRQ_MODE_FALLING_EDGE);
    csi_gpio_irq_enable(&tp_int_gpio, TP_INT, true);
}

// ==================== 触摸模块总初始化 ====================
void icnt8952bn_init(void)
{
    tp_icnt8952bn_chip_init();
    tp_iic_init(TP_IIC_IDX);
    tp_gpio_falling_edge_interrupt();
}