// uart_esp.c — ESP32 -> TD1601 情感链路接收（UART3：**仅 PB18-RX**）
//
// ★ 2026-10-28 按实际 PCB 改为单向接收：
//   已流片的板子上 ESP32 与 TD1601 之间**只走了一根 PB18**（ESP32 GPIO14 ->
//   TD1601 PB18/UART3_RXD），PB19/TXD 没有引出。此前本文件按双向设计，
//   既配 PB19 为 UART3_TXD、又开机往它发一行 banner —— 都是在驱动一个
//   焊盘上不存在的线。现在：不配 PB19、不发 banner，与 coworker 版
//   （UserFolder/esp_link.c，只 portb_high_mux(18,0)）的硬件假设一致。
//   UART3 仍按标准初始化（接收必需），只是不再主动发送。
// ============================================================================
// 落位：**干净主线** projects(A)/baozi_LCD（main.c V2.1）。
// 说明见 include/uart_esp.h。这里讲四个"必须知道"的实现要点 + 一条移植注意。
//
// 1. **这是第二条独立串口**，与 uart_hal.c 的 UART0（PC/console）并存，互不干扰。
//    两条链路收到的命令都通过同一个钩子 BAOZI_SetEmotion() 驱动屏幕 ——
//    这正是 baozi_hooks.h 里"钩子层"存在的意义。
//
// 2. **ESP32 侧没有帧信封**：它就是把裸字节写进 Serial2（见 esp32_baozi.ino 的
//    onFrame()）。所以这里不能用 uart_protocol.c 那套 A5|LEN|CMD|DATA|CHK 解析器，
//    得自己写一个小的三态解析器。
//
// 3. **中断里只记账、不干活**（照抄 uart_hal.c 的纪律）。理由很硬：
//    BAOZI_SetEmotion() → ui_app_force_emotion() 会**整屏重绘**，
//    在中断里调会跟每字节的 RX 中断挤在一起（uart_hal.c 注释里点名的实机花屏高发窗口）；
//    printf 在中断里也不安全。
//    所以中断只把"待处理命令"存进邮箱并置标志，真正的执行在主循环 uart_esp_poll()。
//
// 4. **中断 + 轮询兜底**：与 uart_hal.c 同款（那边实测过中断可能不触发）。
//    轮询兜底在中断正常时是空转（FIFO 已被取空），无副作用。
//
// ---------------------------------------------------------------------------
// ⚠️ 移植注意：本模块从**上一代副本**（projects(A) - fuben(3)/baozi_LCD）搬来时，
//    只有下面"情绪映射"两张表需要改 —— 因为两代的 BAOZI_SetEmotion 行为不同：
//
//      上一代 baozi_app.c：BAOZI_SetEmotion(emo) → LCD_ShowEmotion(emo)
//        底表 emotions.c 有 **8** 张脸（emo_group_count = 8），[7] = NEUTRAL 彩色包子脸，
//        所以 emo_id=7 可以直接映成 7。
//
//      本代  baozi_proto_app.c：BAOZI_SetEmotion(emo) 先挡一道 ——
//            if (emo > EMOTION_ANGRY) return;      /* 0x06 以上**静默丢弃** */
//        再经 k_emo_name[7] 转成名字交给 ui_app_force_emotion()。
//        底表 anim/face_model.c 只有 **7** 个模型：
//            neutral / happy / sad / angry / surprised / loving / asleep
//        且**没有独立的 NEUTRAL 编号** —— neutral 就是 0x00(IDLE)。
//        所以本代所有映射值必须 ≤ 0x06，否则脸根本不变（无报错、无日志）。
//        （另外 "loving" 在协议里没有对应编号，只能由协议之外的通道指定。）
// ---------------------------------------------------------------------------
//
// ⚠️ 串口输出一律纯 ASCII 英文（README_交付说明.md 的规定）—— 中文只写在注释里。
// ============================================================================
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <drv/uart.h>
#include <drv/pin.h>
#include <drv/gpio.h>
#include <drv/common.h>
#include <soc.h>

#include "board_config.h"
#include "uart_esp.h"
#include "uart_protocol.h"   /* EMOTION_IDLE .. EMOTION_ANGRY */
#include "baozi_hooks.h"     /* BAOZI_SetEmotion() */
#include "net_link.h"        /* 配网命令 QR: / NET:（v2.1）*/

/* ============================ 引脚与外设 ============================ */
// PB18 = UART3_RXD（收 ESP32 的 GPIO14）——**实板只引出这一根**，见文件头。
//   idx = 3（uart_pinmap[] 里 PB18/PB19 的 idx 列），不是引脚号。
//   ⚠️ board_config.h 里**没有** UART3 的 EXAMPLE_PIN_* 宏（只有 UART0/1/2），
//      所以下面直接用 soc.h 的功能枚举 —— 这正是 pin_mux_table.md 第 7.3 节的写法。
#define ESP_UART_IDX        3
#define ESP_UART_BAUD       115200

#define ESP_PORTCTL_B       0x60030100U   /* PORTB：某位=1 表示该脚走复用功能 */
#define ESP_IOMUX0H_B       0x6003010CU   /* PORTB 的 PIN16~31，每脚占 2 bit */
#define ESP_REG32(a)        (*(volatile uint32_t *)(a))

/* 收到 0xAA 就认为后面跟着一个状态码（ESP32 固件 CTRL_STATE 的做法：
 *   Serial2.write(0xAA); Serial2.write(payload[1]);  共 2 字节，**不带换行**） */
#define ESP_STATE_MAGIC     0xAAu

/* 开机往 ESP32 回一行问候 —— **已关闭**。
 *
 * 原设计用途：ESP32 固件 loop() 里有 `while (Serial2.available())`，会把收到的
 * 字节转印到 USB 串口日志上，于是这行出现在 ESP32 监视器里就能证明
 * PB19 -> ESP32 GPIO21 这根线是通的。
 *
 * ★ 2026-10-28 置 0 的原因：实际 PCB 只有 PB18 一根线，PB19 未引出，
 *   发出去只会丢在空气里。保留这个宏是为了将来若补上回传线，
 *   改成 1 即可复用现有自检手段（此时启动日志会显示 TX=PB19）。 */
#define ESP_UART_TX_BANNER  0

/* 文本行缓冲。
 *
 * 64 -> 160（v2.1）：配网要收一整条二维码内容，最坏情况
 *     "QR:WIFI:T:WPA;S:" + 32 字节 SSID + ";P:" + 63 字节口令 + ";;"
 *   = 117 字节。原来 64 字节会把这条行当"超长"整行丢掉，配网永远收不到。
 * 超长仍然整行丢弃、绝不截断 —— 截断会造出 "INTENSITY:EMO:SAD" 那种幽灵命令。
 * 代价：两处静态缓冲各 160B（s_line + s_mb_line），对 192KB SRAM 无压力。 */
#define ESP_LINE_MAX        160

/* ============================ 情绪映射 ============================ */
// ESP32/PC 侧的状态码把三个量压成 1 个字节：code = (emo_id*8 + intent_id)*3 + intensity_id
// 取值范围 0..215 ⇒ 唯一自洽的维数是 emo_id 0..8 / intent_id 0..7 / intensity_id 0..2
// （216 = 9×8×3）。
//
// —— 下表已核对完毕，落地值**全部 ≤ 0x06**，依据是本代真实的面部模型表：——
//   UserFolder/anim/face_model.c 的 face_model_table() 只有 7 个模型；
//   baozi_proto_app.c 的 k_emo_name[7] 给出协议编号 → 名字的对应：
//       [0] "neutral"    [1] "happy"   [2] "sad"      [3] "surprised"
//       [4] "asleep"     [5] "neutral"（无 thoughtful 模型，退回中性）
//       [6] "angry"
//   且 BAOZI_SetEmotion() 开头就把 > EMOTION_ANGRY(0x06) 的值**直接丢掉**。
//   所以 emo_id 0..6 同序直映到 0..6 合法；7、8 没有对应，一律回 IDLE(0)。
//   （上一代把 7 映成 7 是有意义的 —— 那代有第 8 张 NEUTRAL 脸；本代没有，
//     映 7 会被丢掉，所以这里必须改成 EMOTION_IDLE。）
//
// ★ 权威依据（2026-10-17 拿到同学的《情绪状态切换_对接参考.md》）：
//   ESP/PC 侧 emo_id 的语义顺序已确认，**不是**按 TD1601 协议编号排的：
//       0 neutral / 1 happy / 2 sad / 3 angry / 4 surprised
//       5 loving / 6 idle / 7 bored / 8 sleeping      （9 项）
//   所以下面是"按名字搬"，不是"顺移一位"。
//
//   两处错位最坑：
//     · 3=angry（不是 surprised）、4=surprised（不是 angry）
//     · 6=idle 是**心情**，不是情绪。它的名字 "IDLE" 会命中本文件 k_emo_names 里
//       "IDLE" -> EMOTION_IDLE，语义上恰好等价（待机），所以这里也落 IDLE。
//   5=loving 本代面部栈**有**这个模型（anim/face_model 的 loving），但
//   uart_protocol.h 的编号里没有它（只到 0x06=ANGRY）→ 只能退回中性。
//   7 bored 也是心情：bored 没有独立模型，落 THOUGHTFUL 是"歪头想事情"的近似。
//   8 sleeping 落 SLEEPY —— baozi_proto_app.c 的 k_emo_name[4] 正是 "asleep"。
static const uint8_t k_esp_emo_to_td[9] = {
    EMOTION_IDLE,       /* 0 neutral   */
    EMOTION_HAPPY,      /* 1 happy     */
    EMOTION_SAD,        /* 2 sad       */
    EMOTION_ANGRY,      /* 3 angry     */
    EMOTION_SURPRISED,  /* 4 surprised */
    EMOTION_IDLE,       /* 5 loving    —— 无协议编号，退中性 */
    EMOTION_IDLE,       /* 6 idle      —— 心情，等价待机 */
    EMOTION_THOUGHTFUL, /* 7 bored     —— 心情，近似"歪头发呆" */
    EMOTION_SLEEPY,     /* 8 sleeping  —— 心情，对应 asleep 脸 */
};

/* 文本通道的名字 -> TD1601 协议情绪编号（一律 ≤ 0x06）。
 * 等价写法都列上，避免对方换个词就失灵。
 * 名字集按同学《对接参考》的 9 个 emo 名 + 3 个心情名 + 若干同义词补齐。
 * "LOVING" 落 IDLE：本代面部栈有 loving 模型，但协议编号里没有它（没有 0x07），
 * 只能由协议之外的通道（UI 直接调 ui_app_force_emotion）指定。 */
typedef struct { const char *name; int8_t td; } emo_name_t;

static const emo_name_t k_emo_names[] = {
    { "IDLE",       EMOTION_IDLE       },
    { "NEUTRAL",    EMOTION_IDLE       },
    { "SMILE",      EMOTION_IDLE       },
    { "HAPPY",      EMOTION_HAPPY      },
    { "LAUGH",      EMOTION_HAPPY      },
    { "JOY",        EMOTION_HAPPY      },
    { "SAD",        EMOTION_SAD        },
    { "CRY",        EMOTION_SAD        },
    { "SURPRISED",  EMOTION_SURPRISED  },
    { "SHOCKED",    EMOTION_SURPRISED  },
    { "SLEEPY",     EMOTION_SLEEPY     },
    { "YAWN",       EMOTION_SLEEPY     },
    { "ASLEEP",     EMOTION_SLEEPY     },
    { "SLEEPING",   EMOTION_SLEEPY     },   /* 心情名（emo_id 8）*/
    { "SLEEP",      EMOTION_SLEEPY     },
    { "THOUGHTFUL", EMOTION_THOUGHTFUL },
    { "PONDER",     EMOTION_THOUGHTFUL },
    { "THINK",      EMOTION_THOUGHTFUL },
    { "BORED",      EMOTION_THOUGHTFUL },   /* 心情名（emo_id 7），近似歪头发呆 */
    { "ANGRY",      EMOTION_ANGRY      },
    { "MAD",        EMOTION_ANGRY      },
    { "LOVING",     EMOTION_IDLE       },   /* 有模型无编号，退中性 */
};

/* ============================ 解析状态机 ============================ */
typedef enum {
    ESP_RX_IDLE = 0,     // 等 0xAA（状态帧）或可打印字符（文本行开头）
    ESP_RX_STATE_CODE,   // 刚收到 0xAA，等 1 字节状态码
    ESP_RX_LINE,         // 正在攒一行文本，等 \n / \r
} esp_rx_state_t;

/* 待处理"邮箱"：中断里写，主循环里取走执行 */
typedef enum {
    ESP_MB_NONE = 0,
    ESP_MB_STATE = 1,    // s_mb_code 有效
    ESP_MB_LINE  = 2,    // s_mb_line 有效
} esp_mailbox_kind_t;

static csi_uart_t       g_uart;
/* ⚠️ 这个接收缓冲**故意不加 volatile**（与 uart_hal.c 的 g_rx_byte 一致）：
 *    csi_uart_receive_async() 的形参是 `void *data`，传 volatile 指针会被
 *    判为 -Wdiscarded-qualifiers。驱动保证"收到字节 → 写缓冲 → 回回调"的顺序，
 *    回调里的读取跨过函数调用边界，编译器不会把它缓存在寄存器里，所以安全。 */
static uint8_t          g_rx_byte;

static esp_rx_state_t   s_state;          /* 只在中断(或轮询)上下文里动 */
static char             s_line[ESP_LINE_MAX];
static uint8_t          s_line_n;

static volatile uint8_t s_mb_kind;        /* esp_mailbox_kind_t，写它=交接完成 */
static volatile uint8_t s_mb_code;
static char             s_mb_line[ESP_LINE_MAX];

static uart_esp_stat_t  s_stat;
static volatile uint8_t s_banner_done;

/* ============================ 编码 / 解码 ============================ */
uint8_t uart_esp_pack_state(uint8_t emo_id, uint8_t intent_id, uint8_t intensity_id)
{
    /* 与 ESP32/PC 侧 state_code() 同式：code = (emo*8 + intent)*3 + intensity */
    return (uint8_t)(((emo_id % 9u) * 8u + (intent_id % 8u)) * 3u + (intensity_id % 3u));
}

void uart_esp_unpack_state(uint8_t code, uint8_t *emo_id, uint8_t *intent_id,
                           uint8_t *intensity_id)
{
    uint8_t rest;

    if (code > 215u) {
        code = 0u;                     // 越界当 IDLE，别让算出的 id 乱飞
    }
    rest = (uint8_t)(code / 3u);
    if (intensity_id) *intensity_id = (uint8_t)(code % 3u);
    if (intent_id)    *intent_id    = (uint8_t)(rest % 8u);
    if (emo_id)       *emo_id       = (uint8_t)(rest / 8u);
}

/* ============================ 分发（主循环上下文） ============================ */
static void esp_apply_state_code(uint8_t code)
{
    uint8_t e = 0, i = 0, s = 0, td;

    uart_esp_unpack_state(code, &e, &i, &s);
    td = k_esp_emo_to_td[e % 9u];

    /* 原始 id 和映射结果一起打 —— 这是校准 k_esp_emo_to_td 的唯一依据 */
    printf("[ESP] state code=%u -> emo_id=%u intent=%u intensity=%u => td_emo=%u\r\n",
           (unsigned)code, (unsigned)e, (unsigned)i, (unsigned)s, (unsigned)td);

    BAOZI_SetEmotion(td);
}

static int8_t esp_emo_name_to_td(const char *name)
{
    uint8_t k;
    for (k = 0; k < (uint8_t)(sizeof(k_emo_names) / sizeof(k_emo_names[0])); k++) {
        if (strcmp(name, k_emo_names[k].name) == 0) {
            return k_emo_names[k].td;
        }
    }
    return -1;
}

static void esp_apply_line(const char *line)
{
    if (line[0] == '\0') {
        return;                        /* 空行（对方补的 \n 造成的）忽略 */
    }

    /* ★ v2.1：配网命令优先（QR: / NET:）。放在情绪命令之前是因为它更长、
     * 更需要"整行不被误解析"，而且前缀与 EMO:/INTENT:/INTENSITY: 不冲突。 */
    if (net_link_handle_line(line)) {
        return;
    }
    if (strncmp(line, "EMO:", 4) == 0) {
        int8_t td = esp_emo_name_to_td(line + 4);
        if (td >= 0) {
            printf("[ESP] line EMO:%s -> td_emo=%d\r\n", line + 4, (int)td);
            BAOZI_SetEmotion((uint8_t)td);
        } else {
            printf("[ESP] line EMO unknown: %s\r\n", line + 4);
        }
        return;
    }
    if (strncmp(line, "INTENT:", 7) == 0) {
        /* 意图目前只打日志 —— 屏幕不按意图变脸（那是 emotion_fsm 移植进来之后的事）。
         * 想用起来就在这里加一个 BAOZI_SendEvent(...) 映射。 */
        printf("[ESP] line INTENT=%s\r\n", line + 7);
        return;
    }
    if (strncmp(line, "INTENSITY:", 10) == 0) {
        printf("[ESP] line INTENSITY=%s\r\n", line + 10);
        return;
    }
    /* 其余任意行（图片描述等）原样打印，**不当命令执行** */
    printf("[ESP] text: %s\r\n", line);
}

/* ============================ 收字节（中断/轮询上下文，只记账） ============================ */
static void esp_feed_byte(uint8_t b)
{
    s_stat.rx_bytes++;

    switch (s_state) {
    case ESP_RX_IDLE:
        if (b == ESP_STATE_MAGIC) {
            s_state = ESP_RX_STATE_CODE;
        } else if (b >= 0x20u && b < 0x7Fu) {
            s_line[0] = (char)b;
            s_line_n  = 1;
            s_state   = ESP_RX_LINE;
        }
        /* 其它（控制字符 / 噪声）直接丢 */
        break;

    case ESP_RX_STATE_CODE:
        s_stat.last_code = b;
        s_stat.state_frames++;
        s_mb_code = b;                 /* 存邮箱，主循环执行 */
        s_mb_kind = (uint8_t)ESP_MB_STATE;
        s_state   = ESP_RX_IDLE;
        break;

    case ESP_RX_LINE:
        if (b == '\n' || b == '\r') {
            s_line[s_line_n] = '\0';
            s_stat.text_lines++;
            memcpy(s_mb_line, s_line, (size_t)s_line_n + 1u);
            s_mb_kind = (uint8_t)ESP_MB_LINE;
            s_line_n  = 0;
            s_state   = ESP_RX_IDLE;
        } else if (b >= 0x20u && b < 0x7Fu) {
            if (s_line_n + 1u < ESP_LINE_MAX) {
                s_line[s_line_n++] = (char)b;
            } else {
                s_stat.dropped++;      /* 超长：整行丢弃，不截断 */
                s_line_n = 0;
                s_state  = ESP_RX_IDLE;
            }
        } else {
            s_stat.dropped++;          /* 行里混进非 ASCII（含 0xAA）：整行丢弃 */
            s_line_n = 0;
            s_state  = ESP_RX_IDLE;
        }
        break;

    default:
        s_state = ESP_RX_IDLE;
        break;
    }
}

/* 中断回调：每收满 1 字节触发一次。
 * ⚠️ 定义必须放在 uart_esp_init() **之前**，否则是隐式声明，严格 C 下编译不过。
 * ⚠️ 这里只做记账 + 存邮箱（都是几十纳秒的活），**不做 printf、不重绘**。 */
static void esp_uart_cb(csi_uart_t *uart, csi_uart_event_t event, void *arg)
{
    (void)arg;
    if (event == UART_EVENT_RECEIVE_COMPLETE) {
        esp_feed_byte(g_rx_byte);
        /* 重新挂 1 字节异步接收（芯片驱动官方示例同款写法） */
        csi_uart_receive_async(uart, &g_rx_byte, 1);
    }
}

/* ============================ 初始化 ============================ */
int uart_esp_init(void)
{
    csi_error_t ret;
    uint32_t    imx_before, imx_after;

    s_state   = ESP_RX_IDLE;
    s_line_n  = 0;
    s_line[0] = '\0';
    s_mb_kind = (uint8_t)ESP_MB_NONE;
    s_mb_code = 0;
    s_mb_line[0] = '\0';

    s_stat.rx_bytes     = 0;
    s_stat.state_frames = 0;
    s_stat.text_lines   = 0;
    s_stat.dropped      = 0;
    s_stat.last_code    = 0;

    /* ① 先内部上拉，再设复用 —— 顺序与 uart_hal.c / SDK 官方示例一致。
     *    ★ 2026-10-28：只配 PB18。PB19 在实板上没有引出（UART3_TXD 那根线不存在），
     *      配它等于把外设输出驱动到悬空焊盘，没有意义；且 coworker 版
     *      UserFolder/esp_link.c 同样只处理 PB18。 */
    csi_pin_mode(PORTB, PIN18, GPIO_MODE_PULLUP);
    csi_pin_set_mux(PORTB, PIN18, PB18_UART3_RXD);   /* 功能值 0 */

    /* ② ⚠️ 官方 pinmux.c 在 PORTB 分支漏了 `offset -= 16`，给 PB16~PB31 配外设时
     *    功能值被写进了 IOMUX0L（应为 IOMUX0H），目标脚自己的 IOMUX0H 纹丝不动。
     *    PB18/PB19 的功能值恰好是 0、与复位默认值相同，所以"写错寄存器"碰巧
     *    等于"写对了" —— 但如果这两个字段此前被别的代码写成了非 0，引脚就配不上，
     *    而且**从寄存器上完全看不出区别**（pin_mux_table.md 第 7.2 节的结论）。
     *    这里显式清零兜底：PB18 = IOMUX0H 的 bit4~5。
     *    （PB19 = bit6~7 不再处理 —— 该脚未引出，见上文 2026-10-28 说明。） */
    imx_before = ESP_REG32(ESP_IOMUX0H_B);
    ESP_REG32(ESP_IOMUX0H_B) = imx_before & ~(3u << 4);
    imx_after = ESP_REG32(ESP_IOMUX0H_B);
    if (imx_after != imx_before) {
        printf("[ESPUART] IOMUX0H fixed: 0x%X -> 0x%X (PB18 func -> 0)\r\n",
               (unsigned)imx_before, (unsigned)imx_after);
    }

    /* ③ 外设初始化：每一步都查返回值，杜绝"静默失败" */
    ret = csi_uart_init(&g_uart, ESP_UART_IDX);
    if (ret != CSI_OK) {
        printf("[ESPUART] csi_uart_init(3) FAILED ret=%d\r\n", (int)ret);
        return -1;
    }
    ret = csi_uart_baud(&g_uart, ESP_UART_BAUD);
    if (ret != CSI_OK) {
        printf("[ESPUART] csi_uart_baud FAILED ret=%d\r\n", (int)ret);
    }
    ret = csi_uart_format(&g_uart, UART_DATA_BITS_8, UART_PARITY_NONE, UART_STOP_BITS_1);
    if (ret != CSI_OK) {
        printf("[ESPUART] csi_uart_format FAILED ret=%d\r\n", (int)ret);
    }

    /* ④ 取证：中断号 + 关键寄存器（读出 0/全 1 说明外设没时钟、没使能） */
    printf("[ESPUART] irq_num=%d\r\n", (int)g_uart.dev.irq_num);
    {
        volatile uint32_t *ub = (volatile uint32_t *)g_uart.dev.reg_base;
        printf("[ESPUART] regs: IER=%u LSR=%u USR=%u\r\n",
               (unsigned)ub[0x004 / 4], (unsigned)ub[0x014 / 4], (unsigned)ub[0x07C / 4]);
    }

    /* ⑤ 挂中断 + 启动异步接收（驱动里 callback/receive 为空时会直接返回错误、
     *    什么都不做，且没有任何提示 —— 所以这里必须查返回值） */
    ret = csi_uart_attach_callback(&g_uart, esp_uart_cb, NULL);
    if (ret != CSI_OK) {
        printf("[ESPUART] attach_callback FAILED ret=%d\r\n", (int)ret);
    }
    ret = csi_uart_receive_async(&g_uart, &g_rx_byte, 1);
    if (ret != CSI_OK) {
        printf("[ESPUART] receive_async FAILED ret=%d (callback/receive not ready)\r\n",
               (int)ret);
    }

    /* ⑥ MUX 回读自检：pin_mux_table.md 反复强调"别凭感觉，要看寄存器"。
     *    PORTCTL 的 bit18 必须为 1（=已切复用），见该文档第 8.3 节的判读表。
     *    （bit19 不再检查：PB19 未引出，本文件不配置它。） */
    printf("[ESPUART] PORTCTL_B=0x%X (bit18 must be 1) IOMUX0H=0x%X\r\n",
           (unsigned)ESP_REG32(ESP_PORTCTL_B), (unsigned)ESP_REG32(ESP_IOMUX0H_B));

    printf("[ESPUART] UART3 ready @115200 8N1: RX=PB18 only (TX=PB19 not wired on PCB)\r\n");
    printf("[ESPUART] expect: AA <code>  or  lines like \"EMO:HAPPY\"\r\n");
    return 0;
}

/* ============================ 轮询（主循环上下文） ============================ */
int uart_esp_poll(void)
{
    uint8_t  kind;
    uint8_t  code;
    char     line[ESP_LINE_MAX];

#if ESP_UART_TX_BANNER
    /* 开机问候：ESP32 会把串口收到的字节转印到它的日志里（见 esp32_baozi.ino loop()）。
     * 所以这行出现在 ESP32 串口监视器上 == PB19 -> ESP32 GPIO21 这根线通了。
     * ⚠️ 用 csi_uart_send(&g_uart,...) 走 **UART3**；用 UART_WriteByte() 会发到
     *    UART0(console/PC) 去，那是个很隐蔽的错（对方一直收不到、这边还"发送成功"）。 */
    if (!s_banner_done) {
        static const char banner[] = "TD1601/baozi-lcd uart3-link ok\r\n";
        s_banner_done = 1;
        (void)csi_uart_send(&g_uart, banner, (uint32_t)(sizeof(banner) - 1u), 100);
    }
#endif

    /* ---- 轮询兜底（不依赖接收中断）----
     * csi_uart_receive(...,timeout=0) 是"FIFO 里有就读 1 字节、没有立刻返回 0"的语义。
     * 中断正常时 FIFO 已被中断取空，这里读不到东西，无副作用。
     * guard 上限防止异常状态下死循环。 */
    {
        uint8_t b;
        int guard = 0;
        while (guard++ < 64 && csi_uart_receive(&g_uart, &b, 1, 0) == 1) {
            esp_feed_byte(b);
        }
    }

    /* ---- 取邮箱执行（这里才能 printf / 重绘）---- */
    kind = s_mb_kind;
    if (kind == (uint8_t)ESP_MB_NONE) {
        return 0;
    }
    s_mb_kind = (uint8_t)ESP_MB_NONE;     /* 先清标志，免得执行期间来的新帧被吞掉 */
    code = s_mb_code;
    memcpy(line, s_mb_line, ESP_LINE_MAX);

    if (kind == (uint8_t)ESP_MB_STATE) {
        esp_apply_state_code(code);
    } else {
        esp_apply_line(line);
    }
    return 1;
}

const uart_esp_stat_t *uart_esp_stats(void)
{
    return &s_stat;
}
