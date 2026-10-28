/******************************************************************************
 * @file     servo_range_test.c
 * @brief    舵机活动范围测试（串口交互单步，115200-8N1）
 *
 * HW:  TD1601 EVB + 2 路舵机（底部排针引出）
 *   水平 YAW   = PA29 -> PA29_PWM_CH6（pinmap 6 -> csi ch3；2026-09 由 PA0 换脚）
 *   垂直 PITCH = PA27 -> PA27_PWM_CH4（pinmap 4 -> csi ch2）
 *   电源：舵机红线接外部 5V（不要用核心板 3V3），棕/黑线接电源负并与核心板
 *   共地；信号线接 PA29/PA27。TD1601 IO 是 1.8V 电平，舵机不动时先查电平转换。
 *
 * PWM 用法（对齐官方 projects/drivers/pwm/pwm_output）：
 *   csi_pin_set_mux() -> csi_pwm_init() -> csi_pwm_out_config(period,pulse,HIGH)
 *   -> csi_pwm_out_start()；改脉宽 = out_stop -> out_config -> out_start。
 *   注意 PWM_POLARITY_HIGH 时第二个参数就是【高电平脉宽】（驱动内部写
 *   compare = period - pulse，输出高电平 = pulse us，与 pwm_output 示例注释一致）。
 *
 * 为什么交互式而不是盲扫：舵机没有位置/电流反馈，软件无法判断"堵转"，
 * 自动扫描很容易把舵机顶死在机械极限上。交互式每次只走一步，人眼确认无
 * 异响再走下一步，到极限立刻反向退回，全程安全且可复现。
 *
 * 命令（串口助手 115200-8N1，回车结束）：
 *   h            帮助
 *   0 / 1        选择操作通道（0 = 水平 PA29，1 = 垂直 PA27）
 *   +[us]        脉宽增加（默认 20us），如 `+`、`+50`
 *   -[us]        脉宽减小（默认 20us），如 `-`、`-50`
 *   w<us>        直接设置脉宽，如 w1500（大跳变慎用）
 *   c            当前通道回中位
 *   b            两个通道都回中位
 *   z            把当前脉宽记为当前通道的新中位（中位标定）
 *   l<min>,<max> 设置当前通道软限位，如 l433,852
 *   i / p        打印状态（含本次会话到达过的脉宽范围 probed）
 *
 * 典型流程：
 *   1) 上电自动回中位（水平 1000us / 垂直 600us）；
 *   2) 选 `0`，反复发 `-` 一步一步逼近负方向极限，直到出现异响/堵转，
 *      立刻发 `+` 退回一步，记下退回后的脉宽；
 *   3) 反复发 `+` 逼近正方向极限，同样记录；
 *   4) 选 `1` 对垂直舵机重复 2)~3)；
 *   5) 用 `i` 读出两通道 probed 范围 -> 写进控制层限位宏（servo_core.h）。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <soc.h>
#include <drv/pwm.h>
#include <drv/pin.h>
#include <drv/uart.h>
#include <drv/common.h>     /* mdelay */
#include "board_config.h"
#include "servo_range_test.h"

/* console UART：components/minilibc/v1.0/src/minilibc_port.c 里的全局定义
 * （csi_uart_t console_uart）。发送走 printf，这里只借它做接收。 */
extern csi_uart_t console_uart;

/* 单通道状态 */
typedef struct {
    const char *name;       /* 自由度名 + 引脚（打印用） */
    uint32_t    ch;         /* csi PWM 通道号 */
    port_name_t port;       /* 引脚端口 */
    pin_name_t  pin;        /* 引脚号 */
    pin_func_t  func;       /* 引脚复用功能 */
    uint32_t    center;     /* 中位脉宽 us（z 命令可重新标定） */
    uint32_t    cur;        /* 当前脉宽 us */
    uint32_t    min;        /* 软限位下限 us */
    uint32_t    max;        /* 软限位上限 us */
    uint32_t    probed_min; /* 本次会话到达过的最小脉宽 */
    uint32_t    probed_max; /* 本次会话到达过的最大脉宽 */
} servo_ch_t;

static csi_pwm_t s_pwm;

static servo_ch_t s_ch[2] = {
    { "YAW   " SERVO_YAW_PIN_TXT, SERVO_YAW_CH,
      SERVO_YAW_PORT, SERVO_YAW_PIN, SERVO_YAW_FUNC,
      SERVO_YAW_CENTER, SERVO_YAW_CENTER,
      SERVO_DEFAULT_MIN_US, SERVO_DEFAULT_MAX_US,
      SERVO_YAW_CENTER, SERVO_YAW_CENTER },
    { "PITCH " SERVO_PITCH_PIN_TXT, SERVO_PITCH_CH,
      SERVO_PITCH_PORT, SERVO_PITCH_PIN, SERVO_PITCH_FUNC,
      SERVO_PITCH_CENTER, SERVO_PITCH_CENTER,
      SERVO_DEFAULT_MIN_US, SERVO_DEFAULT_MAX_US,
      SERVO_PITCH_CENTER, SERVO_PITCH_CENTER },
};

/* s_ch[] 的【数组下标】—— 注意它【不是】csi 通道号！
 * PITCH 的 csi 通道号是 2（见 servo_pins.h），但它在数组里的下标是 1。
 * 早前把 SERVO_PITCH_CH(=2) 直接当下标用，导致 s_ch[2] 越界读 ->
 * CPU Exception NO.5 (Load access fault)。数组下标一律用下面的 IDX_*。 */
#define IDX_YAW             0U
#define IDX_PITCH           1U

static uint32_t s_active = IDX_YAW;         /* 当前操作的通道（数组下标）*/

/* ============================== 工具函数 ============================== */

/* 脉宽钳制：软限位 + 硬限位双保险，任何输入都不会输出越界脉宽 */
static uint32_t pulse_clamp(const servo_ch_t *s, int32_t pulse)
{
    uint32_t min = s->min;
    uint32_t max = s->max;

    if (min < SERVO_HARD_MIN_US) {
        min = SERVO_HARD_MIN_US;
    }
    if (max > SERVO_HARD_MAX_US) {
        max = SERVO_HARD_MAX_US;
    }
    if (min > max) {
        min = max;
    }

    if (pulse < (int32_t)min) {
        pulse = (int32_t)min;
    }
    if (pulse > (int32_t)max) {
        pulse = (int32_t)max;
    }
    return (uint32_t)pulse;
}

/* 打印一通道状态（含距中位偏移与本次会话探到的范围） */
static void print_one(uint32_t idx)
{
    const servo_ch_t *s = &s_ch[idx];
    int32_t off = (int32_t)s->cur - (int32_t)s->center;
    const char *sign = (off >= 0) ? "+" : "-";
    uint32_t mag = (off >= 0) ? (uint32_t)off : (uint32_t)(-off);

    printf("[range] %s ch%u pulse=%uus center=%uus off=%s%uus limit=[%u,%u] probed=[%u,%u]%s\r\n",
           s->name, s->ch, s->cur, s->center, sign, mag,
           s->min, s->max, s->probed_min, s->probed_max,
           (idx == s_active) ? "  <=active" : "");
}

static void print_all(void)
{
    print_one(IDX_YAW);
    print_one(IDX_PITCH);
}

static void print_help(void)
{
    printf("\r\n==== servo range test (%s=YAW ch%u / %s=PITCH ch%u, 50Hz) ====\r\n",
           SERVO_YAW_PIN_TXT, SERVO_YAW_CH, SERVO_PITCH_PIN_TXT, SERVO_PITCH_CH);
    printf(" 0/1           select channel (0=horizontal %s, 1=vertical %s)\r\n",
           SERVO_YAW_PIN_TXT, SERVO_PITCH_PIN_TXT);
    printf(" +[us] / -[us] pulse += / -= (default %uus), e.g. `+`, `-50`\r\n", SERVO_STEP_US);
    printf(" w<us>         set pulse directly, e.g. w1500\r\n");
    printf(" c             current channel back to center\r\n");
    printf(" b             both channels back to center\r\n");
    printf(" z             set current pulse as new center (calibration)\r\n");
    printf(" l<min>,<max>  set soft limit of current channel, e.g. l433,852\r\n");
    printf(" i / p         print status (probed = min/max seen this session)\r\n");
    printf(" h / ?         this help\r\n");
    printf(" step by step: watch/hold for buzzing, back off immediately if jammed\r\n\r\n");
}

/* 解析无符号十进制（不依赖 minilibc 的 atoi） */
static int parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    if (*s < '0' || *s > '9') {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

/* 解析 "min,max" */
static int parse_pair(const char *s, uint32_t *a, uint32_t *b)
{
    if (parse_u32(s, a) != 0) {
        return -1;
    }
    while (*s != '\0' && *s != ',') {
        s++;
    }
    if (*s != ',') {
        return -1;
    }
    return parse_u32(s + 1, b);
}

/* ============================== PWM 输出 ============================== */

/* 设置某通道脉宽（us），自动钳制并记录探到的范围 */
static int servo_write(uint32_t idx, int32_t pulse, int quiet)
{
    servo_ch_t *s = &s_ch[idx];
    uint32_t p = pulse_clamp(s, pulse);

    csi_pwm_out_stop(&s_pwm, s->ch);

    if (csi_pwm_out_config(&s_pwm, s->ch, SERVO_PERIOD_US, p,
                           PWM_POLARITY_HIGH) != CSI_OK) {
        printf("[range] %s ch%u config fail\r\n", s->name, s->ch);
        return -1;
    }
    if (csi_pwm_out_start(&s_pwm, s->ch) != CSI_OK) {
        printf("[range] %s ch%u start fail\r\n", s->name, s->ch);
        return -1;
    }
    s->cur = p;
    if (p < s->probed_min) {
        s->probed_min = p;
    }
    if (p > s->probed_max) {
        s->probed_max = p;
    }
    if (p != (uint32_t)((pulse < 0) ? 0 : pulse)) {
        /* 被限位钳制时明确提示，避免误判成"舵机不动" */
        printf("[range] clamped to limit [%u,%u]\r\n", s->min, s->max);
    }
    if (!quiet) {
        print_one(idx);
    }
    return 0;
}

/* ============================== 串口接收 ============================== */

/* 阻塞读一行（回车/换行结束），返回长度；期间超时继续等待 */
static int read_line(char *buf, int size)
{
    int n = 0;

    for (;;) {
        char c = 0;
        int32_t r = csi_uart_receive(&console_uart, &c, 1, SERVO_RX_TIMEOUT_MS);

        if (r <= 0) {
            continue;               /* 超时：继续等，PWM 保持输出不变 */
        }
        if (c == '\r' || c == '\n') {
            if (n == 0) {
                continue;           /* 忽略空行 */
            }
            break;
        }
        if ((c == 0x08 || c == 0x7F) && n > 0) {    /* Backspace */
            n--;
            continue;
        }
        if (n < size - 1) {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return n;
}

/* ============================== 命令处理 ============================== */

static void handle_command(char *line)
{
    char cmd;
    uint32_t v = 0;

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    cmd = line[0];

    switch (cmd) {
    case 'h':
    case 'H':
    case '?':
        print_help();
        break;

    case '0':
        s_active = IDX_YAW;
        print_one(s_active);
        break;

    case '1':
        s_active = IDX_PITCH;
        print_one(s_active);
        break;

    case '+':
        v = SERVO_STEP_US;
        if (parse_u32(line + 1, &v) != 0) {
            v = SERVO_STEP_US;
        }
        if (v == 0) {
            v = SERVO_STEP_US;
        }
        servo_write(s_active, (int32_t)s_ch[s_active].cur + (int32_t)v, 0);
        break;

    case '-':
        v = SERVO_STEP_US;
        if (parse_u32(line + 1, &v) != 0) {
            v = SERVO_STEP_US;
        }
        if (v == 0) {
            v = SERVO_STEP_US;
        }
        servo_write(s_active, (int32_t)s_ch[s_active].cur - (int32_t)v, 0);
        break;

    case 'w':
    case 'W':
        if (parse_u32(line + 1, &v) != 0) {
            printf("[range] usage: w<us>, e.g. w1500\r\n");
            break;
        }
        servo_write(s_active, (int32_t)v, 0);
        break;

    case 'c':
    case 'C':
        servo_write(s_active, (int32_t)s_ch[s_active].center, 0);
        break;

    case 'b':
    case 'B':
        servo_write(IDX_YAW, (int32_t)s_ch[IDX_YAW].center, 1);
        servo_write(IDX_PITCH, (int32_t)s_ch[IDX_PITCH].center, 1);
        print_all();
        break;

    case 'z':
    case 'Z':
        s_ch[s_active].center = s_ch[s_active].cur;
        printf("[range] new center of ch%u = %uus\r\n",
               s_ch[s_active].ch, s_ch[s_active].center);
        print_one(s_active);
        break;

    case 'l':
    case 'L': {
        uint32_t min = 0;
        uint32_t max = 0;

        if (parse_pair(line + 1, &min, &max) != 0 || min >= max) {
            printf("[range] usage: l<min>,<max>, e.g. l433,852 (min<max)\r\n");
            break;
        }
        if (min < SERVO_HARD_MIN_US) {
            min = SERVO_HARD_MIN_US;
        }
        if (max > SERVO_HARD_MAX_US) {
            max = SERVO_HARD_MAX_US;
        }
        s_ch[s_active].min = min;
        s_ch[s_active].max = max;
        printf("[range] ch%u soft limit = [%u,%u]us\r\n",
               s_ch[s_active].ch, min, max);
        /* 立刻按新限位重新下发一次，保证当前位置也在限位内 */
        servo_write(s_active, (int32_t)s_ch[s_active].cur, 0);
        break;
    }

    case 'i':
    case 'I':
    case 'p':
    case 'P':
        print_all();
        break;

    default:
        printf("[range] unknown command \"%s\" (h for help)\r\n", line);
        break;
    }
}

/* ============================== 测试入口 ============================== */

void servo_range_test(void)
{
    /* 1) 引脚复用（通路定义在 servo_pins.h）：PA29 -> CH6(csi 3)，PA27 -> CH4(csi 2) */
    csi_pin_set_mux(s_ch[IDX_YAW].port, s_ch[IDX_YAW].pin,
                    s_ch[IDX_YAW].func);
    csi_pin_set_mux(s_ch[IDX_PITCH].port, s_ch[IDX_PITCH].pin,
                    s_ch[IDX_PITCH].func);

    /* 2) PWM 控制器初始化 */
    if (csi_pwm_init(&s_pwm, SERVO_PWM_IDX) != CSI_OK) {
        printf("[range] pwm init fail\r\n");
        return;
    }

    /* 3) 先回中位，避免上电乱动 */
    servo_write(IDX_YAW, (int32_t)s_ch[IDX_YAW].center, 1);
    servo_write(IDX_PITCH, (int32_t)s_ch[IDX_PITCH].center, 1);
    mdelay(1000);

    printf("\r\n[range] servo range test @ %u baud, period=%uus\r\n",
           SERVO_CMD_BAUD, SERVO_PERIOD_US);
    print_help();
    print_all();
    printf("[range] ready. type command + Enter.\r\n");

    /* 4) 命令循环：PWM 一直输出，只有收到命令才改变脉宽 */
    for (;;) {
        char line[32];

        if (read_line(line, (int)sizeof(line)) > 0) {
            handle_command(line);
        }
    }
}
