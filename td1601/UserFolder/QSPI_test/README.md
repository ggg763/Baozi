# 屏幕测试（QSPI_test）—— 3D 四面体 + 双缓冲流水线 DMA

> 这份说明随"屏幕测试包"一起发出。
> **目的**：用**最小依赖**验证 **ST7701S 480×480 QSPI 屏**是否正常。
> 我方板子已用 `video_tetra_test()` 验证通过（硬件正常）—— 如果你那边不亮，请先按 §5 逐项核对，
> 大概率是**这几处依赖文件被改动**或**编译登记丢失**，而不是屏幕坏了。

---

## 1. 这个测试是什么

- 本体：`video_tetra_test.c`，入口 **`video_tetra_test()`**；
- 内容：软件 3D（整数定点）算一个正四面体，绕 X/Y 旋转 + 透视投影 + 背面剔除 + 平面光照，
  然后**双缓冲流水线 DMA** 全屏刷新（合成下一段与传输上一段重叠）；
- 每 10 帧在串口打一行 `[tetra] frame N`（用带时间戳的串口助手即可算帧率）。

⚠️ **硬件限制（文件头有实测结论，别改）**：DW DMA 单次传输上限 **4095 像素**
→ 该文件固定按 **8 行（3840 像素）一段**写，每帧 60 段。段长改大 → 面板 RAMWR 数据流长度不符 → **画面错位污染**。

## 2. 入口（两处必须同时满足，否则编译或运行不对）

| # | 位置 | 要求 |
|---|---|---|
| 1 | `src/main.c` 的 `main()` | 依次是 `board_init();` 和 **`video_tetra_test();`** |
| 2 | `package.yaml` 的 `source_file` | **必须有一行 `- UserFolder/QSPI_test/video_tetra_test.c`** |

> ⚠️ `QSPI_test/` 目录下的文件**默认不编译**。忘了第 2 条 → 编译报
> `undefined reference to 'video_tetra_test'`（main.c 引用了但那个 .c 没进编译）。
> 改完 `package.yaml` 后，CDK 需要**重新解析**（Clean→Build，或关掉工程重开）才会把新文件纳入编译。

## 3. 依赖清单（**只有这几个文件跟屏幕有关**）

| 文件 | 作用 | 必需 | 核对用的"关键特征" |
|---|---|---|---|
| `UserFolder/QSPI_test/video_tetra_test.c` | 测试本体 | ✅ | 入口函数 `video_tetra_test()`；`#define CH_IDX 2`；注释里写"段长固定 8 行"；结尾是 `for (;;)` 死循环 |
| `src/video.c` | **屏幕底层**：OSPI/QSPI + DMA + 官方原语 | ✅ | `#define LCD_DEBUG 1`、`#define CONFIG_USE_OSPI_DMA 1`、`#define CH_IDX 2`；并能找到这四个函数：`LCD_Set_Window` / `LCD_startWriteMutileData` / `LCD_endWriteMutileData` / `LCD_WritemutileData16_fast` |
| `include/video.h` | `video_init` / `video_deinit` / `video_flip` / `HAL_Delay` 声明 | ✅ | |
| `src/main.c` | 入口；另外**提供 `frame_count` 与 `HAL_Delay`** | ✅ | 这两个是 `video.c` 依赖的，**不能删** |
| `src/board_init.c` | console（UART0 115200-8N1） | ✅ | 没它就没串口日志 |
| `include/board_init.h`、`include/csi_config.h`、`include/sys/_stdint.h` | 编译所需头 | ✅ | |
| `chip_config.c` | 板级/芯片配置 | ✅ | |
| `package.yaml`、`project.cdkproj`、`project.cdkws` | CDK 工程与编译登记 | ✅ | |
| `utilities/pre_build.sh`、`utilities/aft_build.sh` | CDK 构建前后脚本 | ✅ | 缺了 CDK 构建会报找不到脚本 |

**这些文件之外的任何东西都跑不到屏幕**：舵机（`UserFolder/servo_test/`）、动作库（`movements/`）、
情感状态机（`emotion/`）、红外跟随（`tracking/`）、图形库（`gfx/`）**全都不参与**这个测试。
所以：哪怕它们被改乱了也不影响本测试 —— **前提是 §2 两处入口没被改回去**。

引脚（`src/video.c` 里定义，换排线时对照）：

```
CS = PA2    SCK = PA3    D0~D3 = PB4~PB7    RST = PB15    WAIT = PB10    背光 BLK = PA31（高电平点亮）
```

## 4. 预期现象（正常的话）

1. 上电 → 屏幕自检（彩色图案）→ `HAL_Delay(1500)`；
2. 之后**持续旋转**：浅蓝背景上一个**橙红色正四面体**绕 X/Y 转（不停，`for(;;)` 死循环）；
3. 串口（**115200-8N1**）能看到启动日志，之后每 10 帧一行：

```
[tetra] animation start (band=8 lines, double-buffered)
[tetra] frame 10
[tetra] frame 20
...
```

**算帧率**：`frame N` 与 `frame N+10` 两行的时间差 = 10 帧耗时 → 期望 **35~45fps**
（`frame 10` 到 `frame 20` 约 0.22~0.29s）。

## 5. 不亮 / 异常 → 按这个顺序查

| 现象 | 最可能的原因 | 怎么处理 |
|---|---|---|
| 编译报 `undefined reference to 'video_tetra_test'` | `package.yaml` 没登记，或 CDK 没重新解析 | 补登记（§2 第 2 条）→ Clean → Build / 重开工程 |
| 串口**有** `[tetra] frame` 但屏幕全黑 | 背光或数据线 | 先量 **PA31**（应为高电平点亮）；再查 `PB4~PB7`(D0~D3)、`PA3`(SCK)、`PA2`(CS) 的排线 |
| 串口**没有**启动日志 | 卡在初始化或串口不通 | 查 `PB15`(RST) 与排线；确认串口 115200-8N1、TX/RX 没接反 |
| 花屏 / 画面错位、有拖影污染 | **段长被改大**（超过 4095 像素） | 恢复 `video_tetra_test.c` 原版：必须 8 行/段 |
| 只有自检、没有动画 | 卡在第一帧合成 / DMA | 看串口有没有 `animation start` 那行；没有就是死在自检之后、动画之前，优先对比 `video.c` 是否被改 |
| 能跑但很慢（< 20fps） | DMA 没启用 | 查 `src/video.c` 的 `#define CONFIG_USE_OSPI_DMA 1` |

## 6. 核对"文件是否被改动"的最快方式

不用逐行读，只看这几处（对不上就说明文件被改过）：

- `video_tetra_test.c`：`#define CH_IDX 2`、入口函数名 `void video_tetra_test(void)`、结尾 `for (;;)`
- `video.c`：`#define LCD_DEBUG 1` / `#define CONFIG_USE_OSPI_DMA 1` / `#define CH_IDX 2`，
  且四个原语函数都在（§3 表格列了名字）
- `main.c`：`frame_count` 与 `HAL_Delay()` 两个定义仍在（**删了会链接失败**），`main()` 里调的是
  `video_tetra_test()`

## 7. 与本目录其它测试的关系

`QSPI_test/` 下其它 12 个文件（`qspi_*`、`video_smile*`、`video_partial_test.c` 等）都是**历史实验**，
**默认不编译**、互不依赖。本测试**不需要**它们中的任何一个（也不 include `qspi_lcd.h`）。
要跑其中某个，同样得先在 `package.yaml` 里登记它。
