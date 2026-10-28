# BaoZi 桌面机器人 · TD1601 工程总览（唯一入口文档）

> **位置**：`UserFolder/doc/README.md` —— **本文件是工程唯一入口**，新会话先读这一份。
> **面向**：接手继续开发 / 联调的人（尤其是新开的会话、其它 agent）
> **工程**：`projects/baozi`（TD1601 EVB · 桌面机器人赛道项目）
> **更新**：2026-09

> 📌 **文档分工约定（2026-09 整理）**：**工程级内容只写在本文件**（目录结构、编译规则、约定、串口协议、
> 联调步骤、待办）；**模块专有细节留在各模块自己的 `README.md`**（参数含义、算法、怎么加动作/变体、
> 该模块的调试步骤）。本文件 §3 给出每个模块的**函数接口摘要**与**该模块 README 的位置**。

---

## 0. 30 秒速览

| 项 | 现状 |
|---|---|
| **定位** | TD1601 = 机器人的**行为层**（情绪 / 动作 / 传感器）；PC = **认知层**（ASR / LLM / TTS）|
| **工程入口** | `src/main.c` —— `#define TEST_ENTRY 3` → **`emotion_fsm_run()`**（正式状态机：**舵机 + 屏幕表情 + 红外跟随**）|
| **切换入口** | 改 `main.c` 里的 `TEST_ENTRY`：0=参数化脸 / 1=位图管线 / 2=四面体 / **3=情感状态机（当前）** |
| **串口** | console `UART0` = **PA17/PA18，115200-8N1**；**输出一律纯 ASCII 英文** |
| **舵机** | YAW = **PA29**（csi_ch 3，中位 1000 / 限位 300~1700，2026-09 由 PA0 换脚）；PITCH = **PA27**（csi_ch 2，中位 1100 / 限位 800~1600）|
| **红外** | L = **PB22** / C = **PB23** / R = **PB29**；未遮挡 = 1 |
| **已实机验证** | 两路舵机 · 动作库（8 状态 × 3 变体）· 红外三路 + 状态解码 · **YAW 单轴跟随（稳定）** · LCD 480×480 |
| **刚完成待上机** | 情感状态机（`emotion/`）· 串口交互测试入口 · 双轴跟随（PITCH 方向待整定）· 变体池全铺开 |
| **下一站** | 触摸 / MPU6050 接进状态机 · 表情内容扩充（`anim/`）· LED · ESP32 语音模块接进 UART3 |
| **未实现** | 触摸、MPU6050 敲击、ESP32 语音模块通信、LED |

---

## 1. 工程意图（为什么这样分层）

赛道评分核心是**体现 TD1601 作为机器人核心控制器**：PC 只负责"听见 / 思考 / 说话"，
所有**行为决策**（什么情绪、做什么动作、传感器怎么响应）都在 TD1601 上跑。

```
        PC 侧（Baozi_PC，Python）                     TD1601 侧（本工程，C）
  ┌──────────────────────────────┐            ┌──────────────────────────────────┐
  │ baozis_ear  ASR              │            │  emotion/emotion_fsm.c  编排层    │
  │ brain       LLM → 情绪/意图   │  语义下发   │   ├─ 解析 EMO:/INTENT:            │
  │ baozis_mouth TTS             │  EMO:HAPPY  │   ├─ (心情,情绪) → 选动作          │
  │ communication.py             │ ─────────► │   └─ 连续状态 → 调制幅度/速度     │
  └──────────────────────────────┘  UART0     └───────────┬──────────────────────┘
                                     115200               ▼
                                             movements/ 动作库（9 状态 · 24 变体）
                                                          ▼
                                             servo_test/servo_core.c（限位 + PWM）
                                                          ▼
                                                    YAW(PA29) / PITCH(PA27)

  另一条独立链路：tracking/ 红外跟随（PB22/23/29 → 舵机），与动作库**互斥**（§7.3）
```

| 层 | 目录 | 职责 | 不负责 |
|---|---|---|---|
| 编排 | `UserFolder/emotion/` | 协议解析、心情降级、选动作、调制参数 | 具体轨迹、PWM 细节 |
| 动作 | `UserFolder/movements/` | 每个状态的轨迹（多段长链 + 变体池随机）| 何时播、播多久 |
| 控制 | `UserFolder/servo_test/servo_core.c` | 限位钳制、脉宽 → PWM 寄存器 | 动作语义 |
| 跟随 | `UserFolder/tracking/` | 红外闭环驱动舵机（模式互斥的另一半）| 情绪 |
| 屏幕 | `UserFolder/gfx/`、`LCD.c`、`src/video.c` | 图形 / 文字 / 刷屏 | 情绪（尚未接入）|

---

## 2. 目录结构与【必须知道的一条规则】

> ⚠️ **只有 `package.yaml` 的 `source_file` 里登记的文件才会被编译。**
> 新写 `.c` 必须去登记，否则现象是"函数找不到"或"改了没反应"。

当前登记（**29 项**）：`./*.c` · `src/*.c` · `UserFolder/test.c` ·
`servo_test/{servo_core,servo_range_test,servo_mvp_test,servo_dual_test}.c` ·
`movements/{moves,moves_idle,moves_happy,moves_eight,moves_sad,moves_angry,moves_surprised,moves_loving,moves_bored,moves_sleep,moves_demo}.c` ·
`emotion/{emotion_manager,mood_fsm,emotion_fsm}.c` ·
`test/{ir_test,ir_state_test,emotion_test}.c` ·
`tracking/{track_yaw,track_dual}.c` · `gfx/{gfx,gfx_demo,gfx_sprite_demo}.c`

| 目录 / 文件 | 作用 | 是否编译 |
|---|---|---|
| `src/` | `main.c`（**入口** + "测试入口切换区" + `HAL_Delay`）、`board_init.c`（console/UART0）、`video.c`（刷屏底层） | ✅ `src/*.c` |
| `UserFolder/doc/` | **本文件** + `io_plan.md`（引脚权威）+ `pin_mux_table.md`（复用表）+ `ir_tracking_plan.md`（跟随方案） | ❌ 纯文档 |
| `UserFolder/servo_test/` | 舵机：`servo_core.{h,c}`（控制层）· `servo_pins.h`（引脚/通道）· 三个测试 | ✅ 4 个 `.c` |
| `UserFolder/movements/` | **动作库**：`moves.{h,c}`（引擎 + 变体池 + 调制增益 + tick 钩子）+ 9 个状态文件 + `moves_demo.c` | ✅ 全部 |
| `UserFolder/emotion/` | **情感状态机三层**：`emotion_manager` / `mood_fsm` / `emotion_fsm` | ✅ 三个 `.c` |
| `UserFolder/tracking/` | 跟随：`track_yaw.{h,c}`（单轴，已验证）· `track_dual.{h,c}`（双轴） | ✅ 两个 `.c` |
| `UserFolder/test/` | `ir_test.c`（电平）· `ir_state_test.c`（状态解码）· `emotion_test.c`（**串口交互测试**）| ✅ 三个 `.c` |
| `UserFolder/gfx/` | 图形库 `gfx.{h,c}` + 两个 demo + `tools/png2c.py` | ✅ 三个 `.c` |
| `UserFolder/QSPI_test/` | 13 个屏幕实验文件（历史）| ❌ **不编译** |
| `UserFolder/test.c` / `lcd_test.c` / `LCD.c` / `FONT.h` | 早期屏幕测试与字体 | 仅 `test.c` ✅ |
| 工程根 `./*.c` | `chip_config.c` 等板级配置 | ✅ |

---

## 3. 模块状态与函数接口

> 这里给的是**接口摘要**；每个模块的**细节**（参数含义、算法推导、调试步骤）在各自的 `README.md`（§10 索引）。

### 3.1 `servo_test/servo_core` —— 舵机控制层（**唯一该直接写舵机的地方**）

```c
int  servo_core_init(void);                         /* 引脚 + PWM + compare 校准 + 两路回中位 + 开输出 */
int  servo_set_pulse(uint32_t ch, uint32_t pulse);   /* 限位钳制 + 【只写 compare】*/
/* 通道 SERVO_YAW_CH(3) / SERVO_PITCH_CH(2)；中位与限位宏见 servo_core.h */
```

- ✅ 实测中位/行程：YAW **1000 / 300~1700us**、PITCH **1100 / 800~1600us**；软限位由 `servo_set_pulse()` 强制。
- ★ **2026-09 输出方式改为"只写 compare"**：不再每步 `stop→config→start`（那样每步都关一次输出使能，
  两轴关断时刻不同 → 8 字这类两轴同时运动的动作看着"不同步/有时延"）。
- **`csi_ch = pinmap channel ÷ 2`**（不是引脚手册的通道号）；同 `csi_ch` 的两个引脚不能当两路独立 PWM。
- 换脚只改 `servo_pins.h`。细节见 `servo_test/README.md`。

### 3.2 `movements/` —— 动作库（9 状态 · 24 变体）

```c
/* 生命周期 */
int  moves_init(void);                              /* 内部会调 servo_core_init() */
const char *moves_name(move_id_t id);
void moves_play_once(move_id_t id);                 /* 固定播主变体 */
void moves_interrupt(void);  int moves_interrupted(void);

/* 变体池：同一状态多个动作，随机呼出（不与上次相同）*/
void     moves_play_variant(move_id_t id);              /* ★ 正式状态机用这个 */
void     moves_play_variant_at(move_id_t id, uint8_t n);/* 固定播第 n 个（1..N，调参用）*/
uint8_t  moves_variant_count(move_id_t id);
uint8_t  moves_last_variant(move_id_t id);
uint32_t moves_rand(void);

/* 调制（连续情绪状态 → 动作幅度/速度）与播放期钩子 */
void moves_set_gain(uint16_t amp_permille, uint16_t spd_permille); /* 1000 = 名义值；输出还乘 MOVE_AMP_BASE(1200) */
void moves_set_tick_hook(void (*fn)(void));            /* 每 20ms 插值步回调一次（状态机靠它收命令）*/

/* 轨迹原语（写 moves_xxx.c 时用；偏移量都是"相对中位的 us"） */
void move_line(y0,p0,y1,p1,ms); void move_to(y,p,ms); void move_by(dy,dp,ms);
void move_figure8(yaw_amp,pitch_amp,ms_per_lap,laps);
void move_center(ms); void move_hold(ms);
int16_t move_yaw_off(void); int16_t move_pitch_off(void);
```

| 状态 ID | 文件 | 变体 | 说明 |
|---|---|---|---|
| `MOVE_IDLE` | `moves_idle.c` | 3 | 缓慢环视 / 远望俯视 / 扁 8 字巡游（≈18~23s）|
| `MOVE_HAPPY` | `moves_happy.c` | 3 | 欢快摇晃 / 点头雀跃 / 绕圈庆祝 |
| `MOVE_EIGHT` | `moves_eight.c` | 1 | 画横 8 字（**暂不做变体**：有堵转待定位，见 §9）|
| `MOVE_SAD` | `moves_sad.c` | 3 | 垂头丧气 / 长叹 / 无力摇头 |
| `MOVE_ANGRY` | `moves_angry.c` | 3 | 甩头 / 瞪着你 / 生闷气 |
| `MOVE_SURPRISED` | `moves_surprised.c` | 3 | 猛抬头 / 吓一跳 / 疑惑定住 |
| `MOVE_LOVING` | `moves_loving.c` | 3 | 温柔歪头 / 轻轻蹭 / 慢慢看你 |
| `MOVE_BORED` | `moves_bored.c` | 3 | 歪头发呆 / 垂头发呆 / 东倒西歪 |
| `MOVE_SLEEP` | `moves_sleep.c` | 3 | 垂头呼吸 / 侧头睡 / 浅眠 |

两条经验（写动作必读）：**需求速度 ≤5us/ms + 每个极点 40ms 停顿**（否则舵机没到位就反向、摆幅被削平）；
参数位置约定：**主变体①在 `moves.h`，变体②③在各自动作文件顶部**。细节见 `movements/README.md`。

### 3.3 `emotion/` —— 情感状态机（三层，对应 PC 的 `emotion/`）

```c
/* emotion_manager.h —— 离散情绪 → 连续状态（happiness/energy/curiosity，千分制）*/
emo_id_t emo_parse(const char *name);  void emo_apply(emo_id_t e);
emo_id_t emo_current(void);  emo_state_t emo_state(void);  void emo_reset(void);

/* mood_fsm.h —— 心情状态机（ACTIVE/IDLE/BORED/SLEEPING；20s 无聊 / 30s 睡觉）*/
void mood_fsm_init(void);  void mood_fsm_touch(void);  mood_id_t mood_fsm_tick(uint32_t now_ms);
void mood_fsm_force(mood_id_t m);  void mood_fsm_wake(void);
mood_id_t mood_fsm_mood(void);  uint32_t mood_fsm_idle_ms(void);
void mood_fsm_set_thresholds(uint32_t bored_ms, uint32_t sleep_ms);   /* 测试用：缩短等待 */

/* emotion_fsm.h —— 编排层 */
int  emotion_fsm_init(void);   void emotion_fsm_step(void);   void emotion_fsm_run(void);
/* 供测试/其它入口复用（同一份协议解析与算法）*/
void      emotion_fsm_feed_line(const char *line);
move_id_t emotion_fsm_move_for(mood_id_t mood, emo_id_t emo);
void      emotion_fsm_apply_gain(mood_id_t mood, emo_id_t emo);
```

**舵机活动算法（两层）**：语义层 `emotion_fsm_move_for()`（心情优先于情绪：SLEEPING→sleep、BORED→bored，
其余按情绪映射）；强度层 `emotion_fsm_apply_gain()`（`amp = 700 + 500×(happiness+energy)/2/1000`、
`spd = 700 + 700×energy/1000`；BORED 再 ×0.7；SLEEPING 固定 amp1000/spd600）。
输入/输出规划见 §7.4。

### 3.4 `tracking/` —— 红外跟随（与动作库互斥）

```c
void track_yaw_test(void);        /* 单轴：初始化 + 死循环自测（★ 未拆成 init/step/exit，接状态机前需改造）*/
void     track_dual_init(void);   /* 双轴：引脚 + 舵机 + 复位状态（会调 servo_core_init）*/
void     track_dual_step(void);   /* 推进一次；调用方每 TD_SAMPLE_MS(5ms) 调一次，非阻塞 */
void     track_dual_exit(void);   /* 两轴回中 + 复位状态 */
uint32_t track_dual_lost_ms(void);/* 无目标持续时长（显示/调试用）*/
uint8_t  track_dual_sweep_done(void);/* ★ 丢失 5s 后自动扫一圈；扫完仍无目标 -> 退出跟随 */
void     track_dual_test(void);   /* init + 死循环 step */
```

判定规则：YAW 只看到单侧才转；PITCH 见 C（且非全覆盖）→ 抬头、见 L+R 而 C 没看到 → 低头。
节拍：5ms 采样 → 60ms 去抖 → 100ms 走一步 → 丢失分档（200ms 保持 / 1s 半速搜 / 超时吸附回中）。
细节（含二维判定表、方向整定三种改法）见 `tracking/README.md`。

### 3.5 屏幕与表情（三层：模型 / 动作 / 渲染）

**所有权**：屏幕归 `ui_app.c`（表情态 + 触摸卡片层），表情渲染由 `anim/face_app.c` 负责。
`emotion_fsm` 只调 `face_ui_*`（`anim/face_ui_card.c` 转发），从不直接碰屏。

```
pose（姿势）= 模型 + 动作                            anim/face_app.c    g_poses[]
   │                                                anim/face_model.c  g_models[]（长什么样）
   ├── face_model_build()  → 几何部件               anim/face_action.c 关键段表（怎么随时间变）
   └── face_show_parts()   → 屏幕（整屏 60 段 DMA）  anim/face_render.c
```

- **模型**（`face_model.c`）：左眼 / 右眼 / 嘴 + ≤4 个装饰，纯数据。现有 12 个：
  `neutral happy sad angry surprised loving loving_kiss loving_heart asleep think pet look`。
  其中 `loving_kiss`（参考 😗：拱形闭眼 + 嘟嘴）与 `loving_heart`（参考 😍：爱心眼 + 大笑嘴）
  是 2026-11 替换掉"原 loving/pet 眯眼椭圆"的两张新脸。
- **动作**（`face_action.c`）：关键段序列（滑行时长 / 保持时长 / 目标视线 / 闭眼量 /
  眼睛大小 / 张嘴 / 是否到位眨一下），按 tick 插值。现有 7 个：
  `idle`（慢巡视）、`look_around`（左看右看，到位眨眼）、`think`（视线在右上/左上漂）、
  `kiss`（嘟嘴一张一合）、`heart`（eye_scale 心跳两下）、`pet`/`wait`（旧版/柔和对视）。
- **变体池**：`g_poses[]` 里**同一个名字连续写多行** = 一个变体池，呼出时随机挑一个、
  且不与上次相同 —— `loving` 与 `pet` 各挂 2 个变体（kiss / heart），同一句 `EMO:LOVING`
  不会每次都长一个样（消除机械感）。单行 = 单一变体，行为与以前一致。
- **渲染**（`face_render.c`）：7 种形状（含 2026-11 新增的 `FS_HEART` 心形）+ 2×2 子采样
  抗锯齿，零位图零解压；整屏 480×480 分 60 段 DMA，段间用 `face_set_bus_callback()`
  放行舵机写 PWM（防白块）。

**表情切换**是 300ms 参数过渡（非阻塞状态机，起点取 `face_get_base()` 防叠加两次），
不是硬切；重绘节流 50ms/帧（整屏一帧 ≈20ms，跟着 20ms 刷会把 SPI 跑满 → 撕裂白块）。

**加一个新表情 = 改三张表**（逻辑不动）：
1. `face_model.c` 的 `g_models[]` 加一条（长什么样；跑 `_probe/preview_model.py` 看图）；
2. `face_action.c` 加一张关键段表并在 `g_acts[]` 登记（想让它会动）；
3. `face_app.c` 的 `g_poses[]` 加一行把两者绑起来（同名多行 = 变体池，随机播放）。

改完在 PC 上自检名字引用 / 幅度 / 时间轴：`pwsh -File D:\Baozi\_probe\check_actions.ps1`
想直接看效果（不用烧录）：双击 `D:\Baozi\_probe\face_sim.html` —— 它逐函数复刻了
`face_render.c / face_model.c / face_action.c / face_app.c`，点按钮就能切 13 个姿势、
看动作轨迹曲线；改了 C 之后跑 `pwsh -File D:\Baozi\_probe\face_sim_build.ps1` 重新抽取数据。

**数据通路**：`INTENT:THINK` → `emotion_fsm`（心情 > 意图 > 情绪，三级优先）→
`face_ui_intent()` → `ui_app_set_emotion()` → `face_app_try_emotion()` → 模型 + 动作 + 过渡。

引脚：`PA3=SCK`、`PB4~PB7=D0~D3`、`PA2=CS`、`PB10=WAIT`、`PB15=RST`、`PA31=BLK`。
性能实测：全屏约 20ms（≈50fps 上限）、局部刷新几十~几百 us → **动画只画变化区域**。

```c
/* gfx.h（图元/精灵，卡片层用；480×480、RGB565）*/
void gfx_init(void); void gfx_clear(uint16_t color);
void gfx_fill_rect(x,y,w,h,color);  void gfx_fill_circle(cx,cy,r,color,bg);
/* UserFolder/ui.h：分片刷屏（ui_flush_begin / ui_flush_step / ui_flush_busy）*/
/* include/video.h（官方底层）：video_init / video_deinit / video_flip */
/* UserFolder/LCD.h（早期 ST7701S 驱动）：LCD_Init / LCD_Clear / LCD_Fill / LCD_ShowString / ... */
```


### 3.6 传感器测试

```c
void ir_test(void);        /* 三路红外电平（验证接线/极性）*/
void ir_state_test(void);  /* 三路状态解码（去抖 + 方位判定，不驱动舵机）*/
```

### 3.7 其它测试入口

```c
void servo_range_test(void);   /* 舵机标定：0/1 选通道、+/- 步进、w<us> 直设、z 标中位、l<min>,<max> 限位、i 打印 */
void servo_mvp_test(void);     /* 单路三模式（官方方波 / SPWM 正弦 / GPIO 1Hz）—— 怀疑引脚或测量时用 */
void servo_dual_test(void);    /* 双路回归（控制层 + 限位 + 独立性）*/
void moves_demo(void);         /* 动作库单动作演示（改 DEMO_MOVE 切换）*/
void gfx_demo(void);  void gfx_sprite_demo(void);   /* 图形 / 精灵 demo */
void test_lcd(void);           /* ST7701S 屏测试 */
```

### 3.8 入口切换（`src/main.c` 的"测试入口切换区"）

```c
int main(void) { board_init(); emotion_test(); /* ← 联调时改这一行 */ return 0; }
```

| 想做什么 | 换成 |
|---|---|
| **联调：串口逐个看动作 + 验证情感状态** | `emotion_test()`（当前）|
| **正式运行（接 PC）** | `emotion_fsm_run()` |
| 校准舵机中位/限位 | `servo_range_test()` |
| 看红外 | `ir_test()` / `ir_state_test()` |
| 跟随 | `track_yaw_test()` / `track_dual_test()` |
| 屏幕 | `gfx_demo()` / `gfx_sprite_demo()` / `test_lcd()` |

---

## 4. 串口协议与调试命令

### 4.1 PC → TD1601（语义下发，正式运行用）

```
EMO:HAPPY | SAD | ANGRY | SURPRISED | LOVING | NEUTRAL     ← 情绪
EMO:IDLE | BORED | SLEEPING                                ← 心情
INTENT:CHAT | GREETING | QUESTION | COMMAND | NONE          ← 意图（记录 + 视为一次交互）
TRACK:ON | TRACK:OFF                                       ← ★ 红外跟随模式（舵机交给 track_dual，
                                                             屏幕继续显示表情；发 EMO:/INTENT: 会自动退出）
```

- 与 `Baozi_PC/communication.py` 的 `EMOTION_MAP` 一致；不认识的情绪 → 归一化为 `neutral`。
- 收到任意 `EMO:`/`INTENT:` 都会**重置空闲计时**（打断无聊/睡眠 → ACTIVE）。
- ⚠️ 行结束只能用 **CR/LF**（空格是参数分隔符；历史上把空格当行尾，导致所有带参数命令失效）。

### 4.2 `emotion_test()` 的命令（联调主力）

| 命令 | 作用 |
|---|---|
| `help` / `list` | 命令表 / 动作清单（含每个状态的变体数）|
| `next` / `play <id> [变体1..3]` / `repeat` | 动作巡览（`play 0` 随机、`play 0 2` 固定第 2 个变体）|
| `pause` / `center` | 暂停（保姿态）/ 回中并暂停 |
| `gain [amp spd]` | 手动调制幅度/速度（千分比；`gain 1000 500` 放慢一倍，可诊断"没到位"）|
| `mode [manual\|fsm]` | 手动指定动作 / 按情感状态自动选 |
| `thr <bored_s> <sleep_s>` | 临时缩短空闲降级阈值（几秒看完 20s/30s 链路）|
| `state` | 打印模式/动作/变体/增益/情绪/心情/空闲/阈值 |
| `EMO:...` / 裸情绪名 | 走正式协议（自动切 fsm 模式）|

该文件头部还有一份**「改动验证清单」**（七条：这版有没有编进去、三条待机变体、随机不重复、幅度对比…）。

### 4.3 正式入口 `emotion_fsm_run()` 的命令（当前 `TEST_ENTRY=3`）

开机自动打印 help。协议同 §4.1，另有跟随模式命令 —— **这一档是"一件事全测"的入口**：

| 命令 | 作用 |
|---|---|
| `EMO:HAPPY\|SAD\|ANGRY\|SURPRISED\|LOVING\|NEUTRAL` | 情绪 → **舵机动作 + 屏幕表情**（会自动退出跟随）|
| `EMO:IDLE\|BORED\|SLEEPING` | 心情（表情侧：sleeping→闭眼睡、bored→降级 neutral）|
| `INTENT:CHAT\|...` | 记录意图 + 视为一次交互 |
| **`TRACK:ON` / `TRACK:OFF`** | **进 / 退红外跟随**（舵机交给 `track_dual`，屏幕继续跑表情）|
| `TRK:SHOW` / `TRK:DEF` | 查看当前跟随手感参数 / 恢复编译默认 |
| `TRK:SPD <yaw> <pitch>` | 追踪速度（每拍位移 us，默认 4 / 3）|
| `TRK:ACC <us>` | 加速度（每拍速度增量，默认 2；0→满速两拍）|
| `TRK:GLIDE <yaw> <pitch> <ticks>` | **死区滑行量 / 拍数**（默认 28 / 22 us、14 拍）—— "再走 2~3° 减速收尾"就是它 |
| `TRK:TICK <ms>` | 控制周期（默认 25ms，细分 4 倍；越小越细腻）|
| `track` / `follow` / `stop` | 同上（裸名，本机串口助手调试用）|
| `state` | 打印 `mode=FSM\|TRACK` + 情绪/心情/动作/增益；TRACK 时另打一行 `lost=...ms` |
| `help` | 命令表 |

跟随模式行为：进跟随 → 两轴回中 → 朝目标缓慢跟（每 100ms 一步、15us/步）→
丢失 200ms 保持 / 1s 半速搜 / 缓慢回中 → **丢失 5s 后主动"扫一圈"找目标** →
一圈扫完仍没找到才退出跟随、交还动作库（扫视途中瞄到目标会立刻恢复跟踪）。

---

## 5. 写代码前必读的约定

1. **串口统一 115200-8N1**（console = UART0 = PA17/PA18，**禁止复用**）。
2. **`csi_ch = pinmap channel ÷ 2`**，不是引脚手册上的通道号（PA29→**3**；PA27→**2**）。
   同 `csi_ch` 的两个引脚共享 load 寄存器，**不能当两路独立 PWM**（如 PA29/PA30 同属 csi 3）。
3. **舵机方向**：脉宽增大 = YAW **右转** / PITCH **抬头**。
4. **红外极性**：未遮挡 = **1**（高），遮挡 = **0**（低）；代码里 `seen = !raw`。
5. **TD1601 的 IO 是 1.8V 域** —— 接 3.3V/5V 模块必须分压或电平转换。
6. **引脚占用以 `doc/io_plan.md` 为准**（每行都标了代码出处）；复用功能查 `doc/pin_mux_table.md`。
7. `printf` **不要用浮点**（minilibc 的 `%.1f` 不可靠），用整数。
8. **串口输出一律纯 ASCII 英文** —— 串口终端没有 UTF-8 渲染能力，中文发出去就是乱码。
   `printf`/`puts` 的**字符串字面量只写英文**（连状态表里的名字、提示语也一样）；
   **中文只放在注释里**（注释不上串口，随便写）。
9. **只有 `package.yaml` 的 `source_file` 登记的文件才会编译**（§2）。

---

## 6. 已踩过的坑（别重犯）

| 坑 | 现象 | 结论 |
|---|---|---|
| **接线接触不良** | 叠加态读不到、未遮挡脚高电平被拖到 0.8V、隔一会儿自愈 | **间歇性故障先查接线**（杜邦线/排针/面包板），别先怀疑固件或几何。曾误判成"探头间距不匹配" |
| `pinmux.c` PORTB 缺陷 | PB16~PB31 的**非 GPIO**（func1~3）功能配不上（写进了 IOMUX0L）；GPIO 模式还依赖 UB 移位侥幸正确 | **PORTB 上只有 func0 的外设可靠**；改 PB 引脚后要回读寄存器验证 |
| 回中震荡 | 丢失后回中，在中位两侧每 100ms 来回跳（1001↔996） | 回中必须**吸附**到中位（`track_yaw.c` / `track_dual.c` 已修） |
| `servo_set_pulse` 内部 `stop→config→start`（历史） | 每步都关一下输出使能再打开；两路关的时刻不同 → 8 字这类两轴同时运动的动作看着"不同步/有时延"（脉冲还可能被截断） | **2026-09 改为"只写 compare"**：`servo_core_init()` 里校准 + `start` 一次，之后只写 compare_a/b（不碰 load、不关输出）|
| 串口把**空格**当行尾（历史） | `gain 1200 1200` / `play 0 2` 等带参数命令全部失效（参数被切成独立命令）| 行结束**只能用 CR/LF**，空格是参数分隔符 |
| 快速往复动作"没到位就反向" | HAPPY/ANGRY 实际摆幅比名义小很多 | 舵机开环，段太短会削幅：**需求速度 ≤5us/ms + 极点 `move_hold(30~60ms)`**；自诊断：`gain 1000 500` 放慢一倍再看 |
| 舵机无位置/电流反馈 | **软件无法判堵转** | 靠电源限流 + 软限位 + 机械留余量 |
| PA0/PA1 pad 层联动（历史） | 曾以为短路，实为**同属 `csi_ch 0`** | 换新主板后 PA1 可用，但**同 `csi_ch` 仍不能双路** |

---

## 7. 情感状态机与"跟随模式"（**均已落地**）

### 7.1 现状

| 部分 | 位置 | 状态 |
|---|---|---|
| 情绪 + 连续状态 | `emotion/emotion_manager.{h,c}` | ✅ 与 PC `EmotionManager` 逐项一致（千分制）|
| 心情状态机 | `emotion/mood_fsm.{h,c}` | ✅ ACTIVE/IDLE/BORED/SLEEPING，20s/30s 降级（非阻塞 tick）|
| 编排层 | `emotion/emotion_fsm.{h,c}` | ✅ `EMO:`/`INTENT:` 解析 + (mood,emo)→动作 + 调制算法 |
| 动作执行 | `movements/` | ✅ 9 状态 · 24 变体（随机不重复）|
| ~~旧 `test/emotion.c`~~ | — | **已删除**（枚举/串口切换升级为 `emotion/`；动作表迁入 `movements/`）|

### 7.2 动作库的三个关键能力（状态机依赖它们）

- **变体池**：`moves_play_variant()` 随机呼出（不与上次相同）→ 消除"同一动作循环"的机械感；
- **调制增益**：`moves_set_gain(amp, spd)` 由连续情绪状态实时给出（另有整体基准 `MOVE_AMP_BASE=1200`）；
- **播放期 tick 钩子**：`moves_set_tick_hook()` 每 20ms 回调 → **动作播放期间也能收串口命令并立即打断**
  （这是"长动作 + 随时响应"能同时成立的原因）。

### 7.3 舵机冲突的解法：跟随是状态机的一个「模式」（**已实现**，2026-10）

`track_yaw.c` / `track_dual.c` / `movements/moves.c` **都直接调 `servo_set_pulse()`**，不能并行 ——
但不需要复杂的运行时仲裁：把**红外跟随做成情感状态机的一个互动模式**，由指令呼起，模式之间天然互斥。

```
        指令 / 触摸 / 语音 / 定时
                  │
                  ▼
        ┌─────────────────────┐
        │   情感状态机 (FSM)   │   任一时刻只有【一个】模式在写舵机
        └──────┬───────┬──────┘
               ▼       ▼
      MODE_EMOTION   MODE_TRACK
   (movements/ 播     (track_dual
    情绪动作)          驱动两轴)
               └──► 舵机 ◄──┘
```

```c
moves_interrupt();                        /* ① 先停动作库（释放舵机）*/

track_dual_init();                        /* ② 进：引脚 + 舵机 + 复位内部状态（两轴回中）*/
/* ③ 跟随中（每 5ms 一拍）：track_dual_step() —— 采样/去抖/每 100ms 走一步，非阻塞
 *                         face_ui_tick()（每 20ms）—— 屏幕照常显示表情，不罢工 */
track_dual_exit();                        /* ④ 退：两轴回中位，交还舵机控制权 */
```

| 命令 | 作用 |
|---|---|
| `TRACK:ON`（或 `TRACK:START` / `TRACK:1`）| 进入跟随（`moves_interrupt()` → `track_dual_init()`，节拍切 5ms）|
| `TRACK:OFF`（或 `TRACK:0`）| 退出跟随（`track_dual_exit()` 两轴回中 → 回 FSM 重选动作）|
| `track` / `follow` / `stop` | 同上（裸名调试）|
| `state` | `mode=FSM\|TRACK`；TRACK 时另打 `lost=...ms sweep=idle/scanning/DONE` + 当前手感参数 |

实现落点（`emotion_fsm.c`）：`s_mode` 模式变量 + `s_step_ms` 节拍变量；进跟随顺手
`mood_fsm_touch()`（跟随算一次互动，别一退出就进无聊/睡觉）；屏幕切到 `face_ui_emotion("surprised")`
（专注看着目标）。查询模式：`emotion_fsm_mode()`。

**★ 丝滑化（2026-10，`track_dual.c`）**：原来"每 100ms 跳 15us、进死区立刻停"，
观感一格一格 + 到位一顿。现在三件事一起上（细节见 `tracking/README.md` §3.8）：

1. **控制周期细分 4 倍**：100ms → 25ms，每拍位移等比减小（巡航速度不变）→ 台阶小 4 倍；
2. **速度斜坡**：起步/反向每拍只变 `TD_ACCEL_US`(2us)，0→满速两拍 → 不"弹射"；
3. **死区滑行尾巴**：目标居中不再急停，用 `TD_GLIDE_US`（YAW 28us ≈ 2.5°）分
   `TD_GLIDE_TICKS`(14) 拍**逐拍递减**走完 —— 即"再走 2~3° 减速停住"。

手感可现场调、不用重新烧录：`TRK:SPD` / `TRK:ACC` / `TRK:GLIDE` / `TRK:TICK` / `TRK:SHOW` / `TRK:DEF`。

退出条件（任一）：串口 `TRACK:OFF` / `stop` · **收到任何 `EMO:` / `INTENT:`**（语义是"要它做表情动作"）·
`track_dual_sweep_done()` —— 即：丢失 5s → 自动扫一圈 → **一圈扫完仍无目标**才退出。

⚠️ `track_yaw.c`（单轴回归基线）**没有**拆成 init/step/exit —— 要在状态机里用它，照 `track_dual.c` 拆一遍（见 `tracking/README.md` §2.7）。

### 7.4 输入 / 输出现状

- **输入**：串口 `EMO:` / `INTENT:` / **`TRACK:ON|OFF`**（均已实现）、触摸（未）、IMU 敲击（未）
- **输出**：动作库（舵机，已实现）· **屏幕表情 `anim/face_ui`（已接入：情绪/心情 → 表情，含过渡/眨眼/视线）**·
  LED / 语调（未）
- **总线仲裁（重要）**：屏幕 DMA 与舵机写 PWM 会抢 AHB → 已用
  `face_set_bus_callback(servo_bus_event)` 让"屏幕传输窗口内，舵机只登记不落地、段间空隙补写"，
  解决"舵机一动、屏幕就闪白块"
- 架构约束：**行为逻辑放 TD1601（行为层）**，PC 只下发音义状态

---

## 8. 联调 checklist（建议按顺序）

**阶段 0 · 环境**
1. CDK 打开 `projects/baozi` 编译烧录；串口 115200-8N1。
2. 确认串口输出是纯 ASCII（看到中文 = 烧的是旧版，或新文件没登记 `source_file`）。

**阶段 1 · 舵机（`servo_range_test()`）**
3. `w1000`/`w1100` 回中位、`w300`/`w1700`、`w800`/`w1600` 到两端 —— 确认行程与限位。
4. 脉宽与实际位置对不上 → 先怀疑接线/供电，再怀疑 compare 换算（§3.1）。

**阶段 2 · 传感器与跟随（`ir_test()` → `track_yaw_test()`）**
5. 三路电平：未遮挡=1、逐路遮挡看位序；**三路叠加态（LR/LC/CR/LCR）必须出现**，否则接线/几何有问题。
6. YAW 单轴跟随（回归基线）：手左→左转、手右→右转、正前→不动、移开→200ms 保持→1s 搜→回中。
7. 双轴（`track_dual_test()`）：确认 PITCH 方向，反了按 `tracking/README.md` §3.7 改一处。

**阶段 3 · 情感状态机（`emotion_test()`，当前入口）**
8. `list` 看 8 状态 × 3 变体；`play <id> <1..3>` 逐个过一遍动作观感。
9. `gain` 试幅度/速度手感（记住：实际输出还乘整体基准 1.2）。
10. `thr 3 6` → 等 4s 看自动降级 bored → sleeping；`EMO:SAD` 验证"睡眠中可被命令叫醒"。
11. `play 2` 看八字：**目前有堵转嫌疑**（§9），先别长时间跑。

**阶段 4 · PC 联调（`emotion_fsm_run()` + PC）**
12. `main()` 改调 `emotion_fsm_run()` 烧录；PC 侧 `python main.py --serial COMx`。
13. PC 说一句话 → 观察 `[fsm] emotion=... mood=... -> move=... (amp/spd)` 与舵机动作是否对应。
14. 断线/超时：板子 20s→无聊、30s→睡觉；PC 再下发 `EMO:` 应立即唤醒并换动作。

---

## 9. 未完成 / 待办与已知问题

- [ ] **八字（`MOVE_EIGHT`）堵转待定位**：指令层已排除（脉宽 700~1300 / 956~1244，速度仅 1.05us/ms）→
      优先查**供电带载**（两轴持续同时动、电流峰值最大）、机械干涉（用 `servo_range_test` 复现组合位姿）；
      定位前 `MOVE_EIGHT` 保持单实现、不加变体
- [ ] **情感状态机上机整定**：用 `test/emotion_test.c` 巡览动作并调参数（幅度/节奏在 `movements/moves.h`，
      调制公式在 `emotion/emotion_fsm.c`）
- [ ] **双轴跟随上机**：确认 PITCH 方向（三种整定改法见 `track_dual.c` 末尾），调 `TD_PITCH_STEP_US`
- [x] **红外跟随接入 `MODE_TRACK`**（2026-10 完成：串口 `TRACK:ON|OFF`，见 §7.3/§4.3）—— 待上机确认跟随手感与"5s + 扫一圈"退出
- [ ] 触摸 + MPU6050：先定 IIC0 引脚方案（`doc/io_plan.md` §2.2 给了三种解法）
- [ ] ESP32 UART3 通信：⚠️ 1.8V↔3.3V 电平转换
- [x] 屏幕表情接入状态机（2026-10 完成：`anim/face_ui`，情绪/心情 → 表情）
- [x] **表情内容扩充**（2026-11）：改成 `pose = 模型 + 动作` 三层（`anim/face_action.c` 时间轴引擎），
      新增 `think` / `pet` / `look` 三个**会动**的表情 + 300ms 表情过渡 + 意图表情优先
      （`INTENT:THINK/LISTEN/QUESTION/GREET`）。加新表情只改三张表，见 §3.5。
      自检：`pwsh -File D:\Baozi\_probe\check_actions.ps1`
- [x] **loving 一族重做 + 变体池**（2026-11）：参考 😗 / 😍 两张 emoji 换成
      `loving_kiss`（拱形闭眼 + 嘟嘴）与 `loving_heart`（新增 `FS_HEART` 心形眼 + 大笑嘴），
      并挂成 `loving` / `pet` 的 2 个变体随机播放（不与上次相同），消除"每次都长一个样"。
- [ ] **瞬时表情层**（下一步）：触摸驱动 ICNT8952BN 已在跑，缺的是"事件表情 + 到期回退"——
      即"被抚摸 → `pose pet` 保持 3s → 自动回到原表情"，以及长按/抚摸手势 → `TOUCH:HEAD`。
      当前 `face_ui_*` 只做"切到某姿势并保持到下次命令"。
- [ ] 接线可靠化：焊接 / 星形供电 / 去耦 / 电源限流 ≥500mA（见 `doc/ir_tracking_plan.md` §0.1）
- [ ] `track_yaw.c` 拆成 init/step/exit（要把它接进状态机时）
- [ ] `track_dual.c:242` 的 warning（`changed` set but not used）—— 与功能无关，顺手可清

---

## 10. 文档索引

| 文档 | 位置 | 内容 |
|---|---|---|
| **本文件** | `doc/README.md` | **工程唯一入口**：意图 / 目录与编译规则 / 接口摘要 / 协议 / 约定 / 坑 / 联调 / 待办 |
| 引脚分配（权威）| `doc/io_plan.md` | 按代码核实，每行带出处；空闲/不可用清单 |
| 引脚复用表 | `doc/pin_mux_table.md` | 全芯片复用功能、功能号编码、`pinmux.c` 的 PORTB 缺陷 |
| 红外跟随方案 | `doc/ir_tracking_plan.md` | 光学几何、二维状态表、控制律、验证步骤与进度 |
| 动作库 | `../movements/README.md` | 变体池、调制增益、动作清单、**怎么加动作/加变体** |
| 红外跟随 | `../tracking/README.md` | 两模块的公开与内部接口、判定表、**状态机集成**、方向整定 |
| 舵机调试/标定 | `../servo_test/README.md` | `csi_ch` 规则与三个坑、通路表、MVP/dual/range 三个测试、标定流程 |
| 图形库 | `../gfx/README.md`、`../gfx/HANDOVER.md` | gfx 用法与交接说明 |
| **表情动作交接** | `../anim/HANDOVER.md` | **pose = 模型 + 动作**：数据通路、加新表情三步、硬约束与坑、**已知问题（抚摸表情被情绪同步抢走）**、下一步候选 |
| **表情模拟器**（PC 端，不需烧录）| `D:\Baozi\_probe\face_sim.html` | 双击即用：13 个姿势点按钮就切，含动作轨迹曲线；数据由 `face_sim_build.ps1` 从 C 抽取，`node face_sim_test.js` 自测与 C 是否一致 |
| 表情数据自检 | `D:\Baozi\_probe\check_actions.ps1` | pose→model/action 引用、视线幅度、滑行速度、时间轴轨迹 |

---

## 11. 编译 / 校验环境（本机没有 `make` 时的用法）

| 项 | 值 |
|---|---|
| IDE / 构建 | CDK（`project.cdkproj` / `Baozi.mk` 由 CDK 生成）|
| 工具链 | `D:\Baozi\CDK\CDKRepo\Toolchain\XTGccElfNewlib\V3.4.0\R\bin\riscv64-unknown-elf-gcc.exe`（14.3.0）|
| 链接脚本 | `boards/td1601_evb/v1.0/gcc_flash.ld` |

**不改 IDE 也能做语法/符号校验**（在 `projects/baozi` 下执行，用于快速自检）：

```powershell
$G = "D:\Baozi\CDK\CDKRepo\Toolchain\XTGccElfNewlib\V3.4.0\R\bin\riscv64-unknown-elf-gcc.exe"
$SDK = "D:\Baozi\CDK\official_SDK\td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59\td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59\td1601_evb_v0.2.0_cdk"
$INC = "-I$SDK/components/chips/chip_td1601/v1.0/include -I$SDK/components/chips/chip_td1601/v1.0/sys " +
       "-I$SDK/components/console/v1.0 -I$SDK/components/csi/v1.0/include -I$SDK/components/csi/v1.0/include/core " +
       "-I$SDK/components/minilibc/v1.0/include -I$SDK/components/mm/v1.0/include " +
       "-I$SDK/boards/td1601_evb/v1.0/include -Iinclude -Iinclude/sys -IUserFolder"
$DEF = "-DUSE_CHIP_TD1601 -DUSE_CONSOLE -DUSE_CSI -DUSE_MINILIBC -DUSE_MM -DUSE_TD1601_EVB " +
       "-DCONFIG_CHIP_TD1601=1 -DCONFIG_CPU_E906FD=1 -DCONFIG_KERNEL_NONE=1 -DCONFIG_XIP=1 " +
       "-DCONFIG_DEBUG_MODE=1 -DCONFIG_ARCH_INTERRUPTSTACK=4096 -DCONFIG_SUPPORT_TSPEND=1 -DCONFIG_BENCHMARK=1"

# 例：只做语法检查（不产出 .o）。⚠️ Windows 下 gcc 不会自己展开 *，要检查的文件请显式列出
& $G -fsyntax-only -mcpu=e906fd -Wall -Os $DEF $INC `
     src/main.c `
     UserFolder/emotion/emotion_manager.c UserFolder/emotion/mood_fsm.c UserFolder/emotion/emotion_fsm.c `
     UserFolder/movements/moves.c UserFolder/movements/moves_idle.c

# 想一次检查某几个目录下的全部 .c，这样收集路径：
$SRC = (Get-ChildItem UserFolder/emotion/*.c, UserFolder/movements/*.c, UserFolder/tracking/*.c).FullName
& $G -fsyntax-only -mcpu=e906fd -Wall -Os $DEF $INC $SRC
```

> 说明：这只是**开发期自检**（能抓拼写/类型/未定义符号），正式产出仍走 CDK 编译烧录。
