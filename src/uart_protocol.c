// uart_protocol.c — UART 通信协议实现（v1.1 · TD1601 硬件版）
// ============================================================================
// 来源：从旧工程 2/projects（A)/baozi/src/uart_protocol.c 移入 baozi_LCD，
//       协议逻辑一字未改（帧头 0xA5 / LEN / CMD / DATA / 异或校验）。
//
// 与硬件/功能层的两条边界都在别处实现：
//   · UART_WriteByte()      → src/uart_hal.c       （CSI 驱动真发字节，走 UART2）
//   · BAOZI_xxx 功能钩子     → src/baozi_proto_app.c（对接 face_app / servo）
//
// 注意：UART_RxFeed() 在 UART 中断里被调用，里面不能 printf（校验失败只计数）。
// ============================================================================
#include "uart_protocol.h"
#include "baozi_hooks.h"
#include <stdio.h>
#include <string.h>

// 校验失败计数（调试用：主循环里可以随时看）
volatile uint32_t g_uart_checksum_err = 0;

// ===== 接收解析器 =====

void UART_RxInit(UartRx_t *rx) {
    rx->state     = RX_WAIT_HEAD;
    rx->len       = 0;
    rx->cmd       = 0;
    rx->data_idx  = 0;
    rx->checksum  = 0;
}

// 每次喂一个字节，帧完整时返回 1
// 注意：此函数在 UART 中断里被调用，不要在这里 printf 或做重活
int UART_RxFeed(UartRx_t *rx, uint8_t byte, uint8_t *out_cmd, uint8_t *out_data, uint8_t *out_len) {
    switch (rx->state) {

    case RX_WAIT_HEAD:
        if (byte == FRAME_HEAD) {
            rx->checksum = FRAME_HEAD;
            rx->state = RX_WAIT_LEN;
        }
        break;

    case RX_WAIT_LEN:
        rx->len = byte;
        rx->checksum ^= byte;
        rx->state = RX_WAIT_CMD;
        break;

    case RX_WAIT_CMD:
        rx->cmd = byte;
        rx->checksum ^= byte;
        rx->data_idx = 0;
        if (rx->len == 0) {
            rx->state = RX_WAIT_CHECK;
        } else {
            rx->state = RX_RECV_DATA;
        }
        break;

    case RX_RECV_DATA:
        if (rx->data_idx < FRAME_MAX_DATA) {
            rx->data[rx->data_idx++] = byte;
        } else {
            // 越界保护：丢掉这帧，回等帧头
            rx->state = RX_WAIT_HEAD;
            break;
        }
        rx->checksum ^= byte;
        if (rx->data_idx >= rx->len) {
            rx->state = RX_WAIT_CHECK;
        }
        break;

    case RX_WAIT_CHECK:
        if (byte == rx->checksum) {
            // 校验通过！帧完整
            *out_cmd  = rx->cmd;
            *out_len  = rx->len;
            if (rx->len > 0 && out_data) {
                memcpy(out_data, rx->data, rx->len);
            }
            rx->state = RX_WAIT_HEAD;  // 回到等帧头
            return 1;
        } else {
            // 校验失败，丢帧（中断里不能 printf，只计数）
            g_uart_checksum_err++;
            rx->state = RX_WAIT_HEAD;
        }
        break;
    }
    return 0;
}

// 计算校验和
uint8_t UART_CalcChecksum(uint8_t cmd, const uint8_t *data, uint8_t len) {
    uint8_t cs = FRAME_HEAD ^ len ^ cmd;
    for (uint8_t i = 0; i < len; i++) {
        cs ^= data[i];
    }
    return cs;
}

// ===== 发送函数 =====

// 发送一帧（UART_WriteByte 在 uart_hal.c 里实现）
void UART_SendFrame(uint8_t cmd, const uint8_t *data, uint8_t len) {
    uint8_t checksum = FRAME_HEAD ^ len ^ cmd;
    uint8_t i;

    UART_WriteByte(FRAME_HEAD);
    UART_WriteByte(len);
    UART_WriteByte(cmd);
    for (i = 0; i < len; i++) {
        UART_WriteByte(data[i]);
        checksum ^= data[i];
    }
    UART_WriteByte(checksum);
}

// 状态上报（RSP_STATUS）
void UART_SendStatus(void) {
    uint8_t data[6];
    uint16_t idle = BAOZI_GetIdleTime();
    uint16_t emo  = BAOZI_GetEmotionTime();

    data[0] = BAOZI_GetEmotion();            // 当前情绪 0x00-0x06
    data[1] = BAOZI_GetEnergy();             // 能量 0-100
    data[2] = (uint8_t)((idle >> 8) & 0xFF); // 无互动秒数 高字节
    data[3] = (uint8_t)(idle & 0xFF);        // 无互动秒数 低字节
    data[4] = (uint8_t)((emo >> 8) & 0xFF);  // 情绪持续秒数 高字节
    data[5] = (uint8_t)(emo & 0xFF);         // 情绪持续秒数 低字节

    UART_SendFrame(RSP_STATUS, data, 6);
}

// 传感器事件上报（RSP_SENSOR）
void UART_SendSensorEvent(uint8_t type) {
    UART_SendFrame(RSP_SENSOR, &type, 1);
}

// 脚本完毕上报（RSP_SCRIPT_DONE）
void UART_SendScriptDone(void) {
    UART_SendFrame(RSP_SCRIPT_DONE, NULL, 0);
}

// 舵机到位上报（RSP_SERVO_DONE）
void UART_SendServoDone(void) {
    UART_SendFrame(RSP_SERVO_DONE, NULL, 0);
}

// 错误报告（RSP_ERROR）
void UART_SendError(uint8_t code) {
    UART_SendFrame(RSP_ERROR, &code, 1);
}

// 打卡上报（RSP_HABIT_CHECKIN）：用户在屏幕上点了某一格
// action: 0=打卡 1=取消（同一格再点一下）
void UART_SendHabitCheckin(uint8_t idx, uint8_t slot, uint8_t action) {
    uint8_t data[3];
    data[0] = idx;
    data[1] = slot;
    data[2] = action;
    UART_SendFrame(RSP_HABIT_CHECKIN, data, 3);
}

// 习惯同步完成（RSP_HABIT_SYNCED）：整批习惯已落板，PC 端可据此确认下发成功
void UART_SendHabitSynced(uint8_t count) {
    UART_SendFrame(RSP_HABIT_SYNCED, &count, 1);
}

// 习惯同步请求（RSP_HABIT_REQ）：板子手上没有数据（比 PC 先开机 / PC 重启过），
// 用户点了空卡片催一把。PC 收到立刻重发 0x11~0x13，不用等下一个周期。
void UART_SendHabitReq(void) {
    UART_SendFrame(RSP_HABIT_REQ, NULL, 0);
}

// 日程状态切换上报（RSP_SCHED_CHECK）：用户在 TODAY 页点了某条日程
// action: 0=标记完成 1=撤销完成
void UART_SendSchedCheck(uint8_t idx, uint8_t action) {
    uint8_t data[2];
    data[0] = idx;
    data[1] = action;
    UART_SendFrame(RSP_SCHED_CHECK, data, 2);
}

// 日程同步完成（RSP_HABIT_SYNCED 0x91 复用）：整批日程已落板
void UART_SendSchedSynced(uint8_t count) {
    UART_SendFrame(RSP_HABIT_SYNCED, &count, 1);
}

// 日程同步请求（RSP_SCHED_REQ）：TODAY 页空手时催 PC 重发
void UART_SendSchedReq(void) {
    UART_SendFrame(RSP_SCHED_REQ, NULL, 0);
}

void UART_SendRemindClosed(uint8_t type, uint8_t reason) {
    uint8_t d[2];
    d[0] = type;
    d[1] = reason;
    UART_SendFrame(RSP_REMIND_CLOSED, d, 2);
}

// ===== 帧处理回调（收到完整帧后调用，在主循环里跑，可以 printf）=====
// 把协议命令映射到功能层钩子
void UART_HandleFrame(uint8_t cmd, const uint8_t *data, uint8_t len) {
    switch (cmd) {

    case CMD_SET_EMOTION: {  // 0x01 设置情绪
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        if (data[0] > EMOTION_ANGRY) {
            UART_SendError(ERR_EMOTION_RANGE);
        } else {
            printf("[UART] set emotion 0x%02X\r\n", data[0]);
            BAOZI_SetEmotion(data[0]);
        }
        break;
    }

    case CMD_SEND_EVENT: {  // 0x02 发送事件
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        printf("[UART] event 0x%02X\r\n", data[0]);
        BAOZI_SendEvent(data[0]);
        break;
    }

    case CMD_SWITCH_PERSON: {  // 0x03 切换性格
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        uint8_t name_len = data[0];
        if (name_len >= len || name_len >= 32) {
            UART_SendError(ERR_LEN_MISMATCH);
            break;
        }
        char name[33];
        memcpy(name, &data[1], name_len);
        name[name_len] = '\0';
        printf("[UART] switch person: %s\r\n", name);
        if (!BAOZI_SwitchPerson(name)) {
            UART_SendError(ERR_PERSON_NOT_FOUND);
        }
        break;
    }

    case CMD_PLAY_SCRIPT: {  // 0x04 播放脚本
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        uint8_t name_len = data[0];
        if (name_len >= len || name_len >= 64) {
            UART_SendError(ERR_LEN_MISMATCH);
            break;
        }
        char name[65];
        memcpy(name, &data[1], name_len);
        name[name_len] = '\0';
        printf("[UART] play script: %s\r\n", name);
        if (!BAOZI_PlayScript(name)) {
            UART_SendError(ERR_SCRIPT_NOT_FOUND);
        }
        break;
    }

    case CMD_STOP_SCRIPT:  // 0x05 停止脚本
        printf("[UART] stop script\r\n");
        BAOZI_StopScript();
        break;

    case CMD_SET_SERVO: {  // 0x06 设置舵机
        if (len < 2) { UART_SendError(ERR_LEN_MISMATCH); break; }
        printf("[UART] servo id=%u angle=%u\r\n", data[0], data[1]);
        BAOZI_SetServo(data[0], data[1]);
        break;
    }

    case CMD_RESET:  // 0x07 复位
        printf("[UART] reset\r\n");
        BAOZI_Reset();
        break;

    case CMD_QUERY_STATUS:  // 0x08 查询状态
        printf("[UART] query status\r\n");
        UART_SendStatus();
        break;

    case CMD_HEARTBEAT:  // 0x09 心跳（推进1秒+状态上报）
        BAOZI_Tick(1);
        UART_SendStatus();
        break;

    case CMD_DANCE:  // 0x0A 跳舞
        printf("[UART] dance\r\n");
        BAOZI_Dance();
        break;

    /* ===== v1.1 习惯看板（布局见 uart_protocol.h 末尾）===== */

    case CMD_SYNC_TIME: {  // 0x10 对时（v1.9 起第 6 字节是秒，可缺省）
        uint8_t ss = 0xFF;             /* 0xFF = 没带秒，板子保留当前秒不动 */
        if (len < 5) { UART_SendError(ERR_LEN_MISMATCH); break; }
        if (len >= 6 && data[5] <= 59) {
            ss = data[5];
        }
        printf("[UART] sync time %02u/%02u w%d %02u:%02u ss=%u\r\n",
               data[0], data[1], data[2], data[3], data[4], (unsigned)ss);
        BAOZI_SyncTime(data[0], data[1], data[2], data[3], data[4], ss);
        break;
    }

    case CMD_HABIT_BEGIN: {  // 0x11 习惯列表开始
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        printf("[UART] habit begin, count=%u\r\n", data[0]);
        BAOZI_HabitBegin(data[0]);
        break;
    }

    case CMD_HABIT_ITEM: {  // 0x12 下发一个习惯
        int rc;
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        rc = BAOZI_HabitItem(data, len);
        if (rc == -2) {
            UART_SendError(ERR_HABIT_FULL);
        } else if (rc != 0) {
            UART_SendError(ERR_HABIT_RANGE);
        }
        break;
    }

    case CMD_HABIT_END: {  // 0x13 习惯列表结束
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        printf("[UART] habit end, count=%u\r\n", data[0]);
        BAOZI_HabitEnd(data[0]);
        break;
    }

    /* ===== v1.5 TODAY 页日程 ===== */

    case CMD_SCHED_BEGIN: {  // 0x14 日程列表开始
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        printf("[UART] sched begin, count=%u\r\n", data[0]);
        BAOZI_SchedBegin(data[0]);
        break;
    }

    case CMD_SCHED_ITEM: {  // 0x15 下发一条日程
        int rc;
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        rc = BAOZI_SchedItem(data, len);
        if (rc == -2) {
            UART_SendError(ERR_HABIT_FULL);
        } else if (rc != 0) {
            UART_SendError(ERR_HABIT_RANGE);
        }
        break;
    }

    case CMD_SCHED_END: {  // 0x16 日程列表结束
        if (len < 1) { UART_SendError(ERR_LEN_MISMATCH); break; }
        printf("[UART] sched end, count=%u\r\n", data[0]);
        BAOZI_SchedEnd(data[0]);
        break;
    }

    case CMD_REMIND: {  // 0x17 提醒弹卡（时机判定在 PC，板子只展示）
        uint8_t rtype, hh, mm, chars, w, h;
        if (len < 6) { UART_SendError(ERR_LEN_MISMATCH); break; }
        rtype = data[0]; hh = data[1]; mm = data[2];
        chars = data[3]; w = data[4]; h = data[5];
        if (w < 8 || (w & 7) || chars > REMIND_TITLE_MAX_CHARS ||
            len < (uint8_t)(6 + chars * (w / 8) * h)) {
            UART_SendError(ERR_LEN_MISMATCH);
            break;
        }
        printf("[UART] remind type=%u %02u:%02u chars=%u\r\n",
               rtype, hh, mm, chars);
        BAOZI_Remind(rtype, hh, mm, data + 6, chars, w, h);
        break;
    }

    default:
        UART_SendError(ERR_UNKNOWN_CMD);
        break;
    }
}
