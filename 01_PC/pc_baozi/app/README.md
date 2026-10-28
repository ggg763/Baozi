# app/ —— 组员 app 整合层（习惯 / 日程 / 提醒 / 小程序）

> 来源：组员交付的 `app.baozi/baozi_demo`（PC 侧）+ `esp32_baozi/app.baozi.arduino`（web_console）。
> 原始目录仍然保留在 `pc_baozi/app.baozi/` 与 `esp32_baozi/app.baozi.arduino/` 作为参考，
> **运行用的是本目录这份**（下面的改动都记在本 README 里）。

## 1. 这一层负责什么

| 能力 | 入口 | 说明 |
|---|---|---|
| 习惯打卡 / 日程 / 设置的数据与接口 | `backend/`（FastAPI + SQLite，默认 `127.0.0.1:8000`） | 手机小程序读写这里；数据库是 `backend/baozi.db` |
| 手机端界面 | `miniprogram/`（微信小程序） | 用微信开发者工具打开这个目录 |
| 数据上屏（TD1601 卡片） | `link.py`（经 `board.py` 组帧） | 习惯看板 0x11/0x12/0x13、今日日程 0x14/0x15/0x16、提醒弹卡 0x17、对时 0x10 |
| 屏幕操作回流 | `link.py` | 屏幕打卡 0x90 / 催数据 0x92·0x94 / 弹卡关闭 0x95 → 写回后端 |
| 到点提醒的措辞 | `phrases.py` + `remind_pool.json` | 洗牌袋防重复；池子空了自动用内置话术 |

## 2. 它怎么跟主程序连（整合后的形态）

```
            手机小程序
                │ HTTP
                ▼
  main.py ──► 后端子进程 :8000        (AppService._start_backend)
     │            ▲
     │            │ 拉数据 / 写打卡
     │       桥接线程 app/link.py
     │            │ A5 帧（串口）
     │            ▼
     │        TD1601 屏幕（UART0，CH340 那个 COM 口）
     │
     └──► 提醒到点：link.speak() → pipeline.say() → TTS → ESP32 → 喇叭
```

- 启动：`python main.py`（或 `run.bat`）默认就带这一层；
  关掉用 `--no-app`，或 `config.json` 里 `app_enabled: false`。
- **没有再起第二个 TCP 8765 语音服务端**。组员原版 `baozi_voice.py` 那个服务端
  已经废弃：端口归 `pc_baozi/voice_server.py`，再起一个必然撞"地址已在使用"。
  提醒语音改走主链路 `pipeline.say()`，正在对话时自动让路（只弹卡不出声）。

## 3. 串口 / 端口分工（最容易踩的坑）

| 用途 | 谁占用 | 备注 |
|---|---|---|
| ESP32（语音/图像/情感） | `main.py` 的 transport（WiFi TCP 8765 或 USB 串口） | 情感走 ESP32 → UART(GPIO14) → TD1601 PB18 |
| TD1601 屏幕卡片 | 本层 `link.py` 的串口 | 默认自动握手认口（发 0x10 对时，认 `[UART] sync time` 回应） |
| 手机后端 | 本层后端子进程 :8000 | 已被占用时不会重复启动 |

所以 **TD1601 的两条通道是并行的**：UART3（ESP32 转发，发情绪字节帧）和
UART0（本层串口，发习惯/日程/提醒 A5 帧）。两者互不冲突，也不要在同一个
COM 口上跑两个程序。

指定屏幕串口（自动识别不出来时）：`config.json` 的 `app_board_port` 写 `"COM9"`。

## 4. 单独运行（调试用，不进主程序）

```powershell
cd D:\CDK2\pc_baozi

# 只跑后端（手机端联调）
.\.venv\Scripts\python.exe -m uvicorn main:app --host 127.0.0.1 --port 8000 --app-dir app\backend

# 只跑屏幕桥接（语音不参与，提醒只弹卡）
.\.venv\Scripts\python.exe app\link.py                    # 自动认串口
.\.venv\Scripts\python.exe app\link.py -p COM9 --no-log    # 指定串口、不打印板子日志
.\.venv\Scripts\python.exe app\link.py --dry --once        # 不开串口，只打印会发什么
```

`app/link.py` 也支持被当模块调用：`import app.link` → `link.set_speaker(fn)` +
`link.run(SimpleNamespace(...))` + `link.stop()`。

## 5. 常见问题

**手机显示"后端连不上"**
1. 主程序是否在跑（`main.py` 会拉起后端）；看日志 `[app] 后端就绪`
2. 手机和电脑是否同一个 2.4G 热点
3. 换网络后电脑 IP 变了：小程序里的地址要更新（原组员用 `sync_ip.bat` 改 `app.js`）

**屏幕卡片不刷新**
- 看主程序日志有没有 `[i] 串口 COMx 已打开`、`[↓] 下发 N 个习惯`
- 没有就是串口没认出来：确认屏幕已上电（板子启动约 11 秒才开串口），
  或直接在 `config.json` 写死 `app_board_port`
- 排障时把 `app_board_log` 设 `true`，能看到板子的 printf 日志

**到点不响 / 只弹卡不出声**
- 只弹卡是设计行为之一：正在对话（THINKING/WAITING/SPEAKING）时提醒不插话
- 完全不弹卡：查 `app_board_port`、以及后端有没有拉到数据
- 连续两次提醒至少隔 `remind_cooldown_min` 分钟（小程序设置页可改）

**改了免打扰设置不生效**
- 桥接每 60 秒回后端拉一次设置，改完一分钟内生效

## 6. 相对组员原版的改动清单

| 文件 | 改动 | 原因 |
|---|---|---|
| `link.py` | 删掉 `baozi_voice` 依赖与 `_get_voice_link()`/`speak_local()`；改成 `set_speaker(fn)` 注入 | 与 `voice_server.py` 抢 8765 端口；提醒语音统一走主链路 |
| `link.py` | `main()` 拆出 `run(args)` + 模块级 `STOP` 事件 + `stop()` | 需要在后台线程里跑、能优雅停 |
| `link.py` | 串口选不出/打不开由 `sys.exit(1)` 改成 `raise RuntimeError` | 后台线程里 sys.exit 会连带杀掉语音链路 |
| `link.py` | 顶部改成包内相对导入（`from . import board, phrases`） | 从 `baozi-link.py`/`remind_phrases.py`/`baozi-uart-test.py` 改名 |
| `board.py` | 原样（仅改名） | 组帧/点阵只有一份，避免两边布局理解走偏 |
| `phrases.py` | 原样（仅改名），池子 JSON 同目录 | — |
| `backend/db.py` | `DB_PATH` 由相对路径改成基于文件位置的绝对路径（可用 `BAOZI_DB` 覆盖） | 主程序从 `pc_baozi/` 启动，相对路径会新建空库，表现是"手机和屏幕全空" |
| `service.py` | 新增 | 编排：后端子进程 + 桥接线程 + 生命周期 |
