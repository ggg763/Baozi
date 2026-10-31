# 变更记录

本包对应分支 `v2`（[github.com/ggg763/Baozi](https://github.com/ggg763/Baozi/tree/v2)）。
`main` 是 2026-08 的 PC 单端原型（ear-mouth 架构），仅作历史保留。

按时间倒序。每条记录「为什么改 / 改了什么 / 怎么验证」。

---

## 2026-10-31 · 二维码配网上线（PC 自动下发 + UART0 行缓冲修正）

### 为什么

TD1601 的 NET 页一直显示 `waiting for network / from ESP32 over UART3`。查下来
**板子侧其实早就写完了**（`qrgen` 编码器、`net_card` 绘制、`net_link` 的
`QR:`/`NET:` 解析都在），缺的是三件事，而且都是一句话就能补的：

1. **没人发码**：ESP32 固件里没有 `QR:`/`NET:` 的下发代码，`pc_baozi/main.py`
   也没接，只有台架脚本。
2. **PC 侧前缀表漏了**：`td1601.py` 的 `_CTRL_FOR_PREFIX` 只认
   `EMO/INTENT/INTENSITY/TRACK`，`send_line("QR:...")` 会被打成
   `[td] 未知命令前缀，已忽略` 后**静静丢掉**。
3. **UART0 收行上限 64**：`src/uart_hal.c` 的 `LINE_MAX=64`，而
   `line_feed()` 对超长行是**整行丢弃**。UART3 那条路（`uart_esp.c` 的
   `ESP_LINE_MAX`）早就是 160，唯独 PC 直连的 UART0 漏了。

### 改了什么

| 端 | 文件 | 改动 |
|---|---|---|
| PC | `pc_baozi/wifi_cred.py`（新增） | `netsh` 读本机 SSID/口令 → 拼 `WIFI:` 串 → 三条上限（159 字节 / 127 字节 / ASCII）校验。规则只此一份 |
| PC | `pc_baozi/td1601.py` | `_CTRL_FOR_PREFIX` 补 `"QR": 0x0B`、`"NET": 0x0B`（CTRL_TD_TEXT） |
| PC | `pc_baozi/main.py` | 新增 `--no-qr` 与 `_start_provision_qr()`：连上 ESP32 → 停 3s（等板子 `net_link_init()` 的 reset 过去）→ 推 `NET:1` + `QR:`，只推一次 |
| PC | `pc_baozi/config.py` | 新增 `provision_qr_enabled = True` |
| PC | `pc_baozi/tools/push_qr.py`（新增） | PC→UART0 直连的兜底工具，零参数即自动读 WiFi + 选串口 + 读回 `[NET] qr set` 给结论 |
| 固件 | `src/uart_hal.c` | `LINE_MAX` 64 → 160（bss +384B） |
| 固件 | `UserFolder/app_core.c` | 取行缓冲 `line[64]` → `line[160]` |
| 文档 | `UserFolder/doc/qr_provision.md` | §5 改写为"主路径 = main.py 自动推"，补前缀表这个坑；§7 补两条 UART 的行缓冲 |

### 怎么验证

- 固件全量编译通过：`text 109604 / data 652 / bss 60624`。
- `qr_dump`（`_probe/qr/`）实测本机真实 payload 43 字节 → `VER 4 SIZE 33`；
  127 字节满容量 → `VER 8 SIZE 49`，卡片区按 `pick_scale(200)` 放大后不溢出。
- 用 mock `send_frame` 验证 `QR:`/`NET:` 现在带 `ctrl=0x0B` 发出，
  `EMO:` 仍 `0x09`，`TRACK:` 仍 `0x0B`。
- `push_qr.py --dry-run / --no-pass / --ip / --raw` 四种组合输出正确；
  中文 SSID、超长行、payload 超限都被提前拦下。
- 屏上出码：`python main.py` 后控制台打印 `[qr] 配网码已推给屏幕：SSID=...`。

### 注意

- 只推一次是**有意**的：板子每收到 `QR:` 都会 `ui_app_show_net()` 跳回 NET 页，
  NET 页按设计不自动回落，周期重推会打断正常对话。想安静开机用 `--no-qr`。
- ESP32 侧一行没改 —— 复用的是既有的 `CTRL_TD_TEXT`（"任意一行，原样转发给
  TD1601"）通道，和 `TRACK:ON` 完全同路。

---

## 2026-10-31 · TD1601 固件存档同步到当前工程

`03_TD1601/baozi` 这份存档落后于真实工程（SDK 内 `projects/baozi`）：
17 个文件缺、41 个文件内容不同，其中就包括**整个配网模块**（`UserFolder/qr/*`、
`src/net_link.c`、`include/net_link.h`）以及 `demo/`、`face_picker.*`、
`voice_link.*`。照原样推上仓库，等于发布一份缺功能的固件。

现已把工程当前状态全量覆盖到存档（排除 `Obj/`、`Lst/`、`face_report.json` 等
构建产物与生成物）。此后改固件的流程是：**先改 SDK 工程，再同步到本存档**，
不要直接改存档（它不在编译路径里，改了不起作用）。

---

## 2026-10-28 · build.bat 构建缓存自愈

拆分 `wifi_secrets.h` 时踩到一个坑：临时把该文件移走再放回，下一次编译直接报

```
xtensa-esp-elf-g++.exe: fatal error: cannot specify '-o' with '-c', '-S' or '-E' with multiple files
```

**不是代码问题**，是 `%TEMP%\baozi_build` 里残留的 `.o` 让编译命令行错乱
（源文件集合变了，缓存没失效）。清掉缓存即恢复：`build.bat clean`。

现在 `build.bat` 会记一份源文件清单指纹（`*.h *.cpp *.ino` 的文件名列表）：

- 清单不变 → 保留缓存，增量编译（约 1 分半）
- 清单变了 → 自动清空缓存再编（避免再撞这个假错误）
- `build.bat clean` → 强制清空

顺带把脚本里的注释改回纯 ASCII：cmd 按系统代码页解析 `.bat`，
UTF-8 中文注释里的括号/破折号会被拆成命令行，直接让脚本执行错乱（实测如此）。

---

## 2026-10-28 · 敏感信息移出仓库

仓库是公开的，而固件/配置里散落着明文 WiFi 密码、局域网 IP、真实 AppID，
以及编进 `.bin` 的一份拷贝。全部迁到 `.gitignore` 覆盖的本机文件里。

### 1. WiFi 凭据 → `wifi_secrets.h`

`config.h` 不再写 SSID/密码，改为：

```c
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#warning "..."
#define WIFI_SECRETS_MISSING 1
static const char* const WIFI_SSID_LIST[]     = { "YOUR_WIFI_SSID" };
static const char* const WIFI_PASSWORD_LIST[] = { "YOUR_WIFI_PASSWORD" };
#endif
```

- 仓库只留 `wifi_secrets.example.h` 模板
- `WIFI_NET_COUNT` 改由数组长度推出（`sizeof`），不会再出现"加了热点忘了改计数"
- 缺少 secrets 文件时**仍可编译**（退回占位值），固件启动会在串口喊
  `!! 没有 wifi_secrets.h`；实测占位固件 1,057,846 B、正常固件 1,058,082 B
- `AP_PASSWORD` 一并移入 secrets（`#ifndef` 兜底）

### 2. 局域网 IP → 通用占位 / 本机配置

| 位置 | 改前 | 改后 |
|---|---|---|
| ESP32 `STATIC_IP/GW/DNS` | 真实网段 | `192.168.1.x` 占位（`USE_STATIC_IP=0` 时本就不生效） |
| ESP32 注释里的调试 IP | 真实 | `192.168.1.100` |
| PC `config.py` / `config.json` 的 `esp32_ip` | 真实 | 空字符串（走 UDP 发现） |

PC 侧新增 **`config.local.json`** 覆盖机制（`Config.load` 里 `config.json` 之后读它，
同名键覆盖）。IP、串口、外部 LLM 的 key 都该写这里，模板 `config.local.example.json`。
实测：无该文件时 `esp32_ip=''`；建一个含 `192.168.1.77` 的文件后读到该值。

### 3. 小程序：AppID 与后端地址

- `project.config.json` 的 `appid` → `touristappid`（测试号）
- `app.js` 的 `apiBase` → `http://127.0.0.1:8000`
- 真机调试的局域网地址改为**运行时**设置，不再写进源码：
  `wx.setStorageSync('apiBase', 'http://192.168.x.x:8000')`（`utils/request.js` 优先读缓存）
- `project.private.config.json`（开发者工具按人生成）改为不入库

### 4. 预编译固件不再入库

`firmware/*.bin` 里编进了 WiFi 明文密码，等于把密码换个格式提交一次。
已 `git rm --cached`（本地文件保留），`.gitignore` 去掉原来的例外规则。
需要时跑 `02_ESP32/build.bat` 重新生成。

### 5. 仍未处理：历史提交里的旧凭据

`.gitignore` 只能挡住"以后"。**旧密码仍在 Git 历史里**（`49fbc14`、`326e02e` 等），
公开仓库上一查就有。要彻底清掉得重写 `v2` 分支历史并强推，属破坏性操作，
需要确认后再做；另一条路是把仓库转为 Private。

---

## 2026-10-28 · 实机联调：修掉「板子上不了线」的真凶

上一轮编译通过后烧进真板子，暴露出 WiFi 模式从未被真正跑过。

**硬件**：ESP32-S3（QFN56 rev0.2，16 MB Flash + 8 MB 内嵌 OPI PSRAM），
板载 CH340 → `COM14`，MAC `28:84:85:4A:31:20`。

### 1. 致命 bug：`discoveryLoop()` 从来没有被调用 ★

**现象**：板子连上了热点（`IP=192.168.1.204, RSSI=-41 dBm`），PC 能 ping 通它、
`http://192.168.1.204/` 能打开，但 PC 端永远等不到连接；板子串口每 5 秒刷
`[net] 还没收到 PC 广播（当前占用兜底地址 0.0.0.0）`。

**根因**：`setup()` 里调了 `discoveryBegin()`（绑定 8766）就再没人调
`discoveryLoop()` —— 端口开着却没人 `parsePacket()`，PC 信标全烂在 socket 缓冲区里。
全工程 grep 只有注释提到 `discoveryLoop`，没有一处调用。

**修法**：`loop()` 开头补上，且必须排在 `linkLoop()` 之前（先拿到地址才能连）：

```cpp
#if !USE_USB_LINK
  discoveryLoop();   // WiFi 模式：先发现 PC，再连它
#endif
  linkLoop();
```

**验证**（实机，本机 PC + 手机热点）：

```
[disc] >>> 已发现 PC: 192.168.1.232（UDP 发现成功）
[net] 已连接 PC 服务器
```

PC 侧同时看到：

```
[link] ESP32 已连接: 192.168.1.204:62288
[link] 设备上线: esp32s3cam/baozi-voice/16k-mono [+cam] fw/2026-10-28b
[state] LISTENING
[pipe] 检测到语音开始
```

即：发现 → TCP → HELLO → 麦克风音频 → VAD 全部打通。

### 2. WiFi 模式下一个字都看不到

`logEmit()` 在 WiFi 模式走 `Serial.write()`，而 `CDCOnBoot=cdc` 时 `Serial`
是**原生 USB CDC**；板子只引了 CH340（UART0），所以日志全丢。这次能定位到
bug 1，靠的就是先把它修好。

改法：`log.cpp` 在 WiFi 模式下同时写 `Serial0`（GPIO43/44，CH340 那一路），
`setup()` 里先 `Serial0.begin(115200)`。UART0 在本工程没有别的用途（TD1601 走
UART2，PC 在 WiFi 模式下走 TCP）。USB 模式完全不受影响。

### 3. 顺手把 HELLO 里的固件标记对齐

`kHelloText` 里是 `fw/2026-11-05a`（一个未来日期），改为 `fw/2026-10-28b`，
PC 侧日志里能直接看出板子跑的是哪一版。启动横幅同步更新。

### 4. PC 端加了「广播被过滤」的兜底（未启用，留在代码里）

排查过程中一度怀疑手机热点过滤广播，于是给 `discovery.py` 加了兜底：
12 秒还没收到板子信标，就扫本网段 80 端口、用首页标题 `包子控制台` 认板子
（实测 254 个地址 1.5 s），拿到 IP 后改用**单播**信标。

**后来证实这条路不用走**：热点广播是通的，之前的"收不到"全是 bug 1 造成的。
兜底逻辑保留但默认惰性（正常网络 2 秒内就发现，永远不触发扫描），
留给现场可能遇到的其他热点。

### 5. 新增两个脚本：`02_ESP32/build.bat`、`02_ESP32/flash.bat`

因为板级参数只存在 Arduino IDE 的界面状态里，换台机器/手滑选错板子就会
报 `esp_camera.h: No such file or directory`（只有 esp32/s2/s3 目标带摄像头组件）。

- `build.bat`：把 FQBN 写死（`esp32s3 / 16M / OPI PSRAM / 3MB APP / USB CDC`），
  直接调 Arduino IDE 自带的 arduino-cli，产物自动拷进 `firmware/`
- `flash.bat COMx`：调 core 自带的 esptool 烧三件套；不带参数则列串口

两个脚本都已实跑验证（编译 0 error / 0 warning；烧录 Hash verified）。

**最终固件**：1,058,082 B = 33% of 3 MB APP；全局变量 104,600 B = 31%。

---

## 2026-10-28 · 换热点 + ESP32 首次真正编译通过

### 1. 热点从 cowork 手机切到本人手机

路由设备换了，ESP32 必须跟着换凭据。

**改了什么**（`02_ESP32/esp32_baozi/config.h`）

| | 改前 | 改后 |
|---|---|---|
| `[0]` SSID | 旧热点 | 新热点（本人手机） |
| `[0]` 密码 | （旧） | （新，见文件） |
| `[1]` | 旧备选 | 原样保留作备选 |

`WIFI_NET_COUNT` 仍为 2：删掉已不存在的那个，最坏开机等待从 24 s 降到 12 s。

**为什么不用改 IP**：ESP32 的 `SERVER_HOST` 是 `0.0.0.0`，连上网后靠 UDP 8766 向
`255.255.255.255` 广播 `ESP32:<ip>:<port>` 找 PC。PC 侧 `discovery.py` 同理。
所以换网段（这次从旧热点换到 `192.168.1.0/24`）只需换 SSID/密码。

**换网后必做**：`01_PC/pc_baozi/allow_firewall_lan.bat`（管理员），放行 TCP 8765 + UDP 8766。
当前热点被 Windows 判为「公用网络」，默认拒绝入站。

**顺带确认的三项硬性约束**（任一不满足就连不上）：
2.4 GHz（通道 6 ✅）、WPA2-个人 ✅、SSID 纯英文 ✅。

### 2. ESP32 代码在 3.2.x 上编不过 —— 两处 API 是 3.3.0 才有的

首次真正执行编译时暴露。此前只有预编译 bin，没人在这台机器上编过。

| # | 报错 | 原因 | 修法 |
|---|---|---|---|
| 1 | `'ESP_ARDUINO_DMA_BUF_ALIGN' was not declared` （`audio_in.cpp:314`） | 该宏 3.3.0 才加进 `cores/esp32/esp32-hal.h` | 在 `audio_in.cpp` 加 `#ifndef` 兜底，按上游同一套逻辑推导（L2 行大小 → L1 → 4 字节） |
| 2 | `no matching function for call to 'WebServer::send(int, const char[11], MemoryStream&, size_t&)'`（`esp32_baozi.ino:695`） | `send(..., Stream&, size_t)` 重载 3.3.0 才加 | `handleCapture()` 改用 `setContentLength(len)` + `send(200,"image/jpeg","")` + 1 KB 分块 `sendContent()`；同时删掉因此不再使用的 `MemoryStream` 类 |

两处都是**编译兼容层，不改行为**：宏取值与 3.3.x 完全一致；分块写与 Stream 重载
在 HTTP 层等价（都是先声明 Content-Length 再写裸字节），且分块避免了把整帧 JPEG
复制进 `String` 的峰值内存与堆碎片。

### 3. 重新构建固件并入库

`02_ESP32/esp32_baozi/firmware/` 三件套已替换为本机构建产物，旧版（2026-10-25）
移到 `firmware/archive_20261025/` 保留（同时仍在 Git 历史里）。

**验证**

| 项 | 结果 |
|---|---|
| 编译 | 通过，0 error / 0 warning |
| 固件体积 | 1,057,502 B = 33% of 3 MB APP |
| 全局变量 | 104,592 B = 31%（余 223,088 B） |
| 凭据入镜像 | `strings` 能在 bin 里搜到 SSID / 密码明文（故本次起不再入库） |
| 板级参数 | `FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc` |

**未验证**：实机联网、三端联调（无硬件在手）。

### ⚠️ 待处理：WiFi 密码在公开仓库里

`config.h` 的明文凭据（本次的、以及历史上那两条）以及
`firmware/*.bin` 都会出现在公开仓库中。若要收紧，建议二选一：

- 仓库转 Private；
- 或把凭据抽到 `wifi_secrets.h`（`.gitignore` 排除，仓库只留 `.example` 模板）。

第二种会牺牲「clone 下来直接能编」，需要接手人自己填一次，故未擅自改动。

---

## 2026-10-28 · 首版交付

- 三端源码整合：`01_PC` / `02_ESP32` / `03_TD1601`（源码存档）/ `04_DOCS`
- 修 `server.py`：`config.json` 从未被读取（`make_config()` 把 `None` 交给
  `Config.load()` 直接返回硬编码默认值）。改为无显式 `--config` 时自动加载同目录配置。
- 隐私清理：`memory.json` 换空模板；删除含 21 条日程 / 106 条打卡记录的 `baozi.db`。
- 建立 `v2` 分支并推送。
