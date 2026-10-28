#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
baozi-uart-test.py — 包子串口协议自检工具（PC ↔ TD1601）

作用：验证「PC → USB-TTL → TD1601 UART0(PA18/PA17)」这条通信链路是否打通。
      发一条命令，看板上屏幕/舵机是否有反应；也可以挂在一边收上报。

⚠️ 协议口和 printf 是同一根线（UART0 = console 口）：
   板子 → PC 的线上同时有 printf 日志和协议帧，本工具会自动把两者分开显示
   （扫帧头 0xA5 + 验校验），所以看到「[板子日志]」行是正常的，不影响解析。
   另外这根口不能被别的程序占着（串口助手 / CDK 的终端窗口），否则打不开。

帧格式（见 include/uart_protocol.h）：
    [0xA5][LEN][CMD][DATA...][CHECKSUM]
    CHECKSUM = 0xA5 ^ LEN ^ CMD ^ DATA0 ^ DATA1 ...

用法：
    python baozi-uart-test.py --list                      # 列出本机串口
    python baozi-uart-test.py -p COM5 ping                # 连通性自检（发心跳，等状态上报）
    python baozi-uart-test.py -p COM5 emo happy           # 让包子笑
    python baozi-uart-test.py -p COM5 emo sad             # 难过
    python baozi-uart-test.py -p COM5 servo 0 120         # 水平舵机转到 120°
    python baozi-uart-test.py -p COM5 reset               # 复位（待机脸 + 舵机回中）
    python baozi-uart-test.py -p COM5 raw A5 01 01 01 A4  # 原样发一帧
    python baozi-uart-test.py -p COM5 monitor             # 持续解析（日志与帧分开显示）
    python baozi-uart-test.py -p COM5 listen              # 持续原始字节

    python baozi-uart-test.py -p COM5 time                # 对时（干掉屏幕顶部写死的 14:38）
    python baozi-uart-test.py -p COM5 habits              # 对时 + 下发演示习惯，HABIT 卡片立刻有内容

依赖：pip install pyserial
     habits 想显示中文习惯名还需要 Pillow（pip install pillow）；
     没装也能跑，只是习惯名退化成"只有圆点、没有文字"。

习惯看板协议见 include/uart_protocol.h 末尾：
     0x10 对时 / 0x11~0x13 习惯下发三连 / 0x90 打卡上报 / 0x91 同步完成
     / 0x92 板子催数据（屏幕还没收到习惯时，点一下空卡片就会发这条）
"""
import os
import sys
import time
import argparse

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("缺少 pyserial，请先安装：pip install pyserial")
    sys.exit(1)


FRAME_HEAD = 0xA5

# ---- 下行命令字（PC → 板子），与 include/uart_protocol.h 保持一致 ----
CMD = {
    "emo":       0x01,   # DATA=[emotion]
    "event":     0x02,   # DATA=[event]
    "person":    0x03,   # DATA=[len][name...]
    "script":    0x04,   # DATA=[len][name...]
    "stop":      0x05,
    "servo":     0x06,   # DATA=[id][angle]
    "reset":     0x07,
    "status":    0x08,
    "heartbeat": 0x09,
    "dance":     0x0A,
    # ---- v1.1 习惯看板 ----
    "time":        0x10,  # DATA=[mon][day][weekday][hh][mm]
    "habit_begin": 0x11,  # DATA=[count]
    "habit_item":  0x12,  # DATA=15 字节头 + 名字点阵（见 uart_protocol.h）
    "habit_end":   0x13,  # DATA=[count]
    # ---- v1.5 TODAY 页日程 ----
    "sched_begin": 0x14,  # DATA=[count]
    "sched_item":  0x15,  # DATA=10 字节头 + 标题点阵（见 uart_protocol.h）
    "sched_end":   0x16,  # DATA=[count]
    # ---- v1.8 提醒弹卡 ----
    "remind":      0x17,  # DATA=6 字节头 + 标题点阵（见 uart_protocol.h）
}

# ---- 情绪名 → 编号（板子 face_model.c 支持的 7 张脸）----
EMO = {
    "neutral":   0x00,
    "happy":     0x01,
    "sad":       0x02,
    "surprised": 0x03,
    "asleep":    0x04,
    "thoughtful": 0x05,   # 板上暂无对应模型，会退回 neutral
    "angry":     0x06,
}

# ---- 上行报告字（板子 → PC）----
RSP = {
    0x81: "RSP_STATUS",
    0x82: "RSP_SENSOR",
    0x83: "RSP_SCRIPT_DONE",
    0x84: "RSP_SERVO_DONE",
    0x8F: "RSP_ERROR",
    0x90: "RSP_HABIT_CHECKIN",
    0x91: "RSP_HABIT_SYNCED",
    0x92: "RSP_HABIT_REQ",
    0x93: "RSP_SCHED_CHECK",
    0x94: "RSP_SCHED_REQ",
    0x95: "RSP_REMIND_CLOSED",
}

# ---- 习惯看板：与 uart_protocol.h 的常量对齐 ----
HABIT_MAX_COUNT     = 12          # 板子最多缓存几个习惯
HABIT_MAX_SLOT      = 32          # 单个习惯最多几格
HABIT_NAME_MAX_CHARS = 6          # 名字最多几个字
HABIT_MODE_SINGLE   = 0
HABIT_MODE_MULTI    = 1
HABIT_STATE_FUTURE  = 0
HABIT_STATE_PENDING = 1
HABIT_STATE_DONE    = 2
HABIT_STATE_MISSED  = 3
HABIT_SLOT_NONE     = 0xFF
HABIT_OPEN_NONE     = 0xFF        # open_max 的"一格都没开窗"
HABIT_ITEM_HDR_LEN  = 16          # CMD_HABIT_ITEM 的定长头（v1.9 起含 open_max）
HABIT_FRAME_MAX     = 255         # FRAME_MAX_DATA：DATA 区上限

# ---- v1.5 TODAY 页日程：与 uart_protocol.h 的常量对齐 ----
SCHED_MAX_ITEMS       = 12
SCHED_TITLE_MAX_CHARS = 6      # 板端 2x 放大绘制（32px 大字），6 字刚好占满卡片宽度
SCHED_STATUS_PENDING  = 0
SCHED_STATUS_DONE     = 1
SCHED_ITEM_HDR_LEN    = 10

# ---- v1.8 提醒弹卡：与 uart_protocol.h 的常量对齐 ----
REMIND_TYPE_HABIT      = 0
REMIND_TYPE_SCHED      = 1
REMIND_TITLE_MAX_CHARS = 7      # 6 + 7*2*16 = 230 < 255，单帧装得下
REMIND_ITEM_HDR_LEN    = 6

# 演示习惯（还没接后端时用它们把屏幕点亮）
# (名字, mode, total, done, state, cur_slot, streak, mask)
# mask 的 bit i = 第 i 格已完成。这里刻意把四种状态都占上，
# 这样一屏就能看清 done / pending / missed / future 四种样式。
DEMO_HABITS = [
    ("作息",   HABIT_MODE_SINGLE, 1,  1, HABIT_STATE_DONE,    HABIT_SLOT_NONE, 12, 0x0001),
    ("喝水",   HABIT_MODE_MULTI,  12, 6, HABIT_STATE_PENDING, 8,               1,  0x003F),
    ("锻炼",   HABIT_MODE_SINGLE, 1,  0, HABIT_STATE_MISSED,  HABIT_SLOT_NONE, 3,  0x0000),
    ("护眼",   HABIT_MODE_MULTI,  16, 7, HABIT_STATE_PENDING, 10,              3,  0x007F),
    ("背单词", HABIT_MODE_SINGLE, 1,  0, HABIT_STATE_FUTURE,  HABIT_SLOT_NONE, 0,  0x0000),
]

# 中文字体候选（习惯名要在板子上显示，靠这里渲染成点阵传下去）
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",     # 微软雅黑
    r"C:\Windows\Fonts\simhei.ttf",   # 黑体
    r"C:\Windows\Fonts\simsun.ttc",   # 宋体
]


def checksum(cmd, data):
    cs = FRAME_HEAD ^ (len(data) & 0xFF) ^ cmd
    for b in data:
        cs ^= b
    return cs & 0xFF


def build_frame(cmd, data=None):
    data = list(data or [])
    return bytes([FRAME_HEAD, len(data) & 0xFF, cmd] + data + [checksum(cmd, data)])


def hexs(b):
    return " ".join("%02X" % x for x in b)


def _load_font(px):
    """挑一个能用的中文字体。没有 Pillow / 没有字体就返回 None。"""
    try:
        from PIL import ImageFont
    except ImportError:
        return None
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, px)
            except Exception:
                continue
    return None


def text_bitmap(text, cell=16):
    """把字符串渲染成 1bpp 点阵，格式与板子 ui_add_bitmap() 完全一致：

        逐行扫描，每行 ceil(W/8) 字节，MSB = 该行最左像素。

    返回 (data, W, H)；渲染不出来时返回 (b"", 0, 0)，板子那边会只画圆点。
    """
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return (b"", 0, 0)
    font = _load_font(cell - 1)          # 字号略小于格子，留出边距不顶格
    if font is None or not text:
        return (b"", 0, 0)

    W, H = cell * len(text), cell
    img = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(img)
    for i, ch in enumerate(text):
        bb = d.textbbox((0, 0), ch, font=font)
        gw, gh = bb[2] - bb[0], bb[3] - bb[1]
        x = i * cell + (cell - gw) // 2 - bb[0]
        y = (cell - gh) // 2 - bb[1]
        d.text((x, y), ch, font=font, fill=1)

    px = img.load()
    stride = (W + 7) // 8
    out = bytearray()
    for y in range(H):
        row = bytearray(stride)
        for x in range(W):
            if px[x, y]:
                row[x >> 3] |= 0x80 >> (x & 7)
        out += row
    return (bytes(out), W, H)


def build_habit_item(idx, name, mode, total, done, state, cur_slot, streak, mask,
                     open_max=0xFF):
    """按 CMD_HABIT_ITEM 的布局拼一帧（16 字节头 + 名字点阵）。

    ⚠️ name_w 是【单个字】的宽度，不是整串宽度。
       协议里板子按 need = 16 + ceil(chars*name_w/8)*name_h 算长度、
       按 drawn*(name_w/8) 取每行字节，所以这里必须传单字宽（16）。
       （曾把 text_bitmap 返回的整串宽直接塞进去，板子按 16+32*16/8*16=144
        校验，而实际只有 79 字节 → 整条习惯被丢。）

    open_max（v1.9）：已开窗的最大格子序号（含），0xFF = 一格都没开窗。
       板子靠它区分"还没到时间"和"已错过可补卡"，从而拒掉提前打卡。
       判定源仍然只有后端 —— 这里只是把后端算好的结果传下去。
    """
    if len(name) > 6:                       # HABIT_NAME_MAX_CHARS
        print("  ! 习惯名「%s」超过 6 字，截断为「%s」" % (name, name[:6]))
        name = name[:6]
    bmp, bw, bh = text_bitmap(name)
    if bmp and name:
        chars = len(name)
        cellw = bw // chars                 # 单字宽
        cellh = bh
    else:
        chars = cellw = cellh = 0

    head = [
        idx & 0xFF, mode & 0xFF, total & 0xFF, done & 0xFF,
        state & 0xFF, cur_slot & 0xFF,
        streak & 0xFF, (streak >> 8) & 0xFF,
        mask & 0xFF, (mask >> 8) & 0xFF, (mask >> 16) & 0xFF, (mask >> 24) & 0xFF,
        chars, cellw, cellh,
        open_max & 0xFF,
    ]
    payload = head + list(bmp)
    if len(payload) > HABIT_FRAME_MAX:
        raise ValueError("habit item too big: %d > %d" % (len(payload), HABIT_FRAME_MAX))
    return build_frame(CMD["habit_item"], payload), len(payload)


def build_sched_item(idx, title, mon, day, weekday, hh, mm, status):
    """按 CMD_SCHED_ITEM 的布局拼一帧（10 字节头 + 标题点阵）。

    布局（与 include/uart_protocol.h 的 v1.5 SCHED 段一致）：
        [0]idx [1]mon [2]day [3]weekday [4]hh [5]mm [6]status
        [7]chars [8]nw(单字宽!) [9]nh  + 点阵
    ⚠️ nw 同样是【单个字】的宽度（16），不是整串宽 —— 习惯侧踩过的坑。
    """
    if len(title) > SCHED_TITLE_MAX_CHARS:
        title = title[:SCHED_TITLE_MAX_CHARS]
    bmp, bw, bh = text_bitmap(title)
    if bmp and title:
        chars = len(title)
        cellw = bw // chars
        cellh = bh
    else:
        chars = cellw = cellh = 0

    head = [
        idx & 0xFF, mon & 0xFF, day & 0xFF, weekday & 0xFF,
        hh & 0xFF, mm & 0xFF, status & 0xFF,
        chars, cellw, cellh,
    ]
    payload = head + list(bmp)
    if len(payload) > HABIT_FRAME_MAX:
        raise ValueError("sched item too big: %d > %d" % (len(payload), HABIT_FRAME_MAX))
    return build_frame(CMD["sched_item"], payload), len(payload)


def build_remind(rtype, hh, mm, title):
    """按 CMD_REMIND(0x17) 的布局拼一帧（6 字节头 + 标题点阵）。

    布局（与 include/uart_protocol.h 的 v1.8 REMIND 段一致）：
        [0]type [1]hh [2]mm [3]chars [4]nw(单字宽!) [5]nh  + 点阵
    板端把点阵 2x 放大成 32px 大字，居中显示在弹卡上。
    """
    if len(title) > REMIND_TITLE_MAX_CHARS:
        title = title[:REMIND_TITLE_MAX_CHARS]
    bmp, bw, bh = text_bitmap(title)
    if bmp and title:
        chars = len(title)
        cellw = bw // chars
        cellh = bh
    else:
        chars = cellw = cellh = 0

    head = [rtype & 0xFF, hh & 0xFF, mm & 0xFF, chars, cellw, cellh]
    payload = head + list(bmp)
    if len(payload) > HABIT_FRAME_MAX:
        raise ValueError("remind too big: %d > %d" % (len(payload), HABIT_FRAME_MAX))
    return build_frame(CMD["remind"], payload), len(payload)


def list_serial_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("没有发现任何串口。检查 USB-TTL 是否插好、驱动是否装了。")
        return
    print("本机串口：")
    for p in ports:
        print("  %-8s  %s" % (p.device, p.description))


def parse_stream(buf):
    """从「printf 日志 + 协议帧」的混流里把两者分开。

    返回 (frames, logs)：
      frames = [(cmd, data_bytes), ...]   校验通过的协议帧
      logs   = ["[BaoZi] xxx", ...]       被判定为文本的日志行

    判定规则：字节 0xA5 起，若后面的长度够、且异或校验对得上，才算一帧；
    否则 0xA5 只是文本里的巧合，原样归入日志。
    """
    i, n = 0, len(buf)
    frames, text = [], bytearray()
    while i < n:
        if buf[i] != FRAME_HEAD:
            text.append(buf[i])
            i += 1
            continue
        if i + 3 > n:                     # 头部还没收全，尾部留待下轮
            break
        ln = buf[i + 1]
        end = i + 4 + ln                  # 整帧结束位置（含校验字节）
        if end > n:                       # 数据没到齐：当成半个帧先放弃
            text.append(buf[i])
            i += 1
            continue
        cmd = buf[i + 2]
        data = buf[i + 3:i + 3 + ln]
        cs = buf[i + 3 + ln]
        if checksum(cmd, data) == cs:
            frames.append((cmd, bytes(data)))
            i = end
        else:                             # 校验不过 → 不是帧，当普通字节
            text.append(buf[i])
            i += 1

    logs = []
    if text:
        s = text.decode("ascii", "replace").replace("\r", "\n")
        for line in s.split("\n"):
            # 只留可打印字符，去掉串口噪声/控制字符
            line = "".join(c for c in line if c == "\t" or 32 <= ord(c) < 127)
            if line.strip():
                logs.append(line)
    return frames, logs


def show(frames, logs):
    """打印解析结果：先日志（板子在说什么），后协议帧。返回帧数量。"""
    for line in logs:
        print("  [板子日志] %s" % line)
    for cmd, data in frames:
        name = RSP.get(cmd, "0x%02X" % cmd)
        print("  <- %s  data=[%s]  checksum=OK" % (name, hexs(data)))
    return len(frames)


def open_port(port, baud):
    try:
        return serial.Serial(port, baud, timeout=0.3)
    except Exception as e:
        print("打开串口 %s 失败：%s" % (port, e))
        print("提示：串口号对不对？这个口是不是被别的程序占着（串口助手 / CDK 的终端）？")
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(
        description="包子串口协议自检工具", add_help=True,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="列出本机串口")
    ap.add_argument("-p", "--port", help="串口号，如 COM5")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("action", nargs="?",
                    help="ping/emo/servo/reset/status/heartbeat/dance/time/habits/"
                         "monitor/listen/raw")
    ap.add_argument("args", nargs="*", help="动作参数")
    a = ap.parse_args()

    if a.list:
        list_serial_ports()
        return

    if not a.port:
        print("请用 -p 指定串口，例如：python baozi-uart-test.py -p COM5 ping")
        print()
        list_serial_ports()
        return

    ser = open_port(a.port, a.baud)
    time.sleep(0.2)
    ser.reset_input_buffer()

    act = (a.action or "ping").lower()
    data = []

    if act == "ping":
        # 心跳：板子会推进 1 秒并回一帧 RSP_STATUS([emo][energy][idle_h][idle_l][emo_h][emo_l])
        print("连通性自检：向 %s 发心跳，等 2 秒看上不上报…" % a.port)
        ser.write(build_frame(CMD["heartbeat"]))
        ser.flush()
        time.sleep(0.4)
        buf = ser.read(512)
        if buf:
            print("收到 %d 字节：" % len(buf))
            frames, logs = parse_stream(buf)
            show(frames, logs)
            if frames:
                print("==> 链路通了！板子能收到你的命令，也能回话。")
            elif logs:
                print("==> 单向通了：能收到板子日志，但没收到协议帧。")
                print("    再等等或再发一次（心跳回包可能排在日志后面）。")
        else:
            print("没有任何回包。逐项排查：")
            print("  1) TX/RX 是不是交叉接了？（PC 的 TX → 板子 PA17 / RX，PC 的 RX → 板子 PA18 / TX）")
            print("  2) GND 共地了吗？（不共地最常见）")
            print("  3) 板子那边有没有打印 '[BaoZi] uart ready (UART0 PA18/PA17 ...)'？")
            print("     没有的话说明 uart_hal_init() 失败，先解决板子侧。")
            print("  4) 线插的不是 PA18/PA17？那就改 src/uart_hal.c 里复用的那个 UART 序号。")
        ser.close()
        return

    if act == "monitor":
        print("持续解析 %s（日志与协议帧分开显示，Ctrl+C 退出）…" % a.port)
        try:
            while True:
                b = ser.read(128)
                if not b:
                    continue
                frames, logs = parse_stream(b)
                show(frames, logs)
        except KeyboardInterrupt:
            print("\n退出。")
        ser.close()
        return

    if act == "listen":
        print("原始监听 %s（Ctrl+C 退出）…" % a.port)
        try:
            while True:
                b = ser.read(64)
                if b:
                    print("  raw:", hexs(b))
        except KeyboardInterrupt:
            print("\n退出。")
        ser.close()
        return

    if act == "habits":
        from datetime import datetime

        now = datetime.now()
        print("== 习惯看板下发 ==")

        # 1) 先对时：屏幕顶部时间就活了（没对过时显示 --:--）
        tframe = build_frame(CMD["time"], [now.month, now.day, now.isoweekday(),
                                           now.hour, now.minute])
        ser.write(tframe)
        ser.flush()
        print("对时：%02d/%02d 周%d %02d:%02d  帧=%s"
              % (now.month, now.day, now.isoweekday(), now.hour, now.minute, hexs(tframe)))
        time.sleep(0.2)

        # 2) 列表开始
        ser.write(build_frame(CMD["habit_begin"], [len(DEMO_HABITS)]))
        ser.flush()
        time.sleep(0.15)

        # 3) 逐个下发
        state_name = {HABIT_STATE_FUTURE: "future", HABIT_STATE_PENDING: "pending",
                      HABIT_STATE_DONE: "done", HABIT_STATE_MISSED: "missed"}
        for i, (name, mode, total, done, state, cur, streak, mask) in enumerate(DEMO_HABITS):
            fr, blen = build_habit_item(i, name, mode, total, done, state,
                                        cur, streak, mask)
            ser.write(fr)
            ser.flush()
            time.sleep(0.15)        # 板子主循环要处理，别灌太快
            print("  [%d] %-6s %-6s %d/%d  %-7s cur=%-4s streak=%dd  点阵=%dB  帧长=%d"
                  % (i, name, "single" if mode == 0 else "multi", done, total,
                     state_name.get(state, "?"),
                     "-" if cur == HABIT_SLOT_NONE else cur, streak, blen, len(fr)))

        # 4) 结束
        ser.write(build_frame(CMD["habit_end"], [len(DEMO_HABITS)]))
        ser.flush()
        time.sleep(0.6)

        print("已下发 %d 个习惯，板子应回 RSP_HABIT_SYNCED。" % len(DEMO_HABITS))
        print("现在长按包子屏幕进卡片层 → 左右滑到 HABIT 页看效果；"
              "点某一行 = 打卡，再点一下 = 取消。")
        buf = ser.read(1024)
        if buf:
            frames, logs = parse_stream(buf)
            show(frames, logs)
        ser.close()
        return

    if act == "emo":
        name = (a.args[0] if a.args else "happy").lower()
        if name not in EMO:
            print("未知情绪名 %r。可选：%s" % (name, ", ".join(EMO)))
            ser.close()
            return
        data = [EMO[name]]
        frame = build_frame(CMD["emo"], data)
        print("发：emo=%s(0x%02X)  帧=%s" % (name, EMO[name], hexs(frame)))

    elif act == "servo":
        if len(a.args) < 2:
            print("用法：servo <id> <angle>，例如 servo 0 120")
            ser.close()
            return
        sid, ang = int(a.args[0]), int(a.args[1])
        data = [sid & 0xFF, ang & 0xFF]
        frame = build_frame(CMD["servo"], data)
        print("发：servo id=%d angle=%d  帧=%s" % (sid, ang, hexs(frame)))

    elif act == "reset":
        frame = build_frame(CMD["reset"])
        print("发：reset  帧=%s" % hexs(frame))

    elif act == "status":
        frame = build_frame(CMD["status"])
        print("发：query status  帧=%s" % hexs(frame))

    elif act == "heartbeat":
        frame = build_frame(CMD["heartbeat"])
        print("发：heartbeat  帧=%s" % hexs(frame))

    elif act == "dance":
        frame = build_frame(CMD["dance"])
        print("发：dance  帧=%s" % hexs(frame))

    elif act == "time":
        from datetime import datetime
        now = datetime.now()
        data = [now.month, now.day, now.isoweekday(), now.hour, now.minute]
        frame = build_frame(CMD["time"], data)
        print("发：sync time %02d/%02d 周%d %02d:%02d  帧=%s"
              % (now.month, now.day, now.isoweekday(), now.hour, now.minute, hexs(frame)))
        print("（板子不回包；看长按进卡片层后顶部时间是不是变成现在了）")

    elif act == "raw":
        try:
            frame = bytes(int(x, 16) for x in a.args)
        except ValueError:
            print("raw 后面要跟十六进制字节，例如：raw A5 01 01 01 A4")
            ser.close()
            return
        print("发（原样）：%s" % hexs(frame))

    else:
        print("未知动作 %r。可用：ping / emo / servo / reset / status / heartbeat / dance / "
              "time / habits / monitor / listen / raw" % act)
        ser.close()
        return

    ser.write(frame)
    ser.flush()
    time.sleep(0.4)
    buf = ser.read(512)
    if buf:
        print("收到 %d 字节：" % len(buf))
        frames, logs = parse_stream(buf)
        show(frames, logs)
        if not frames and not logs:
            print("  raw:", hexs(buf))
        elif not frames:
            print("  （只收到日志、没有协议回包：emo/servo/reset 这类命令本来就不回包，"
                  "看板子屏幕/舵机反应即可）")
    else:
        print("（没有回包；emo/servo/reset 这类命令本来就不回包，看板子屏幕/舵机反应即可）")
    ser.close()


if __name__ == "__main__":
    main()
