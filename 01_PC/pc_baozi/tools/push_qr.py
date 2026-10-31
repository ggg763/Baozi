# -*- coding: utf-8 -*-
"""push_qr.py —— 把配网二维码推进 TD1601（PC-UART0 直连，绕开 ESP32 的兜底路）。

板子的 NET 页只要一次都没收到过 `QR:` 行，就永远显示
    waiting for network / from ESP32 over UART3
（UserFolder/qr/net_card.c 的 !s_has_qr 分支）。正常跑 `python main.py` 时
由主程序的 _start_provision_qr() 经 ESP32 转发推一次；本脚本是**不经射频、
不经 ESP32** 的直连版本，用来单独验证屏幕或应急：

    ① 读本机当前连着的 WiFi（netsh，含口令）
    ② 找板子的串口（只有一个 USB 串口时自动选）
    ③ 按真实时序发两行文本（115200 8N1，与固件 console 一致）：
           NET:1;<ssid>                        → 屏上跳到 NET 页
           QR:WIFI:T:WPA;S:<ssid>;P:<pwd>;;    → 板子自己编码并显示二维码
    ④ 把板子的 [NET] 日志读回来给结论（qr set / qr rejected / 什么都没收到）

用法：
    python tools/push_qr.py                 # 自动读 WiFi + 自动选口
    python tools/push_qr.py --port COM9     # 指定串口（--list 先看有哪些）
    python tools/push_qr.py --ssid MyHome --pass 12345678
    python tools/push_qr.py --raw "QR:http://192.168.4.1/"
    python tools/push_qr.py --off           # 清掉二维码
    python tools/push_qr.py --dry-run       # 只打印将发什么、多长

长度/字符集这些"板子能收多少"的规矩在 wifi_cred.py 里（main.py 也用它），
本脚本不再自己判一遍，免得两边标准飘。

依赖：pyserial（pc_baozi/.venv 里已装）。没装会明确提示，不会静默失败。
"""

import argparse
import os
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:                     # pragma: no cover
    serial = None
    list_ports = None

# 让 tools/ 下的脚本能 import 上层目录的 wifi_cred（单一实现，不复制粘贴）
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from wifi_cred import (  # noqa: E402
    LINE_MAX,
    current_wifi,
    provision_lines,
    wifi_password,
)

# 与固件 src/board_init.c 的 console.baudrate 一致（项目统一 115200）
DEFAULT_BAUD = 115200


# ------------------------------------------------------------------ 串口
def list_serial_ports():
    if list_ports is None:
        return []
    ports = list(list_ports.comports())
    # USB 上的串口排前面：本机蓝牙虚拟串口一堆，不排序的话板子那个口很容易看漏
    ports.sort(key=lambda p: 0 if "USB" in (p.description or "").upper() else 1)
    return ports


def pick_port(explicit):
    """显式给了就用；只找到唯一一个可用口就用它；否则让用户挑。"""
    ports = list_serial_ports()
    if explicit:
        return explicit, ports
    if len(ports) == 1:
        return ports[0].device, ports
    return None, ports


def print_ports(ports):
    if not ports:
        print("没有扫到任何串口。检查：板子插了没 / 驱动装了没 / 串口是不是被")
        print("CDK 的串口终端或另一个脚本占着（Windows 上同一口不能被两方同时打开）。")
        return
    print("可用串口：")
    for p in ports:
        print("  %-8s %s" % (p.device, p.description))
    print("用 --port COMx 指定板子 UART0 那个口。")


# ------------------------------------------------------------------ 主流程
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="把配网二维码推进 TD1601（PC -> UART0 直连）",
    )
    ap.add_argument("--port", help="串口名，如 COM9；不给则自动选（只有一个可用口时）")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--ssid", help="热点名；不给则读本机当前连着的 WiFi")
    ap.add_argument("--pass", dest="pwd", help="热点口令；不给则从 netsh 读")
    ap.add_argument("--ip", default=None,
                    help="顺带发 NET:3;<ssid>;<ip> 让屏上显示 IP"
                         "（默认不发，因为板子自己并没有联网）")
    ap.add_argument("--raw", help="直接发这一行（原样，不含换行）")
    ap.add_argument("--off", action="store_true", help="发 QR:OFF 清掉二维码")
    ap.add_argument("--no-pass", action="store_true", help="只发 SSID，不发口令")
    ap.add_argument("--delay", type=float, default=0.35, help="命令之间的间隔（秒）")
    ap.add_argument("--list", action="store_true", help="只列出串口")
    ap.add_argument("--dry-run", action="store_true", help="只打印将要发送的内容")
    args = ap.parse_args(argv)

    if args.list:
        print_ports(list_serial_ports())
        return 0

    if serial is None and not args.dry_run:
        print("缺少 pyserial：pip install pyserial")
        print("（本机已在 pc_baozi\\.venv 里装好，用 .venv\\Scripts\\python.exe 跑即可）")
        return 2

    # ---- ① 决定要发哪几行 ----
    try:
        if args.raw:
            lines = [args.raw]
        elif args.off:
            lines = ["QR:OFF"]
        else:
            ssid = args.ssid
            if not ssid:
                ssid, _bssid = current_wifi()
                if not ssid:
                    raise ValueError(
                        "读不到本机当前 WiFi（没连无线，或者不是 Windows）。\n"
                        "手动给 --ssid/--pass，或只想验证屏幕时 --raw \"QR:http://192.168.4.1/\"")
            if args.no_pass:
                pwd = ""
            elif args.pwd is not None:
                pwd = args.pwd
            else:
                pwd = wifi_password(ssid) or ""
            lines = provision_lines(ssid, pwd)
            print("SSID：%s%s" % (ssid, "" if pwd else "（无口令）"))
            if args.ip:
                lines.append("NET:3;%s;%s" % (ssid, args.ip))
    except ValueError as exc:
        print("%s" % exc)
        return 2

    # 追加的 NET:3 行也要过一遍长度闸
    for line in lines:
        if len(line) > LINE_MAX - 1:
            print("这一行 %d 字节，超过固件单行上限 %d，会被整行丢弃："
                  % (len(line), LINE_MAX - 1))
            print("  %s" % line)
            return 2

    print("\n将发送：")
    for line in lines:
        print("  %s" % line)

    if args.dry_run:
        print("\n--dry-run：未实际发送。")
        return 0

    # ---- ② 开串口发 ----
    port, ports = pick_port(args.port)
    if port is None:
        print("\n找到 %d 个串口，需要你指定 --port：" % len(ports))
        print_ports(ports)
        return 2

    try:
        ser = serial.Serial(port, args.baud, timeout=0.2)
    except Exception as e:                                   # noqa: BLE001
        print("打开 %s 失败：%s" % (port, e))
        print("提示：串口被别的程序占用时也会这样（CDK 的串口终端、另一个脚本）。")
        return 1

    ser.reset_input_buffer()          # 板子启动日志先排空，免得读成我们的回显

    seen = []
    for line in lines:
        ser.write((line + "\r\n").encode("ascii", "replace"))
        ser.flush()
        print("--> %s" % line)
        time.sleep(args.delay)
        for raw in ser.read(ser.in_waiting or 1).splitlines():
            text = raw.decode("ascii", "replace").strip()
            if text:
                print("    %s" % text)
                seen.append(text)

    # ---- ③ 再等一会儿，把 [NET] 的结果行收干净 ----
    time.sleep(0.4)
    for raw in ser.read(ser.in_waiting or 1).splitlines():
        text = raw.decode("ascii", "replace").strip()
        if text:
            print("    %s" % text)
            seen.append(text)

    ser.close()

    # ---- ④ 给结论 ----
    blob = "\n".join(seen)
    print()
    if "qr set" in blob:
        print("OK：板子已经编码出二维码，屏上应该停在 NET 页的码上。")
        print("点一下屏幕放大成全屏码，手机相机直接扫。")
        return 0
    if "qr rejected" in blob:
        print("板子拒了这个 payload（qrgen 编码失败）。上面 [NET] qr rejected 那行")
        print("有 rc 和实际收到的长度 len —— len 和这里打印的长度对不上，就是串口")
        print("这一行被丢字节了。")
        return 3
    print("没等到板子的 [NET] 回显。可能：")
    print("  · 这个口不是板子的 UART0（--list 看描述，换一个）")
    print("  · 波特率不是 %d" % args.baud)
    print("  · 板子跑的还是旧固件（旧版 UART0 单行上限 64 字节，会把 QR: 整行丢掉）")
    print("  · 板子没在跑：先看 UART0 有没有正常启动日志")
    return 4


if __name__ == "__main__":
    sys.exit(main())
