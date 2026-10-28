# tracking —— 红外跟随（桌面物品 / 手势追踪）

把"红外三路 → 舵机跟随"的代码集中在这里。两个模块，**共用一个硬件前提**，但成熟度不同：

| 文件 | 作用 | 状态 | 公开函数 |
|---|---|---|---|
| `track_yaw.c` / `.h` | **YAW 单轴跟随**（只驱动水平舵机）| ✅ **实机验证稳定** —— 留作回归基线 | 1 个 |
| `track_dual.c` / `.h` | **双轴跟随**（YAW + PITCH 同时）| 🟡 代码完成待上机；**PITCH 方向需现场整定** | 5 个 |

> 为什么要两个？单轴版先在实机上把"节拍/去抖/丢失分档"这套骨架验证稳了（用户确认"完美、非常稳定"），
> 双轴版在它基础上把 PITCH 加上、并按二维状态表**同时**驱动两轴 —— 结构更完整，接状态机也改造过了。

---

## 1. 共同前提（两个模块都一样）

**硬件**：红外三路 L / C / R，三路都是 **二值**（有/无目标），没有距离、没有误差幅值。

| 项 | 值 |
|---|---|
| 引脚 | L = **PB22**、C = **PB23**、R = **PB29**（都在 PORTB）。宏名：`TD_IR_LEFT_PIN` / `TD_IR_CENTER_PIN` / `TD_IR_RIGHT_PIN`（单轴版是 `TRK_IR_LEFT_PIN` / `TRK_IR_CENTER_PIN` / `TRK_IR_RIGHT_PIN`）；`TD_IR_ALL_PIN` / `TRK_IR_ALL_PIN` 是三路位或，用于 `csi_gpio_dir()` 设方向 |
| GPIO 控制器 | PORTB → `csi_gpio_init(idx = 1)`（宏 `TD_IR_GPIO_IDX` / `TRK_IR_GPIO_IDX`）|
| 极性 | **未遮挡 = 1（高）、遮挡 = 0（低）**；引脚配 `GPIO_MODE_PULLUP`，代码里 `seen = (~raw) & 0x07` |
| 位序 | bit2 = L、bit1 = C、bit0 = R；取值宏 `TD_SEEN_L(seen)` / `TD_SEEN_C(seen)` / `TD_SEEN_R(seen)`（单轴版 `TRK_SEEN_L` / `TRK_SEEN_C` / `TRK_SEEN_R`）|

**节拍（两个模块完全一致）**：

```
IR 采样 5ms  →  三路各自去抖 12 次（12×5ms = 60ms）  →  每 25ms 走一步  →  按需要打印
```

> `track_yaw.c`（单轴回归基线）仍是 100ms / 15us 的老节拍；**双轴版 2026-10 起改为
> 25ms 细分 + 速度斜坡 + 死区滑行**（丝滑化），见 §3.8。

**舵机**：YAW = PA29（csi_ch 3；2026-09 由 PA0 换脚）、PITCH = PA27（csi_ch 2）；**脉宽增大 = YAW 右转 / PITCH 抬头**。
两轴都走 `servo_test/servo_core.h` 的 `servo_set_pulse()`，所以自动享受实测限位钳制
（YAW 1000 / 300~1700、PITCH 1100 / 800~1600）。

> ⚠️ **与动作库（`movements/`）互斥**：`track_*.c` 和 `movements/moves.c` 都直接写舵机，
> 同一时刻只能有一个在跑。进跟随前先 `moves_interrupt()`，退出后交还动作库 ——
> 模式互斥是情感状态机的职责，方案见 `doc/README.md` §5.3。

**为什么不用 PID**：IR 只有二值，拿不到连续误差 → 用**步进爬坡**：每个控制周期朝误差方向走一小步，
配合去抖、限速、软限位。双轴版另外做了"到位不急停"—— **速度斜坡 + 死区滑行尾巴**（§3.8），
这正是"跟随时看着顺不顺"的关键。

---

## 2. `track_yaw` —— YAW 单轴跟随（回归基线）

### 2.1 公开接口

```c
/* track_yaw.h */
void track_yaw_test(void);      /* 自测入口：初始化 + 死循环，不返回（曾被 src/main.c 调用）*/
```

整个模块**只暴露这一个函数**，且它是死循环自测入口 —— 也就是说它**不是**为"被状态机推着走"设计的（见 §2.6）。

### 2.2 内部实现（`static`，外部不可见）

| 函数 | 签名 | 作用 |
|---|---|---|
| `ir_read_raw` | `uint8_t ir_read_raw(void)` | 读三路原始电平 → 位掩码（1 = 高 = 未遮挡）|
| `trk_decide` | `trk_dir_t trk_decide(uint8_t seen)` | 方位判定（**只用 L/R**，表见 §2.4）|
| `trk_clamp` | `int32_t trk_clamp(int32_t v)` | 软限位钳制到 `SERVO_YAW_MIN~MAX` |
| `trk_step_one` | `void trk_step_one(int32_t *yaw, trk_dir_t *last_dir, uint32_t *lost_ms, uint8_t seen)` | **一个控制周期**：按判定方向走一步 / 丢失分档处理，最后 `servo_set_pulse()` 下发 |

```c
/* 文件内枚举（不在 .h 里）*/
typedef enum { TRK_DIR_NONE, TRK_DIR_LEFT, TRK_DIR_RIGHT, TRK_DIR_STOP } trk_dir_t;
```

`trk_step_one()` 用**指针进出参**（`yaw` / `last_dir` / `lost_ms` 都是调用方的变量），
所以状态全在调用方手里 —— 这是它唯一"可复用"的地方。

### 2.3 配置宏（`track_yaw.h`，全部 `TRK_*`）

| 宏 | 默认值 | 含义 / 调参方向 |
|---|---|---|
| `TRK_IR_LEFT_PIN` / `TRK_IR_CENTER_PIN` / `TRK_IR_RIGHT_PIN` | PIN22 / PIN23 / PIN29 | 三路 IR 引脚（`TRK_IR_ALL_PIN` = 三路位或）|
| `TRK_IR_GPIO_IDX` | 1 | PORTB 的 GPIO 控制器 idx |
| `TRK_SAMPLE_MS` | 5 | IR 采样周期（抓短脉冲）|
| `TRK_DEBOUNCE_N` | 12 | 连续 N 次一致才认可（60ms）。**目标在中位还老动** → 加大它 |
| `TRK_CTRL_MS` | 100 | 控制周期（舵机响应不了更快）|
| `TRK_STEP_US` | 15 | 每步脉宽变化。**追不上** → 加大；**左右抖/冲过头** → 减小 |
| `TRK_CENTER_STEP_US` | 5 | 丢失后回中步长（更慢更柔和）|
| `TRK_LOST_HOLD_MS` | 200 | < 200ms 视为区域过渡：保持不动 |
| `TRK_LOST_SEARCH_MS` | 1000 | 200ms~1s 按最后方向半速搜索；> 1s 缓慢回中 |
| `TRK_PRINT_MS` | 1000 | 状态无变化时每 1s 打一行心跳 |

### 2.4 判定表（单轴只用水平信息）

| seen (L C R) | 判定 | 动作 |
|---|---|---|
| 1 1 0（L+C）| 左 | 脉宽 −15us |
| 1 0 0（L）| 左 | 脉宽 −15us |
| 0 1 1（C+R）| 右 | 脉宽 +15us |
| 0 0 1（R）| 右 | 脉宽 +15us |
| 0 1 0（C）| 居中 → `STOP` | **不动（死区 = 锁定）** |
| 1 0 1（L+R）| 跨中/很近 | 不动 |
| 1 1 1（全）| 很近 | 不动 |
| 0 0 0 | 无目标 → `NONE` | 走丢失分档 |

### 2.5 控制律与丢失分档

```
有目标：LEFT → yaw -= STEP_US；RIGHT → yaw += STEP_US；STOP → 不动；每步都 clamp 后下发
无目标：lost_ms += CTRL_MS
        < 200ms          → 保持（区域过渡）
        200ms ~ 1s       → 按 last_dir 走半步（STEP_US/2）慢速搜索
        > 1s             → 每步朝中位挪 CENTER_STEP_US，并【吸附】到中位
```

> ⚠️ **回中必须吸附**：若 `yaw` 与中位只差不到一个步长（例如 1001 vs 1000），直接 ±5 会跨过中位
> （1001−5=996），下一拍又跨回来（996+5=1001）→ 每 100ms 在中位两侧来回跳，舵机永久微抖。
> 代码里已经吸附（`if (target < CENTER) target = CENTER;`）。

### 2.6 自测入口行为（`track_yaw_test()`）

上电后依次做：打印标题与参数 → 三路 IR 切 GPIO + 内部上拉 → `csi_gpio_init` →
**`servo_core_init()`（引脚复用 + PWM + 两路回中位）** → 用"上电瞬间的 IR 状态"当去抖初值
（避免开头输出一段假状态）→ 打印 `[TRK] stat: ...` → 进入死循环（采样 → 去抖 → 每 100ms 一步 → 打印）。

串口表现（115200-8N1）：

```
[TRK] YAW single-axis tracking  L=PB22 C=PB23 R=PB29
[TRK] step=15us ctrl=100ms debounce=12 x 5ms
[TRK] stat: yaw=1000us center=1000us range=[300,1700] ir_seen=111
[TRK] seen=110 dir=LEFT  yaw=985us lost=0ms  <-changed
```

上机表现（正常）：手放左侧 → 左转直到目标进中央视区停住；正前中央 → 不动（锁定）；
移开手 → 200ms 保持 → 1s 半速搜 → 之后缓慢回中。

### 2.7 ⚠️ 要接进情感状态机，必须先改造它

`track_yaw_test()` 是**单函数死循环**，`yaw` / `last_dir` / `lost_ms` / `stable` 全是**函数内局部变量**，
外部**无法驱动它一步一步走**，也拿不到"当前有没有目标"。所以状态机要复用单轴版，得照 `track_dual.c`
的样子拆一遍：

1. 把局部状态提成**文件级 static**（`s_yaw` / `s_last_dir` / `s_lost_ms` / `s_stable` …）；
2. 拆成 `track_yaw_init()`（= 初始化那段）/ `track_yaw_step()`（= 循环体一次）/ `track_yaw_exit()`（回中 + 复位）；
3. `track_yaw_test()` 保留并改成 `init() + 死循环 step()` —— **行为与改造前完全一致**，继续当独立自测入口。

> 现状：**还没拆**（双轴版已经拆好了，改造时可以直接照抄它的骨架）。

---

## 3. `track_dual` —— 双轴跟随（YAW + PITCH）

### 3.1 公开接口（★ 状态机就用这一套）

```c
/* track_dual.h */
void     track_dual_init(void);        /* 进入：引脚 + 舵机初始化 + 复位内部状态 */
void     track_dual_step(void);        /* 推进一次（采样/去抖/按节拍步进/打印）*/
void     track_dual_exit(void);        /* 退出：两轴回中位 + 复位内部状态 */
uint32_t track_dual_lost_ms(void);     /* 当前"无目标"持续时长（ms）—— 显示/调试用 */
uint8_t  track_dual_sweep_done(void);  /* ★ 丢失 5s 后自动"扫一圈"；扫完仍无目标 -> 1（状态机据此退出）*/
void     track_dual_set_sweep(uint32_t wait_ms, uint32_t step_us);  /* 调：多久开始扫 / 扫视步长 */
void     track_dual_set_speed(uint32_t yaw_us, uint32_t pitch_us);  /* 调：追踪速度（每拍位移）*/
void     track_dual_set_accel(uint32_t accel_us);                   /* 调：加速度 */
void     track_dual_set_glide(uint32_t yaw_us, uint32_t pitch_us, uint16_t ticks); /* 调：死区滑行 */
void     track_dual_set_ctrl_ms(uint32_t ms);                       /* 调：控制周期 */
void     track_dual_reset_params(void);                             /* 恢复编译默认 */
void     track_dual_show_params(void);                              /* 打印当前参数 */
void     track_dual_test(void);        /* 自测入口：init + 死循环 step，不返回 */
```

每个函数的**语义、副作用、调用要求**（这些是最容易踩的地方）：

| 函数 | 做什么 | 副作用 / 注意事项 |
|---|---|---|
| `track_dual_init()` | 打印配置 → 三路 IR 切 GPIO + 上拉 + `csi_gpio_init` → **`servo_core_init()`** → 复位所有内部状态 → 用上电瞬间 IR 当去抖初值 → 打印 stat → `s_inited = 1` | **会重新初始化舵机（两路回中）**；失败时打印错误并**直接返回**（此时 `s_inited` 仍为 0，后续 `step` 会自己退出）|
| `track_dual_step()` | 采样 → 三路各自去抖 → 累计到 `TD_CTRL_MS` 就走一步（两轴**同时**）→ 按变化/心跳打印 | **非阻塞**；调用方负责节拍：**每 `TD_SAMPLE_MS`(5ms) 调一次**（外面 `mdelay(5)`）；未 init 时直接返回 |
| `track_dual_exit()` | 若已 init：两轴回中位 + 打印；然后复位全部状态、`s_inited = 0` | 幂等（未 init 时只复位状态、不下发舵机）|
| `track_dual_lost_ms()` | 返回"无目标"已持续多少 ms | 值在 `step()` 里增长；**只用于显示/调试**（退出判据改用下面的 `sweep_done`）|
| `track_dual_sweep_done()` | 丢失满 `TD_LOST_SWEEP_MS`(5s) 后自动扫一圈；**一圈扫完仍无目标 → 返回 1** | **状态机用它决定退出跟随**；扫视途中一旦瞄到目标立刻清零 |
| `set_sweep()` / `set_speed()` / `set_accel()` / `set_glide()` / `set_ctrl_ms()` / `reset_params()` / `show_params()` | 运行时改手感参数（串口 `TRK:*` 调的就是它们）| 改完下一拍即生效；`reset_params()` 恢复编译默认 |

### 3.2 内部实现（`static`）

| 函数 | 签名 | 作用 |
|---|---|---|
| `td_read_raw` | `uint8_t td_read_raw(void)` | 三路电平 → 位掩码 |
| `td_decide` | `td_move_t td_decide(uint8_t seen)` | **二维方位判定**（见表 §3.4），返回两轴方向 |
| `td_axis_step` | `int32_t td_axis_step(int32_t val, int8_t dir, uint32_t step, uint32_t mn, uint32_t mx)` | 单轴走一步（dir>0 / <0 / 0），带软限位 |
| `td_axis_recenter` | `int32_t td_axis_recenter(int32_t val, uint32_t center, uint32_t step)` | 单轴回中（**吸附**到中位，防抖）|
| `td_clamp` | `int32_t td_clamp(int32_t v, uint32_t mn, uint32_t mx)` | 通用钳制 |
| `td_print_line` | `void td_print_line(uint8_t seen, uint8_t changed)` | 按格式打印一行状态 |

```c
/* 文件内类型（不在 .h 里）*/
typedef struct { int8_t yaw; int8_t pitch; } td_move_t;   /* -1 / 0 / +1 */
```

**文件级 static 状态**（`init`/`exit` 都会复位）：
`s_ir_gpio` · `s_inited` · `s_stable` · `s_pend[3]` · `s_cnt[3]` · `s_last_seen` · `s_lost_ms` ·
`s_ctrl_acc` · `s_print_acc` · `s_yaw` · `s_pitch` · `s_prev_yaw` · `s_prev_pitch` ·
`s_last_yaw_dir` · `s_last_pitch_dir` · `s_mv_now`。

### 3.3 配置宏（`track_dual.h`，全部 `TD_*`）

| 宏 | 默认值 | 含义 / 调参方向 |
|---|---|---|
| `TD_IR_LEFT_PIN` / `TD_IR_CENTER_PIN` / `TD_IR_RIGHT_PIN` | PIN22 / PIN23 / PIN29 | 三路 IR 引脚（`TD_IR_ALL_PIN` = 三路位或）|
| `TD_IR_GPIO_IDX` | 1 | PORTB 的 GPIO 控制器 idx |
| `TD_SAMPLE_MS` | 5 | IR 采样周期（**也是 `step()` 的调用节拍**）|
| `TD_DEBOUNCE_N` | 12 | 去抖次数（60ms）|
| `TD_CTRL_MS` | 25 | 每 25ms 走一步（2026-10 由 100ms 细分，台阶小 4 倍）|
| `TD_YAW_STEP_US` | 4 | YAW 每拍位移（等效原 15us/100ms 的速度）|
| `TD_PITCH_STEP_US` | 3 | PITCH 每拍位移（等效原 10us/100ms）|
| `TD_CENTER_STEP_US` | 5 | 丢失回中步长（两轴共用）|
| `TD_ACCEL_US` | 2 | **速度斜坡**：每拍速度增量（0→满速 2 拍）|
| `TD_GLIDE_US` / `TD_GLIDE_PITCH_US` | 28 / 22 | **死区滑行量**（≈2.5° / 2°）：到位后用减速段收尾 |
| `TD_GLIDE_TICKS` | 14 | 滑行分几拍走完（14 × 25ms = 350ms）|
| `TD_LOST_HOLD_MS` / `TD_LOST_SEARCH_MS` | 200 / 1000 | 丢失分档阈值 |
| `TD_PRINT_MS` | 1000 | 心跳打印周期 |

> 以上 6 个手感参数都能**串口实时改**（`TRK:SPD/ACC/GLIDE/TICK/SHOW/DEF`，§3.8），不用重新烧录。

### 3.4 二维判定表（`td_decide`）

| seen (L C R) | YAW | PITCH | 判定 |
|---|---|---|---|
| 0 0 0 | — | — | 无目标 → 走丢失分档 |
| 1 0 0 | 左 | — | 左（高度未知，不动 PITCH）|
| 1 1 0 | 左 | 抬 | 左上 |
| 1 1 1 | — | — | 全覆盖（很近）→ 不动 |
| 1 0 1 | — | 低 | 下方（下面两路都看到、上方 C 没看到）|
| 0 1 0 | — | 抬 | 中上 |
| 0 1 1 | 右 | 抬 | 右上 |
| 0 0 1 | 右 | — | 右 |

规则：**YAW 只看到单侧才转**（两侧都看到 / 都没看到 → 水平方向无法判断）；
**PITCH：C 看到且未全覆盖 → 抬头；L+R 都看到但 C 没看到 → 低头**。

### 3.5 控制律与丢失分档（两轴各自独立）

```
有目标：两轴按 td_move_t 各自走一步 —— 位移由【速度状态】给出（不再是死步长）：
        dir != 0 : v += dir×ACCEL（限幅 ±STEP_US）→ 位移 = v     （起步/反向都平滑）
        dir == 0 : 进死区 → 不原地急停！安排 GLIDE_US 的滑行尾巴，
                   分 GLIDE_TICKS 拍逐拍递减走完（位移 = 剩余量 / 剩余拍数）
        各自 clamp 后一起下发
无目标：lost_ms += CTRL_MS（同时把速度/滑行状态清零，不留残余惯性）
        < 200ms     → 两轴都保持
        200ms ~ 1s  → 各自按"最后方向"半速搜索
        1s ~ 5s     → 各自朝中位挪 CENTER_STEP_US（缓慢回中），静静等目标回来
        >= 5s       → ★ 主动"扫一圈"：朝一端扫到限位 → 反向扫到另一端；
                      扫完仍无目标 → sweep_done 置位（状态机据此退出，见 §3.9）
```

### 3.6 接进情感状态机（**已接入**，2026-10）

```c
moves_interrupt();                       /* ① 先停掉动作库正在播的动作（不能同时写舵机）*/
track_dual_init();                       /* ② 引脚 + 舵机 + 复位内部状态 */

while (mode == MODE_TRACK) {
    track_dual_step();                   /* ③ 每 5ms 推进一次（非阻塞）*/
    mdelay(TD_SAMPLE_MS);

    if (track_dual_sweep_done()) {       /* ④ 自动退出：丢失 5s → 扫一圈 → 还是没找到 */
        mode = MODE_EMOTION;
        break;
    }
    /* 两次 step 之间可以处理串口命令 / 触摸 / 表情（这正是"非阻塞"的意义）*/
}

track_dual_exit();                       /* ⑤ 两轴回中位，交还舵机控制权 */
```

> ✅ **已按这个骨架接入**（2026-10）：`emotion_fsm.c` 的 `EMO_MODE_TRACK` —— 串口
> `TRACK:ON` / `TRACK:OFF` 切换；收到 `EMO:` / `INTENT:` 会自动退出；
> 丢失目标 5s 先"扫一圈"找（§3.9），一圈扫完仍没找到才退出。
> 设计意图与命令表见 `doc/README.md` §7.3 / §4.3。

### 3.7 方向整定（上机后某轴转反了就改一处）

约定：**脉宽增大 = YAW 右转 / PITCH 抬头**。反了的话只改 `td_decide()`：

| 现象 | 改法 |
|---|---|
| 水平反了（手在左却右转）| 把 `m.yaw = -1;` 与 `m.yaw = +1;` 两处赋值对调 |
| 竖直反了（手抬高却低头）| 把 `m.pitch = +1;` 与 `m.pitch = -1;` 两处赋值对调 |
| 不该"只见 C 就抬头"（比如 C 装在屏幕下沿）| 把 PITCH 那两行的 `C` / `(L && R)` 判断互换 |

限位、丢失分档、节拍都不用动。

### 3.8 ★ 丝滑化：加速度斜坡 + 死区滑行（2026-10）

**原来哪里不丝滑**：① `每 100ms 跳 15us` → 走起来一格一格；② 目标居中（死区）**立刻停**，
速度从 15us/100ms 直接归零 → 像"刹车"，到位置一顿。

**改法**（三件事叠加，默认值见 §3.3）：

| # | 做了什么 | 参数 | 效果 |
|---|---|---|---|
| ① | 控制周期细分 4 倍、每拍位移等比减小 | `TD_CTRL_MS` 100→25、步长 15/10→4/3 | 巡航速度不变，台阶小 4 倍 |
| ② | 加速度斜坡 | `TD_ACCEL_US` = 2 | 起步/反向不再瞬间满速 |
| ③ | **死区滑行尾巴** | `TD_GLIDE_US` = 28(≈2.5°)、`TD_GLIDE_TICKS` = 14 | 到位后用 2~3° 减速收尾，不再急停 |

滑行尾巴算法（`td_axis_smooth()`，每轴一份状态）：

```
进死区那一拍：glide_dir = 刚才的运动方向；glide_left = GLIDE_US；glide_ticks = GLIDE_TICKS
以后每拍    ：step = glide_left / glide_ticks；glide_left -= step；glide_ticks--
              → 位移逐拍递减（14 拍把 28us 走完，最后几拍几乎不动 = 自然停下）
滑行途中目标又出现 → 立刻取消滑行、改回追踪（响应不会变慢）
丢失目标           → 速度/滑行状态整体清零（不拖惯性乱走）
```

**现场调参（不用烧录）**：在状态机里 `TRACK:ON` 进跟随，然后调：

| 命令 | 作用 | 建议 |
|---|---|---|
| `TRK:SHOW` | 打印当前全部参数 | 每次调前先看一眼 |
| `TRK:GLIDE <yaw> <pitch> <ticks>` | 滑行量 / 拍数 | **先调这个**：还觉得顿 → 加大（如 `TRK:GLIDE 40 30 20`）；觉得"刹不住/过冲" → 减小 |
| `TRK:SPD <yaw> <pitch>` | 追踪速度（每拍位移）| 更跟手 → 6 4；更悠闲 → 3 2 |
| `TRK:ACC <us>` | 加速度 | 起步太猛 → 1（0→满速 4 拍）|
| `TRK:TICK <ms>` | 控制周期 | 默认 25；15 更细腻但抖动与总线压力上升 |
| `TRK:DEF` | 恢复编译默认 | 调乱了用它 |

串口打印行尾出现 ` <glide` 就表示此刻正在走减速尾巴（收尾有没有发生一眼可见）。
参数只影响跟随手感，不碰判定表、限位与丢失分档。

### 3.9 ★ 丢失 5s → 主动"扫一圈"（2026-10）

**为什么**：原来是"丢失 3s 就直接退出跟随"——**太快**。目标只是被短暂挡一下、
或人低头看眼手机，机器人就把头转回去做表情动作了，观感"没耐心"。

**现在的丢失分档**：

```
丢失时长      行为
< 200ms      保持（区域过渡，别乱动）
200ms ~ 1s   按最后方向半速搜（原来就有的"先找一下"）
1s ~ 5s      缓慢回中位，静静等目标回来
>= 5s        ★ 从当前位置朝一端扫到限位 → 掉头扫到另一端（覆盖整个视野）
             一圈扫完仍无目标 → track_dual_sweep_done() 置位 → 状态机退出跟随
扫视途中瞄到目标 → 立刻取消扫视、恢复跟踪（回来的目标不会漏掉）
```

**时间账**（默认值）：`TD_LOST_SWEEP_MS` = 5s，扫描本身 YAW 全行程 1400us、
`TD_SWEEP_STEP_US` 15us/拍（= 600us/s）→ 从中间扫到一端再折回另一端 ≈ 3~4s，
**合计约 8~9s 才退出** —— 比原来从容得多。

**现场调**：`TRK:SWEEP <wait_ms> <step_us>`（丢多久开始扫 / 扫多快），
例如 `TRK:SWEEP 8000 20` = 8s 才扫、扫得快些；`TRK:SHOW` 看当前值。
`state` 里的 `sweep=idle/scanning/DONE` 分别表示"没在扫 / 正在扫 / 扫完将退出"。

串口日志能看到两个时间点：

```
[TD] lost 5025ms -> sweep to find target      ← 开始扫
[TD] sweep done, no target -> exit signal     ← 扫完没找到，置位退出信号
```

---

## 4. 两个模块对照

| 项 | `track_yaw` | `track_dual` |
|---|---|---|
| 驱动轴 | 只有 YAW | YAW + PITCH（同时）|
| 公开函数 | 1（`track_yaw_test`，死循环）| 5（init / step / exit / lost_ms / test）|
| 能否被状态机驱动 | ❌ 需要先拆分（§2.7）| ✅ 直接可用 |
| 判定依据 | 只用 L/R | L/C/R 二维表 |
| 实机验证 | ✅ 稳定（回归基线）| 🟡 待上机（PITCH 方向待整定）|
| 内部状态 | 函数内局部变量 | 文件级 static |

## 5. 调试与验证 checklist

- **先用 `ir_test` / `ir_state_test` 确认传感器**（`UserFolder/test/`）：未遮挡应为 1，
  逐路遮挡看位序对不对；三路叠加态（LR / LC / CR / LCR）出现是跟随的**必要条件**。
- **单轴回归**：`track_yaw_test()`（已完成实机验证，改动任何公共代码后可以拿它当基线）。
- **双轴**：`track_dual_test()` → 手放左/右/上/下看两轴反应；方向不对按 §3.7 改。
- **接线优先**：历史上"叠加态读不到、未遮挡脚被拖到 0.8V、隔一会儿自愈"是**接触不良**（杜邦线/排针/面包板），
  别先怀疑固件或几何。
- **`print` 心跳**：无变化时每 `*_PRINT_MS` 打一行，变化时立刻打并带 `<-changed` —— 调试先看它。

## 6. 相关文档

工程级文档统一在 **`UserFolder/doc/`**：`README.md`（**唯一入口**；**§7.3 是"把跟随做成状态机的一个模式"的
接入方案与骨架代码**）、`io_plan.md`（引脚分配权威）、`ir_tracking_plan.md`（跟随方案与验证进度）、
`pin_mux_table.md`（复用表与 `pinmux.c` 的 PORTB 缺陷）。
模块内其它文档：`../servo_test/README.md`（中位/限位是怎么标出来的）。
