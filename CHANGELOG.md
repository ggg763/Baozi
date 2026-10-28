# 变更记录

本包对应分支 `v2`（[github.com/ggg763/Baozi](https://github.com/ggg763/Baozi/tree/v2)）。
`main` 是 2026-08 的 PC 单端原型（ear-mouth 架构），仅作历史保留。

按时间倒序。每条记录「为什么改 / 改了什么 / 怎么验证」。

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
