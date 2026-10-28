// uart_protocol.h — UART 通信协议定义（v1.0）
// ============================================================================
// 三板斧通信"宪法"的代码版。所有命令字、编号、函数签名全在这。
// TD1601 端 #include 这个头文件，实现 uart_protocol.c 即可接入现有情感系统。
//
// 协议文档详见 docs/UART通信协议.md
// ============================================================================
#ifndef _UART_PROTOCOL_H_
#define _UART_PROTOCOL_H_

#include <stdint.h>

// ===== 帧头 =====
#define FRAME_HEAD      0xA5

// ===== 下行命令字（PC/ESP32 → TD1601）=====
typedef enum {
    CMD_SET_EMOTION     = 0x01,  // 设置情绪 DATA=[emotion:1]
    CMD_SEND_EVENT      = 0x02,  // 发送事件 DATA=[event:1]
    CMD_SWITCH_PERSON   = 0x03,  // 切换性格 DATA=[name_len:1][name:N]
    CMD_PLAY_SCRIPT     = 0x04,  // 播放脚本 DATA=[name_len:1][name:N]
    CMD_STOP_SCRIPT     = 0x05,  // 停止脚本 DATA=无
    CMD_SET_SERVO       = 0x06,  // 设置舵机 DATA=[servo_id:1][angle:1]
    CMD_RESET           = 0x07,  // 复位     DATA=无
    CMD_QUERY_STATUS    = 0x08,  // 查询状态 DATA=无（触发上行RSP_STATUS）
    CMD_HEARTBEAT       = 0x09,  // 心跳     DATA=无（推进1秒+状态上报）
    CMD_DANCE           = 0x0A,  // 跳舞     DATA=无（播放内置舞蹈脚本，ASRPRO"跳舞"命令）
    // ---- v1.1 新增：时间 / 习惯看板（包子屏幕打卡用，布局见文件末尾）----
    CMD_SYNC_TIME       = 0x10,  // 对时     DATA=[mon][day][weekday][hh][mm][ss?]
    CMD_HABIT_BEGIN     = 0x11,  // 习惯列表开始 DATA=[count]
    CMD_HABIT_ITEM      = 0x12,  // 下发一个习惯 DATA=变长（16 字节头 + 名字点阵）
    CMD_HABIT_END       = 0x13,  // 习惯列表结束 DATA=[count]
    // ---- v1.5 新增：TODAY 页日程（布局见文件末尾 SCHED 段）----
    CMD_SCHED_BEGIN     = 0x14,  // 日程列表开始 DATA=[count]
    CMD_SCHED_ITEM      = 0x15,  // 下发一条日程 DATA=变长（10 字节头 + 标题点阵）
    CMD_SCHED_END       = 0x16,  // 日程列表结束 DATA=[count]
    // ---- v1.8 新增：提醒弹卡（PC 判定时机，板子只负责展示）----
    CMD_REMIND          = 0x17,  // 提醒弹卡 DATA=变长（6 字节头 + 标题点阵）
} DownCmd_t;

// ===== 上行报告字（TD1601 → PC/ESP32）=====
typedef enum {
    RSP_STATUS          = 0x81,  // 状态上报 DATA=[emo:1][energy:1][idle_h:1][idle_l:1][emo_h:1][emo_l:1]
    RSP_SENSOR          = 0x82,  // 传感器事件 DATA=[sensor_type:1]
    RSP_SCRIPT_DONE     = 0x83,  // 脚本完毕 DATA=无
    RSP_SERVO_DONE      = 0x84,  // 舵机到位 DATA=无
    RSP_HABIT_CHECKIN   = 0x90,  // 打卡上报 DATA=[idx][slot][action]（action 0=打卡 1=取消）
    RSP_HABIT_SYNCED    = 0x91,  // 习惯同步完成 DATA=[count]（日程同步完成也复用它）
    RSP_HABIT_REQ       = 0x92,  // 习惯同步请求 DATA=无（板子空手时说"请把习惯发一遍"）
    RSP_SCHED_CHECK     = 0x93,  // 日程状态切换上报 DATA=[idx][action]（0=完成 1=撤销）
    RSP_SCHED_REQ       = 0x94,  // 日程同步请求 DATA=无（TODAY 页空手时催 PC）
    RSP_REMIND_CLOSED   = 0x95,  // 提醒弹卡已关闭 DATA=[type][reason]（0=点击 1=超时）
    RSP_ERROR           = 0x8F,  // 错误报告 DATA=[error_code:1]
} UpRsp_t;

// ===== 上行通知字（TD1601 → ASRPRO 主动通知，天问侧按此配对）=====
#define CMD_NOTIFY_LEAVE    0x0B   // 通知"我退下了"：进入休眠序列前广播，帧 A5 00 0B AE

// ===== 下行唤醒字（ASRPRO → TD1601，天问侧唤醒词"包子"触发时发送）=====
#define CMD_WAKEUP          0x0C   // 唤醒：浅睡眠中收到则亮屏回待机微笑，帧 A5 00 0C A9

// ===== 情绪编号（与 Emotion_t 一致，永远不改）=====
#define EMOTION_IDLE       0x00
#define EMOTION_HAPPY      0x01
#define EMOTION_SAD        0x02
#define EMOTION_SURPRISED   0x03
#define EMOTION_SLEEPY     0x04
#define EMOTION_THOUGHTFUL 0x05
#define EMOTION_ANGRY      0x06

// ===== 事件编号（与 FsmEvent_t 对应）=====
#define EVT_CODE_TOUCH       0x01
#define EVT_CODE_INFRARED    0x02
#define EVT_CODE_VOICE_HAPPY 0x03
#define EVT_CODE_VOICE_SAD   0x04
#define EVT_CODE_VOICE_THINK 0x05
#define EVT_CODE_RESET       0x06

// ===== 传感器类型编号（RSP_SENSOR 的 DATA）=====
#define SENSOR_TOUCH      0x00
#define SENSOR_INFRARED   0x01

// ===== 错误码（RSP_ERROR 的 DATA）=====
typedef enum {
    ERR_UNKNOWN_CMD      = 0x01,  // 未知命令字
    ERR_LEN_MISMATCH     = 0x02,  // 数据长度错误
    ERR_CHECKSUM         = 0x03,  // 校验失败
    ERR_EMOTION_RANGE    = 0x04,  // 情绪编号越界（>0x06）
    ERR_PERSON_NOT_FOUND = 0x05,  // 性格包未找到
    ERR_SCRIPT_NOT_FOUND = 0x06,  // 脚本未找到
    ERR_HABIT_RANGE      = 0x07,  // 习惯序号越界 / 点阵长度不足
    ERR_HABIT_FULL       = 0x08,  // 习惯缓存已满（超过 HABIT_MAX_COUNT）
} ErrorCode_t;

// ============================================================================
// v1.1 习惯看板协议（包子屏幕打卡）
// ----------------------------------------------------------------------------
// 为什么加这一组：原 10 条下行命令全是给情感系统设计的，没有一条能承载
// "今天有哪些习惯、哪一格打没打"。屏幕要显示内容、要能打卡，必须扩协议。
//
// 交互闭环：
//   PC 桥接脚本                          TD1601
//   ── 0x10 对时 ──────────────────────►  更新顶部时钟
//   ── 0x11 HABIT_BEGIN ──────────────►  清空旧缓存
//   ── 0x12 HABIT_ITEM ×N ────────────►  逐个写入
//   ── 0x13 HABIT_END ────────────────►  重绘 HABIT 卡片 + 回 0x91
//   ◄──────────── 0x92 REQ ───────────  板子还没数据，催 PC 重发一遍
//   ◄──────────── 0x90 CHECKIN ───────  用户点了某一格（0=打卡 1=取消）
//
// 为什么要有 0x92：板子可能比 PC 先开机（或者 PC 那边脚本重启过），
// 这时屏幕是空的。用户只能干等下一次推送；有了它，点一下空卡片就等于
// "喂，把数据发过来"，PC 收到立刻重推。没有它，实机上表现为
// "屏幕显示 waiting data...，点什么都没反应"，看起来像坏了。
//
// CMD_HABIT_ITEM(0x12) 的 DATA 布局：15 字节定长头 + 变长名字点阵
//   [0]     idx         习惯序号 0..count-1
//   [1]     mode        HABIT_MODE_SINGLE / HABIT_MODE_MULTI
//   [2]     total       格子总数 1..HABIT_MAX_SLOT
//   [3]     done        已确认格子数
//   [4]     state       HABIT_STATE_xxx（总状态，决定卡片配色）
//   [5]     cur_slot    当前可打卡的格子序号；HABIT_SLOT_NONE = 当前没有
//   [6..7]  streak      连续天数（小端 16 位）
//   [8..11] mask        已完成格子位掩码（小端；bit i = 第 i 格）
//   [12]    name_chars  名字字数 0..HABIT_NAME_MAX_CHARS（0 = 不画名字）
//   [13]    name_w      点阵宽（像素，须为 8 的倍数）
//   [14]    name_h      点阵高（像素）
//   [15]    open_max    已开窗的最大格子序号（含）；HABIT_OPEN_NONE = 一格都没开窗
//   [16..]  bitmap      名字点阵，逐行扫描，每行 name_w/8 字节，MSB = 最左
//
// open_max 为什么必须有（v1.9）：板子只拿得到"总状态 + cur_slot + mask"，分不出
// 某格是"还没到时间"还是"已经错过、可以补卡"。一天三次的习惯，13:00 时前两格
// missed、第三格 future，cur_slot = 0xFF，板子于是把第三格也当成能点 —— 用户
// 点一下，未来那一次就被提前记上了。PC 端把"已开窗到第几格"算好发下来，
// 板子只比大小，不需要懂任何时间规则（判定源仍然只有后端一个）。
//
// 长度校验：len >= 16 + ceil(chars*name_w/8)*name_h，否则丢帧回 ERR_LEN_MISMATCH。
// 帧长上限：6 字 × 16×16(32B) = 192 + 16 = 208 < FRAME_MAX_DATA(255)，单帧装得下。
//
// 为什么名字用点阵而不是"字库中第几个字"：习惯名是用户在手机上自己起的中文，
// 板载字库不可能覆盖。让 PC 端渲染好点阵传下来，任意中文都能显示，也不占 Flash。
// ============================================================================

// ===== 习惯看板：设备端上限（PC 端下发前请对照）=====
// 8 -> 12：小程序里超过 8 个的习惯会被 PC 端静默截断（日志"板子只存得下 8 个"），
// 用户新建的第 9、10 条在屏幕上根本看不到。每条约 210B(含 192B 点阵)，
// 多 4 条只多吃 840B BSS，对当前余量无压力。
// ⚠️ 改这个值必须同步 PC 端 baozi-uart-test.py 的 HABIT_MAX_COUNT 和
//    _check_habit_proto.py 的断言，否则握手校验会对不上。
#define HABIT_MAX_COUNT      12    // 板子最多缓存几个习惯
#define HABIT_MAX_SLOT       32    // 单个习惯最多几格
#define HABIT_NAME_MAX_CHARS 6     // 名字最多几个字
#define HABIT_MODE_SINGLE    0
#define HABIT_MODE_MULTI     1
#define HABIT_STATE_FUTURE   0     // 还没到时间
#define HABIT_STATE_PENDING  1     // 窗口内，该打卡
#define HABIT_STATE_DONE     2     // 已完成
#define HABIT_STATE_MISSED   3     // 窗口已过、未确认
#define HABIT_SLOT_NONE      0xFF  // cur_slot 的"当前无"取值
#define HABIT_OPEN_NONE      0xFF  // open_max 的"一格都没开窗"取值

// ===== 最大帧大小 =====
#define FRAME_MAX_DATA     255     // DATA 区最大字节数
#define FRAME_MAX_SIZE     259     // 总帧长 = 1(头)+1(LEN)+1(CMD)+255(DATA)+1(校验)

// ============================================================================
// v1.5 TODAY 页日程协议
// ----------------------------------------------------------------------------
// CMD_SCHED_ITEM(0x15) 的 DATA 布局：10 字节定长头 + 变长标题点阵
//   [0]    idx      日程序号 0..count-1（按日期+时间排好序再下发）
//   [1]    mon      月 1..12
//   [2]    day      日 1..31
//   [3]    weekday  1=周一 .. 7=周日
//   [4]    hh       时 0..23
//   [5]    mm       分 0..59
//   [6]    status   SCHED_STATUS_PENDING(0) / SCHED_STATUS_DONE(1)
//   [7]    title_chars 标题字数 0..SCHED_TITLE_MAX_CHARS(7)，0=不画标题
//   [8]    title_w  点阵单字宽（像素，8 的倍数，如 16）
//   [9]    title_h  点阵高（像素，如 16）
//   [10..] bitmap   标题点阵，逐行扫描，每行 title_w/8 字节，MSB=最左
//
// 长度上限：10 + 7字*(16/8)*16 = 234 < FRAME_MAX_DATA(255)。
// 下发顺序：按 (date,time) 升序；只发"今天及以后"的日程；最多 SCHED_MAX_ITEMS 条。
// 交互：点事件行 → 上行 0x93 [idx][action]，PC 翻译成
//       PATCH /api/v1/events/{id}/status，成功后重推日程校正。
// ============================================================================

// ===== 日程：设备端上限（PC 端下发前请对照）=====
// 标题 6 字：16px 点阵在板端 2x 放大成 32px 大字，6 字刚好占满卡片宽度；
// 帧 DATA 上限 255B 也只装得下 6 字（10 + 6*2*16 = 202）。
#define SCHED_MAX_ITEMS        12
#define SCHED_TITLE_MAX_CHARS  6
#define SCHED_STATUS_PENDING   0
#define SCHED_STATUS_DONE      1

// ============================================================================
// v1.8 提醒弹卡协议
// ----------------------------------------------------------------------------
// 谁判定时机：**只有 PC 端**（桥接脚本轮询后端：习惯 slot 变 pending、
// 日程开始前 10 分钟）。板子不存时间表、不做任何判定，收到 0x17 就弹卡。
// 声音也从 PC 出（板子无喇叭）：PC 端 EdgeTTS 先播，再发 0x17。
//
// CMD_REMIND(0x17) 的 DATA 布局：6 字节定长头 + 变长标题点阵
//   [0]  type        REMIND_TYPE_HABIT(0) / REMIND_TYPE_SCHED(1)
//   [1]  hh          时 0..23（弹卡上显示的时间）
//   [2]  mm          分 0..59
//   [3]  title_chars 标题字数 0..REMIND_TITLE_MAX_CHARS(7)，0=只显示时间
//   [4]  title_w     点阵单字宽（像素，8 的倍数，如 16）
//   [5]  title_h     点阵高（像素，如 16）
//   [6..] bitmap     标题点阵（同 0x12/0x15 格式）
//
// 长度上限：6 + 7字*(16/8)*16 = 230 < FRAME_MAX_DATA(255)。
// 板子行为：弹"即将开始"卡片，点一下或 60 秒自动关闭，回 RSP_REMIND_CLOSED。
// 番茄钟计时中不打断（只留 PC 的声音），番茄钟结束后再弹。
// ============================================================================
#define REMIND_TYPE_HABIT      0
#define REMIND_TYPE_SCHED      1
#define REMIND_TITLE_MAX_CHARS 7

// ===== 接收状态机状态 =====
typedef enum {
    RX_WAIT_HEAD,    // 等待帧头 0xA5
    RX_WAIT_LEN,     // 等待 LEN
    RX_WAIT_CMD,     // 等待 CMD
    RX_RECV_DATA,    // 接收 DATA
    RX_WAIT_CHECK,   // 等待校验
} RxState_t;

// ===== 接收解析器（每次喂一个字节，帧完整时回调）=====
// 用法：UART 中断里每收到一个字节调 UART_RxFeed(byte)
//      帧完整时会自动调用 UART_HandleFrame(cmd, data, len)
typedef struct {
    RxState_t state;
    uint8_t   len;           // 本帧 DATA 长度
    uint8_t   cmd;           // 本帧 CMD
    uint8_t   data[FRAME_MAX_DATA]; // DATA 缓冲区
    uint8_t   data_idx;      // 已接收 DATA 字节数
    uint8_t   checksum;      // 累计校验值
} UartRx_t;

// ===== 公开接口 =====

// 初始化接收解析器
void        UART_RxInit(UartRx_t *rx);

// 喂一个字节给解析器（在 UART 中断里逐字节调用）
// 帧完整时返回 1，否则返回 0
int         UART_RxFeed(UartRx_t *rx, uint8_t byte, uint8_t *out_cmd, uint8_t *out_data, uint8_t *out_len);

// 计算校验和（帧头 ^ LEN ^ CMD ^ DATA）
uint8_t     UART_CalcChecksum(uint8_t cmd, const uint8_t *data, uint8_t len);

// 构建一帧并发送（TD1601 端用：状态上报/传感器事件/错误报告）
// 调用者需实现 UART_WriteByte() 硬件函数
void        UART_SendFrame(uint8_t cmd, const uint8_t *data, uint8_t len);

// 便捷发送函数
void        UART_SendStatus(void);              // 状态上报
void        UART_SendSensorEvent(uint8_t type);  // 传感器事件
void        UART_SendScriptDone(void);           // 脚本完毕
void        UART_SendServoDone(void);            // 舵机到位
void        UART_SendError(uint8_t code);        // 错误报告
void        UART_SendHabitCheckin(uint8_t idx, uint8_t slot, uint8_t action); // 打卡上报
void        UART_SendHabitSynced(uint8_t count);                              // 习惯同步完成
void        UART_SendHabitReq(void);                                          // 请求 PC 重发习惯
void        UART_SendSchedCheck(uint8_t idx, uint8_t action);                 // 日程状态切换上报
void        UART_SendSchedSynced(uint8_t count);                              // 日程同步完成（0x91）
void        UART_SendSchedReq(void);                                          // 请求 PC 重发日程
void        UART_SendRemindClosed(uint8_t type, uint8_t reason);              // 提醒弹卡已关闭（0x95）

// 帧处理回调（收到完整帧后调用，用户在 uart_protocol.c 里实现）
void        UART_HandleFrame(uint8_t cmd, const uint8_t *data, uint8_t len);

// 底层硬件函数（用户在 uart_hal.c 里实现，适配具体单片机）
void        UART_WriteByte(uint8_t byte);       // 发送一个字节

#endif
