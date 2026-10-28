# 表情系统交接（v9：参数化图元 + 姿势表 + 正弦动作）

> 适用范围：`projects/baozi/UserFolder/anim/` 下的表情渲染与动作。
> 最后更新：2026-10-27（v9 全链路落地：图元光栅器 → 姿势表 → 动作层 → 状态机）。
> 总览文档仍是 `UserFolder/doc/README.md`（§3.5 屏幕与表情、§9 待办）。

---

## 0. 一句话现状

表情系统已整体换成 **v9**：8 个解析图元 + 30 个从模拟器导出的姿势 + 正弦叠加动作 +
按情绪着色。渲染、动作、过渡三条链路都能在 **PC 上编译跑出图**，与浏览器模拟器逐像素
对齐（29 个姿势平均 IoU **0.995**），固件 `Obj/Baozi.elf` 全量链接通过（text 94.9 KB）。

改表情 = 改模拟器 → 跑 `gen_poses.py` → 重编。**不需要动任何 C 逻辑。**

---

## 1. 数据通路

```
main.c (TEST_ENTRY=4)
  └─ app_core_init()            ui_app_init() → face_app_init()   ← 屏幕初始化 + neutral
  └─ app_core_step() 每轮：
       ① 触摸   ICNT8952BN → ui_app_handle_gesture()（含抚摸手势）
       ② 串口   UART0 文本行 → emotion_fsm_feed_line()（EMO:/INTENT:/TRACK:）
       ③ UI     ui_app_tick(dt) → face_app_tick(dt)
       ④ 刷屏   ui_flush_step()

emotion_fsm 定表情（心情 > 意图 > 情绪）
  → face_ui_emotion / face_ui_mood / face_ui_intent     [anim/face_ui_card.c]
  → ui_app_set_emotion()                                 [ui_app.c]（卡片层里只记不抢屏）
  → face_app_try_emotion(name)                           [anim/face_app.c]
        │  名字映射（angry→angry_high / greet→happy …）
        │  姿势表查表 → 过渡起点=当前屏上那一帧
        ▼
  face_model_mix（过渡插值）→ face_model_apply（眨眼/视线/张嘴）
        ▼
  face_show_parts(parts, n, 颜色)                        [anim/face_render.c]
        ▼
  fd_cov_part（逐行覆盖度）→ fd_blend_row（调色）→ 60 段 DMA        [anim/face_draw.c]
```

**所有权**：UART0 归 `src/uart_hal.c`；屏幕归 `ui_app.c`；`emotion_fsm` 只通过 `face_ui_*`
影响表情，从不直接碰屏。

---

## 2. 文件与职责

| 文件 | 编译 | 依赖外设 | 职责 |
|---|---|---|---|
| `anim/face_draw.h/.c` | ✅ | ❌ 纯计算 | **图元光栅器**：8 种形状 → 每行 x 区间 → 覆盖度缓冲（2 条子扫描线做纵向 AA） |
| `anim/face_sup_tab.h` | 数据 | — | 自动生成：超椭圆剖面表（n=2.1~3.2）+ 正弦表。**没有 FPU，pow() 不可用** |
| `anim/face_poses_v9.h` | 数据 | — | 自动生成：30 个姿势的部件表 + RGB565 颜色 |
| `anim/face_model.c/h` | ✅ | ❌ 纯计算 | **姿势库 + 几何变换**：查表、两帧插值、眨眼/视线/眼睛大小/张嘴 |
| `anim/face_action.c/h` | ✅ | ❌ 纯计算 | **动作层**：`时间 → 参数`，每个动作一两行正弦 |
| `anim/face_render.c/h` | ✅ | ✅ video/DMA | **传输层**：行缓冲 → 分段 DMA + 总线仲裁回调。只认"一组部件 + 一个颜色" |
| `anim/face_app.c` | ✅ | ❌ | **状态机**：切姿势、280ms 过渡、微扫视、眨眼、50ms 重绘节流、临时表情层 |
| `anim/face_ui_card.c` | ✅ | ❌ | `face_ui_*` 门面 → 转发 `ui_app`；`face_ui_intent()` 用 `face_app_has_pose()` |
| `anim/face_ui.h` | — | — | 门面契约（返回 `0/-1`） |
| `anim/face_emo_test.c` | ❌ **不编译** | — | 旧的自带主循环调试入口（与 UART0 冲突）。**已与新 API 脱节，只作历史参考** |
| `anim/face_anims.h` | ❌ 不用 | — | 旧逐帧管线（23 个 clip，372 KB）。`face_render.c` 已不再 include 它 |
| `anim/anim_player.c`、`anim_res.c`、`third_party/puff.c` | ❌ 注释掉 | — | 位图管线（ANM3 + puff，~1 MB），只作对照实验 |

---

## 3. 改表情的流程（唯一入口：模拟器）

```
_probe/face_sim_v9.html          ← 唯一事实来源（形状、颜色、动作曲线）
        │  node -e "...导出 POSES..."   （见 §5-②，导出脚本在命令历史里）
        ▼
_probe/faces_v9.json             ← 冻结的规格
        │  python _probe/gen_poses.py
        ▼
anim/face_poses_v9.h             ← 自动生成，勿手改
```

**加一个新表情**：
1. 模拟器 `POSES` 加一条（`eye()/mouth()/deco()` 三个助手）。
2. `ACT` 里加/复用一条动作曲线。
3. 跑导出 + `gen_poses.py` + 重编。

**加一个新图元**（少见）：`face_draw.h` 加 `FD_*` + `face_draw.c` 的 `spans_of()` 加一个
case（返回该行的 x 区间），模拟器那边同步加 `xxxSpan()`。图元必须是"一行可解析成 1~3 段"，
否则性能会崩。

---

## 4. 当前清单

**30 个姿势**（含 `pet` 别名）：

| 类别 | 名字 |
|---|---|
| 情绪 8 | `neutral` `happy` `laugh` `sad` `sad_anguish` `angry_low` `angry_high` `surprised` |
| 互动 5 | `shy`(`pet`) `loving` `wink` `mischief` `smug` |
| 意图 5 | `think` `listen` `question` `look` `glance` |
| 状态 6 | `asleep` `sleepy` `pout` `yawn` `bored` `nervous` |
| 强表现 6 | `cry` `shock` `idea` `star_struck` `dizzy` `laugh`(同上) |

**8 个图元**：`FD_SUP`（超椭圆，n=2 即圆）`FD_CUT`（椭圆 ∩ 旋转半平面）`FD_CRES`（外减内 =
月牙）`FD_CAP`（胶囊）`FD_DISC` `FD_RING` `FD_HEART` `FD_STAR`（多边形奇偶填充）。

**名字映射**（`face_app.c` 的 `g_alias[]`，给上层情绪词表用）：
`angry → angry_high`、`greet → happy`、`idle → neutral`、`sleeping → asleep`。

---

## 4.5 表情怎么被触发（情绪 + 强度 → 表情）

状态机（`emotion/emotion_fsm.c`）对表情层只暴露 4 类信号，**没有改动它一行**，
全部映射都在表情层做。两层触发：

### 第一层：状态驱动（连续）

`face_ui_card.c` 在每次切表情前把连续状态推给表情层
（`face_app_set_state(happiness, energy, curiosity)`，千分制），
`face_app.c` 的 `g_picks[]` 表按顺序匹配，第一条全部条件满足者胜出：

| 情绪 | 条件 | 表情 |
|---|---|---|
| neutral | energy ≤ 320 | `sleepy` |
| neutral | 其余 | `neutral` |
| happy | curiosity ≥ 950 | `star_struck` |
| happy | happiness ≥ 960 (且 energy ≥ 700) | `laugh` |
| happy | 其余 | `happy` |
| sad | happiness ≤ 120 | `cry` |
| sad | happiness ≤ 180 | `sad_anguish` |
| sad | 其余 | `sad` |
| angry | energy ≥ 780 | `angry_high` |
| angry | energy ≥ 550 | `angry_low` |
| angry | 其余 | `pout` |
| surprised | happiness ≤ 400 | `shock` |
| surprised | 其余 | `surprised` |
| loving | energy ≤ 480 | `shy` |
| loving | 其余 | `loving` |

阈值围绕 `emotion_manager.c` 的 `s_emo_states[]` 标称值定，**标称值显示"标准脸"**，
只有被刺激顶过头才换更强的脸（例如 happy 标称 900 -> 笑脸，被夸到 970 -> 大笑）。

### 第二层：心情 / 意图 / 事件（离散）

| 来源 | 名字 | 表情 |
|---|---|---|
| 心情 SLEEPING | — | `asleep` |
| 心情 BORED | — | `bored`，并且每 9~18s 自动冒一次 `yawn`（1.8s，face_app 内部定时） |
| INTENT:CHAT | chat | `listen` |
| INTENT:GREETING | greeting | `wink`（**以前查不到，因为表里叫 greet —— 已修**） |
| INTENT:QUESTION | question | `question` |
| INTENT:COMMAND | command | `think` |
| INTENT:LOOK | look | `look` |
| 被抚摸 | EMO_STIM_PET | `pet`（`ui_app.c` 每次有效移动续期 2.5s） |
| 被夸奖 | EMO_STIM_PRAISE | `smug` 2.0s（`behavior.c` → `face_ui_reaction`） |
| 被责备 | EMO_STIM_SCOLD | `nervous` 2.0s |
| 一起玩 | EMO_STIM_PLAY | `mischief` 2.2s |

事件类走 `face_app_play(name, ms)` 的**临时表情层**：盖住当前情绪，到期自动回落。
期间若情绪层推来同名情绪会被忽略（`ci_same`），所以不会被每拍的同步顶掉。

### 覆盖率

**27/30 可触发**。剩下三个需要本工程还没有的信号：

- `glance` — 视线瞬移（等红外跟随/注意力层给"目标切换"事件）
- `dizzy` — 转圈结束（等 `movements/` 的旋转动作播完给个回调）
- `idea` — "想到办法了"（等 PC 侧发 `EVENT:IDEA` 之类）

### 一个已知延迟（不影响正确性）

`EMO:` / `INTENT:` 命令触发的表情，要等**当前这轮舵机动作播完**才会切
（同步点在 `emotion_fsm_step()` 的 ④ 播放块里，动作是阻塞播放 1.5~7s）。
刺激类（摸/夸/凶/玩）是立即生效的，因为 `emotion_fsm_stimulus()` 里当场同步。
若要让命令也立即生效，在 `fsm_tick_hook()` 里加一次 `sync_face()`（约 3 行），
但那是状态机文件的改动，这轮按约定没动。

## 5. 验证链（三层，全部不用烧录）

```bash
# ① 模拟器：浏览器打开 _probe/face_sim_v9.html?pose=shy
#    改完 HTML 必须跑一次无头导出，否则渲染 bug 看不出来：
cd /d/Baozi/_probe && node sim_headless.js     # -> sim_poses.ppm / sim_poses.txt

# ② PC 端跑固件代码（mingw gcc）：姿势静止帧
cd /d/Baozi/_probe/c_host
gcc -O2 -std=c99 -I<anim> render_poses2.c -o render2.exe
./render2.exe c2_poses.ppm          # 与 sim_poses.ppm 逐像素比对（脚本见命令历史）
./render2.exe c2_motion.ppm 1200    # 带动作，看 t=1200ms 的姿态

# ③ 动作曲线数值对齐
gcc -O2 -std=c99 -I<anim> act_dump.c -o act_dump.exe
./act_dump.exe 0 700 1500 2600 4000 > c_act.tsv   # 与 JS 的 ACT 表逐项比

# ④ 状态机端到端（过渡/眨眼/节流）：给 face_render 打桩，抓每一帧
gcc -O2 -std=c99 -I<anim> sim_app.c -o sim_app.exe && ./sim_app.exe app_frames.ppm

# ⑤ 播放验证（60 秒剧本，自动检查 7 条不变量）
gcc -O2 -std=c99 -I<anim> play_test.c -o play_test.exe && ./play_test.exe
#   空脸帧 / 部件越界 / 过渡卡住 / 眨眼频率 / 视线幅度 / 无聊哈欠 / 实际帧率
#   带参数可导出固定 20ms 节拍的一段：./play_test.exe play.ppm（用于做 GIF）

# ⑥ 固件全量编译（RISC-V）
cd projects/baozi
export PATH="/d/Baozi/CDK/CDKRepo/Toolchain/XTGccElfNewlib/V3.4.0/R/bin:/d/Coding/VSCODE/mingw64/bin:$PATH"
mingw32-make -f Baozi.mk all -j4
# 末尾的 crc32 / Baozi.modify.bat 在 CDK 之外跑不了，属正常；看 "size of target" 即可
```

**当前基线**：静态 29 姿势平均 IoU **0.9949**（最低 `star_struck` 0.976 属非凸多边形
亚像素差）；动作 145 个采样点除 `shy` 的负 blink 被钳到 0 外全部一致（±3）。

上板验证（`TEST_ENTRY=4`，串口 115200）：`EMO:HAPPY` `INTENT:THINK` `INTENT:LOOK`
`EMO:SAD`（看颜色渐变）`EMO:ANGRY`（应落在 `angry_high`）。

---

## 6. 硬约束与坑（每条都真踩过）

**单位**
1. 坐标 `x/y` = **1/2 像素**（相对屏幕中心），`rx/ry` = **半径**，也是 1/2 像素。
   旧代码里 `rx/ry` 是**直径**——两套单位混用是历史上最大的一次事故，现已统一。
2. `face_expr_t.gaze_dx/dy` 也是 1/2 像素（≈ 模拟器的 `st.gx`），**不要再乘 2**。
3. 渲染层内部换 1/256 像素定点：`X = 61440 + x*128`。

**几何**
4. 扫描线求交**不能写 `(a-y)*(b-y) <= 0`**：a、y 是 1/256px、量级 ±246k，乘积 6e10 溢出
   int32，判据变成随机数 → 胶囊/多边形整片横条伪影。用 `crosses()` 比较符号。
5. **镜像必须同时翻朝向**：法线角 `a → 180-a`、旋转角 `rot → -rot`、胶囊终点 `x1 → -x1`。
   只翻 x 会让左右眼切向同向（`angry_high` 因此错过一次）。
6. `FD_CRES`（月牙）必须返回**两段互不相连**的区间（外减内）。早先写成
   `[cx-a,cx+a]` 与 `[cx-b,cx+b]` 一起返回，并集就是整个外椭圆 —— 月牙被填成实心半圆，
   模拟器和固件因此对不上，双向比对才发现。
7. `FD_CUT` 的切弦必须**夹回椭圆范围**，否则弧外时区间会溢出到脸的另一侧。
8. 非凸多边形（五角星）必须用**奇偶规则**（交点排序后两两配对）。用 min/max 会把凹口填平。

**过渡**
9. 两帧配对必须**先按角色分组、组内按 x 排序**。只按顺序配对会让 laugh 的"嘴"配上 shy 的
   "第一道腮红"，插值出一道竖直细弧（屏幕上就是一条莫名其妙的斜线）。
10. 新增/消失的部件用 **alpha 淡入淡出**，不要用"半径缩到 0"：胶囊的端点坐标是绝对值，
    按比例缩会被拉向屏幕中心 —— 又一条从画面中央窜出的斜线。

**观感**
11. **D 形嘴（`FD_CUT` 平顶圆底）就是笑嘴**，不能用在哭上；`cry` 因此被读成"笑哭"。
    哭 = 垂眼 + 泪 + **下弯的嘴**（三段 cap 折线）。
12. 浅弧在 480px 上会被读成**眉毛/帽子**，不像闭眼。闭眼要么足够厚（laugh 的月牙），
    要么就别闭（`cry` 最后改成睁眼流泪）。
13. 参考素材里 `angry` 的眼**比中性眼还大**（r≈90 vs 77.5）——"愤怒＝瞪大"，缩小是错的。

**固件**
14. 一 tick 只重绘一次：`face_show_parts()` 是整屏 60 段 DMA 长事务，中途再 `LCD_Set_Window`
    会打断它 → 固定位置白块。
15. 重绘节流 50ms（`FACE_REDRAW_MIN_MS`）。累加器要**减去**而不是清零，否则 20ms 的 tick
    下实际间隔变成 60ms（16fps 而不是 20fps）。
16. `LCD_DmaSend` 的起点要对齐 cache 行；每段后必须 `LCD_WaitIdle()`（且必须显式声明，
    否则编译器按隐式 `int LCD_WaitIdle()` 处理，等待失效 → 屏幕底部白块）。
17. 加新 `.c` 后 `Baozi.mk` 与 `Obj/Baozi.txt` 都要有它。CDK 打开工程会重新扫描生成，
    但**命令行直接 make 时必须手工补**（本轮补了 `anim_face_draw.o`）。

---

## 7. 已知问题 / 下一步

### 7-1. ~~抚摸表情被情绪同步抢走~~ **已修复**

根因：`emotion_fsm_step()` 每轮（10ms）都调 `face_ui_emotion(emo_current_name())`，
把屏幕改回当前情绪。修法两层：

1. `face_app.c` 的**临时表情层**：`face_app_play(name, ms)` 播 ms 毫秒后自动回落；
   期间若情绪层推来的名字与"进入前的情绪"相同，直接忽略（`ci_same` 判定）。
2. `ui_app.c` 的抚摸判定：进入时 `face_app_play("pet", 2500)`，每次有效移动续期，
   抬手 `face_app_hold_end()`。

**顺带修掉的隐藏 bug**：`apply_pose()` 里"同名短路"的条件原本是
`idx == g_pose_idx && g_morph_on == 0`，于是过渡没走完时情绪层再推一次同名姿势会把
`morph_step` 清零 —— 10ms 推一次 ⇒ 过渡永远走不完，表情卡在中间。现在无条件短路。
（这个 bug 只在"重复推同一个姿势"时出现，靠 `sim_app.c` 的 hold 场景才抓到。）

### 7-2. `ui.c` 里有一对重复的段缓冲（15 KB SRAM）

`UserFolder/ui.c` 也定义了 `g_segA/g_segB`（各 7680 B），与 `face_render.c` 的那对重名
（都是 static，所以链接不报错）。当前 bss 48.6 KB，而 SRAM 区实际是 **192 KB**
（链接脚本 `SRAM: ORIGIN=0x1FFF0000, LENGTH=0x30000`，注释里写 64KB 是错的），
余量充足；真要省内存先查这里。

### 7-3. 未做：脏行带传输（M4）

现在每帧整屏 460 KB。眨眼/视线只影响眼区 ~180 行，只发那一段可以到 8ms/帧、30fps。
窗口设为 `(0, y0, 480, h)` 即可，事务形状不变。**这是唯一需要上板验证的优化**，
失败就回退全屏，功能不受影响。

### 7-4. 未做：脖子联动

`look` / `glance` 只有眼睛动，舵机不动。`movements/` 加一条 `MOVE_LOOK_AROUND` 即可。

### 7-5. `face_emo_test.c` 已与新 API 脱节

它还在用 `face_part_t` / `face_show_mix` / `face_clip_*`，**编译会直接报错**（当前不在
编译列表里所以没人发现）。要么删掉，要么下次要用时按新 API 重写。

---

## 8. 本轮（2026-10-27）改了什么

| 文件 | 改动 |
|---|---|
| `anim/face_draw.h/.c` | **新增**：8 图元光栅器 + 逐行区间 + 2 条子扫描线纵向 AA + 部件 alpha |
| `anim/face_sup_tab.h` | **新增（自动生成）**：超椭圆剖面表 + 正弦表 |
| `anim/face_poses_v9.h` | **新增（自动生成）**：30 个姿势 |
| `anim/face_model.c/h` | **重写**：姿势库 + 按角色分组的过渡插值 + 眨眼/视线/张嘴 |
| `anim/face_action.c/h` | **重写**：30 条正弦动作，与模拟器 `ACT` 表逐项对齐 |
| `anim/face_render.c/h` | **重写**：只剩传输（行缓冲 → DMA），不再有任何形状代码 |
| `anim/face_app.c` | **重写**：姿势表、280ms 过渡、微扫视、眨眼、临时表情层、名字映射 |
| `anim/face_ui_card.c` | 未改（`face_app_has_pose` 契约不变） |
| `UserFolder/ui_app.c` | 抚摸进入/续期调 `face_app_play("pet", 2500)`，抬手 `face_app_hold_end()` |
| `anim/face_app.c` | 加 `g_picks[]`（情绪 + 强度 → 表情）+ `face_app_set_state()`；无聊时每 9~18s 自动 `yawn` |
| `anim/face_ui_card.c` | 切表情前推连续状态；`bored` 改用 `bored` 脸（原来是 neutral）；意图名映射（GREETING→wink / CHAT→listen / COMMAND→think）；新增 `face_ui_reaction()` |
| `anim/face_ui.h` | 新增 `face_ui_reaction(uint8_t kind)` 声明 |
| `UserFolder/behavior.c` | 刺激后追加 `face_ui_reaction()`（夸/凶/玩各给 2 秒瞬时表情）|
| `Baozi.mk` / `Obj/Baozi.txt` | 登记 `anim_face_draw.o`（CDK 重新扫描后会自动一致） |
| `_probe/gen_poses.py` | 生成器：补 `alpha` 字段、不再输出 `fd_pose_t`（已移到 `face_draw.h`） |
| `_probe/c_host/render_poses2.c` | **新增**：PC 端跑整个模型层出图 |
| `_probe/c_host/act_dump.c` | **新增**：动作层数值导出，用于与 JS 比对 |
| `_probe/c_host/sim_app.c` | **新增**：给 `face_render` 打桩，端到端抓状态机的每一帧 |

**验证状态**：RISC-V 全量链接通过（text 96 236 / data 644 / bss 48 660）；五个模块
`-Wall -Wextra` 0 warning；静态 IoU 0.9949；动作 145 点仅 2 点因"blink 不能为负"有意不同。


---

## 9. 里程碑

| | 内容 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| **M1** | 形状代数 + 纵向 AA + bbox 行裁剪 | ✅ 完成 | 8 图元（`face_draw.c`）；每条扫描线 2 条子采样做纵向 AA；`face_render.c` 的 `part_rows()` 逐部件裁掉无关行 |
| **M2** | 表情库重建 | ✅ 完成 | 30 个姿势；与模拟器逐像素 **IoU 0.9949** |
| **M3** | 动作引擎 + 微扫视 + 单眼眨 | ⚠️ 60% | 微扫视 ✅（1.2~2.8s 抖一次，240ms）；**平滑用的是一阶 IIR**（τ=60/130ms）而不是欠阻尼弹簧 —— 没有超调，扫视略"软"；**单眼眨未做**（眨眼对所有眼睛同时生效） |
| **M4** | 脏行带 + 自适应节流 | ❌ 未开始 | 现在整屏 460 KB/帧、20 fps、SPI 占空约 40%。行带方案不需要额外 RAM（`part_rows` 已经能给出任何一帧的纵向范围），只需把窗口设成 `(0,y0,480,h)` |
| **M5** | 临时层 + pet/listen/think 接线 | ✅ 完成（待上板） | `face_app_play/hold_end` + `ui_app.c` 抚摸三处；listen/think 走 INTENT；夸/凶/玩走 `face_ui_reaction` |
| **M6** | 触发映射（情绪 + 强度） | ✅ 完成 | `g_picks[]`；覆盖率 27/30（剩 glance / dizzy / idea 缺信号源） |

**唯一还没验证的环节是上板。** 上板要确认四件事：
1. 按情绪着色在真实屏上的观感（RGB565 只有 5/6/5，蓝色会偏）；
2. 20 fps 下有没有白块/撕裂（DMA 时序）；
3. 每帧实际耗时（PC 上算不出，要看是不是真能在 20ms 内画完）；
4. 触摸/串口在表情动画期间是否还跟手。

**建议顺序**：先上板跑一次（哪怕只用默认 neutral→happy→sad 走一遍），
拿到真实帧耗时之后再决定 M4 的值不值得做。
