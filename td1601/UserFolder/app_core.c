/******************************************************************************
 * @file     app_core.c
 * @brief    主循环编排层实现（见 app_core.h）
 *
 * 一轮做的事（顺序有讲究）：
 *   ① 触摸轮询 → ui_app_handle_gesture()（长按/点击/滑动语义在 ui_app 里）
 *   ② 串口：UART0 二进制帧（uart_hal_poll）+ 文本命令行（EMO:/INTENT:/TRACK:）
 *      → emotion_fsm_feed_line()；UART3 = ESP32 情绪链路（uart_esp_poll）
 *   ③ 用【真实】dt 推进 ui_app_tick()（时钟/长按计时/眨眼都靠它）
 *   ④ 分片刷屏最多 2 片（每片 8 行），让下一轮尽快回到①
 *
 * ★ dt 用 csi_tick_get()（SysTick，1 tick = 1ms）而不是 csi_tick_get_ms()：
 *   后者依赖 CPU 主频换算，主频对不上时时钟会系统性走快/走慢（历史实机故障）。
 *
 * ★ 重入保护：emotion_fsm 的播放钩子与 main 主循环都会调 app_core_step()，
 *   用 s_in_step 挡住"刷屏中又被钩子调进来"的情况。
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui_app.h"                 /* ui_app_init / handle_gesture / tick */
#include "ui.h"                     /* ui_flush_busy / ui_flush_step（分片刷屏）*/
#include "uart_esp.h"               /* uart_esp_init / uart_esp_poll */
#include "net_link.h"               /* 配网命令 QR: / NET:（v2.1）*/
#include "emotion/emotion_fsm.h"    /* emotion_fsm_feed_line（文本命令）*/
#include "app_core.h"

/* ---- 触摸驱动（src/ICNT8952BN.c）---- */
extern void     icnt8952bn_init(void);
extern int      tp_get_gesture(uint16_t *x, uint16_t *y);
extern void     tp_idle_check(uint32_t now_ms);
extern int      tp_is_down(void);            /* 1 = 手指此刻按着 */
extern uint32_t tp_last_touch_ms(void);      /* 最后一次"确实读到触点"的时刻 */

/* ---- 协议串口（src/uart_hal.c）---- */
extern int  uart_hal_init(void);
extern void uart_hal_poll(void);
extern int  uart_hal_take_line(char *out, int cap);   /* 取一行文本命令（EMO:xxx 等）*/

/* ---- 系统毫秒计时（SysTick，1 tick = 1ms）---- */
extern uint32_t csi_tick_get(void);

#define APP_FLUSH_PER_STEP   2      /* 每轮最多刷 2 片（16 行），够快又不霸占总线 */
#define APP_DT_MAX_MS        60000U /* 断点/异常停顿：别把累计误差灌进时钟 */

static uint32_t s_last_tick   = 0;
static uint8_t  s_in_step     = 0;
static uint8_t  s_touch_down  = 0;
static uint32_t s_touch_last  = 0;   /* 最后一次触摸事件时刻（含抬起）*/
static uint8_t  s_ready       = 0;

int app_core_touch_down(void)
{
    return s_touch_down;
}

uint32_t app_core_touch_idle_ms(void)
{
    return (uint32_t)(csi_tick_get() - s_touch_last);
}

void app_core_init(void)
{
    char line[64];

    /* 屏幕 + 参数化脸先就绪：emotion_fsm_init() 里的 face_set_bus_callback()
     * 与首帧清屏都要在屏幕初始化之后才有意义。 */
    ui_app_init();

    /* 配网卡片：清空上一轮的二维码与状态（NET 页此时显示 waiting）*/
    net_link_init();

    printf("[app] --- icnt8952bn_init() ---\r\n");
    icnt8952bn_init();
    printf("[app] touch ready\r\n");

    printf("[app] --- uart_hal_init() ---\r\n");
    if (uart_hal_init() != 0) {
        printf("[app] uart init failed (UART0 PA18/PA17)!\r\n");
    } else {
        printf("[app] uart ready (UART0 PA18/PA17 @115200, shared with printf)\r\n");
    }

    printf("[app] --- uart_esp_init() ---\r\n");
    if (uart_esp_init() != 0) {
        printf("[app] esp uart init failed (UART3 PB18/PB19)!\r\n");
    } else {
        printf("[app] esp uart ready (UART3 PB18/PB19 @115200, from ESP32)\r\n");
    }

    /* 丢掉开机瞬间可能残留的半行文本（PC 重连时常见）*/
    while (uart_hal_take_line(line, (int)sizeof(line))) {
        /* 只清空，不执行 */
    }

    s_last_tick  = csi_tick_get();
    s_touch_last = s_last_tick;
    s_touch_down = 0;
    s_in_step    = 0;
    s_ready      = 1;
}

void app_core_step(void)
{
    uint32_t now_tick, dt;
    int i;

    if (!s_ready) {
        return;
    }
    if (s_in_step) {
        return;                     /* 刷屏中又被钩子调进来：本轮跳过 */
    }
    s_in_step = 1;

    /* ① 触摸：手势语义交给 ui_app（长按/点击/滑动/换页） */
    {
        uint16_t tx = 0, ty = 0;
        int g = tp_get_gesture(&tx, &ty);

        if (g != 0) {
            s_touch_last = csi_tick_get();
            ui_app_handle_gesture(g, tx, ty);
        }
        tp_idle_check(csi_tick_get());        /* IRQ 丢了别让状态卡死 */
        s_touch_down = tp_is_down() ? 1 : 0;
        if (s_touch_down) {
            s_touch_last = tp_last_touch_ms(); /* 按住期间持续刷新"有操作" */
        }
    }

    /* ② 串口：先排空二进制帧，再把文本命令行喂给情感状态机。
     *    两条通路共用 UART0（与 printf 同一根线），字节级分流在 uart_hal.c。 */
    uart_hal_poll();
    {
        char line[64];
        int guard = 0;
        while (uart_hal_take_line(line, (int)sizeof(line)) && guard < 4) {
            guard++;
            /* ★ 配网命令先截走：QR: / NET: 不属于情感状态机的语法。
             * 这条通路让"没有 ESP32 也能台架联调"—— 串口助手敲一行即可。 */
            if (net_link_handle_line(line)) {
                continue;
            }
            printf("[app] cmd: %s\r\n", line);
            emotion_fsm_feed_line(line);
        }
    }
    uart_esp_poll();

    /* ③ 真实 dt → 推进 UI（时钟、长按、眨眼、dirty 补刷都在里面） */
    now_tick = csi_tick_get();
    dt = (uint32_t)(now_tick - s_last_tick);
    s_last_tick = now_tick;
    if (dt == 0) {
        dt = 1;                     /* 同一毫秒内连转两轮：至少推 1ms */
    }
    if (dt > APP_DT_MAX_MS) {
        dt = APP_DT_MAX_MS;
    }
    ui_app_tick(dt);

    /* ④ 分片刷屏：每轮 2 片（16 行，≈6~16ms），片间回到① */
    for (i = 0; i < APP_FLUSH_PER_STEP; i++) {
        if (!ui_flush_busy()) {
            break;
        }
        ui_flush_step();
    }

    s_in_step = 0;
}
