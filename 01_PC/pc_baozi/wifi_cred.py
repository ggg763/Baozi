# -*- coding: utf-8 -*-
"""wifi_cred.py —— 读本机当前 WiFi 凭证，拼成配网二维码要的那两行文本。

为什么单独拆一个模块：这件事有两个使用者，规则必须只有一份。

    · main.py            启动并连上 ESP32 后，经 CTRL_TD_TEXT 推给 TD1601 屏幕
    · tools/push_qr.py   PC -> UART0 直连（绕开 ESP32）的台架/现场兜底

两条路的文本完全相同，所以长度/字符集这些"板子能收多少"的规矩也放在这里，
免得两边各写一套、改一处忘一处。

板子的三个硬限制（都是固件里写死的）：

    · 行 ≤ 159 字节 —— src/uart_hal.c 的 LINE_MAX=160（含结束符），
      uart_esp.c 的 ESP_LINE_MAX 同值。超长**整行丢弃**，绝不截断。
    · 二维码内容 ≤ 127 字节 —— UserFolder/qr/net_card.h 的 NET_PAYLOAD_MAX=128。
    · 只收可打印 ASCII —— line_feed() 会把非 ASCII 字节丢掉，中文 SSID
      走文本命令这条路一定出坏码（屏上字库也只有 ASCII）。

只支持 Windows（netsh）。读不到就返回 None，由调用方决定怎么提示。
"""

from __future__ import annotations

import re
import subprocess

# 与固件 src/uart_hal.c / uart_esp.c 一致（含结束符）
LINE_MAX = 160
# 与 UserFolder/qr/net_card.h 的 NET_PAYLOAD_MAX 一致（实际可用 127）
PAYLOAD_MAX = 127


# ------------------------------------------------------------------ netsh
def _netsh(args: list[str]) -> str | None:
    """跑一条 netsh，返回解码后的文本；失败返回 None。"""
    try:
        out = subprocess.run(["netsh"] + args, capture_output=True, timeout=8).stdout
    except Exception:                                        # noqa: BLE001
        return None
    for enc in ("utf-8", "gbk", "latin-1"):
        try:
            return out.decode(enc)
        except UnicodeDecodeError:
            continue
    return None


def current_wifi() -> tuple[str | None, str | None]:
    """本机当前连着的 WiFi -> (ssid, bssid)；读不到就 (None, None)。"""
    text = _netsh(["wlan", "show", "interfaces"])
    if not text:
        return None, None

    ssid = None
    bssid = None
    for line in text.splitlines():
        line = line.strip()
        # 英文系统 "SSID : xxx"、中文系统同样以 SSID 开头；SSID 后面可能直接是 BSSID 行
        m = re.match(r"^SSID\s*:\s*(\S.*)$", line)
        if m and ssid is None:
            ssid = m.group(1).strip()
            continue
        m = re.match(r"^(?:BSSID)\s*:\s*([0-9a-fA-F:]{17})", line)
        if m:
            bssid = m.group(1).lower()
    return ssid, bssid


def wifi_password(ssid: str | None) -> str | None:
    """从本机已保存的配置里取该网络的口令（明文）。取不到返回 None。"""
    if not ssid:
        return None
    text = _netsh(["wlan", "show", "profile", "name=%s" % ssid, "key=clear"])
    if not text:
        return None
    for line in text.splitlines():
        # 英文 "Key Content" / 中文 "关键内容"
        m = re.match(r"^\s*(?:Key Content|关键内容)\s*:\s*(.*)$", line)
        if m:
            return m.group(1).strip()
    return None


# ------------------------------------------------------------------ 拼串
def _esc(s: str) -> str:
    """Zxing 的 WIFI: 转义规则：\\ ; , : " 都要反斜杠转义。"""
    return (s or "").replace("\\", "\\\\").replace(";", "\\;") \
                    .replace(",", "\\,").replace(":", "\\:").replace('"', '\\"')


def wifi_payload(ssid: str, pwd: str | None) -> str:
    """手机相机原生认的 WIFI: 内容（二维码里装的就是它）。"""
    return "WIFI:T:WPA;S:%s;P:%s;;" % (_esc(ssid), _esc(pwd or ""))


def first_non_ascii(text: str | None) -> str | None:
    """板子只收可打印 ASCII；返回第一个越界字符（没有就 None）。"""
    for ch in text or "":
        if not (0x20 <= ord(ch) <= 0x7E):
            return ch
    return None


def provision_lines(ssid: str, pwd: str | None) -> list[str]:
    """(ssid, pwd) -> 要依次发给板子的整行文本。

    推不了就抛 ValueError，消息直接给人看（调用方原样打印即可）。
    """
    if not ssid:
        raise ValueError("SSID 是空的")

    bad = first_non_ascii(ssid) or first_non_ascii(pwd)
    if bad is not None:
        raise ValueError(
            "SSID/口令里有非 ASCII 字符（%r）。板子的文本行只收可打印 ASCII，"
            "非 ASCII 字节会被整行丢掉，只能换一个纯英文热点名，"
            "或者手动用 --raw 推一个不含中文的 payload。" % bad)

    lines = ["NET:1;%s" % ssid, "QR:" + wifi_payload(ssid, pwd)]
    for line in lines:
        n = len(line)
        if n > LINE_MAX - 1:
            raise ValueError("这一行 %d 字节，超过固件单行上限 %d：%s"
                             % (n, LINE_MAX - 1, line))
        if line.startswith("QR:") and (n - 3) > PAYLOAD_MAX:
            raise ValueError("二维码内容 %d 字节，超过板子上限 %d"
                             % (n - 3, PAYLOAD_MAX))
    return lines


def local_provision_lines() -> tuple[list[str], str, bool]:
    """一步到位：读本机 WiFi -> 要发的整行。

    返回 (lines, ssid, has_password)；读不到 SSID 就抛 ValueError。
    """
    ssid, _bssid = current_wifi()
    if not ssid:
        raise ValueError("读不到本机当前 WiFi（没连无线，或者不是 Windows）")
    pwd = wifi_password(ssid) or ""
    return provision_lines(ssid, pwd), ssid, bool(pwd)
