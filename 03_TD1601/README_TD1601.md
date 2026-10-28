# TD1601 固件

## 两种形态，别混

| 形态 | 位置 | 能不能编译 |
|---|---|---|
| **源码存档**（本目录 `baozi/`） | 随包分发 / Git 仓库里 | ❌ 不能 |
| **可编译工程**（SDK 内） | `...\td1601_evb_v0.2.0_cdk\projects\baozi` | ✅ 能 |

`baozi/` 是**同一份代码的只读存档**，用途是让人能看到 TD1601 端写了什么，
以及在脱离本机时仍保留完整源码。

**编译和烧录必须在 SDK 里做。** 原因是 CDK 按**相对路径**解析依赖：

```
<SDK>/td1601_evb_v0.2.0_cdk/
├── components/          ← 芯片、csi、minilibc、mm 等组件
├── boards/td1601_evb/   ← 板级定义 + 链接脚本 gcc_flash.ld
└── projects/
    └── baozi/           ← 工程必须在这里
```

生成的 `Baozi.mk` 里写的是 `-I../../components/...`、`-T../../boards/...`。
把 `baozi/` 拷到别处，编译立刻失败（找不到头文件 / 链接脚本丢失）。

## 想在别的机器上编译？三步

1. 装 CDK（平头哥 YoC 工具链），拿到一份 `td1601_evb_v0.2.0_cdk` SDK
2. 把本目录的 `baozi/` **整个复制**到 `<SDK>/projects/baozi`
3. 用 CDK 打开 `project.cdkproj`，编译 / Download

⚠️ 第 2 步之后，`package.yaml` 是唯一的编译清单权威源。
`Baozi.mk`、`Obj/Baozi.txt` 都是 CDK 生成的产物，只改它们会在
下次重新生成时丢失（2026-10-28 就踩过：`behavior.c`、`play_card.c`、
`face_draw.c` 漏登记，靠旧 `.o` 撑着才能链接）。

## 本机快速打开

| 方式 | 文件 |
|---|---|
| 双击打开资源管理器 | `打开固件工程.bat` |
| 浏览器打开目录 | `SDK母文件夹_projects.url` |
| 直接粘贴路径 | 见下方 |

```
D:\Baozi\CDK\official_SDK\td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59\
  td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59\
    td1601_evb_v0.2.0_cdk\projects\baozi
```

## 编译与烧录

```bash
export PATH="/d/Baozi/CDK/CDKRepo/Toolchain/XTGccElfNewlib/V3.4.0/R/bin:/d/Coding/VSCODE/mingw64/bin:$PATH"
cd ".../projects/baozi"
mingw32-make -f Baozi.mk -j4
```

**该看到**：

```
   text    data     bss     dec     hex  filename
  96132     644   48656  145432   23818  ./Obj/Baozi.elf
```

末尾 `crc32` 报 `Error 2`、`mv` 报 `Error 1 (ignored)` —— **已知无害**，
ELF 已正确产出，烧录用 CDK 的 Download 按钮。串口 115200-8N1。

## 当前状态（2026-10-28）

| 项 | 值 |
|---|---|
| 入口 | `src/main.c` 的 `TEST_ENTRY 4`（`app_run`：卡片 UI + 触摸 + 舵机 + 情感） |
| 体积 | `text 96132 / data 644 / bss 48656`（SRAM 192 KB，余量充足） |
| 规模 | 69 个 `.c` / 44 个 `.h` |
| 改动记录 | `baozi/CHANGELOG.md` |
| SDK 内工程的版本控制 | 那个目录另有独立 `.git`（基线 `147d03f`） |

## 与另外两端的关系

| 链路 | 介质 | 说明 |
|---|---|---|
| ESP32 → TD1601 | **仅 PB18**（单向） | ESP32 `GPIO14` → TD1601 `PB18/UART3_RXD`，115200-8N1 |
| PC → TD1601 | UART0 `PA17/PA18` | 独立 USB-TTL，`A5 LEN CMD DATA XOR` 帧（习惯/日程/提醒） |

⚠️ **PB19 未引出**：PCB 只走了一根 PB18，所以固件 `src/uart_esp.c` 已按
单向接收实现（不配置 PB19、不发开机 banner）。详见 `baozi/CHANGELOG.md` 的 `821ecdb`。

## 改代码前的三条铁律

1. **加新 `.c` 只改 `package.yaml` 的 `source_file`**。其余都是生成物。
2. **串口输出一律纯 ASCII**，中文只写在注释里（串口终端没有 UTF-8 渲染能力）。
3. **UART0 只有一个接收者**（`src/uart_hal.c`），屏幕只有一个驱动者
   （`UserFolder/ui_app.c`）。详见 `baozi/MERGE_NOTES_触摸UI与行为层.md`。
