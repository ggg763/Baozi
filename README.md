# 包子 BaoZi · 桌面陪伴机器人

> CIE 全国 RISC-V 大赛 · TD1601 赛道参赛作品
> 交付包版本：2026-10-28
>
> **仓库分支说明**：本目录对应 `v2` 分支。
> `main` 是 2026-08 的 PC 单端原型（`brain/` `robot/` `baozis_ear.py` 那套 ear-mouth 架构），
> 3 个提交后停更，保留作历史，**不代表当前项目**。
> 克隆后请先切分支：`git checkout v2`

一个会听、会说、会看、有表情、能摆姿态的桌面机器人。三块处理器分工：

- **PC（Python）** 当"大脑"：语音识别、大模型对话、语音合成、视觉理解、记忆
- **ESP32-S3** 当"耳朵和嘴"：麦克风采集、喇叭播放、摄像头取图
- **TD1601（RISC-V）** 当"身体和脸"：480×480 表情、双舵机动作库、情绪状态机、习惯/日程卡片

核心设计原则是**语义与动作分离**：PC 只告诉下位机"现在是什么情绪、什么意图、多强"，
具体演哪段动作、用多大增益、表情怎么变，全部由 TD1601 自己决定。
换掉 PC 的模型、换掉说话方式，固件一行都不用改。

---

## 1. 目录结构

```
delivery/
├── README.md                  ← 本文件
├── 01_PC/                     PC 认知层完整源码（Python）
│   └── pc_baozi/
├── 02_ESP32/                  ESP32-S3 固件完整源码（Arduino）
│   └── esp32_baozi/
│       ├── *.ino *.cpp *.h    ← 12 个源文件（最新版）
│       └── firmware/*.bin     ← 预编译固件（不装 Arduino 也能烧）
├── 03_TD1601/                 TD1601 固件
│   ├── baozi/                 ← 源码存档（69 个 .c / 44 个 .h，只读）
│   ├── README_TD1601.md       ← 编译烧录方法 + 为什么必须放回 SDK
│   ├── 打开固件工程.bat        ← 双击跳到 SDK 里的工程目录
│   └── SDK母文件夹_projects.url
└── 04_DOCS/
    ├── 01_通信协议.md          ← 三套协议的完整字段定义
    └── 02_环境搭建与联调.md     ← 从零到跑通的每一步
```

**TD1601 有两个形态**：`03_TD1601/baozi/` 是**源码存档**（看代码、留底用），
**不能直接编译**；真正能编译烧录的工程必须留在 SDK 的 `projects/baozi`。
原因是 CDK 按相对路径解析 `../../components` 和 `../../boards`，拷出来就找不到。
详见 `03_TD1601/README_TD1601.md`。

---

## 2. 三端架构

```
        ┌──────────────────────────────────────────────┐
        │  PC · Python（认知层）                        │
        │  VAD → SenseVoice ASR → qwen2.5 → edge-tts   │
        │  视觉：YOLOv8n 优先，qwen3-vl 兜底            │
        │  记忆：长期画像 + 情绪状态                     │
        └───────────┬──────────────────┬───────────────┘
                    │ ①USB CDC / TCP    │ ③USB-TTL
                    │ A5 5A 二进制帧     │ A5 LEN CMD XOR
        ┌───────────▼──────────┐        │
        │  ESP32-S3（传输层）   │        │
        │  MIC→帧 / PCM→I2S    │        │
        │  摄像头 JPEG 分片     │        │
        └───────────┬──────────┘        │
                    │ ②UART2 GPIO14 115200（仅 PB18 单向）
                    │ 0xAA+code 或 "EMO:xxx" 文本
        ┌───────────▼──────────────────▼───────────────┐
        │  TD1601 · RISC-V（行为层）                    │
        │  emotion_fsm → movements(9态×24变体) → 双舵机 │
        │  v9 参数化表情（480×480，60 段 DMA）           │
        │  habit/sched 卡片 + 触摸交互                   │
        └───────────────────────────────────────────────┘
```

三条链路、三套协议，字段定义见 `04_DOCS/01_通信协议.md`。

| 链路 | 介质 | 协议 | 方向 |
|---|---|---|---|
| ① PC ↔ ESP32 | USB CDC 或 WiFi/TCP:8765 | `A5 5A` + type/flags/seq/len + payload(≤2048) | 双向 |
| ② ESP32 → TD1601 | UART2 `GPIO14` → `PB18`，115200-8N1 | `0xAA` + code(0~215)，兼容 `EMO:xxx\n` 文本 | **单向** |
| ③ PC ↔ TD1601 | 独立 USB-TTL → UART0 `PA17/PA18` | `A5` + LEN + CMD + DATA + XOR | 双向 |

> ②为什么只有一根线：PCB 只引出了 PB18。所以 ESP32 侧 `TD_UART_RX_GPIO 21`
> 标注为未使用，TD1601 侧 `uart_esp.c` 也只配置 RX、不发 banner。

### 传输模式必须两端一致

链路 ① 有两种走法，固件和 PC 配置**必须配对**，否则握手永远等不到：

| 固件 `esp32_baozi/config.h` | PC `pc_baozi/config.json` | 说明 |
|---|---|---|
| `#define USE_USB_LINK 1` | `"transport": "serial"` | USB 线直连，不需要同网段、不需要 IP |
| `#define USE_USB_LINK 0` | `"transport": "tcp"` | WiFi + TCP 8765，靠 UDP 8766 双向广播自动发现 |

**当前交付的组合是 `USE_USB_LINK 0` + `"transport": "tcp"`（WiFi 模式）。**
如果你的现场没有路由器，把固件改成 `1`、PC 改成 `serial` 即可，协议帧完全不变。

---

## 3. 快速开始

### 3.1 PC 端（先跑这个，不需要任何硬件）

```bash
cd 01_PC/pc_baozi

# 建虚拟环境（Python 3.11~3.13；包内不含 .venv）
python -m venv .venv
.venv\Scripts\activate

# 最小依赖（够跑通链路自测）
pip install numpy openai pyserial edge-tts miniaudio requests pillow pypinyin

# 离线自测：不需要硬件、不需要网络、不需要模型
python selftest.py
```

期望结果：`84 通过, 2 失败`。那 2 项是代码与测试的规格冲突（见第 6 节），
不是环境问题。

完整依赖（语音识别、视觉、后端）：

```bash
pip install -r requirements.txt
```

⚠️ **GPU 用户注意**：pypi 上 Windows 的 `torch` 是 **CPU 版**。
要用显卡必须走 PyTorch 官方源：

```bash
pip install torch==2.11.0+cu128 torchaudio==2.11.0+cu128 ^
  torchvision==0.26.0+cu128 --index-url https://download.pytorch.org/whl/cu128
```

（`cu128` 通道最高到 torch 2.11；`cu130` 有 2.14 但需要 R580+ 驱动。）

### 3.2 配置文件

`config.json` 与 `main.py` 同目录，**启动时自动加载**，不需要加 `--config`。
（2026-10-28 修复：此前 `make_config()` 把 `None` 直接交给 `Config.load()`，
而 `Config.load(None)` 会立即返回内置默认值 —— 也就是说这份文件**从未被读取过**，
里面调好的参数全都没生效。现在启动时会打印一行 `[cfg] 使用配置文件: ...`。）

想用另一份配置：`python main.py --config other.json`（显式指定优先）。

常改的键：

| 键 | 作用 | 当前值 |
|---|---|---|
| `transport` | `tcp` = WiFi，`serial` = USB 直连 | `tcp` |
| `llm_model` | 对话模型 | `qwen2.5:3b` |
| `vision_model` | 视觉兜底模型 | `qwen3-vl:2b` |
| `vad_threshold_db` 等 | VAD 灵敏度 | −33.0 / 起 −31 / 止 −30 |
| `play_target_s` | 推流目标水位（秒） | 0.35 |
| `echo_guard_ms` | 播放后屏蔽麦克风时长 | 600 |
| `app_enabled` | 习惯/日程/卡片总开关 | `true` |

### 3.3 接硬件前先确认链路

```bash
# 列出串口（找 ESP32，VID 通常是 303A 或 1A86）
python main.py --list-ports

# 不接硬件也能验证整条流水线（模拟 ASR + echo LLM + 提示音 TTS）
python server.py --asr-backend mock --llm-backend echo --tts-backend tone
```

### 3.4 ESP32 端

Arduino IDE 打开 `02_ESP32/esp32_baozi/esp32_baozi.ino`。

⚠️ **文件夹必须叫 `esp32_baozi`**（Arduino 硬性要求：`.ino` 所在文件夹名必须与文件名一致）。

开发板设置（少一项就会花式报错）：

| 项 | 值 |
|---|---|
| 开发板 | ESP32S3 Dev Module |
| Flash Size | 16MB |
| PSRAM | **OPI PSRAM**（不设这个摄像头和播放任务都起不来） |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB CDC On Boot | **Enabled** | USB 模式（`USE_USB_LINK 1`）必需；WiFi 模式下不影响 |

需要 **esp32 core ≥ 3.0.0**（用了 `ESP_I2S` 和 `esp_adc/adc_continuous`）。
零第三方库，全部是 core 自带。

不装 Arduino 也能烧（应急）：

```bash
esptool.py --chip esp32s3 -p COMx -b 921600 write_flash ^
  --flash_mode dio --flash_freq 80m --flash_size 16MB ^
  0x0     firmware/esp32_baozi.ino.bootloader.bin ^
  0x8000  firmware/esp32_baozi.ino.partitions.bin ^
  0x10000 firmware/esp32_baozi.ino.bin
```

### 3.5 TD1601 端

见 `03_TD1601/README_TD1601.md`。一句话：用 CDK 打开 SDK 里的 `projects/baozi`，
Download 烧录，串口 115200-8N1。

### 3.6 全系统启动

```bash
cd 01_PC/pc_baozi
run.bat                       # 语音 + 视觉 + 情感 + 卡片，全部拉起
```

`run.bat` 会自动：起语音服务 → 连 ESP32 → 注入视觉 → 起 FastAPI 后端(:8000)
→ 起屏幕桥接线程。关窗口即全停。

---

## 4. 硬件接线速查

| 连接 | 引脚 | 备注 |
|---|---|---|
| 麦克风 MAX9814 | ESP32 `GPIO1` | 必须 GPIO1~10（ADC2 与 WiFi 冲突） |
| I2S 功放 BCLK/LRC/DIN | `GPIO38 / 39 / 40` | 功放 SD/EN → `GPIO41` |
| ESP32 → TD1601 | `GPIO14` → `PB18` | **只有这一根**，115200-8N1 |
| 摄像头 | 板内固定 GPIO4~18 | OPI PSRAM 占 GPIO33~37，避开 |
| 舵机 YAW / PITCH | TD1601 `PA0` / `PA25` | PWM_CH0 / PWM_CH2 |
| 红外三路 | TD1601 `PB22 / PB23 / PB29` | 跟随 + 近距保护 |
| 触摸 | TD1601 I2C0 `PA21/PA22` | INT `PB2`，RST `PB1` |
| 屏幕 | TD1601 QSPI 4 线 | 480×480 ST7701S |

⚠️ **TD1601 的 IO 是 1.8V 域**。ESP32 的 3.3V TX 直连 `PB18` 会过压灌流，
必须做电平转换/分压。

---

## 5. 环境要求

| 端 | 要求 |
|---|---|
| PC | Windows 10/11，Python 3.11~3.13，8 GB 内存（跑 3B 模型） |
| PC 可选 | NVIDIA GPU（CUDA 12.8 驱动）—— ASR/VL 明显加速，YOLO 无收益 |
| PC 外部服务 | Ollama（`qwen2.5:3b` 对话、`qwen3-vl:2b` 视觉）、edge-tts 联网、wttr.in 天气 |
| ESP32 | Arduino IDE 2.x + esp32 core ≥ 3.0.0 |
| TD1601 | CDK + 工具链 `XTGccElfNewlib V3.4.0` + `mingw32-make` |

---

## 6. 当前状态与已知问题

### 已验证

| 项 | 结果 |
|---|---|
| PC 离线自测 | **84 通过 / 2 失败**（重建环境后结果一致） |
| PC 对话链路 | `qwen2.5:3b` 输出合法 JSON（reply/emotion/intent/intensity） |
| PC 视觉链路 | `qwen3-vl:2b` 走 Ollama，**100% GPU**，1.1–1.4 s/次 |
| PC 目标检测 | YOLOv8n 25 ms/帧（GPU 与 CPU 持平，小模型传输开销抵消） |
| PC 语音识别 | SenseVoiceSmall 本地缓存离线加载，`cuda:0`，0.6 s 推理 |
| TD1601 编译 | 通过，`text 96132 / data 644 / bss 48656` |
| 三端协议一致性 | 逐条比对通过（见 `04_DOCS/01_通信协议.md`） |

### 未验证（缺硬件）

- ESP32 实际编译（本机无 Arduino core；有预编译 bin 可烧）
- 三端联调（麦克风/喇叭/舵机/屏幕均未上电实测）

### 已知问题

1. **自测有 2 项失败**：测试 `3a2b` 要求"噪声不能顶掉已备好的回复"，
   而 `pipeline.py` 的 `_settle_after_rejected_utterance()` 注释写
   "2026-10 之后改为直接丢弃"。两个相反方向都声称在修同一故障，属规格冲突。
2. **LLM 冷启动前两轮会退化**：`llm.warmup()` 只发 `max_tokens=1` 的请求，
   实测首 2 轮可能返回 `reply='neutral'`（会被 TTS 念出来），第 3 轮起正常。
   绕过办法：启动后先随便说两句话；或改 `warmup()` 发一次完整 `chat_ex`。
3. **`config.json` 曾长期不生效（已修，2026-10-28）**：`make_config()` 原先把
   `None` 直接交给 `Config.load()`，后者立即返回内置默认值，导致这份文件
   **从未被读取**。当时实际跑的是 `config.py` 的硬编码默认，其中
   `llm_model` 还是本机不存在的 `qwen2.5:7b`。现已改为自动加载同目录
   `config.json`，启动时打印 `[cfg] 使用配置文件: ...` 可确认。
4. **VAD 门限有三处值**：`config.py` 默认 −38/−28、`config.json` 覆盖
   −31/−30（`vad_threshold_db` 另为 −33）、旧注释提到"原来 −30/−22"。
   现在以 `config.json` 为准（上一条修复后才真正成立）。
5. **`moves_yield` 让位门未回灌**（TD1601）：`TRACK:ON` 若恰好落在
   `moves_play_variant()` 播放前清标志那一瞬，中断可能失效，睡眠类长动作
   会与追踪抢舵机。详见 TD1601 工程内 `CHANGELOG.md` 的待办。

---

## 7. 交付边界

| 内容 | 位置 |
|---|---|
| PC / ESP32 源码 | 本包 `01_PC`、`02_ESP32` |
| TD1601 源码 | SDK 内 `projects/baozi`（不在本包，见 `03_TD1601`） |
| 预编译 ESP32 固件 | 本包 `02_ESP32/esp32_baozi/firmware/` |
| 模型权重 | **不随包分发**：`yolov8n.pt` 首次运行自动下载 |
| ASR 模型 | SenseVoiceSmall 首次运行从 ModelScope 下载（约 900 MB） |
| 对话/视觉模型 | 需本机 Ollama 拉取：`ollama pull qwen2.5:3b`、`ollama pull qwen3-vl:2b` |
| 长期记忆 | `memory.json` 已清空为初始值（原文件含真实姓名与生日，不随包分发） |
