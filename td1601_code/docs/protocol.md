# PC ↔ TD1601 通信协议 v1.0

> 架构原则：PC 只下发"语义状态"（emotion / intent），**不下发动作指令**。
> 动作（表情/舵机/LED）由 TD1601 行为层自主决策。

## 1. 物理层

- 接口：UART
- 波特率：`115200`（可配置，见 `hal_uart.h` 的 `HAL_UART_BAUDRATE`）
- 数据格式：8N1
- 消息分隔：`\r\n` 或 `\n`

## 2. 轻量协议（推荐）

每行一条消息，`KEY:VALUE` 格式，全部大写：

```
EMO:HAPPY\r\n
INTENT:CHAT\r\n
```

### 2.1 情绪（EMO）

| 值 | 含义 | TD1601 行为（预置） |
|---|---|---|
| `EMO:IDLE` | 待机 | neutral 表情、白灯、回正 |
| `EMO:HAPPY` | 开心 | 笑脸、黄灯、左右轻摆 |
| `EMO:SAD` | 难过 | 悲伤表情、蓝灯、低头 |
| `EMO:ANGRY` | 生气 | 生气表情、红灯、歪头 |
| `EMO:SURPRISED` | 惊讶 | 惊讶表情、黄灯、后仰 |
| `EMO:LOVING` | 亲昵 | 爱心、粉灯、前倾 |

`EMO:BORED` / `EMO:SLEEPING` 由 TD1601 空闲超时**自触发**，PC 无需（也不应）下发。

### 2.2 意图（INTENT）

| 值 | 含义 |
|---|---|
| `INTENT:CHAT` | 闲聊 |
| `INTENT:GREETING` | 问候 |
| `INTENT:QUESTION` | 提问/求助 |
| `INTENT:COMMAND` | 命令式（如"转个头"） |
| `INTENT:NONE` | 无特别意图 |

当前 TD1601 优先响应 emotion；intent 为预留扩展。

## 3. JSON 协议（备选）

```
{"type":"emotion","value":"happy"}\r\n
{"type":"intent","value":"chat"}\r\n
```

TD1601 端解析实现为轻量提取（不引入 JSON 库），仅识别 `"value":"..."` 片段。
若后续字段增多，建议改用正式 JSON 解析库（如 cJSON，注意 MCU 内存开销）。

## 4. 消息处理流程（TD1601 端）

```
PC ──EMO:HAPPY──▶ UART 中断逐字节入缓冲
                     │
                     ▼
              uart_comm.c 按行拆解
                     │
                     ▼
              Emotion_Update(HAPPY)
                     │
                     ▼
              Emotion FSM 回调
                     │
                     ▼
              behavior.c 行为表
                     │
        ┌────────────┼────────────┐
        ▼            ▼            ▼
  screen_display  led_control  motion(servo)
```

## 5. 示例会话

```
PC → TD1601:  EMO:HAPPY\r\n
TD1601:       [HAL-LCD] show: happy
              [HAL-LED] rgb(255,200,0)
              [HAL-PWM] ch0 -> 1133us (angle 110)
              ...

PC → TD1601:  INTENT:CHAT\r\n
TD1601:       （intent 预留，暂无输出动作）
```

## 6. 待办（真机阶段）

- [ ] 确认 TD1601 UART 引脚与 PC 转接（USB-TTL）
- [ ] 若 PC 端串口未插，运行 `python main.py` 时通信层保持"模拟模式"打印
- [ ] 协议帧加校验（如 CRC8）可选，赛道演示可先省略
