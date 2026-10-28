# 包子 BaoZi

参赛 CIE 全国 RISC-V 大赛 **TD1601 赛道**的桌面机器人。两套代码在同一个仓库里：

| 目录 | 内容 | 语言 / 工具 |
|---|---|---|
| 仓库根 | **PC 大脑**：语音识别、TTS、ollama 对话、情绪状态机、串口通信 | Python |
| `td1601/` | **TD1601 固件**：屏幕表情、卡片 UI、舵机动作、扫码配网 | C（RISC-V 裸机） |

设计原则：**PC 只下发语义**（emotion / intent），不下发动作；表情、舵机、LED
全部由 TD1601 自主决策。

---

## 目录

```
.
├── main.py              PC 主程序（ASR → LLM → 情绪 → 串口）
├── communication.py     PC ↔ TD1601 串口层（EMO:/INTENT: 文本行）
├── net_share.py         把本机 WiFi 凭证经局域网交给 ESP32（配网 ①）
├── brain/ emotion/ robot/ memory/ prompts/
├── tools/push_qr.py     台架联调：直接向 TD1601 推配网命令
└── td1601/              TD1601 固件（见下）
```

## 跑起来

**PC**

```bash
pip install -r requirements.txt   # 或按 docs/project_code.md
python main.py
```

**TD1601**

源码必须放在 CDK 工程的 `projects/baozi` 下才能被 CDK 认到，所以这个目录是
用 `git subtree` 并进来的副本，**日常开发仍在 CDK 里那份**（见下节）。

```bash
cd td1601
export PATH="<CDK>/CDKRepo/Toolchain/XTGccElfNewlib/V3.4.0/R/bin:$PATH"
mingw32-make -f Baozi.mk all -j4
```
（末尾 `crc32` 一步在 CDK 之外跑不了，属正常；看 `size of target` 即可。）

## 固件为什么在 `td1601/` 里

固件原本是一个独立仓库（`projects/baozi/.git`），2026-10-28 用
`git subtree` 并入本仓库的 `td1601/`，**原仓库的 6 次提交历史完整保留**
（`git log -- td1601` 能看到）。

麻烦的地方在于：CDK 要求源码在 `CDK/official_SDK/.../projects/baozi`，
所以那份才是"能编译的那份"。两边的同步规则：

```bash
# 固件改完、在本仓库的 feat/qr-provision 分支上：
git subtree pull --prefix=td1601 "<CDK 路径>/projects/baozi" feat/qr-provision
git push
```

反过来（别人从 GitHub 拉了想编译）直接把 `td1601/` 整个目录拷到 CDK 的
`projects/baozi` 即可，`.git` 不用带。

>`td1601/Obj/`、`Lst/`、`*.o *.d *.elf *.ihex *.bin` 都在 `.gitignore` 里，
不入库 —— 它们由 CDK / make 重新生成。

## 分支

| 分支 | 内容 |
|---|---|
| `main` | 稳定线 |
| `feat/qr-provision` | 扫码配网（PC → 局域网 → ESP32 → UART3 → 屏）+ 时间显示改圆角数码 |

## 最近两轮改动

* 配网：`td1601/UserFolder/doc/qr_provision.md`（链路、`QR:` / `NET:` 协议、
  ESP32 侧可粘贴片段、联调顺序）
* 全部改动逐条记录：`td1601/CHANGELOG.md`
