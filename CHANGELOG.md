# 变更记录

本包对应分支 `v2`（[github.com/ggg763/Baozi](https://github.com/ggg763/Baozi/tree/v2)）。
`main` 是 2026-08 的 PC 单端原型（ear-mouth 架构），仅作历史保留。

按时间倒序。每条记录「为什么改 / 改了什么 / 怎么验证」。

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

**现象**：板子连上了热点（`IP=192.168.193.204, RSSI=-41 dBm`），PC 能 ping 通它、
`http://192.168.193.204/` 能打开，但 PC 端永远等不到连接；板子串口每 5 秒刷
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

**验证**（实机，本机 PC + vivo S30 Pro mini 热点）：

```
[disc] >>> 已发现 PC: 192.168.193.232（UDP 发现成功）
[net] 已连接 PC 服务器
```

PC 侧同时看到：

```
[link] ESP32 已连接: 192.168.193.204:62288
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
| `[0]` SSID | `shubei` | `vivo S30 Pro mini` |
| `[0]` 密码 | （旧） | （新，见文件） |
| `[1]` | `lanlll` | `lanlll`（保留作备选） |

`WIFI_NET_COUNT` 仍为 2：删掉已不存在的 `shubei`，最坏开机等待从 24 s 降到 12 s。

**为什么不用改 IP**：ESP32 的 `SERVER_HOST` 是 `0.0.0.0`，连上网后靠 UDP 8766 向
`255.255.255.255` 广播 `ESP32:<ip>:<port>` 找 PC。PC 侧 `discovery.py` 同理。
所以换网段（这次从旧热点换到 `192.168.193.0/24`）只需换 SSID/密码。

**换网后必做**：`01_PC/pc_baozi/allow_firewall_lan.bat`（管理员），放行 TCP 8765 + UDP 8766。
当前 `vivo S30 Pro mini` 被 Windows 判为「公用网络」，默认拒绝入站。

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
| 凭据入镜像 | `strings` 能在 bin 里搜到 `vivo S30 Pro mini` / 新密码 |
| 板级参数 | `FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc` |

**未验证**：实机联网、三端联调（无硬件在手）。

### ⚠️ 待处理：WiFi 密码在公开仓库里

`config.h` 的明文凭据（本次的、以及历史上 `shubei` / `lanlll` 两条）以及
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
