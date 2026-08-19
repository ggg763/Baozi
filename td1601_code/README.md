# TD1601 端代码（Behavior Layer）

> 依据《BaoZi 桌面机器人架构调整说明 v1.0》预置。
> 目标：TD1601 作为机器人**核心控制器**，自主完成
> `情绪 -> 行为 -> 动作 -> 硬件` 的决策与执行；
> PC 只负责认知层（ASR / LLM / TTS）并下发语义状态。
>
> ⚠️ 当前为**预置骨架**（TD1601 硬件尚未到手），HAL 层是 printf 模拟，
> 全部代码可用 gcc 编译检查，真机接入点以 `TODO(真机)` 标注。

## 目录结构

```
td1601_code/
├── README.md               本文件
├── docs/
│   └── protocol.md         PC ↔ TD1601 通信协议 v1.0
├── inc/                    头文件（公共类型 + 各模块接口）
│   ├── robot_types.h       公共枚举/结构（情绪、意图、颜色、动作）
│   ├── emotion_state.h     情绪状态机（FSM）
│   ├── behavior.h          行为规划层（情绪→行为映射）
│   ├── motion.h            动作组合层（高层动作）
│   ├── servo_control.h     舵机控制
│   ├── screen_display.h    LCD 表情
│   ├── led_control.h       LED
│   ├── sensor_manager.h    传感器（触摸/红外）
│   └── uart_comm.h         UART 通信
├── src/                    实现
│   ├── main.c              主循环
│   └── ...
└── hal/                    硬件抽象层（占位实现，真机替换）
    ├── hal_pwm.c/.h        舵机 PWM
    ├── hal_lcd.c/.h        LCD
    ├── hal_led.c/.h        LED
    ├── hal_gpio.c/.h       GPIO（触摸/红外/延时）
    └── hal_uart.c/.h       UART
```

## 数据流（主循环 20ms 节拍）

```
UART 收消息(PC) ──▶ uart_comm 解析 ──▶ Emotion_Update
                                            │
传感器(触摸/红外) ──▶ sensor_manager ────────┤
                                            ▼
                                    Emotion FSM（空闲自动降级）
                                            │ 回调
                                            ▼
                                    behavior 行为表
                                   ┌────────┼────────┐
                                   ▼        ▼        ▼
                              screen    led     motion
                              (LCD表情) (颜色)  (舵机动作)
```

- 空闲 20 秒 → `BORED`（无聊）；再 10 秒 → `SLEEPING`（睡眠）——**TD1601 自主完成**，PC 断开也能表现。

## 真机接入清单（拿到 TD1601 后）

1. **HAL 层**：按目标 SDK（如 CH32V/沁恒 RISC-V 库）实现 `hal_*.c` 中所有 `TODO(真机)`：
   - PWM 输出（舵机脉宽 500~2500us）
   - LCD 驱动 + 表情位图素材
   - LED RGB 输出
   - GPIO 读取（触摸 TTP223、红外）
   - UART 收发（RX 中断调 `UART_OnByteReceived`）
   - 时钟源（`sys_now_ms`）与延时（`HAL_DelayMs`）
2. **引脚映射**：确认舵机/LCD/LED/传感器实际接线，更新 HAL 内通道号。
3. **动作调参**：`motion.c` 中角度/时长按舵机安装位置调整。
4. **编译烧录**：用 SDK 提供的工具链编译 `src/` + `hal/` + `inc/`。

## PC 端配套

- `baozi_pc/communication.py`：PC 侧协议封装（模拟/串口两种后端）。
- 运行 `python main.py` 时默认模拟模式，会打印 `UART> EMO:HAPPY` 等下发内容；
  接真机后用 `--serial COMx` 切换到串口模式。

## PC 调试期（无硬件）

全部模块不依赖真实硬件，可用 gcc 编译并通过仿真输入观察行为：

```bash
gcc -std=c99 -Wall -Wextra -Iinc -I. -Isrc src/*.c hal/*.c -o td1601_sim
```

> 注意：`main.c` 为无限循环，仿真时建议改跑单步逻辑测试；
> 各模块（emotion_state / uart_comm / behavior）逻辑可在单元测试中单独驱动。
