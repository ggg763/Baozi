# 修改记录 CHANGELOG

本文件**随代码走**，记录对 `projects/baozi` 的每一次改动。
新增/修改代码时请在这里加一条，并在提交信息里引用对应条目。

---

## 版本控制

本工程自 **2026-10-28** 起纳入 Git（此前无任何版本控制，改坏了无法回滚）。

| 项 | 值 |
|---|---|
| 仓库位置 | `projects/baozi/.git`（仅本工程，不含整个 SDK） |
| 基线提交 | `147d03f` baseline: 合并版现状快照（2026-10-27） |
| 跟踪文件 | 146 个（源码 + 文档 + 配置文件） |
| 忽略内容 | `Obj/` `Lst/` `.cache/` `.cdk/`、`*.o *.d *.elf *.ihex *.bin *.map *.asm`、`*.bak` |

两条与普通仓库不同的约定：

1. **`.gitattributes` 设为 `* -text`**：禁止 Git 做 LF/CRLF 转换。
   这是嵌入式工程，源文件换行符被改写会让后续 diff 全变脏，
   也可能影响 `.bat` / `.sh` 的行为。
2. **`Baozi.mk` 入库，但它是生成产物**。它入库是为了让"当前能编译的状态"
   可追溯；真正的权威源是 `package.yaml` 的 `source_file`。
   ⚠️ 加新 `.c` 只改 `package.yaml`，不要只改 `Baozi.mk` —— 见 `5e3dc83`。

常用命令：

```bash
cd ".../projects/baozi"
git log --oneline                 # 看历史
git diff 147d03f --stat           # 与基线比改了什么
git checkout -- <file>            # 丢弃某文件改动
git show <commit>                 # 看某次改动详情
```

---

## 2026-10-28

> 本轮两件事：**v2.1 扫码配网**（赛题功能）+ **v2.2 时间显示改圆角数码**
> （观感修复）。两者都在分支 `feat/qr-provision` 上。

### v2.2 style(ui): 时间显示改圆角数码，不再是放大后的方块像素

**问题**：板载 ASCII 字库是 12×24 的 1bpp 点阵，`ui_add_text(scale 2)` 就是
把每个像素放大成 2×2 的硬方块 —— 48px 高的时钟上每个台阶都看得见；番茄钟的
大数字用 scale 5，等于 5×5 的方块，更明显。用户反馈"像素块太明显"。

**做法**：不再放大点阵，改成**用圆角矩形拼七段笔画**（`ui_add_clock()`）。
每个笔画是一个圆角块、半径 = 笔画厚度的一半，两端是半圆，48px 和 120px 都没有台阶。
不需要新字库、不占 flash，只在绘制列表里多十几个圆角块。

| 文件 | 改动 |
|---|---|
| `UserFolder/ui.h` | 新增 `ui_add_clock()` / `ui_clock_width()` 接口与说明 |
| `UserFolder/ui.c` | **新增**圆角数码实现：七段位掩码表、按 h 的比例算笔画厚度/字宽/字距、`-`（未对时）与 `:` 的处理 |
| `UserFolder/ui.c` | `UI_MAX_ROUND` 64 → 96（见下）|
| `UserFolder/ui_app.c` | 卡片页顶部时钟、提醒弹卡的时间改用圆角数码 |
| `UserFolder/pomo.c` | `top_time` / `big_clock`(MM:SS, 原 scale 5) / `big_clock_hms`(HH:MM:SS, 原 scale 4) / 倒计时设置页三个字段（原 scale 3）全部改用圆角数码 |

**两处几何细节**（都是渲染出来才看出来的）：

1. **竖笔要比半高多伸 t/2**。两个胶囊若是尖端对尖端相接，交界处会收成一个"腰" ——
   `0` 的左右两边中间出现缺口、`2` 的拐角断开。多伸半个笔画让它们真正重叠。
2. **`1` 用窄格**。七段的 1 只有右侧两条竖笔，占满整格会在左右各留一大块空白，
   `09:41` 看着像 `09:4  1`。窄格宽度 `h*38/100`，且 `ui_clock_width()` 必须
   用同一个值，否则居中会偏。
3. **笔画厚度定在 `h*15/100`**（48px → 7px）。试过 20%，在卡片页顶部会明显比
   二维码和正文更抢眼；15% 既保留了圆头，又把视觉权重压回接近原来的字重。

**顺带修掉的隐患**：`UI_MAX_ROUND` 原来是 64，一个 8 字符的 `00:00:00`
就要 6×7 = 42 个圆角块，再加卡片/按钮就顶满 —— 超了会**静默丢弃**后面的元素
（实测：番茄钟大数字最后两位直接不显示）。提到 96（+448B bss）。

**验证**：`_probe/qr/ui_sim.c` 新增 `clock` 模式（`ui_sim clock 120 "25:00" out.bmp`）
与 `old` 模式（旧的点阵放大，用来出对照图），把真实 `ui.c` 渲染成 BMP 逐档看；
`0-9`、`:`、`--:--`、`09:41`、`00:00:00` 都已核对。对照图：
`_probe/qr/cmp_old.bmp`（旧）↔ `cmp_new.bmp`（新）。
RISC-V 全量链接 **text 103 100 / data 644 / bss 60 056**。

### v2.1 feat(qr): 扫码配网 —— 第 5 页 NET + 板上自算二维码

分支 `feat/qr-provision`。赛题要求的"二维码配网"落到 TD1601 侧的全部代码。
详细协议与联调步骤见 `UserFolder/doc/qr_provision.md`。

**链路**：PC(`net_share.py`) ─局域网 UDP─► ESP32 ─UART3(PB18, 单向)─► TD1601 ─► 屏。

| 文件 | 改动 |
|---|---|
| `UserFolder/qr/qrgen.h/.c` | **新增**：QR 编码器（字节模式、版本 1..10、ECC L/M/Q/H、8 掩模按标准罚分选优）。纯整数运算，无 malloc，可在 PC 编译 |
| `UserFolder/qr/net_card.h/.c` | **新增**：配网卡片。5 个状态、二维码打包成 1bpp、卡片内/全屏两套版面 |
| `include/net_link.h`、`src/net_link.c` | **新增**：`QR:` / `NET:` 文本命令解析。UART3 与 UART0 两个入口共用 |
| `UserFolder/ui_app.c` | 卡片层 4 页 → 5 页（圆点、滑动边界、点击分发）；`ui_app_show_net()` 自动跳转；NET 页不自动回落 |
| `UserFolder/ui_app.h` | 新增 `ui_app_show_net()` |
| `src/uart_esp.c` | `ESP_LINE_MAX` 64 → 160（一条 `QR:WIFI:…` 最长 117 字节）；`esp_apply_line()` 先喂 `net_link` |
| `UserFolder/app_core.c` | 初始化调 `net_link_init()`；UART0 文本命令先过 `net_link_handle_line()`（无 ESP32 也能台架联调）|
| `package.yaml` | 登记 `qr/qrgen.c`、`qr/net_card.c` |
| `Baozi.mk` / `Obj/Baozi.txt` | 补 `src_net_link` / `UserFolder_qr_qrgen` / `UserFolder_qr_net_card` 三个目标（生成物，权威仍是 package.yaml）|

**验证**（都不用烧录）：

- `_probe/qr/qr_verify.py`：13 个用例与 `qrcode` 库比对，**去掩模后码字流全部一致**
  （数据 + RS 纠错 + 交错 + 功能图形）；掩模号个别不同，属同分取舍；
- `_probe/qr/ui_sim.c`：把 `LCD_*` 打桩，在 PC 上渲染真实 `ui_app.c` + `net_card.c`
  出 BMP，三种状态版面已检查（`_probe/qr/out_net_*.bmp`）；
- RISC-V 全量链接通过：**text 101 948 / data 644 / bss 59 560**（+5.7KB / +10.9KB）；
- 新增三个文件与改动的 `ui_app.c` 在 `-Wall -Wextra` 下 0 warning。

**踩到的两个坑**（已修，值得记住）：

1. `ui_add_text` 只存指针，`net_card.c` 一开始用栈上 buf 拼 `SSID: xxx`，
   刷出来是一行随机字符 —— `ui_sim` 一眼看出来，不用上板。
2. 只改 `Baozi.mk` 的 `Objects0` 不够，链接用的是 `Obj/Baozi.txt` 那份清单；
   漏了会 `undefined reference`，而 `.o` 明明已经编出来了。

### `821ecdb` fix(uart): 按实际 PCB 改为单向接收，仅 PB18

**硬件事实**：已流片的板子上，ESP32 与 TD1601 之间只走了一根 PB18
（ESP32 `GPIO14` → TD1601 `PB18/UART3_RXD`），`PB19/UART3_TXD` 没有引出。

**原问题**：`src/uart_esp.c` 按双向设计——既把 PB19 配成 `UART3_TXD`，
又开机往它发一行 banner（本意是验证 `PB19 → ESP32 GPIO21` 那根线）。
在单线板上，这两件事都在驱动一个不存在的连接。

**改动**（全在 `src/uart_esp.c`）：

| # | 改动 | 原值 → 新值 |
|---|---|---|
| 1 | PB19 的 `csi_pin_mode` / `csi_pin_set_mux` | 有 → 移除，只配 PB18 |
| 2 | IOMUX0H 兜底清零范围 | `bit4~5 + bit6~7` → 仅 `bit4~5`（PB18） |
| 3 | `ESP_UART_TX_BANNER` | `1` → `0`（不发开机 banner） |
| 4 | 启动日志 | `RX=PB18 ..., TX=PB19` → `RX=PB18 only (TX=PB19 not wired on PCB)` |
| 5 | 文件头 / 引脚注释 | 同步说明硬件约束与将来恢复办法 |

**体积**：`text 96236 → 96132`（−104 B），`bss 48660 → 48656`（−4 B），`data 644` 不变。

**保留项**：banner 代码与宏都留着。将来若补上回传线，把宏改回 `1` 即可复用现有自检手段。

**与 coworker 版的一致性**：`UserFolder/esp_link.c` 只做 `portb_high_mux(18, 0)`，
同样只处理 PB18。本次改动让 SDK 版回到同一硬件假设。

---

### `5e3dc83` fix(build): package.yaml 补登记 3 个文件

**原问题**：`behavior.c`、`play_card.c`、`face_draw.c` 只存在于 `Baozi.mk`
的 Objects0 与编译规则里，`package.yaml` 的 `source_file` 没有登记。

- `main.c:90` 调 `behavior_init()`
- `ui_app.c:396/549` 调 `play_card_draw()` / `play_card_tap()`
- `face_model.h` / `face_render.h` 都 `#include "face_draw.h"`

**后果**：当前能链接，只是因为旧的 `.o` 还在。一旦用 CDK 重新打开工程、
重新生成 makefile，这三条规则消失 → `undefined reference`。

**改动**：`package.yaml` 的 `source_file` 追加三条，并在注释里写明
「`Baozi.mk` 是生成产物，本表才是唯一权威源」。

**验证**：

- `package.yaml` 展开 52 个 `.c` == `Baozi.mk` Objects0 的 52 个对象，双向无差
- 删除这三个 `.o` 强制重编 → 全部重新编译、链接成功
- `text 96236 / data 644 / bss 48660`，与修复前**逐字节一致**（无副作用）

---

## 待办（未执行）

以下三项已评估但**按项目负责人决定暂不执行**，留待后续：

1. **`moves_yield()` 让位门**（coworker 版有、SDK 版无）
   修的是 `mode_enter_track()` 只用 `moves_interrupt()`（`s_intr` 一次性标志）的竞态：
   若 `TRACK:ON` 恰好落在 `moves_play_variant()` 播放前清标志那一瞬，
   中断失效，睡眠这类长动作会继续和追踪抢舵机。
   → 评估见 `code/projects/MERGE_PLAN_TD1601.md` §3.1

2. **追踪退出策略**（coworker：扫完继续扫到 `TRACK:OFF`；SDK：扫完一圈自动退出）
   → 属产品行为，当前保持 SDK 现状

3. **把 SDK 版同步回 `code/projects/all_code/td1601_baozi`**
   目前交付包里那份是合并前旧快照（`uart_hal_poll()` 无调用点，
   卡片 UI 整链未接线），会误导接手人。

---

## 编译方式（本机实测可用）

```bash
export PATH="/d/Baozi/CDK/CDKRepo/Toolchain/XTGccElfNewlib/V3.4.0/R/bin:/d/Coding/VSCODE/mingw64/bin:$PATH"
cd ".../projects/baozi"
mingw32-make -f Baozi.mk -j4
```

末尾 `crc32` 与 `mv` 两步会报 `Error 2` / `Error 1 (ignored)`，**已知无害**：
ELF 已正确产出，烧录用 CDK 的 Download 按钮。

当前状态：`text 96132 / data 644 / bss 48656`（SRAM 192 KB，余量充足）。
