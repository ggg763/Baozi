# 交付说明（给接手的同事先看这一页）

这个包是 TD1601 端**全部工程代码 + 文档**（`projects/baozi`），已排除 CDK 构建产物
（`Obj/` `Lst/` `.cache/` `.cdk/` 与生成的 makefile），解压后直接可用。

## 上手三步

1. 用 **CDK** 打开 `project.cdkproj`（`Baozi.mk` 等生成文件会在打开/编译时自动重建）；
2. 先读 **`UserFolder/doc/README.md`** —— **工程唯一入口文档**：
   定位与分层、目录与编译登记规则、各模块函数接口、串口协议与命令、写码约定、
   已踩过的坑、**联调 checklist（四阶段）**、待办与已知问题；
3. 编译烧录，串口 **115200-8N1**。当前 `src/main.c` 的 `main()` 调的是 `emotion_test()`
   （情感状态机·串口交互测试），**切正式运行只需把那一行换成 `emotion_fsm_run()`**。

## 两条必须知道的事

- ⚠️ **只有 `package.yaml` 的 `source_file` 里登记的文件才会被编译** ——
  新写 `.c` 必须去登记，否则现象是"函数找不到"或"改了没反应"。
- ⚠️ **串口输出一律纯 ASCII 英文**（中文只写在注释里）—— 串口终端没有 UTF-8 渲染能力。

## 模块速查

| 目录 | 内容 |
|---|---|
| `src/` | 入口 `main.c`（含"测试入口切换区"）、`board_init.c`（console）、`video.c`（刷屏） |
| `UserFolder/servo_test/` | 舵机控制层 `servo_core`（限位 + 只写 compare）+ 标定/回归三个测试 |
| `UserFolder/movements/` | 动作库：9 个状态 · 24 个变体（随机呼出，消除机械感） |
| `UserFolder/emotion/` | 情感状态机三层：情绪 / 心情 / 编排（`EMO:`/`INTENT:` 协议） |
| `UserFolder/tracking/` | 红外跟随：`track_yaw`（单轴，已验证）/ `track_dual`（双轴） |
| `UserFolder/test/` | `ir_test` / `ir_state_test` / **`emotion_test`（串口交互测试）** |
| `UserFolder/gfx/` | 屏幕图形库（480×480 QSPI，已跑通） |
| `UserFolder/doc/` | **文档目录（先读其中的 `README.md`）**：引脚、复用表、跟随方案 |

## 不在本包内、但联调需要的

- **PC 侧代码**：`code/projects/Baozi_PC`（认知层：ASR / LLM / TTS；其中
  `communication.py` 里的 `EMO:`/`INTENT:` 是两端协议契约）；
- **硬件**：TD1601 EVB + 舵机两路（**PA0 = YAW**、**PA27 = PITCH**）+
  红外三路（**PB22 / PB23 / PB29**）；注意 TD1601 的 IO 是 **1.8V 域**，接 3.3V/5V 模块要电平转换。
