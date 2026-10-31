# 一键演示（DEMO 页）· 联调文档

> **模块**：`UserFolder/demo/demo_show.c/.h`（固件）+ `pc_baozi/demo_voice.py`、
> `pc_baozi/demo_bridge.py`（PC）
> **一句话**：屏幕 DEMO 页点一下 START，包子把"表情 + 脖子动作"整套按顺序演一遍，
> 每一步开始前由 PC 的喇叭先播报这一步是什么（预合成 WAV，不现场合成）。
> **更新**：2026-10-31

---

## 1. 怎么用（30 秒版）

| 步骤 | 操作 | 期望 |
|---|---|---|
| ① 生成音频（只做一次） | `pc_baozi\.venv\Scripts\python.exe demo_voice.py` | `assets/demo/` 下 10 个 WAV |
| ② 起 PC 主程序 | `python main.py`（app_enabled 开） | 日志出现「演示播报已接入」 |
| ③ 板子上进 DEMO 页 | 双指按两只眼 → 卡片层 → 连续左滑到第 7 页 | 圆点 7 个，第 7 个亮 |
| ④ 点 START | 卡片收起、回表情态 | 喇叭说「没事的时候…」然后开始待机巡视 |
| ⑤ 中途想停 | 点屏幕任意处 | 立刻停，回当前情绪 |

没有 PC / 没插 ESP32 时**演示照跑**，只是没有播报（板子每步等 2.5s 兜底）。

---

## 2. 演示脚本（10 步，约 100 秒）

顺序与内容定义在 `demo_show.c` 的 `k_steps[]`；**name 是三端对齐的钥匙**——
固件用它发上行行，PC 按它找 `assets/demo/<NAME>.wav`。改一边必须改另一边。

| # | NAME | 表情姿势 | 动作 | 时长 | 播报词（demo_voice.py） |
|---|------|---------|------|------|------------------------|
| 1 | IDLE | neutral | MOVE_IDLE | 9.0s | 没事的时候，我会自己慢慢巡视四周…… |
| 2 | HAPPY | happy | MOVE_HAPPY | 5.0s | 开心的时候，我会笑着摇头晃脑…… |
| 3 | SAD | sad | MOVE_SAD | 5.0s | 难过的时候，我会低下头…… |
| 4 | ANGRY | angry_high | MOVE_ANGRY | 4.5s | 生气了，我会昂起头…… |
| 5 | SURPRISED | surprised | MOVE_SURPRISED | 4.2s | 惊讶的时候，我会猛地抬起头…… |
| 6 | LOVING | loving | MOVE_LOVING | 5.5s | 被摸一摸，我会微微抬头、歪着脑袋…… |
| 7 | EIGHT | laugh | MOVE_EIGHT | 6.5s | 我的脖子还能画一个横八字…… |
| 8 | SLEEP | asleep | MOVE_SLEEP | 5.5s | 困了的话，我会低下头轻轻呼吸…… |
| 9 | WINK | wink | （不动） | 3.5s | 我还会眨眼，也能朝你抛个媚眼。 |
| 10 | GLANCE | glance | （不动） | 3.5s | 眼睛可以左右转动…… |

设计要点：

- **动作不直接写舵机**。每步走 `behavior_request_move()` → emotion_fsm，
  和 PLAY 页按钮完全同一条路（限位 / 增益 / 总线仲裁照旧）。
- **表情走 face_app 临时表情层**（`face_app_play(pose, hold)`），到点自动回落。
- **每步开始都 `mood_fsm_touch()`**：整套约 100s，不自报互动的话 60s 后会被
  心情状态机拖进 BORED、脸和动作全被接管，演示就走样了。
- 等播报期间脸上摆 `listen`（在听喇叭说话），不会"嘴上在说 A、脸上演着 B"。

---

## 3. 串口协议（UART0，纯 ASCII 文本行，与 printf 日志共线）

### 上行（板 → PC）

| 行 | 含义 |
|---|---|
| `DEMO:<NAME>:<IDX>` | 请求播报第 IDX 步；板子随后进入等待 |
| `DEMOEND` | 整套自然结束 |
| `DEMOABORT` | 被用户（点屏幕）或命令打断 |

### 下行（PC → 板）

| 行 | 含义 |
|---|---|
| `DEMOGO` | 开始整套（串口助手敲这个也能触发，不用碰屏幕） |
| `DEMOQUIT` | 立刻停止 |
| `DEMORUN` | 播报放完了，现在开演这一步 |
| `DEMOWAIT:<ms>` | 播报约这么长；板子把等待窗口放宽（上限 12s） |

时序（PC 在场时）：

```
板                          PC
│ DEMO:HAPPY:1 ────────────►│
│                           │ 读 assets/demo/HAPPY.wav
│ ◄──── DEMOWAIT:6700 ──────│   （先放宽等待窗口，再开播）
│                           │ PCM ──► ESP32 ──► 喇叭（约 5.3s）
│ ◄──── DEMORUN ────────────│   （放完才发）
│ 表情 happy + MOVE_HAPPY    │
```

**降级规则（谁都不能把演示卡死）**：

- PC 没接 / 没这步的 WAV / 主链路忙（正在对话）/ ESP32 没连 → PC 立刻回 `DEMORUN`；
- PC 一声不吭 → 板子等 `DEMO_WAIT_DEFAULT_MS`(2.5s) 自己往下走；
- `DEMOWAIT` 上限 `DEMO_WAIT_MAX_MS`(12s)，一行坏数据最多偷走 12 秒。

### 板端命令拦截位置

`app_core.c` 的文本行分流里，顺序是：`QR:/NET:`（配网）→ `DEMO*`（本模块）→
`emotion_fsm_feed_line()`。不拦截的话这些行会被情感状态机当 "unknown" 打日志。

---

## 4. PC 侧

| 文件 | 职责 |
|---|---|
| `demo_voice.py` | 播报词表 + edge-tts 预合成到 `assets/demo/<NAME>.wav` + manifest。`--check` 只看状态；`--missing` 只补缺的；`--voice zh-CN-YunxiNeural` 换音色 |
| `demo_bridge.py` | 收 `DEMO:` 行 → 放 WAV → `DEMOWAIT` / `DEMORUN`。缺音频 / 链路忙都立刻放行 |
| `app/link.py` | `take_demo_requests()` 从板子日志里切 DEMO 行（**带跨轮半行缓冲**：256 字节的读块会把一行切成两半）；`write_line()` 下行文本 |
| `app/service.py` / `main.py` | 把 `pipe.say_pcm` 注入 DemoAnnouncer（复用主语音链路：PC → USB 串口 → ESP32 → 喇叭） |

自测（不连硬件）：

```
D:\Baozi\_probe\test_demo_bridge.py     # 21 项断言：行解析 / 半行拼接 / 三种降级
_probe/qr/ui_sim2.exe demo              # 固件侧协议时序 + DEMO 页渲染（不上板）
```

---

## 5. 已踩过的坑（改这块前先读）

1. **`ui_add_text()` 只存指针不存字符串**。进度行原来用局部 `char line[24]` +
   snprintf，出门就悬空 → 那一行永远画不出来、还不报错。必须用 `static` 缓冲
   （位图链上踩过同一个坑，见 `qr/net_card.c` 的注释）。
2. **播报窗口必须先放宽再开播**。板子默认只等 2.5s，一句播报 5 秒 —— 顺序反了
   板子会在喇叭开口前就开演。
3. **演示必须自报互动**（`mood_fsm_touch`），否则 100 秒的演示演到一半"睡着"。
4. **卡片层会盖住脸**：START 点下后必须 `cards_close()` 回表情态再开演
   （与 PLAY / FACES 页同一套做法）；且要先收卡片、后 start —— `cards_close()`
   内部会把脸设回当前情绪，反过来会把刚摆好的 listen 脸抹掉。
5. **MOVE_IDLE 一个变体就要 18~23s**，是阻塞播放；演示只看前 9 秒就打断进下一步
   （`s_req_move` 会触发 `moves_interrupt()`，这是既有机制）。

## 6. 没做 / 可以加的

- 演示过程没有逐项上报 PC 的结构化"演示完成"回执（目前靠串口日志 `[demo]` 前缀看）；
- 步骤表改了要三处同步：`demo_show.c k_steps[]` ↔ `demo_voice.py DEMO_SCRIPTS`
  ↔ `assets/demo/*.wav`（manifest 记了生成时的词表，跑 `demo_voice.py --check` 可查）；
- 屏幕卡片层（习惯 / 日程 / 番茄钟 / 配网）没进演示脚本 —— 那些是"数据要 PC 喂"
  的功能，空数据演出来是空的，需要时单独加一步自动翻页。
