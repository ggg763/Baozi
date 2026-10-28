# 合并说明：触摸 + 卡片 UI + 行为层（2026-11）

这份文档只记一件事：**合并后各文件的归属边界、编译入口、上机自检清单**。
写它是因为同一个工程里有两路并行改动（表情层扩展 / 触摸·UI·行为层），
不写清楚必然互相覆盖。

---

## 1. 所有权边界（改之前先看这里）

| 范围 | 文件 | 归谁 |
|---|---|---|
| **触摸驱动** | `src/ICNT8952BN.c` | 本线 |
| **UI 栈** | `UserFolder/ui.c` `ui.h` `ui_app.c` `ui_app.h` `pomo.c` `pomo.h` `play_card.c` `play_card.h` `app_core.c` `app_core.h` | 本线 |
| **数据卡片** | `src/habit_store.c` `src/sched_store.c` + `include/habit_store.h` `include/sched_store.h` | 本线 |
| **行为桥** | `UserFolder/behavior.c` `behavior.h` | 本线 |
| **串口协议栈** | `src/uart_hal.c` `src/uart_protocol.c` `src/baozi_proto_app.c` `src/uart_esp.c` + `include/uart_protocol.h` `include/baozi_hooks.h` `include/uart_esp.h` | 本线 |
| **情感状态机接口**（抑制/请求/刺激三条新接口） | `UserFolder/emotion/emotion_fsm.c` `emotion_fsm.h` `emotion_manager.c` `emotion_manager.h` | 本线（**新增接口向后兼容，别改签名**） |
| **入口** | `src/main.c`（`TEST_ENTRY` 切换区） | 共用，改前先说 |
| **表情层** | `UserFolder/anim/face_app.c` `face_model.*` `face_action.*` `face_render.*` `face_ui.h` `face_anims.h` | **另一路**（本线只调用，不改） |
| **表情层桥** | `UserFolder/anim/face_ui_card.c` | 本线（把 `face_ui_*` 接到 ui_app） |
| **舵机/动作/心情** | `UserFolder/servo_test/*` `movements/*` `emotion/mood_fsm.*` `tracking/*` | 另一路/既有，只调用 |

⚠️ 两边都要用到的三件事，改动前必须同步：

1. **UART0 只有一个接收者**：`src/uart_hal.c`（异步中断 + 字节级分流）。
   `emotion_fsm.c` 的 `poll_uart()` 由宏 `EMO_FSM_OWN_UART` 控制，默认 `0`（关闭）。
   **不要**再让任何模块自己 `csi_uart_receive()`，否则会与帧解析抢 FIFO。
2. **屏幕只有一个驱动者**：`UserFolder/ui_app.c`（表情态 + 卡片层）。
   `face_ui_*` 由 `anim/face_ui_card.c` 实现并转发给 `ui_app`；
   **`face_emo_test.c` 不参与编译**（它自带主循环 + 自己 poll 串口）。
3. **总线仲裁**：任何"整屏分段 DMA"的渲染端都要注册
   `face_set_bus_callback(servo_bus_event)`（表情层）与
   `ui_set_bus_callback(servo_bus_event)`（卡片层），否则舵机一动就闪白块。

---

## 2. 编译

- **`package.yaml` 的 `source_file` 是唯一权威清单**；`Baozi.mk` / `Obj/Baozi.txt`
  是 CDK 生成的产物（本次已手工同步，用 CDK 打开工程会重新生成）。
- 命令行（本机工具链路径，`build.bat` 里写的 `D:\C-Sky\...` 已失效）：

```powershell
$env:PATH='D:\Baozi\CDK\CDKRepo\Toolchain\XTGccElfNewlib\V3.4.0\R\bin;D:\Coding\VSCODE\mingw64\bin;' + $env:PATH
cd <工程目录>
mingw32-make -f Baozi.mk -j4          # 产出 Obj\Baozi.elf
```

- 末尾的 `mv`（转 bin）与 `crc32`（签名）两步在本机不可用，会报 `Error 1 (ignored)` /
  `Error 2`，**不影响 ELF 正确性**；烧录用 CDK 的 Download 按钮。
- 当前体积：`text 90.3KB / data 612B / bss 47KB`（SRAM 192KB，余量充足）。

---

## 3. 启动流程（TEST_ENTRY == 4，正式合并入口）

```
board_init()                      串口 console（UART0 115200-8N1）
 └ app_core_init()
    ├ ui_app_init()               屏幕 + 参数化脸（face_app_init）
    ├ icnt8952bn_init()           触摸（I2C0 PA21/PA22，INT PB2，RST PB1）
    ├ uart_hal_init()             UART0 异步接收（帧 + 文本行分流）
    └ uart_esp_init()             UART3 PB18/PB19（ESP32 情绪）
 ├ ui_set_bus_callback(servo_bus_event)   卡片层总线仲裁
 ├ emotion_fsm_init()             舵机 + 动作库 + 心情 + face_set_bus_callback + face_ui_init
 └ behavior_init()                红外近距（PB22/23/29）+ 注册抑制回调
主循环： emotion_fsm_step()  ↔  app_core_step()
        （moves 播放阻塞期间，由 moves 的 20ms 钩子调 face_ui_tick → app_core_step 保活）
```

---

## 4. 上机自检清单

按顺序做，每步都给了"该看到什么"：

1. **开机**：三色自检 → 米黄/白渐变的表情脸。
   串口应出现 `[app] touch ready` / `uart ready` / `esp uart ready` / `emotion + servo ready` / `[bhv] ir proximity ready`。
2. **点击响应**：在表情态点一下 → 串口 `[GEST] TAP @(x,y)`（`TP_DEBUG` 默认关，可临时开）。
3. **长按 800ms** → 进入卡片层（TODAY 页），应看到 4 个页码圆点。
   左右滑 → TIMER(番茄钟三入口) → HABIT → **PLAY（动作/情绪）**。
4. **摸脸**：回到表情态，用手指在屏幕**上半部**来回滑 4 次以上 →
   串口 `[UI] PET start` → 脸变 loving，并出现 `[fsm] stimulus kind=1 ...`
   抬手/停手 0.6s → `[UI] PET end`，脸回到当前情绪。
   ⚠️ 摸的时候**不会**跳到卡片层（摸不是长按）。
5. **安全抑制**：手指按住屏幕不动 → 脖子应**停止运动**（串口 `[fsm] inhibit ON`），
   松手 1 拍后 `[fsm] inhibit OFF`。把手贴近任一红外探头（非跟随模式）同样触发。
6. **PLAY 页**：点 NOD / SHAKE / WOW / CUDDLE → 串口 `[fsm] ui request move=...`，
   抬手后脖子按正式流程播一段；点 PRAISE / SCOLD → `[fsm] stimulus kind=2/3`
   （情绪只在连续状态攒够后漂移，不会一下跳到 happy/angry）。
7. **串口两条通路共用一根线**：
   - 二进制帧：`A5 LL CMD DATA XOR`（习惯 0x11~0x13 / 日程 0x14~0x16 / 提醒 0x17）
   - 文本命令：`EMO:HAPPY` / `INTENT:CHAT` / `TRACK:ON` / `help`
   两者可混发；出现 `[app] cmd: ...` 说明文本行被正确分流。
8. **红外跟随**（既有功能）：`TRACK:ON` 后舵机交给跟随；此时近距抑制自动豁免
   （否则"看到目标"会被当成"贴太近"而永远不动）。

---

## 5. 可调参数（上机微调用）

| 想要的效果 | 改哪里 |
|---|---|
| 摸几下才算"被摸" | `UserFolder/ui_app.c` 的 `PET_REVERSALS`（默认 4）|
| 抚摸区域（现在是屏幕上半） | `ui_app.c` 的 `PET_ZONE_Y`（默认 300）|
| 摸的时候多久给一次刺激 / 强度 | `PET_REPEAT_MS` / `PET_STIM_ONCE` / `PET_STIM_REPEAT` |
| 停手判定超时 | `PET_IDLE_MS`（默认 600ms）|
| 红外"贴太近"去抖次数 | `UserFolder/behavior.c` 的 `IR_NEAR_DEBOUNCE`（默认 3）|
| 跟随模式也要近距抑制 | `behavior.c` 的 `BEHAVIOR_NEAR_INHIBIT_IN_TRACK` 改 1 |
| 刺激对情绪的影响力 | `UserFolder/emotion/emotion_manager.c` 的 `emo_stimulus()` 增量表与漂移阈值 |
| 触摸 I2C 速率 / 阈值 | `src/ICNT8952BN.c` 的 `IIC_BUS_SPEED_FAST`、`SWIPE_THRESHOLD`（默认 30px）|

---

## 6. 已知取舍 / 风险

1. **`PA21/PA22` 复用为 I2C0**：这两个是 UART0 的 CTS/RTS。`board_init.c` 只配了
   TX=PA18/RX=PA17，未启用硬件流控，所以可以复用（同事的固件已实机跑通）；
   但**以后若有人给 console 打开流控，触摸会立刻失效**。
2. **红外只有阈值没有距离**：能区分"有没有东西贴近"，不能量化远近。
   所以"贴太近停手"只是保守保护；跟随模式下该保护关闭（见第 4 节第 8 条）。
3. **番茄钟页面仍是同步整屏刷**（`pomo.c` 用 `ui_flush()`）：计时界面刷新时约 76ms
   内触摸会排队。要改成分片，需把 pomo 的渲染改走 `ui_flush_begin/step`。
4. **触摸坐标未校准**：`ICNT8952BN` 报的坐标直接当视觉坐标用（当前屏幕方向已对齐）。
   若换屏/贴膜导致偏移，需要在 `ui_app_handle_gesture` 入口加一次线性校准。
5. **情绪刺激是"累积"模型**：`emo_stimulus()` 只改连续状态，情绪由阈值漂移；
   想让"夸一句立刻变开心"，把 `emotion_manager.c` 里 PRAISE 的增量调大或把
   漂移阈值调低 —— 但那样就回到了"直接改情绪"，与设计初衷相反。
