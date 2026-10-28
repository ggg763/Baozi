# TD1601 EVB 引脚复用功能表 + IR_test 审查

> 依据（源码级，非推测）：
> - `components/chips/chip_td1601/v1.0/sys/soc.h`  —— 引脚功能枚举（唯一权威）
> - `components/chips/chip_td1601/v1.0/sys/devices.c` —— uart/iic/spi/pwm/gpio pinmap 表
> - `components/chips/chip_td1601/v1.0/drivers/pinmux.c` —— `csi_pin_set_mux` / `csi_pin_mode` 实现
> - `components/chips/chip_td1601/v1.0/drivers/gpio.c`     —— `csi_gpio_read` 实现
> - `boards/td1601_evb/v1.0/include/board_config.h`
> 日期：2026-08-30

---

## 0. 功能号编码规则（先看这条，后面所有表都用它）

| 功能值 | 含义 | 落到哪个寄存器 |
|---|---|---|
| `0` ~ `3` | **数字复用功能**（IOMUX 只有 2 bit，所以最多 4 个） | `PORTCTL` 置位 + `IOMUX0L/H` 对应 2bit 字段写 功能值 |
| `>= 5` | **模拟功能**（ADC / VREFP…），**不是数字复用** | 只写 `ANA_IO_EN`（PORTA: `0x60030070`，PORTB: `0x60030170`），位号 = `功能值 - 5` |
| `PIN_FUNC_GPIO` | GPIO | `PORTCTL` **清零** + `IOMUX` 对应 2bit 字段清 0 |

**所以："ADC" 不是第 5 个数字功能**，它是一条独立通路。这点被很多例程写错。

寄存器地址（来自 `pinmux.c`）：

| 寄存器 | PORTA (A 组) | PORTB (B 组) |
|---|---|---|
| GPIO PORTCTL（0=GPIO，1=复用） | `0x60030000` | `0x60030100` |
| IOMUX0L（PIN0~15，每个 2bit） | `0x60030008` | `0x60030108` |
| IOMUX0H（PIN16~31，每个 2bit） | `0x6003000C` | `0x6003010C` |
| 上拉 IOPU0 | `0x60030020` | `0x60030120` |
| 下拉 IOPD0 | `0x60030028` | `0x60030128` |
| 模拟功能使能 ANA_IO_EN | `0x60030070` | `0x60030170` |

`csi_gpio_init(&h, idx)`：**idx=0 → PORTA，idx=1 → PORTB**（见 `devices.c` 第 44/45 行）。

---

## 1. PORTA（PA0~PA31）功能表

| 引脚 | func0 | func1 | func2 | func3 | 模拟 |
|---|---|---|---|---|---|
| PA0 | I2S0_SCLK | SPI0_CS | UART0_RX | **PWM_CH0** | ADC_VREFP |
| PA1 | I2S0_WSCLK | SPI0_SCK | UART0_TX | PWM_CH1 | ADC_VREFN |
| PA2 | I2S1_SCLK | IIC0_SCL | SPI1_CS | PWM_CH2 | — |
| PA3 | I2S1_WSCLK | IIC0_SDA | SPI1_SCK | PWM_CH3 | ADC_A1 |
| PA4 | I2S0_SDA | SPI0_MOSI | UART1_CTS | PWM_CH4 | ADC_A2 |
| PA5 | I2S1_SDA | SPI0_MISO | UART1_RTS | PWM_CH5 | ADC_A3 |
| PA6 | I2S0_SCLK | UART0_TX | — | PWM_CH6 | ADC_A4 |
| PA7 | I2S0_WSCLK | — | — | PWM_CH7 | ADC_A5 |
| PA8 | I2S0_SDA | IIC0_SCL | UART0_RX | PWM_CH8 | ADC_A6 |
| PA9 | I2S1_SDA | IIC0_SDA | — | PWM_CH9 | ADC_A7 |
| **PA10** | I2S0_MCLK | UART0_TX | — | — | — |
| **PA11~PA14** | **soc.h 中完全没有定义 → 无外设复用，只做纯 GPIO** | | | | |
| PA15 | IIC0_SCL | SPI0_CS | — | PWM_CH4 | ADC_A9 |
| PA16 | IIC0_SDA | SPI0_SCK | UART1_TX | PWM_CH5 | ADC_A10 |
| PA17 | UART0_RX | SPI0_MOSI | I2S0_SCLK | PWM_CH10 | ADC_A11 |
| PA18 | UART0_TX | SPI0_MISO | I2S0_WSCLK | PWM_CH11 | ADC_A13 |
| PA19 | **JTAG_TMS** | UART1_RX | I2S1_SCLK | IIC0_SCL | ADC_A8 |
| PA20 | **JTAG_TCK** | UART1_TX | I2S1_WSCLK | IIC0_SDA | — |
| PA21 | UART0_CTS | UART1_CTS | I2S0_SDA | IIC0_SCL | — |
| PA22 | UART0_RTS | UART1_RTS | I2S1_SDA | IIC0_SDA | — |
| PA23 | IIC0_SCL | UART0_TX | **PWM_CH0** | SPI0_CS | — |
| PA24 | IIC0_SDA | UART0_RX | PWM_CH1 | SPI0_SCK | — |
| PA25 | — | UART0_CTS | PWM_CH2 | SPI0_MOSI | — |
| PA26 | I2S1_MCLK | UART0_RTS | PWM_CH3 | SPI0_MISO | — |
| PA27 | I2S0_SCLK | UART1_RX | **PWM_CH4** | SPI1_CS | — |
| PA28 | I2S0_WSCLK | UART1_TX | PWM_CH5 | **SPI1_SCK** | — |
| PA29 | I2S1_SCLK | UART1_CTS | PWM_CH6 | — | — |
| PA30 | I2S1_WSCLK | UART1_RTS | PWM_CH7 | — | — |
| PA31 | I2S0_SDA | — | PWM_CH8 | UART0_TX | — |

> PWM 通道有"一通道两引脚"的情况：CH0 = PA0 / PA23；CH1 = PA1 / PA24；CH4 = PA4 / PA15 / PA27；CH5 = PA5 / PA16 / PA28。**同一通道的两个引脚不能当两路独立 PWM 用**，选一个。

## 2. PORTB（PB0~PB31）功能表

| 引脚 | func0 | func1 | 模拟 | 备注 |
|---|---|---|---|---|
| PB0 | UART0_DE | — | — | 单选功能 |
| PB1 | UART0_RE | — | — | 单选功能 |
| PB2 | UART1_DE | — | ADC1_A15 | |
| PB3 | UART1_RE | — | — | |
| PB4 | SPI1_D0 | — | ADC1_A7 | LCD 数据线 |
| PB5 | SPI1_D1 | — | ADC1_A9 | LCD |
| PB6 | SPI1_D2 | — | ADC1_A10 | LCD |
| PB7 | SPI1_D3 | — | ADC1_A11 | LCD |
| PB8 | SPI1_D4 | — | ADC1_A12 | LCD |
| PB9 | SPI1_D5 | — | — | LCD |
| PB10 | SPI1_D6 | — | — | LCD |
| PB11 | SPI1_D7 | — | — | LCD |
| PB12 | UART2_RXD | — | — | |
| PB13 | UART2_TXD | — | ADC1_A8 | |
| PB14 | UART2_CTS_N | — | ADC_A14 | |
| PB15 | UART2_RTS_N | — | ADC_A12 | |
| PB16 | UART2_DE | — | — | 规划：Touch RST |
| PB17 | UART2_RE | — | — | 规划：Touch INT |
| PB18 | UART3_RXD | — | ADC1_A13 | ESP32 通信 |
| PB19 | UART3_TXD | — | ADC1_A14 | ESP32 通信 |
| **PB20** | **UART3_CTS_N** | — | **无**（**全芯片唯一功能**） | 原 IR Left |
| **PB21** | **UART3_RTS_N** | — | ADC_A15 | 原 IR Center |
| **PB22** | TDM_SCLKOUT | — | ADC1_A1 | IR Right（**实测唯一可用**） |
| PB23 | TDM_WSOUT | — | ADC1_A2 | |
| PB24 | IIC1_SCL | — | — | 独立 I2C1 |
| PB25 | IIC1_SDA | — | — | 独立 I2C1 |
| PB26 | CAN_RX | — | — | |
| PB27 | CAN_TX | — | — | |
| PB28 | TDM_D0 | — | ADC1_A3 | |
| PB29 | TDM_D1 | — | ADC1_A4 | |
| PB30 | TDM_D2 | — | ADC1_A5 | |
| PB31 | TDM_D3 | — | ADC1_A6 | |

**关键结论：PB20 是"纯功能引脚"，它的 func0 就是 UART3_CTS_N，而且 `PB20_UART3_CTS_N = 0U`。**
`0` 既是"UART3_CTS_N"的编码，也是"GPIO 模式下 IOMUX 字段清 0"的编码 —— 二者数值相同。
真正的开关是 `PORTCTL` 那一位（1=复用, 0=GPIO）。**只要 PORTCTL 没清干净，PB20 就必然是 CTS_N，且从寄存器上看不出区别。**

---

## 3. 当前已占用 / 规划占用（避免撞车）

| 功能 | 引脚 | 依据 |
|---|---|---|
| LCD QSPI D0~D7 | PB4~PB11 | `src/video.c` |
| LCD CLK | PA28 (SPI1_SCK) | 同上 |
| LCD CS / DC / RST / BL | PA15 / PA6 / PA7 / PA8 | `io_plan.md` |
| 触摸 IIC0 | PA16 (SDA) / PA23 (SCL) | `io_plan.md` |
| 触摸 INT / RST | PB17 / PB16 | `io_plan.md` |
| ESP32 UART3 | PB19 (TX) / PB18 (RX) | `io_plan.md` |
| **舵机 YAW** | **PA29 → PWM_CH6 → csi ch 3**（2026-09 由 PA0 换脚） | servo 定案 |
| **舵机 PITCH** | **PA27 → PWM_CH4 → csi ch 2** | servo 实测定案 |
| console 调试串口 UART0 | PA17 / PA18（`board_init.c` 里 `CONSOLE_IDX`） | 不可挪 |
| JTAG | PA19 / PA20 | 调试用，避让 |

---

## 4. `ir_test.c` 代码审查结论

### 4.1 逻辑本身：写法正确，但**没有一处回读校验**，无法自证

现有调用顺序是对的：

```c
csi_pin_set_mux(PORTB, PIN20, PIN_FUNC_GPIO);   /* 正确：先切 GPIO */
csi_pin_mode(PORTB, PIN20, GPIO_MODE_PULLUP);   /* 正确：再配上拉 */
csi_gpio_init(&s_ir_gpio, 1);                    /* idx=1 → PORTB，正确 */
csi_gpio_dir(&s_ir_gpio, IR_ALL_PIN, GPIO_DIRECTION_INPUT);
```

已核对驱动的正确性：
- `csi_gpio_read()` 读的是 `EXT_PORTA`（**引脚真实电平**，不是输出寄存器）→ 驱动没问题；
- `csi_pin_mode(GPIO_MODE_PULLUP)` 对 PORTB 操作 `0x60030120/0x60030128`，**不会碰 mux** → 顺序无副作用。

问题在于**三行 `csi_pin_set_mux()` 的返回值被丢弃、`csi_pin_get_mux()` 从不回读**，所以"到底切没切过去"这个问题在代码里是无解的 —— 这正是"只有 PB22 有反应"却查不出来的原因。

### 4.2 与官方 `pinmux.c` 实现缺陷的交互（**本项目必须知道的雷**）

**(A) PORTB 分支漏了 `offset -= 16`，依赖未定义行为"侥幸正确"**

```c
/* pinmux.c csi_pin_set_mux，PORTB 分支 */
} else {
    if (pin_name >= PIN0 && pin_name < PIN16) reg_mux0 = HOBBIT1_IOMUX0L_REG;
    else                                       reg_mux0 = HOBBIT1_IOMUX0H_REG;   /* 少了 offset -= 16 */
}
reg_val = ((uint32_t)0x3U << ((uint8_t)offset * 2U));   /* PB20 → 0x3 << 40，uint32_t 上是 UB！ */
```

`offset = 20` → 移位量 40 ≥ 32，是 C 的未定义行为。RISC-V 的 `sll` 只取低 5 位，`40 & 31 = 8`，恰好等于 `(20-16)*2 = 8`，**所以在当前 GCC 下侥幸命中正确字段**。换编译器 / 换优化等级 / 换分支预测都可能让它清错 IOMUX 位 → 引脚"看起来没反应"。**不要试图"修"官方驱动，但必须知道它靠不住：改 PA/PB 引脚后一定要回读验证。**

**(B) 非 GPIO 分支里 PORTB 写错了寄存器**

```c
} else {                       /* 非 GPIO，即设置真正的外设功能时 */
    if (pin_name >= PIN0 && pin_name < PIN16) reg_mux0 = HOBBIT1_IOMUX0L_REG;
    else                                      reg_mux0 = HOBBIT1_IOMUX0L_REG;  /* ← 应为 IOMUX0H！ */
}
```

也就是说：**把 PB16~PB31 设成任何外设功能（UART3/TDM/SPI1…）时，功能值被写进了 PB0~PB15 的 IOMUX0L，而目标引脚自己的 IOMUX0H 字段没被改。**

这条对 IR 的直接影响：**如果此前有任何代码（ESP32 UART3 初始化、TDM 调试）把 PB20/PB21 设成过外设功能，那么 PB20/PB21 的 IOMUX0H 字段会被留成"上电复位值"，同时 PB0~PB15 的功能被污染。** 而 IR 现在唯一"能用"的 PB22，恰好也在 PB16~PB31 段 —— 所以单靠拨弄 mux，**无法解释"PB22 行、PB20/PB21 不行"**，说明 PB20/PB21 还有更深一层原因（见 4.3）。

### 4.3 PB20 / PB21 为什么没反应 —— 已排除项与最可能项

**已排除（有源码证据）：**
- 不是 UART3 抢占：`src/main.c` 全文只有 `board_init()` + `ir_test()`，而 `board_init.c` 只初始化 console（UART0 = PA17/PA18）。**UART3 从未被初始化**，`ir_test` 的 `set_mux` 之后没有任何代码改 PB20/PB21。
- 不是 GPIO 驱动/A 组控制器问题：三路同属 PORTB（csi idx 1），PB22 能读通。
- 不是 mux 顺序/上拉写法问题（见 4.1）。
- 不是"只在变化时打印"导致的假象：`last = 0xFF`，第一轮 `st` 必然 ≠ 0xFF，**上电一定会先打印一行三路初始电平**。如果实测连这一行都没有 → 说明程序没跑到，而不是引脚问题。

**结构上 PB20/PB21 与 PB22 的唯一差别：它们是 UART3 的硬件流控脚（CTS_N / RTS_N）。**
UART3 有专用引脚驱动逻辑：**RTS_N 是芯片的"输出"**。当模块未使能时，该引脚的输出通路在某些条件下仍会呈现驱动/内部固定电平，GPIO 输入方向设置**无法覆盖芯片内部的功能模块驱动**。PB22 的 func0 是 `TDM_SCLKOUT`（TDM 未使能时输出使能关闭）。
→ 这与实测现象（PB22 行、PB20/PB21 恒定不动）**完全吻合**。

**结论（可操作的判断）：**
1. 先跑 4.4 的诊断补丁，看三路回读的 `mux` 是否都变成 `GPIO`、以及原始电平是否**恒定不动**。
   - 若 mux 回读正常、电平恒定 → **引脚层被 UART3 流控脚特性占住**，换脚（方案 A）。
   - 若 mux 回读不是 GPIO → 是缺陷 (A)/(B)，走回读 + 强制重设（方案 B）。
2. **方案 A（推荐，最省事）：IR 三路改用同一族、已验证可用的 TDM 引脚**
   `IR Left = PB22`（已验证）、`IR Center = PB23`（TDM_WSOUT）、`IR Right = PB28`（TDM_D0）。
   备选：PB29/PB30/PB31（同为 TDM_Dx）。**都避开 PB20/PB21**，顺带把 ESP32 硬件流控的可用性还回来（io_plan.md 里"PB20/PB21 被 IR 占用所以不能开硬件流控"这条约束可以删掉）。
3. **方案 B：若必须留 PB20/PB21**，在 `set_mux` 后加回读 + 强制重写（见 4.4 补丁），并接受"可能仍被 UART3 引脚逻辑拖着"的风险。

### 4.4 诊断补丁（建议加进 `ir_test.c`）

一次上电即可定位问题层次，不用示波器。

```c
#include <drv/pin.h>

/* 打印三路的 实际 mux 回读 + 原始端口电平 */
static void ir_dump(void)
{
    /* 一次性读整个 PORTB 引脚电平（csi_gpio_read 读的是 EXT_PORTA 真实电平） */
    uint32_t raw = csi_gpio_read(&s_ir_gpio, 0xFFFFFFFFU);

    printf("[IRDBG] PORTCTL1=0x%08X IOMUX0H=0x%08X IOPU0=0x%08X IOPD0=0x%08X\r\n",
           *(volatile uint32_t *)0x60030100U,
           *(volatile uint32_t *)0x6003010CU,
           *(volatile uint32_t *)0x60030120U,
           *(volatile uint32_t *)0x60030128U);
    printf("[IRDBG] mux: PB20=%u PB21=%u PB22=%u  (0=GPIO)\r\n",
           (unsigned)csi_pin_get_mux(PORTB, PIN20),
           (unsigned)csi_pin_get_mux(PORTB, PIN21),
           (unsigned)csi_pin_get_mux(PORTB, PIN22));
    printf("[IRDBG] raw B20=%u B21=%u B22=%u B23=%u B28=%u\r\n",
           (unsigned)((raw >> 20) & 1U), (unsigned)((raw >> 21) & 1U),
           (unsigned)((raw >> 22) & 1U), (unsigned)((raw >> 23) & 1U),
           (unsigned)((raw >> 28) & 1U));
}
```

用法：在现有 `set_mux/pin_mode` 之后调一次 `ir_dump()`；主循环里改成 **每 1s 无条件打印一次**（别只在变化时打印，否则"恒定不动"和"程序死了"分不清）：

```c
uint32_t tick = 0;
for (;;) {
    ...既有读取...
    if ((st != last) || (++tick % 20U == 0U)) {   /* 50ms × 20 = 1s */
        printf("[IR] L=%u C=%u R=%u (0x%02X)\r\n", ...);
        last = st;
    }
    mdelay(50);
}
```

判读表：

| 现象 | 结论 |
|---|---|
| `mux: PB20=0 PB21=0 PB22=0` 但 B20/B21 恒定 = 1 | 引脚被 UART3 流控脚逻辑占住 → **换脚（方案 A）** |
| `mux` 回读非 0 | mux 没切成功 → 驱动缺陷 (A)/(B)；先回读再想换脚 |
| `IOPU0` 对应位没置 1 | 上拉没生效 → OC 输出的模块会读不到高 |
| 三路 raw 都跟着遮挡变化 | 其实没问题，是打印条件太苛刻 |

---

## 5. 注意事项清单

1. **`PORTCTL` 才是 GPIO/复用的总开关**，`IOMUX0L/H` 只是"复用选第几个功能"。设 GPIO 必须两步都做（`csi_pin_set_mux(PORTB, PINxx, PIN_FUNC_GPIO)` 帮你做两步），**别只手写 IOMUX 寄存器**。
2. **模拟功能（ADC）是独立通路**：`pin_func >= 5` 时 `csi_pin_set_mux` **只**写 `ANA_IO_EN`，**不动 PORTCTL/IOMUX**。要用 ADC 前先确认该脚的 PORTCTL 状态，别指望一次 `set_mux` 搞定。
3. **`PIN_FUNC_GPIO` 与 `func 0` 在寄存器上难以区分**（都表现为 IOMUX 字段 = 0），只能靠 PORTCTL 位判断。查问题务必用 `csi_pin_get_mux()` 回读，不要靠猜。
4. **PORTB 改 P16~P31 的功能设置会写错寄存器**（缺陷 B）→ 涉及 PB16~PB31 的外设（UART3、TDM、SPI1）配置后，**必须回读 `IOMUX0H` 确认**。
5. **PORTB 的 mux 写入依赖 UB 移位**（缺陷 A）→ 换编译器/优化等级后，PB 引脚配置**必须回归测试**。
6. **PB20 是全芯片最"死"的引脚**：只有 UART3_CTS_N 一个功能，且编码为 0。做 GPIO/中断/传感器优先避开它。
7. **PWM 一通道多引脚共用**：CH0=PA0/PA23、CH1=PA1/PA24、CH4=PA4/PA15/PA27、CH5=PA5/PA16/PA28。**同一通道的两个脚是同一个定时器通道，不能当两路独立 PWM** —— servo 那边已踩过（PA0/PA1 实测同源）。
8. **csi PWM 通道号 = pinmap channel ÷ 2**（servo 实测结论）：PA29 (channel 6) → csi ch 3；PA27 (channel 4) → csi ch 2。写 servo 代码时别直接用 channel。
   ⚠️ **PB 口没有任何 PWM 复用**，能出 PWM 的全是 PA 口。
9. **PA11~PA14 在 soc.h 里根本没有定义**：无任何外设复用，只能当纯 GPIO（做按键/中断正合适）。
10. **console UART0 固定 PA17/PA18**（`board_init.c` 的 `CONSOLE_IDX`），调试串口不能挪；**JTAG 占 PA19/PA20**，布局时避让。
11. **1.8V 电平域**：所有 GPIO 都是 1.8V。3.3V 推挽输出的模块（红外、超声波）**必须分压/电平转换**，否则灌流损伤芯片。开漏输出的模块可以直接用芯片内部上拉。
12. **`csi_gpio_read()` 读的是引脚真实电平**（`EXT_PORTA`），可以用来直接读被复用成外设的引脚电平，做"边沿抓取"很方便。
13. **PORTB GPIO 控制器 idx = 1**（`csi_gpio_init(&h, 1)`），PORTA 是 0；`pin_mask` 一律用 `PINxx` 位掩码（如 `PIN20`），不是序号。
14. **ESP32 UART3 与 IR 的取舍**：把 IR 从 PB20/PB21 挪走后，UART3 的 CTS/RTS 就空出来了，可以重新考虑硬件流控。

---

## 6. 追加（2026-08-30）：IR 换脚到 PA 组的可用性评估

**问：三路 IR 切到 PA20~PA22 行不行？—— 不行，这三个恰好是最差的一组。**

| 候选脚 | 原生复用 | 结论 | 原因 |
|---|---|---|---|
| **PA20** | **JTAG_TCK** | ❌ **绝对不要用** | CDK 下载/调试走 JTAG（PA19=TMS、PA20=TCK）。切成 GPIO 输入后调试器会掉线，等于自断退路；且 TAP 逻辑可能把该脚钉住。 |
| **PA21** | UART0_CTS_N | ⚠️ 不推荐 | 是 UART0（**console，活跃外设**，`board_init.c`：`CONSOLE_IDX=0`、TX=PA18、RX=PA17、115200）的流控脚。CTS 是**输入**，所以它"能读到电平"（这解释了为什么三脚里只有它看起来能工作），但它和 console 绑死，动一次 console 配置就可能失效。 |
| **PA22** | UART0_RTS_N | ❌ 不要用 | 同上，且 **RTS 是芯片的输出**。UART0 一使能，输出级可能把该脚驱动成固定电平 → 作 GPIO 输入读不到探头变化（典型症状：恒定读 1，遮挡无反应）。 |
| **PA11 / PA12 / PA13 / PA14** | **soc.h 中无任何功能枚举 → 纯 GPIO** | ✅ **首选**（前提是排针真的引出了） | 全芯片唯一"没有任何外设会来抢"的一组。注意：它们没有 ADC，不会牵扯 `ANA_IO_EN`。 |
| PB22 / PB23 / PB28~PB31 | TDM_SCLKOUT / WSOUT / D0~D3 | ✅ 次选（PB22 已实测可用） | TDM 未被使用；PB23、PB28~PB31 与 PB22 同族。 |

### 6.1 "接线全在 PA 上，但 PA21 读出 RIGHT_IR" 怎么解释

代码事实：仓库里的 `ir_test.c`（改扫描版之前）读的是 **PB20/PB21/PB22**，其中 `RIGHT = PB22`。
所以串口上 `R=` 那一位**只可能来自 PB22 的电平**。

PA21（UART0_CTS）与 PB22（TDM_SCLKOUT）是两个独立 pad，**没有内部通路**，"PA21 的信号跑到 PB22"在电路上不成立。剩下三种可能，用扫描固件一次即可区分：

| 扫描固件现象 | 结论 |
|---|---|
| **PB22 跟着 RIGHT 探头变，PA21 不动** | **接线/丝印认错了**：你插的那个物理位置实际连到芯片 PB22。（本项目已有同类先例：PA2 曾被当成可用脚，实际是 LCD_CS。） |
| **PA21 跟着变，PB22 不动** | 你确实接在 PA21，而代码读 PB22 —— 那串口上的 R 不可能有反应，说明烧进板子的那版代码改过引脚，与仓库这份不一致。 |
| **PA21 与 PB22 同时跟着变** | 两个脚被物理连在一起了（排针/板上短接），需查硬件。 |

### 6.2 扫描版 `ir_test.c`（已改好，可直接编译烧录）

入口名仍是 `ir_test()`，`main.c` 与 `package.yaml` **不用改**。它把 11 个候选脚
（PA11~PA14、PA21、PA22、PB20、PB21、PB22、PB23、PB28）**同时**设为 GPIO 输入并开上拉：

- 上电打印一次寄存器快照：`PORTCTL` / `IOMUX0L` / `IOMUX0H` / `IOPU0` / `IOPD0` / `ANA_IO_EN`，
  外加逐脚 `pctl(1=还在复用模式) func(IOMUX选的功能) pullup(1=上拉已开)`；
- 之后每 1s 无条件打印全部候选脚电平（200ms × 5），任意一路变化时立即额外打印并标 `<== CHANGED`。

**操作**：上电 → 拿一个探头依次遮挡 → 看哪几个脚跟着翻转，即可确定探头真实落点。

**默认不扫描 PA20**（JTAG 风险）。要用就在 `s_cands[]` 里加：
`{ PORTA, PIN20, 20U, "PA20", "JTAG_TCK" }`，代价是 CDK 调试器可能掉线，重新上电即可恢复。

---

## 7. UART3（ESP32 通信）与 IR 三路的资源共存规划

> 触发点：用户确认"后期要用 UART3 把 TD1601 和 ESP32 连起来通信"。

### 7.1 UART3 只占 PB18/PB19；CTS/RTS 不是必需的

| 信号 | 引脚 | 功能值 | 什么时候需要 |
|---|---|---|---|
| UART3_RXD | **PB18** | `PB18_UART3_RXD = 0U` | 始终需要 |
| UART3_TXD | **PB19** | `PB19_UART3_TXD = 0U` | 始终需要 |
| UART3_CTS_N | PB20 | `= 0U` | **仅硬件流控时需要** |
| UART3_RTS_N | PB21 | `= 0U` | **仅硬件流控时需要** |

ESP32 点对点、115200 速率，**不需要硬件流控** → PB20/PB21 永远空着。
但**仍然不要把 IR 放在 PB20/PB21**，两条理由：① 它们始终是 UART3 的引脚，`csi_uart_init(…, 3)` 一执行就会被模块接管/影响电气状态；② 这两脚已被实测证明"探头遮挡读不出电平"。所以留空最干净 —— 将来若真需要流控（比如改成高速率长时间传输），随时能启用。

### 7.2 ⚠️ pinmux 缺陷 B 与 UART3 的"侥幸"关系

`csi_pin_set_mux` 的非 GPIO 分支里，PORTB 的 `else` 写成了 `HOBBIT1_IOMUX0L_REG`（应为 `_IOMUX0H_REG`）：
→ **给 PB16~PB31 配任何外设功能，功能值都会写进 PB0~PB15 的 IOMUX0L，目标脚自己的 IOMUX0H 字段纹丝不动。**

UART3 为什么仍然能通？因为 `PB18_UART3_RXD` 与 `PB19_UART3_TXD` 的功能值**都是 0**，而 IOMUX0H 的复位默认值很可能就是 0 → "写错寄存器"恰好等于"写对了"。这与 PB22（`TDM_SCLKOUT = 0U`）能正常工作同理。

**由此得到一条本项目必须记住的经验规则：**

| PORTB（PB16~PB31）上想用的外设功能 | 能否配上 |
|---|---|
| **func0**（IOMUX 值 0，等于复位默认） | ✅ 能用（UART3 PB18/PB19、TDM PB22/PB23/PB28~PB31、UART2 PB12~PB17、SPI1 PB4~PB11、IIC1 PB24/PB25、CAN PB26/PB27…） |
| **func1 / func2 / func3** | ❌ 配不上，值被写进 IOMUX0L，还会污染 PB0~PB15 |

**自检方法**：扫描版固件启动时 dump 的 `[IRSCAN] REG B: … IOMUX0H=0x……` —— **若该值为 0x00000000，上面这条规则成立**（本项目所有 PB 外设都是 func0，所以一直没暴露）。

### 7.3 UART3 代码要点（将来写通信时按这个来）

```c
csi_pin_set_mux(PORTB, PIN18, PB18_UART3_RXD);   /* 功能值 0 */
csi_pin_set_mux(PORTB, PIN19, PB19_UART3_TXD);   /* 功能值 0 */
csi_uart_init(&s_uart3, 3);                      /* idx=3，见 devices.c 的 uart_pinmap */
```

- **idx = 3**（`uart_pinmap[]` 里 PB18/PB19 的 idx 列就是 3），不是引脚号；
- 中断号：`DW_UART3_IRQn = 16 + 22 = 38`；唤醒号：`WJ_UART3_Wakeupn = 30`；
- 波特率：**115200**（项目统一约定，与 console 一致）；
- **不开硬件流控** → 不要给 PB20/PB21 配 CTS/RTS。

### 7.4 ⚠️ 电平：TD1601 是 1.8V IO，ESP32 是 3.3V（接线前必读）

这是接 ESP32 时最容易烧板子的一步：

- **ESP32_TX(3.3V) → PB18/RXD(1.8V)**：直连会**过压灌流，损伤芯片**，必须降压；
- **PB19/TXD(1.8V) → ESP32_RX(3.3V)**：1.8V 高电平可能**达不到 ESP32 的 VIH**，通信会不稳或完全不通；
- 结论：**必须做电平转换**（双向 level shifter，如 TXS0108E；或分压 + 单向转换）。
- 例外：若所用 ESP32 模组的 IO 供电可切到 1.8V（部分 ESP32-S3 模组的 `VDD_SPI` 可选 1.8V），则可简化 —— **接线前先确认模组型号**。
- 顺带：**GND 必须共地**，两条 UART 线都要短、远离舵机线（舵机堵转电流 0.7~2.5A，串扰会导致丢字节）。

### 7.5 IR 三路的推荐落点（避开 UART3）

| 方案 | 引脚 | 优点 | 代价 |
|---|---|---|---|
| **A（推荐）** | **PB16 + PB22 + PB23** | 全部空闲；PB22 已实测可用；不撞 UART3 / LCD / 触摸 IIC / 舵机 PWM | PB16 与 PB22/PB23 不同排针位置；`io_plan.md` 里 PB16 预留的触摸 RST 用途要放弃 |
| **B** | **PB22 + PB23 + PB28**（或 PB29~PB31） | 同属 TDM 族，很可能物理相邻，接线整齐 | 需扫描确认 PB28~PB31 可用 |
| **C** | **PA11~PA14 任选三** | `soc.h` 中无任何功能枚举 = 纯 GPIO，全芯片最干净 | 需确认排针引出；与 PB 组不共排针 |
| ❌ **不要** | PB20 / PB21 | —— | UART3 流控脚 + 已实测读不出 |
| ❌ **不要** | PA20 | —— | JTAG_TCK，切 GPIO 会掉调试器 |

**决策顺序**：先烧扫描版固件 → 看哪几个脚真的跟着探头翻转 → 在"能用的脚"里挑物理相邻的三个 → 再回填 `ir_test.c` 的三路宏。

---

## 8. IR 选型：候选组全景对比（2026-08-30）

讨论中出现过的**每一组，都"沾"着某个外设的引脚池** —— 这是判断风险的主线。

| 组 | 引脚 | 归属外设 | 该外设当前状态 | 风险 | 结论 |
|---|---|---|---|---|---|
| ① | PB20 / PB21 / PB22 | UART3（CTS_N / RTS_N / TDM） | UART3 未初始化，但**已确认后期要用它连 ESP32** | 🔴 高 | ❌ 不用：PB20/PB21 已实测读不出 + 与 UART3 冲突 |
| ② | **PB9**（PB8/PB9/PB11） | **SPI1_D5**，即屏的数据线 D5 | `src/video.c` 当前只用**四线** `PA3(SCK)+PB4~PB7(D0~D3)`；但 `UserFolder/lcd_test.c` 证明硬件走线具备 D4~D7 | 🟡 中 | ⚠️ 当下可用，**屏改八线即失效**，不宜作最终方案 |
| ③ | **PB0 / PB1 / PB2**（+PB3） | PB0/PB1 = **UART0_DE/RE**；PB2/PB3 = UART1_DE/RE | UART0 就是 console，**活跃**，但 `board_init.c` 只配 `TX=PA18 / RX=PA17`，**DE/RE 全程未被配置**（console 模块与 uart 驱动中均无 DE/RE 使用代码）；UART1 完全未用 | 🟡 中低 | ⚠️ **物理连号三脚**，值得实测；仅当 console 改用 RS485 模式才冲突 |
| ④ | PB16 / PB17 | UART2_DE / UART2_RE | UART2 未用 | 🟢 低 | ✅ 可用（须放弃 `io_plan.md` 里"PB16=触摸 RST、PB17=触摸 INT"的预留） |
| ⑤ | **PB22 / PB23 / PB28~PB31** | TDM（SCLKOUT / WSOUT / D0~D3） | TDM 未用，功能值全为 func0 | 🟢 **最低** | ✅ **首选**（PB22 已实测可用） |
| ⑥ | **PA11~PA14** | `soc.h` 中**无任何功能枚举** | —— | 🟢 **最低** | ✅ 首选（需确认排针引出） |
| ❌ | PA20 | JTAG_TCK | 调试器在用 | 🔴 高 | ❌ 切 GPIO 会掉调试器 |
| ❌ | PB10 | SPI1_D6 + **LCD WAIT** | `video.c:238` 已占用 | 🔴 高 | ❌ 不要用（易与 PB9 混淆） |

**关键结论**：PORTB 上真正"谁都不沾"的只有两组 ——
- **TDM 族：PB22 / PB23 / PB28 / PB29 / PB30 / PB31**
- **UART2 的 DE/RE：PB16 / PB17**

其余脚都属某个外设的引脚池（UART0 / UART3 / SPI1-LCD），能不能用取决于那个外设将来怎么用。

### 8.1 选型建议

| 目标 | 组合 | 备注 |
|---|---|---|
| **长期稳定**（推荐） | **PB22 + PB23 + PB28** | 同属 TDM，无人竞争；PB22 已实测可用 |
| **物理相邻** | **PB0 + PB1 + PB2** | 连号三脚，当下空闲；沾 console 的 DE/RE，需实测确认读得到探头 |
| **完全避开 PORTB 竞争** | **PA11 + PA12 + PA13**（或 PA14） | 纯 GPIO，全芯片最干净；需确认排针引出 |

### 8.2 当前扫描版固件的覆盖范围（20 路）

`PA11 PA12 PA13 PA14 PA21 PA22 | PB0 PB1 PB2 PB3 PB8 PB9 PB11 PB16 PB17 PB20 PB21 PB22 PB23 PB28`

一次烧录即可看清：哪几脚跟着探头翻转（=可用的真实落点）、哪几脚恒定不动（=被外设钉住）、以及启动时 dump 的 `REG A/B` 寄存器快照（`pctl`/`func`/`pullup` 逐脚）。

**实测纪律**：**先让扫描固件说话，再定三路宏** —— 本项目已多次出现"引脚表/丝印与实际不符"（PA2 被当成可用脚实为 LCD_CS），猜测的代价远高于一次实测。

### 8.3 定案（2026-08-30）

IR 三路最终确定 **PB22 / PB23 / PB29**（PORTB 上唯一"谁都不沾"的 TDM 族），`UserFolder/test/ir_test.c` 已改写为对应的**三路简版**（原来的 20 路扫描版被替换）：

| 项 | 值 |
|---|---|
| 引脚宏 | `IR_LEFT_PIN = PIN22`、`IR_CENTER_PIN = PIN23`、`IR_RIGHT_PIN = PIN29` |
| GPIO 控制器 | `IR_GPIO_IDX = 1`（PORTB） |
| 轮询周期 | 50ms；电平变化立即打印；每 1s 无条件打一行心跳 |
| 启动自检 | 打印 4 个寄存器：`PORTCTL1` / `IOMUX0H` / `IOPU0` / `ANA1` |

**脚位变更记录**：
- 2026-08-30 初定 PB22 / PB23 / **PB28**（TDM_D0）；
- 实测 **PB28 一路恒低电平**，故改为 **PB29**（TDM_D1）—— 同族同区域、功能值同为 func0，除引脚宏外其余代码不变；
- 后备：**PB30 / PB31**（TDM_D2 / D3）。

**自检行判读**（这三个脚都在 IOMUX0H 段，而官方 `csi_pin_set_mux` 在 PORTB 分支漏了 `offset -= 16`、移位量 ≥32 属 UB，只是靠 RISC-V `sll` 取低 5 位才碰巧正确 —— 所以必须看寄存器，不能凭感觉）：

- `PORTCTL1`（0x60030100）**bit22 / bit23 / bit29 必须为 0**（0 = 已切 GPIO，1 = 还在复用模式）
- `IOMUX0H`（0x6003010C）**bit12~13(PB22) / bit14~15(PB23) / bit26~27(PB29) 必须为 0**
- `IOPU0`（0x60030120）**bit22 / bit23 / bit29 必须为 1**（内部上拉；为 0 则开漏输出的模块读不到高电平）
- `ANA1`（0x60030170）**bit22 / bit23 / bit29 必须为 0**（为 1 说明该脚被切到 ADC 模拟通路，数字输入会恒读 0）

**仍待实测**：PB22 是此前唯一实测可用的脚；**PB23、PB29 尚未单独验证过**，首次上电重点确认这三路都能跟着遮挡翻转（若 R 路不动，先看自检行，再考虑换 PB30 / PB31）。

**"某一路恒低"三步排查**（PB28 就是这么发现的，已写进 `ir_test.c` 文件头）：
1. 拔掉该路探头信号线：变高 → 引脚/上拉正常，问题在探头或接线；仍低 → 引脚被外部拉低或 mux/上拉未生效；
2. 看自检行四个寄存器的对应位（见上）；
3. 把同一探头换到已知可用的 PB22 上：也恒低 → 探头/线/供电问题；正常 → 该引脚位置特有（换 PB30/PB31）。
