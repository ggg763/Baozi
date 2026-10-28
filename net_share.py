# -*- coding: utf-8 -*-
"""net_share.py —— 把本机所在的局域网（SSID + 口令）共享给 ESP32。

这是"扫码配网"链路的第 ① 环：

    PC ──局域网 UDP──► ESP32 ──UART3(PB18, 单向)──► TD1601 ──► 屏幕显示二维码
    本脚本             收包后入网/开热点         自己编码二维码

为什么要 PC 掺和：TD1601 没有射频，ESP32 又不知道"家里的网叫什么"。
PC 本来就连在那个网上，由它把凭证递过去最省事 —— 用户不用在屏幕上戳键盘。

协议（UDP，默认广播 255.255.255.255:8767，JSON）：
    请求  {"v":1,"t":"wifi","ssid":"MyHome","pwd":"******","token":"baozi"}
    可选  "bssid"（有就用，能少一次扫描）
    应答  {"v":1,"t":"ack","state":"joining"}        ← ESP32 愿意接就回一条

安全性：口令是**明文**广播到本网段的。局域网内可抓包，所以：
  · 只在家里/赛场的自有网络用；
  · 不想发口令就加 --no-pass，ESP32 那边会退回"开热点让手机扫"的模式；
  · 想收紧就加 --ip <ESP32 的地址>，只发给它一台，不用广播。

用法：
    python net_share.py                      # 读本机当前 WiFi，广播三遍
    python net_share.py --dry-run            # 只打印，不发送
    python net_share.py --ssid MyHome --pass 12345678
    python net_share.py --ip 192.168.1.42    # 单播给 ESP32
"""

import argparse
import json
import re
import socket
import subprocess
import sys
import time

DEFAULT_PORT = 8767
DEFAULT_TOKEN = "baozi"
PROTO_VERSION = 1


# ------------------------------------------------------------------ 读本机 WiFi
def current_wifi():
    """返回 (ssid, bssid)；读不到就 (None, None)。

    只支持 Windows 的 netsh（本项目主战场）。别的系统返回 None，
    用 --ssid/--pass 手动指定即可。
    """
    try:
        out = subprocess.run(
            ["netsh", "wlan", "show", "interfaces"],
            capture_output=True, timeout=8,
        ).stdout
    except Exception:
        return None, None

    for enc in ("utf-8", "gbk", "latin-1"):
        try:
            text = out.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    else:
        return None, None

    ssid = None
    bssid = None
    for line in text.splitlines():
        line = line.strip()
        # 注意：既匹配英文 "SSID"，也匹配中文系统的输出
        m = re.match(r"^SSID\s*:\s*(\S.*)$", line)
        if m and ssid is None:
            ssid = m.group(1).strip()
            continue
        m = re.match(r"^(?:BSSID)\s*:\s*([0-9a-fA-F:]{17})", line)
        if m:
            bssid = m.group(1).lower()
    return ssid, bssid


def wifi_password(ssid):
    """用 netsh 取本机保存的该网络口令（明文）。取不到返回 None。

    需要管理员权限的场景：某些系统策略下 key=clear 会被拒。此时用 --pass。
    """
    if not ssid:
        return None
    try:
        out = subprocess.run(
            ["netsh", "wlan", "show", "profile", "name=%s" % ssid, "key=clear"],
            capture_output=True, timeout=8,
        ).stdout
    except Exception:
        return None

    for enc in ("utf-8", "gbk", "latin-1"):
        try:
            text = out.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    else:
        return None

    for line in text.splitlines():
        # 英文 "Key Content" / 中文 "关键内容"
        m = re.match(r"^\s*(?:Key Content|关键内容)\s*:\s*(.*)$", line)
        if m:
            return m.group(1).strip()
    return None


# ------------------------------------------------------------------ 组装
def wifi_qr_payload(ssid, pwd, hidden=False, auth="WPA"):
    """生成标准 WIFI: 二维码内容（手机相机认这个格式）。

    转义规则：内容里的 \\ ; , : " 需要用反斜杠转义（Zxing 的约定）。
    """
    def esc(s):
        return re.sub(r'([\\;,":])', r"\\\1", s or "")

    hidden_flag = "H:true;" if hidden else ""
    return "WIFI:T:%s;S:%s;P:%s;%s;" % (auth, esc(ssid), esc(pwd), hidden_flag)


def build_packet(ssid, pwd, token=DEFAULT_TOKEN, bssid=None, with_pass=True):
    pkt = {
        "v": PROTO_VERSION,
        "t": "wifi",
        "ssid": ssid,
        "token": token,
    }
    if with_pass and pwd:
        pkt["pwd"] = pwd
    if bssid:
        pkt["bssid"] = bssid
    return pkt


# ------------------------------------------------------------------ 发送
def send_udp(packet, host, port, repeat=3, timeout=1.0):
    """发 N 遍（UDP 会丢，重复几遍最省事），并等一会儿 ack。"""
    data = json.dumps(packet, ensure_ascii=False).encode("utf-8")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)

    acks = []
    for i in range(repeat):
        try:
            s.sendto(data, (host, port))
        except OSError as e:
            print("  发送失败：%s" % e)
            break
        # 每遍都等一下应答（ESP32 收到就会回）
        try:
            buf, addr = s.recvfrom(1024)
            acks.append((addr[0], buf.decode("utf-8", "replace")))
        except socket.timeout:
            pass
        if i + 1 < repeat:
            time.sleep(0.15)
    s.close()
    return acks


def share_wifi(ssid=None, pwd=None, host="255.255.255.255", port=DEFAULT_PORT,
               token=DEFAULT_TOKEN, with_pass=True, repeat=3):
    """给别的模块（比如以后接进 main.py 的菜单）直接调的函数。
    返回 (packet, acks)；ssid 为空时返回 (None, [])。"""
    if not ssid:
        ssid, bssid = current_wifi()
    else:
        bssid = None
    if not ssid:
        return None, []
    if with_pass and pwd is None:
        pwd = wifi_password(ssid)

    packet = build_packet(ssid, pwd, token=token, bssid=bssid, with_pass=with_pass)
    acks = send_udp(packet, host, port, repeat=repeat)
    return packet, acks


# ------------------------------------------------------------------ CLI
def main(argv=None):
    ap = argparse.ArgumentParser(description="把本机 WiFi 凭证共享给 ESP32（包子配网 ①）")
    ap.add_argument("--ssid", help="不填则自动读本机当前 WiFi")
    ap.add_argument("--pass", dest="pwd", help="Wi-Fi 口令；不填则从 netsh 里读")
    ap.add_argument("--ip", default="255.255.255.255", help="目标地址，默认广播")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--token", default=DEFAULT_TOKEN, help="简单防误接的标识")
    ap.add_argument("--no-pass", action="store_true", help="只发 SSID，不发口令")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--dry-run", action="store_true", help="只打印将要发送的内容")
    args = ap.parse_args(argv)

    ssid = args.ssid
    bssid = None
    if not ssid:
        ssid, bssid = current_wifi()
        if not ssid:
            print("读不到本机 WiFi（可能没连无线，或不是 Windows）。")
            print("用 --ssid/--pass 手动指定，例如：")
            print('  python net_share.py --ssid MyHome --pass 12345678')
            return 2
        print("本机 WiFi：%s%s" % (ssid, ("（%s）" % bssid) if bssid else ""))

    pwd = args.pwd
    if not args.no_pass and pwd is None:
        pwd = wifi_password(ssid)
        if pwd is None:
            print("读不到口令（可能需要管理员权限）。")
            print("可以补 --pass，或加 --no-pass 走热点扫码模式。")

    packet = build_packet(ssid, pwd, token=args.token, bssid=bssid,
                          with_pass=not args.no_pass)

    print("\n将发送到 %s:%d" % (args.ip, args.port))
    print(json.dumps(packet, ensure_ascii=False, indent=2))
    print("\nESP32 侧的二维码内容会是：")
    print("  " + wifi_qr_payload(ssid, pwd if not args.no_pass else ""))
    print("\n（板上应依次看到：NET:1;<ssid> → QR:WIFI:... → NET:2 → NET:3;<ssid>;<ip>）")

    if args.dry_run:
        print("\n--dry-run：未实际发送。")
        return 0

    acks = send_udp(packet, args.ip, args.port, repeat=args.repeat)
    if acks:
        for addr, body in acks:
            print("ESP32 应答（%s）：%s" % (addr, body))
    else:
        print("没收到应答。可能原因：ESP32 没上电 / 不在同一网段 / 端口不是 %d / "
              "Windows 防火墙拦了入站。" % args.port)
        print("排查：先在 ESP32 上确认它已连上局域网，再试 --ip <它的地址> 单播。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
