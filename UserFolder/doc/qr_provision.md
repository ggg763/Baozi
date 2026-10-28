# 扫码配网（v2.1）

> 适用：`projects/baozi` 分支 `feat/qr-provision`
> 相关文件：`UserFolder/qr/qrgen.c`（编码器）、`UserFolder/qr/net_card.c`（NET 页）、
> `src/net_link.c`（协议解析）、`_probe/qr/`（离线验证工具链）

---

## 0. 一句话

**PC 把自己所在局域网的凭证交给 ESP32，ESP32 把"该显示什么码"做成一行文本推给
TD1601，TD1601 自己算二维码、画在全屏或第 5 张卡片上。** 手机扫这个码就连上热点，
随后在热点里的小网页填家里 WiFi —— 屏幕上从"等扫码"走到"已连接 + IP"。

---

## 1. 链路

```
   ①  PC 侧 net_share.py
       读本机 netsh 的 SSID/口令
              │  UDP 广播 :8266  {"t":"wifi","ssid":…,"pwd":…}
              ▼
   ②  ESP32
       WiFi.begin(ssid, pwd) 入网（失败则 SoftAP 开热点）
       组 WIFI: 串，Serial2 打印两行
              │  UART3  PB18  115200 8N1  ★单向
              ▼
   ③  TD1601
       src/uart_esp.c 收行 → src/net_link.c 解析 → UserFolder/qr/net_card.c
              │
              ▼
       qrgen.c 编码 → ui_add_bitmap2 → ui.c 分段 DMA → 屏
```

键约束：**UART3 在实板上只有 PB18 一根线（ESP32→TD1601），没有回程。**
所以 TD1601 只能"被喂"，不能问、不能确认 —— 所有状态都得 ESP32 主动推。
（要双向必须改板，或让 ESP32 与 PC 之间那条链路代转。）

---

## 2. 屏幕上的五个状态

| `NET:` | 状态 | 卡片页上显示 | 什么时候由 ESP32 发 |
|---|---|---|---|
| 0 | 空闲 | `no network yet` + "waiting for network / from ESP32 over UART3" | 上电、配网被取消 |
| 1 | 热点已开 | `waiting for scan` + `SSID: <热点名>` | 热点起来了、二维码已就绪 |
| 2 | 连接中 | `connecting...` | 手机提交了家里 WiFi |
| 3 | 已连接 | `connected` + `IP: 192.168.x.x` | 拿到 IP |
| 4 | 失败 | `connect failed` | 口令错/超时（ESP32 自己会重开热点）|

界面两档：

* **卡片页（第 5 页）**：标题 + 二维码 + 状态 + SSID/IP。长按进卡片层，左右滑到 NET。
* **全屏码**：在卡片页点一下。模块从 6~7px 放大到 13~14px，手机对焦明显更快；
  再点一下退回卡片页。

两个刻意的行为：

1. **NET 页不自动回落到表情**（其它页 15 秒就回）。二维码是给人举着手机扫的，
   扫到一半屏幕切回笑脸等于把码收走了。
2. **收到二维码时自动跳到 NET 页**（`ui_app_show_net()`），但三种情况不抢屏：
   番茄钟计时中、提醒弹卡中、手指正按着。

---

## 3. 协议：文本行（与人给串口助手敲的一样）

行尾 `\r` 或 `\n`，全 ASCII，大小写敏感。

| 命令 | 说明 |
|---|---|
| `QR:<payload>` | 设置二维码内容并显示。`payload` 就是码里的东西，最长 127 字节 |
| `QR:OFF` | 清掉二维码，回到空闲 |
| `NET:<state>` | 只改状态（0..4） |
| `NET:<state>;<ssid>` | 带 SSID |
| `NET:<state>;<ssid>;<ip>` | 带 SSID 与 IP |

一轮完整的（时间往下）：

```
NET:1;Baozi-A1B2                          ← 热点起来了，屏上跳到 NET 页
QR:WIFI:T:WPA;S:Baozi-A1B2;P:88888888;;   ← 出码
NET:2                                     ← 手机提交了家里 WiFi
NET:3;MyHome-5G;192.168.1.37              ← 连上了，显示 SSID + IP
```

失败就 `NET:4`，然后 ESP32 重开热点、重新发 `NET:1` + `QR:`。

`WIFI:` 串的格式是 Zxing 的约定（手机相机原生认）：`WIFI:T:WPA;S:<ssid>;P:<pwd>;;`，
值里的 `\ ; , : "` 要用反斜杠转义。

---

## 4. ESP32 侧要接的三行

ESP32 固件不在本仓库（在 `cowork/delivery/02_ESP32/esp32_baozi/`）。需要加的东西
很少，核心就是往 `Serial2` 写文本 —— 和现有的 `EMO:HAPPY` 完全同一条路：

```cpp
// ① 热点/入网成功后：告诉板子"可以扫了"，并给出二维码内容
static void push_provision_qr(const char *ap_ssid, const char *ap_pwd) {
    Serial2.printf("NET:1;%s\r\n", ap_ssid);
    Serial2.printf("QR:WIFI:T:WPA;S:%s;P:%s;;\r\n", ap_ssid, ap_pwd);
}

// ② 手机在配网页提交了家里 WiFi
Serial2.print("NET:2\r\n");
WiFi.begin(home_ssid, home_pwd);

// ③ 结果
if (WiFi.status() == WL_CONNECTED) {
    Serial2.printf("NET:3;%s;%s\r\n", home_ssid, WiFi.localIP().toString().c_str());
} else {
    Serial2.print("NET:4\r\n");
}
```

三个必须注意：

1. **每条命令都要整行发完**（`println` / `printf("\r\n")`）。TD1601 是按行解析的，
   半行会被当成无效数据丢掉。
2. **SSID 只支持 ASCII**（板载字库只有 ASCII）。中文 SSID 仍然能进二维码
   （手机扫得出来），但屏上会显示成 `(non-ascii)`。
3. 现在 `esp32_baozi.ino` 的 `USE_USB_LINK` 默认走 USB CDC，走无线时才用 WiFi 分支；
   配网逻辑要挂在 WiFi 分支里，别挂到 USB 分支。

PC 侧那半（收 UDP 拿凭证）可选：如果 ESP32 自己开热点让手机填，就不需要 `net_share.py`；
如果走"PC 共享局域网"的路子，就在 ESP32 上加一个 UDP 监听（见 §5 的包格式）。

---

## 5. PC 侧

### `net_share.py`（`code/projects/Baozi_PC/`）

```bash
python net_share.py                  # 读本机当前 WiFi，广播给 ESP32
python net_share.py --dry-run        # 只打印将发什么（含二维码内容）
python net_share.py --ip 192.168.1.42  # 单播，不广播
python net_share.py --no-pass        # 只给 SSID，口令让手机填
```

包格式（UDP，默认 `255.255.255.255:8266`）：

```json
{"v":1,"t":"wifi","ssid":"MyHome","pwd":"******","token":"baozi","bssid":"aa:bb:..."}
```

ESP32 愿意接就回 `{"v":1,"t":"ack","state":"joining"}`（脚本会打印出来）。
口令是**明文广播**，只在家用/赛场自有网络用；不想发就用 `--no-pass`。

### `tools/push_qr.py`（台架联调）

绕开 ESP32，直接往 TD1601 的 UART0 敲那几行：

```bash
python tools/push_qr.py --port COM7 --ssid Baozi-A1B2 --pass 88888888
python tools/push_qr.py --port COM7 --raw "QR:http://192.168.4.1/"
python tools/push_qr.py --port COM7 --off
```

**这是排查顺序的关键**：屏上能出码 → TD1601 侧全通，问题一定在 ESP32 那一段。

---

## 6. 验证链（全部不用烧录）

### ① 编码器对不对 —— 与成熟实现逐位比对

```bash
cd /d/Baozi/_probe/qr
gcc -O2 -std=c99 -I<proj>/UserFolder/qr qr_dump.c <proj>/UserFolder/qr/qrgen.c -o qr_dump.exe
python qr_verify.py        # 13 个用例，含中文 SSID / v10 满容量 / 四种 ECC
```

判据是两件事同时成立：**去掩模后的码字流完全相同**（数据 + RS 纠错全对）+ **功能图形
逐位相同**。掩模号允许不同 —— 那是 8 个合法里的取舍，函数里也打印出来供参考。

> 别用 `segno` 做补位对照：它 `write_padding_bits()` 写的是 `[0] * (8 - length % 8)`，
> 比特流刚好落在字节边界时会多补 8 个零位（字节模式的终止符恰好总是对齐），
> 于是每个字节模式符号都白吃一个码字。照样能扫，但逐位比对必然对不上。

### ② 版面对不对 —— 把真实 UI 画到 PC

```bash
cd /d/Baozi/_probe/qr
gcc -O2 -std=c99 -I<proj>/include -I<proj>/UserFolder ui_sim.c \
    <proj>/UserFolder/ui.c <proj>/UserFolder/ui_app.c \
    <proj>/UserFolder/qr/qrgen.c <proj>/UserFolder/qr/net_card.c -o ui_sim.exe
./ui_sim.exe card     # -> out_net_card.bmp
./ui_sim.exe full     # -> out_net_full.bmp
./ui_sim.exe none     # -> out_net_empty.bmp
```

`ui_sim.c` 把 `LCD_*` 原语打桩，`ui_flush` 输出的每段像素直接落进内存帧缓冲 ——
跑的是固件里同一份 `ui_app.c` / `net_card.c`，没有"预览专用"绘制代码，不会漂移。
`_probe/qr/out_net_*.bmp` 就是当前版面的样子。

### ③ 固件全量编译

```bash
cd projects/baozi
export PATH="/d/Baozi/CDK/CDKRepo/Toolchain/XTGccElfNewlib/V3.4.0/R/bin:/d/Coding/VSCODE/mingw64/bin:$PATH"
mingw32-make -f Baozi.mk all -j4
# 末尾 crc32 报错属正常（CDK 之外的步骤）
```

当前：**text 101 948 / data 644 / bss 59 560**（v9 表情版是 96 236 / 644 / 48 660；
配网新增约 +5.7KB text、+10.9KB bss，主要是编码器的两张 57×57 矩阵）。

---

## 7. 约束与坑

1. **UART3 单向**。没有 ACK、没有重传。ESP32 必须把"该显示什么"推完就忘 ——
   推失败板子就一直停在旧状态。真要可靠，得让 ESP32 周期性重推（10 秒一次很便宜）。
2. **行缓冲 160 字节**（`uart_esp.c` 的 `ESP_LINE_MAX`）。最坏一条
   `QR:WIFI:…` = 117 字节，原来 64 字节会整行丢弃、配网永远收不到。超长仍然整行丢，
   绝不截断（截断会造出幽灵命令）。
3. **只支持 ASCII**。板载字库是 `FONT.h` 的 `asc2_2412`。中文 SSID 显示成
   `(non-ascii)`，二维码本身没问题。
4. **`ui_add_text` 只存指针**，字形在 `ui_flush` 时才取。任何要显示的文字缓冲
   必须是 static/常量 —— `net_card.c` 的 `s_info[]` 就是踩过这个坑（栈上 buf 刷出随机字符，
   `ui_sim` 里一眼看出来）。
5. **二维码要整数倍缩放**，否则模块边缘发毛、手机对焦变慢。`pick_scale()` 保证这点。
6. **静区靠白底**：卡片填充是 `C_CARD_TOP`(0xFFFE)，全屏是纯白 0xFFFF，
   都比画一个白框省事且不会算错边距。
7. **一 tick 只重绘一次**。`net_card` 只往绘制列表里加元素，刷屏统一由
   `ui_app` 在"串口静默 150ms"的窗口里做 —— 别在 UART 回调里直接刷。
8. **加 `.c` 要登记三处**：`package.yaml`（权威）、`Baozi.mk`（生成物）、
   `Obj/Baozi.txt`（链接清单）。后者漏了会 `undefined reference`，
   而 `.o` 明明编出来了 —— 本轮三个文件就是三处都补过。

---

## 8. 上板联调顺序

1. **台架**：`push_qr.py --port COM7 --ssid X --pass Y`。屏上出现二维码，手机能扫出
   `WIFI:T:WPA;S:X;P:Y;;`（用任意扫码 App 看内容）。点屏能切全屏码。
   → 不通过就别往下走，问题在 TD1601 侧。
2. **发 `NET:3;MyHome;192.168.1.37`**：卡片页下方应变成 `connected` + `IP: …`。
3. **接 ESP32**：串口日志里应出现 `[ESP] text: NET:1;...` → `[NET] qr set: v3 29x29 …`
   （`uart_esp.c` 的 `esp_apply_line` 会先打原始行，再由 `net_link` 打印解析结果）。
4. **手机实测**：扫码 → 连热点 → 配网页填家里 WiFi → 屏上 `connecting...` → `connected`。
5. 若第 3 步日志里没有 `[NET]`，但组件能收到 `[ESP] text:` —— 说明行被截断，
   检查 `ESP_LINE_MAX` 和 ESP32 侧是否漏了 `\r\n`。
