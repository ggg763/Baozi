// uart_hal.c — 协议串口硬件适配层（TD1601 · UART0：PA18-TX / PA17-RX）
// ============================================================================
// 为什么用 UART0？
//   这是 board X7A 排针上的 TXD0 / RXD0，也是 console printf 的口。
//   实测 USB-TTL 就插在这两个脚上，所以协议直接复用这根线：
//   一根线既能看 printf 日志，又能收发协议帧，不用再接第二组线。
//
// 为什么不用 UART1（PA28-TX / PA27-RX）？
//   PA28 被 LCD 的 OSPI 时钟占用（PA28_SPI1_SCK），引脚冲突，不能用。
// 为什么不用 UART2（PB13-TX / PB12-RX）？
//   能用，但要多接一组线。留给将来 ESP32-S3 中转方案（预留）。
//
// 关键实现细节 —— 复用 console 的句柄，不新建实例：
//   printf 走的是 minilibc_port.c 里的全局 `console_uart`
//   （fputc → csi_uart_putc(&console_uart, ch)）。
//   这里直接 extern 复用同一个句柄，好处：
//     1) 不会出现两套 csi_uart_t 状态指向同一个硬件 UART，省掉句柄不同步的坑；
//     2) 波特率 / 数据格式 / 引脚复用 console_init() 已配好（115200 8N1，PA18/PA17），
//        无需重复 csi_uart_init（重复 init 会关中断、清 FIFO，徒增风险）。
//   发送 csi_uart_send()：内部阻塞轮询，只会临时禁"传输中断"，不影响接收中断；
//   接收 attach_callback() + receive_async()：中断收 1 字节 → 主循环处理整帧。
//
// ⚠️ 上行方向（板子 → PC）必然混流：printf 日志 与 协议帧 交织在同一条线上。
//   这是共线的代价，无法回避。PC 侧解析器必须"扫帧头 0xA5 + 验校验"来过滤，
//   见 D:\competition\creativity\baozi-uart-test.py。
//
// 接线（PC ↔ TD1601，TX/RX 必须交叉）：
//   USB-TTL 的 TX   →  TD1601 PA17（UART0 RX）
//   USB-TTL 的 RX   →  TD1601 PA18（UART0 TX）
//   USB-TTL 的 GND  →  TD1601 GND（必须共地，否则收不到任何数据）
//
// 波特率 115200，8N1。
// ============================================================================
#include <stdint.h>
#include <stdio.h>      /* printf：只有主循环里的溢出一行用到，绝不在中断里调 */
#include <string.h>
#include <drv/uart.h>
#include <drv/pin.h>
#include <soc.h>
#include "board_config.h"
#include "uart_protocol.h"

/* printf 用的就是它，定义在 components/minilibc/v1.0/src/minilibc_port.c */
extern csi_uart_t console_uart;

static UartRx_t   g_rx;
static uint8_t    g_rx_byte;        /* 单字节接收缓冲 */

/* 完整帧暂存队列：中断收到完整帧 → 入队 → 主循环取走处理。
 *
 * 【为什么是队列，而不是原来那个"一个标志位 + 一个缓冲区"】
 *   板子主循环 20ms 一轮。原来只存得下**一帧**：这 20ms 里来两帧，
 *   第二帧就把第一帧直接覆盖掉 —— 丢帧无声无息，没日志、没报错。
 *   PC 连续下发 0x11/0x12×N/0x13 时，只要中途夹一次屏幕刷新（几十毫秒）
 *   就会踩上。丢一帧的后果是"屏幕上少一个习惯"，极难查。
 *   所以改成环形队列；PC 侧每帧还留了间隔，双保险。
 *
 *   深度 12：PC 一次整批下发 = 0x10 + 0x11 + 0x12×N + 0x13 ≈ 10 帧。
 *   以前深度 4，主循环只要被一次整屏刷新（100ms+）卡住，队列必满，
 *   丢的就是 0x12 数据帧 → 屏幕上"个别习惯直接消失"（实机已复现）。
 *   现在 12 深 + 帧处理排空，整批一口气吃完。满了丢**最新**帧并计数。 */
#define FRAME_QUEUE_DEPTH  12

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[FRAME_MAX_DATA];
} frame_slot_t;

static frame_slot_t     g_fq[FRAME_QUEUE_DEPTH];
static volatile uint8_t g_fq_head = 0;      /* 写指针（只在中断里改） */
static volatile uint8_t g_fq_tail = 0;      /* 读指针（只在主循环里改） */
static volatile uint32_t g_fq_drop = 0;     /* 队列满丢帧计数（调试看） */

/* 解析临时缓冲：只在中断回调里用，帧完整后再拷进队列槽位
 * （这样"队列满丢帧"的路径不会写到任何有效数据上） */
static uint8_t g_frame_scratch_buf[FRAME_MAX_DATA];

/* ================== 文本命令行（与二进制帧共用 UART0）==================
 *
 * 为什么需要：合并后这根线上跑两套协议 ——
 *   ① PC 桥接脚本的二进制帧（0xA5 | LEN | CMD | DATA | XOR），由帧解析器处理；
 *   ② 情感状态机的文本命令（EMO:HAPPY / INTENT:CHAT / TRACK:ON / help …），
 *      原来由 emotion_fsm.c 自己 csi_uart_receive() 轮询读。
 * 两套抢同一个 FIFO 必然互相吞字节，所以统一到【一个】中断接收者（本文件），
 * 在字节级分流：
 *   · 解析器当前在"等帧头"且这个字节不是 0xA5  → 当文本行字符
 *   · 否则（是 0xA5 或已在一帧之内）           → 交给帧解析器
 * 校验失败时解析器自己回到"等帧头"，后续文本照常收。
 *
 * 文本行由主循环用 uart_hal_take_line() 取走，再喂 emotion_fsm_feed_line()；
 * 中断里绝不 printf、绝不做重活。
 */
#define LINE_MAX         64          /* 单行上限（含结束符）*/
#define LINE_QUEUE_DEPTH 4

static char             g_lq[LINE_QUEUE_DEPTH][LINE_MAX];
static volatile uint8_t g_lq_head = 0;      /* 写指针（只在中断里改）*/
static volatile uint8_t g_lq_tail = 0;      /* 读指针（只在主循环里改）*/
static char             g_line[LINE_MAX];
static uint8_t          g_line_len = 0;

/* 中断里调：喂一个文本字节 */
static void line_feed(uint8_t b)
{
    uint8_t next, i;

    if ((b == '\r') || (b == '\n')) {
        if (g_line_len == 0) {
            return;                    /* 空行：忽略（\r\n 会连来两个）*/
        }
        g_line[g_line_len] = '\0';
        next = (uint8_t)((g_lq_head + 1) % LINE_QUEUE_DEPTH);
        if (next != g_lq_tail) {
            for (i = 0; (i < (LINE_MAX - 1)) && (g_line[i] != '\0'); i++) {
                g_lq[g_lq_head][i] = g_line[i];
            }
            g_lq[g_lq_head][i] = '\0';
            g_lq_head = next;
        }
        g_line_len = 0;
        return;
    }

    if ((b < 32) || (b > 126)) {
        return;                        /* 控制/非 ASCII：丢弃，别脏了命令行 */
    }
    if (g_line_len < (LINE_MAX - 1)) {
        g_line[g_line_len++] = (char)b;
    } else {
        g_line_len = 0;                /* 超长：整行丢弃，绝不截断后当命令执行 */
    }
}

/* 主循环取一行文本命令。返回 1 = 取到；0 = 队列空。*/
int uart_hal_take_line(char *out, int cap)
{
    uint8_t i;

    if ((out == 0) || (cap <= 0)) {
        return 0;
    }
    if (g_lq_tail == g_lq_head) {
        return 0;
    }
    for (i = 0; (i < (uint8_t)(cap - 1)) && (g_lq[g_lq_tail][i] != '\0'); i++) {
        out[i] = g_lq[g_lq_tail][i];
    }
    out[i] = '\0';
    g_lq_tail = (uint8_t)((g_lq_tail + 1) % LINE_QUEUE_DEPTH);
    return 1;
}

/* 距最后一个收到字节的毫秒数（0xFFFF = 很久没收到）。
 * 用途：整批数据下发期间不要整屏刷新 —— 刷屏 100ms+ 会跟每字节的
 * RX 中断挤在一起，是实机花屏（色带）的高发窗口。ui_app_tick 每轮喂
 * dt 进来，重绘前先看 uart_hal_rx_quiet_ms() >= 150 再动手。 */
#define RX_QUIET_IDLE 0xFFFF
static volatile uint16_t g_rx_quiet = RX_QUIET_IDLE;

/* 串口中断回调：每收满 1 字节触发一次 */
static void proto_uart_cb(csi_uart_t *uart, csi_uart_event_t event, void *arg)
{
    (void)arg;
    if (event == UART_EVENT_RECEIVE_COMPLETE) {
        uint8_t cmd, len;
        uint8_t b = g_rx_byte;

        g_rx_quiet = 0;                 /* 收到了字节：静默计时归零 */

        /* ★ 字节级分流（见上方"文本命令行"说明）：
         * 解析器在等帧头、且这一字节不是帧头 0xA5 → 这是文本命令行；
         * 其余情况一律交给帧解析器（帧内出现 0x0A/0x0D 也不会被误判成换行）。 */
        if ((g_rx.state == RX_WAIT_HEAD) && (b != FRAME_HEAD)) {
            line_feed(b);
        } else if (UART_RxFeed(&g_rx, b, &cmd, g_frame_scratch_buf, &len)) {
            /* 帧完整：入队（重活留给主循环） */
            uint8_t next = (uint8_t)((g_fq_head + 1) % FRAME_QUEUE_DEPTH);
            if (next == g_fq_tail) {
                g_fq_drop++;                    /* 满了：主循环没跟上 */
            } else {
                g_fq[g_fq_head].cmd = cmd;
                g_fq[g_fq_head].len = len;
                if (len > 0) {
                    memcpy(g_fq[g_fq_head].data, g_frame_scratch_buf, len);
                }
                g_fq_head = next;
            }
        }
        /* 重新挂 1 字节异步接收（芯片驱动官方示例同款写法） */
        csi_uart_receive_async(uart, &g_rx_byte, 1);
    }
}

int uart_hal_init(void)
{
    /* 引脚复用 / 波特率 / 数据格式：
     * board_init() 里的 console_init() 已经配好了 UART0（PA18/PA17，115200 8N1），
     * 这里不再重复初始化，只把"接收"接上。 */
    UART_RxInit(&g_rx);

    csi_uart_attach_callback(&console_uart, proto_uart_cb, NULL);
    if (csi_uart_receive_async(&console_uart, &g_rx_byte, 1) != CSI_OK) {
        return -1;      /* 挂接收失败，多半是句柄没 init 过 */
    }
    return 0;
}

/* 协议层底层发送（uart_protocol.h 声明，这里实现） */
void UART_WriteByte(uint8_t byte)
{
    csi_uart_send(&console_uart, &byte, 1, 100);
}

/* 主循环里轮询调用：把队列里积压的帧全部处理掉 */
void uart_hal_poll(void)
{
    static uint32_t last_drop = 0;

    if (g_fq_drop != last_drop) {
        /* 只在变化时打一行：正常跑一辈子都不会出现 */
        printf("[UART] frame queue overflow! dropped=%u\r\n", g_fq_drop);
        last_drop = g_fq_drop;
    }

    while (g_fq_tail != g_fq_head) {
        uint8_t i = g_fq_tail;
        uint8_t cmd = g_fq[i].cmd;
        uint8_t len = g_fq[i].len;

        /* 注意：HandleFrame 可能很慢（0x13 会重绘），期间中断会继续往
         * 别的槽位写，不会打架（tail 那格没人写）。 */
        UART_HandleFrame(cmd, g_fq[i].data, len);
        g_fq_tail = (uint8_t)((i + 1) % FRAME_QUEUE_DEPTH);
    }
}

/* ui_app_tick 每轮调用：推进"串口静默"计时 */
void uart_hal_tick(uint16_t dt_ms)
{
    if (g_rx_quiet < RX_QUIET_IDLE - dt_ms) {
        g_rx_quiet = (uint16_t)(g_rx_quiet + dt_ms);
    } else {
        g_rx_quiet = RX_QUIET_IDLE;
    }
}

/* 距最后一个 RX 字节过去了多少毫秒（重绘前应 >= 150 才动手） */
uint16_t uart_hal_rx_quiet_ms(void)
{
    return g_rx_quiet;
}

/* 供自检/上报使用：读一次校验失败计数（调试看串口质量） */
uint32_t uart_hal_checksum_err_count(void)
{
    extern volatile uint32_t g_uart_checksum_err;
    return g_uart_checksum_err;
}

/* 供自检使用：队列溢出丢帧计数（正常应该一直是 0） */
uint32_t uart_hal_frame_drop_count(void)
{
    return g_fq_drop;
}
