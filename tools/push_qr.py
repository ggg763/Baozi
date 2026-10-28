# -*- coding: utf-8 -*-
"""push_qr.py —— 不经 ESP32，直接把配网命令灌进 TD1601 的 UART0（台架联调用）。

为什么需要它：整条链路（PC → 局域网 → ESP32 → UART3 → TD1601）里，任何一环没到位
都看不到屏。这个脚本绕开射频和 ESP32，直接往板子的调试串口敲那几行文本命令 ——
屏幕能出码，就说明 TD1601 侧（QR 编码 + NET 页 + 协议解析）全通，剩下的问题一定
在 ESP32 那一段。

它会按真实时序发三条：
    NET:1;<ssid>                          → 屏上跳到 NET 页
    QR:WIFI:T:WPA;S:<ssid>;P:<pwd>;;      → 生成并显示二维码
    NET:3;<ssid>;<ip>                     → 连上后显示 SSID/IP（可选，--skip-ok 跳过）

用法：
    python tools/push_qr.py --port COM7 --ssid Baozi-A1B2 --pass 88888888
    python tools/push_qr.py --port COM7 --raw "QR:http://192.168.4.1/"
    python tools/push_qr.py --port COM7 --off            # 清掉二维码

依赖：pyserial（pip install pyserial）。没装会给出提示，不会静默失败。
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:                     # pragma: no cover
    serial = None


def wifi_payload(ssid, pwd):
    def esc(s):
        return (s or "").replace("\\", "\\\\").replace(";", "\\;") \
                        .replace(",", "\\,").replace(":", "\\:").replace('"', '\\"')
    return "WIFI:T:WPA;S:%s;P:%s;;" % (esc(ssid), esc(pwd))


def main(argv=None):
    ap = argparse.ArgumentParser(description="把配网文本命令直接推给 TD1601（台架测试）")
    ap.add_argument("--port", required=True, help="串口名，如 COM7")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--ssid", help="热点名（生成 WIFI: 串用）")
    ap.add_argument("--pass", dest="pwd", default="88888888", help="热点口令")
    ap.add_argument("--ip", default="192.168.4.1", help="连上后展示的 IP")
    ap.add_argument("--raw", help="直接发这一行（原样，不含换行）")
    ap.add_argument("--off", action="store_true", help="发 QR:OFF 清掉二维码")
    ap.add_argument("--skip-ok", action="store_true", help="不发最后的 NET:3 已连接状态")
    ap.add_argument("--delay", type=float, default=0.6, help="命令之间的间隔（秒）")
    args = ap.parse_args(argv)

    if serial is None:
        print("缺少 pyserial：pip install pyserial")
        return 2

    lines = []
    if args.raw:
        lines = [args.raw]
    elif args.off:
        lines = ["QR:OFF"]
    elif args.ssid:
        lines.append("NET:1;%s" % args.ssid)
        lines.append("QR:" + wifi_payload(args.ssid, args.pwd))
        if not args.skip_ok:
            lines.append("NET:3;%s;%s" % (args.ssid, args.ip))
    else:
        ap.error("要么给 --ssid，要么给 --raw / --off")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as e:                                   # noqa: BLE001
        print("打开 %s 失败：%s" % (args.port, e))
        print("提示：串口被别的程序占用时也会这样（CDK 的串口终端、另一个脚本）。")
        return 1

    # 板子启动会打一堆日志，先把它排空，免得把旧字节读成我们的回显
    ser.reset_input_buffer()

    for line in lines:
        payload = (line + "\r\n").encode("utf-8")
        ser.write(payload)
        ser.flush()
        print("--> %s" % line)
        time.sleep(args.delay)

        # 板子会把命令回显成 "[NET] ..."，有就顺手打出来，方便确认解析成功
        try:
            tail = ser.read(ser.in_waiting or 1)
            for raw in tail.splitlines():
                text = raw.decode("utf-8", "replace").strip()
                if text:
                    print("    %s" % text)
        except Exception:
            pass

    ser.close()
    print("\n完成。屏上应停在 NET 页：有二维码 + 'waiting for scan'。")
    print("点一下屏幕可放大成全屏码（再点一下退出）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
