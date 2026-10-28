# TD1601 EVB 引脚规划（当前实际占用）

> **依据**：本工程代码**实际占用**（`src/video.c`、`src/board_init.c`、
> `UserFolder/servo_test/servo_pins.h`、`UserFolder/tracking/*.h`、`UserFolder/test/ir_*.c`）
> ＋ `components/chips/chip_td1601/v1.0/sys/soc.h`
> **更新**：2026-08-30 ｜ **引脚复用对照表**：同目录 `pin_mux_table.md`
>
> ⚠️ 本文件替换旧的 v2 版规划。旧版**多处与实际不符**（LCD 曾写 PA28/PA15/PA7/PA8，
> 实际是 PA3/PA2/PB15/PA31；Servo2 的通道号、IR 三路也全变了）。**一切以本文件为准。**

## 0. 一页速查（当前实际）

| 功能 | 引脚 | 复用 / 模式 | 代码出处 |
|---|---|---|---|
| LCD SCK | **PA3** | `SPI1_SCK` | `src/video.c:166` |
| LCD D0~D3 | **PB4 / PB5 / PB6 / PB7** | `SPI1_D0~D3` | `src/video.c:167-170` |
| LCD CS | **PA2** | GPIO 软控（输出） | `src/video.c:232` |
| LCD WAIT | **PB10** | GPIO 输入 | `src/video.c:238` |
| LCD RST | **PB15** | GPIO 输出 | `src/video.c:243` |
| LCD 背光 BL | **PA31** | GPIO 输出 | `src/video.c:248` |
| 舵机 YAW（水平） | **PA29** | `PWM_CH6` → **csi_ch 3** | `servo_pins.h` |
| 舵机 PITCH（垂直） | **PA27** | `PWM_CH4` → **csi_ch 2** | `servo_pins.h` |
| IR Left | **PB22** | GPIO 输入（原 `TDM_SCLKOUT`） | `track_dual.h` / `ir_test.c` |
| IR Center | **PB23** | GPIO 输入（原 `TDM_WSOUT`） | 同上 |
| IR Right | **PB29** | GPIO 输入（原 `TDM_D1`） | 同上 |
| Console UART0 | **PA17 (RX) / PA18 (TX)** | `UART0`，115200-8N1 | `src/board_init.c` |
| 调试口 JTAG | PA19 (TMS) / PA20 (TCK) | 保持不动 | — |
| ESP32 UART3 | **PB18 (RX) / PB19 (TX)** | `UART3` | 规划（**未实现**） |
| 触摸 I2C | **PA8 (SCL) / PA9 (SDA)** | IIC0（或软件 I2C） | 规划（**未实现**） |
| MPU6050 I2C | **PA16 (SDA) / PA23 (SCL)** | IIC0 | 规划（**未实现**） |

## 1. 已实现（以代码为准）

### 1.1 LCD 480×480（四线 QSPI/OSPI）

| 信号 | 引脚 | 说明 |
|---|---|---|
| SCK | PA3 | `PA3_SPI1_SCK`（func 2） |
| D0~D3 | PB4~PB7 | `PB4~PB7_SPI1_D0~D3`（func 0） |
| CS | PA2 | GPIO 软控（`CLR_CS1/SET_CS1`） |
| WAIT | PB10 | GPIO 输入 |
| RST | PB15 | GPIO 输出 |
| 背光 BL | PA31 | GPIO 输出（`BLK_LOW/BLK_HI`），**不做 PWM 调光** |

- ⚠️ **D4~D7（PB8 / PB9 / PB11）当前未用**，但 CON2 走线与 `UserFolder/lcd_test.c`
  都证明**硬件具备八线**；屏若改八线模式，这三脚会被占。**不要把 IR/传感器放到 PB8/PB9/PB11**。
- ⚠️ 反向注意：**PA15 现在空闲**（旧规划曾把它当 LCD_CS，实际 CS 用的是 PA2）。
- ⚠️ D/C 未接（SPI 模式下可不接，CON2 Pin14 未用），因此 **PA6 / PA7 也空闲**。

### 1.2 舵机（两路，实测定案）

| 轴 | 引脚 | csi 通道 | 中位 | 限位 | 方向 |
|---|---|---|---|---|---|
| YAW（水平/脖子左右） | **PA29** | **3** | 1000us | 300~1700 | 脉宽增大 = **右转** |
| PITCH（垂直/抬头低头） | **PA27** | **2** | 1100us | 800~1600 | 脉宽增大 = **抬头** |

- 📌 **2026-09 为做 PCB 把 YAW 由 PA0 换到 PA29**（底部排针**外排第 3 脚**，接线省事；
  PA0 不在排针上、引出要额外走线）。中位/限位沿用原实测值，但**换脚后须上板复验**。
- 📌 选脚硬约束：排针上能出 PWM 的只有 **外排 PA29/PA30（csi 对 3）**、
  **内排 PA28/PA27/PA16（csi 对 2）**；PA16 已被 MPU6050 占、PA27 是 PITCH
  → 只剩 PA29/PA30（二者同对，只能选一个）。**PB 口没有任何 PWM 复用。**
- ⚠️ **铁律**：`csi_pwm_out_config/out_start` 的 channel = **pinmap channel ÷ 2**
  （PA29 的 pinmap 6→ch3；PA27 的 pinmap 4→ch2）。用引脚手册上的 "CH4" 会写到 PA31 那组寄存器。
- ⚠️ **同 csi_ch 的两个引脚共享 load 寄存器**，不能当两路独立 PWM（PA0/PA1 都是 csi_ch 0；
  **PA29/PA30 同属 csi_ch 3，所以用了 PA29 就不能再用 PA30 做独立 PWM**）。
- 控制层：`UserFolder/servo_test/servo_core.{h,c}`（`servo_core_init` / `servo_set_pulse`，内含软限位）。
- PA31 已让出（现作 LCD 背光）；需要时可把 `servo_pins.h` 的 PITCH 5 行换成 `CH 4U / PIN31 / PA31_PWM_CH8` 切回。

### 1.3 IR 三路（跟随用）

| 通道 | 引脚 | 原复用 | 说明 |
|---|---|---|---|
| Left | **PB22** | `TDM_SCLKOUT` | 屏幕左下 |
| Center | **PB23** | `TDM_WSOUT` | 屏幕上方中点（比 L/R 高，提供竖直分辨） |
| Right | **PB29** | `TDM_D1` | 屏幕右下 |

- 极性：**未遮挡 = 1（高），遮挡 = 0（低）**；代码里 `seen = !raw`。
- 三者同属 **TDM 族 func 0**、TDM 未使用 —— 是 PORTB 上唯一"谁都不沾"的一组。
- ❌ **不要再用 PB20 / PB21**：它们是 UART3 的 CTS/RTS，实测读不到探头（详见 `pin_mux_table.md` §8）。

### 1.4 Console 调试串口

- **PA17 (RX) / PA18 (TX)**，`UART0`，**115200-8N1**（`src/board_init.c` 里 `CONSOLE_IDX=0`）。
- 全项目串口波特率统一 **115200**（不再使用 921600）。

## 2. 规划中（尚未写代码）

### 2.1 ESP32 通信：UART3

| 信号 | 引脚 | 功能值 |
|---|---|---|
| UART3_RXD | PB18 | 0 |
| UART3_TXD | PB19 | 0 |
| UART3_CTS_N | PB20 | 仅硬件流控时需要 → **保持空闲** |
| UART3_RTS_N | PB21 | 仅硬件流控时需要 → **保持空闲** |

- 只用两线（TX/RX），**不开硬件流控**（ESP32 点对点 115200 不需要）。
- `csi_uart_init(&u, 3)`（**idx = 3**，见 `devices.c` 的 uart_pinmap）；IRQ = `DW_UART3_IRQn = 38`。
- ⚠️ **电平**：TD1601 是 **1.8V** IO 域，ESP32 是 **3.3V** → **必须做电平转换**
  （或确认 ESP32 模组 IO 供电可切 1.8V）。直连会灌流损伤芯片。

### 2.2 触摸 + MPU6050（⚠️ 有一个待确认的冲突）

`soc.h` 里 **IIC0 的候选引脚**：

- SDA：PA3 / **PA9** / **PA16** / PA20 / PA22 / PA24
- SCL：PA2 / **PA8** / PA15 / PA19 / PA21 / **PA23**

规划中：触摸在 **PA8(SCL) + PA9(SDA)**，MPU6050 在 **PA16(SDA) + PA23(SCL)**。

> ⚠️ **这两组都是 IIC0 的候选组，而 IIC0 同一时刻只能选一组引脚生效** ——
> 所以"触摸一组、MPU 另一组、共用 IIC0"在硬件上是**不成立的**。
>
> 三个可选解法（接之前定一下）：
> 1. **两者挂同一组引脚**（真·共享总线，靠地址区分：触摸 0x5D/0x28、MPU6050 0x68/0x69）——
>    但这样 MPU6050 要接到 CON2 排线的 PA8/PA9 上；
> 2. **触摸改用软件 I2C**（GPIO 模拟 PA8/PA9，很多屏模组就这么接）；
> 3. **MPU6050 改用 IIC1**（SDA=PB25 / SCL=PB24，独立第二路）。
>
> 另外选脚要避开已占用的 PA2（LCD_CS）、PA3（LCD_SCK）。

## 3. 空闲 / 不可用清单

**空闲可用**（截至 2026-08-30）：

- **PA0**（2026-09 舵机 YAW 换到 PA29 后空闲）
- **PA6 / PA7**（原规划 LCD DC/RST，实际未用）
- **PA15**（旧规划 LCD_CS，实际 CS = PA2）
- **PA11~PA14**（`soc.h` 中**无任何外设复用**，纯 GPIO，做按键/中断最合适）
- **PB16 / PB17**（UART2_DE/RE，UART2 未用）
- **PB28 / PB30 / PB31**（TDM_D0/D2/D3，与 IR 三路同族）
- **PB24 / PB25**（IIC1，独立第二路 I2C）
- **PB8 / PB9 / PB11**（SPI1_D4/D5/D7 —— ⚠️ 屏改八线就会被占，慎用）
- PB2 / PB3（UART1_DE/RE）、PB12~PB15（UART2 系列）、PB26 / PB27（CAN）

**不可用 / 慎用**：

| 引脚 | 原因 |
|---|---|
| PA17 / PA18 | Console UART0，**禁止复用** |
| PA19 / PA20 | JTAG（TMS/TCK），切 GPIO 会掉调试器 |
| PA29 / PA27 | 舵机 PWM（已定案；PA0 换脚后空闲） |
| PA2 / PA3 / PB4~PB7 / PB10 / PB15 / PA31 | LCD |
| PB20 / PB21 | UART3 流控脚；且实测作 GPIO 输入读不到探头 |
| PB18 / PB19 | 留给 ESP32 UART3 |

## 4. 参考资料

| 文档 | 内容 |
|---|---|
| `pin_mux_table.md` | 全芯片引脚复用功能表（PORTA/PORTB 逐脚）、功能号编码规则、`pinmux.c` 的两个实现缺陷、注意事项 |
| `ir_tracking_plan.md` | 红外跟随方案：光学几何、二维状态表（含叠加态）、控制律、验证步骤 |
