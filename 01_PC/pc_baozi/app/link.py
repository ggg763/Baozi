#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
baozi-link.py — 包子「设备链路」桥接脚本（PC 端）

它把三块拼起来：

    后端(FastAPI + SQLite)   ←→   本脚本   ←→   TD1601 屏幕（USB-TTL 串口）

下行（数据上屏）
    小程序改习惯 ──► 后端
    本脚本 GET /api/v1/habits/today
        → 0x10 对时 / 0x11 BEGIN / 0x12 ITEM×N / 0x13 END
        → 包子屏幕 HABIT 卡片显示「作息 12d / 喝水 6/12 / 锻炼 MISS …」

上行（打卡回流）
    用户在屏幕上点某一格 → 板子发 0x90 → 本脚本
        → POST /api/v1/habits/{key}/checkin（或 /uncheck，slot 按序号翻译）
        → 后端记 habit_log → 天数由后端现算
    板子空手时点空卡片 → 0x92 → 本脚本立刻重推一遍

【为什么要单独一个常驻脚本，而不是让板子直连后端】
    板子没有网口也没跑 HTTP 栈，串口是它唯一的对外通道。
    所以"翻译官"只能放在 PC 上：串口在左、HTTP 在右。
    这也是答辩里那条链路「小程序 → 云端后端 → PC 端 → 设备」的中间那一跳。

【和 baozi-uart-test.py 的分工】
    baozi-uart-test.py = 手动自检工具（发一条看一条，验证线路通不通）
    baozi-link.py      = 常驻桥接（自动刷新、自动回写，验证业务闭环）
    组帧/点阵渲染的代码只有一份，本脚本直接 import 它，不复制
    （尤其是 name_w = 单字宽 这个坑，两边各写一份迟早对不上）。

依赖：pyserial、Pillow（中文习惯名要点阵渲染；没有 Pillow 也能跑，只是名字变成圆点）
      建议直接用后端那个 venv 的 python（两个都在里面了）。

用法：
    python baozi-link.py --list                    # 列出串口
    python baozi-link.py                           # 自动选唯一的串口，连后端 127.0.0.1:8000
    python baozi-link.py -p COM5                   # 指定串口
    python baozi-link.py --api http://127.0.0.1:8000
    python baozi-link.py --dry --once              # 不开串口，只打印会发什么（离线自检）
"""
import argparse
import json
import os
import sys
import threading
import time
import urllib.request
from datetime import datetime, timedelta

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))

# 默认后端地址（整合后由 pc_baozi/config.json 的 app_backend_port 决定，
# 这个默认值只在独立运行时用）
DEFAULT_API = "http://127.0.0.1:8000"

# ---------------------------------------------------------------------------
# 组帧 / 点阵 / 常量（原 baozi-uart-test.py，整合后改名 app/board.py）
#
# 两种导入方式都支持：
#   · 被 app.service 以包的形式导入  -> from . import board
#   · 直接 python app/link.py 独立跑  -> import board（同目录，靠 sys.path）
# ---------------------------------------------------------------------------
if HERE not in sys.path:
    sys.path.insert(0, HERE)

# 演示播报（pc_baozi/demo_bridge.py）在上一层目录：它能 import 就接上播报，
# 不能（比如单独 python app/link.py 跑）就安静降级 —— 板子那边的 2.5 秒兜底
# 窗口会让演示照常往下演，只是没有解说。
_PARENT = os.path.dirname(HERE)
if _PARENT not in sys.path:
    sys.path.insert(0, _PARENT)
try:
    import demo_bridge
except Exception:                           # noqa: BLE001
    demo_bridge = None

try:                                    # 包导入（pc_baozi 主程序走这条）
    from . import board as bt
    from . import phrases as rp
except ImportError:                      # 独立运行
    import board as bt                   # noqa: F401
    try:
        import phrases as rp             # noqa: F401
    except Exception as _e:
        rp = None
        print("[!] 话术池加载失败，提醒将使用固定话术：%s" % _e)

try:
    import serial
    from serial.tools import list_ports
except ImportError as _e:
    # 不 sys.exit：被 AppService 当模块导入时，退出进程会连带干掉语音链路
    raise ImportError("缺少 pyserial，请先安装：pip install pyserial") from _e


# ---------------------------------------------------------------------------
# 帧切分：printf 日志 与 协议帧 混在同一根线上（UART0 共线）
# ---------------------------------------------------------------------------
class FrameSplitter:
    """把串口流切成「协议帧」和「板子日志」两类。

    为什么不用 baozi-uart-test.py 里的 parse_stream()：
        那个是**一次性**解析，尾部收不全的半帧直接当噪声丢掉。
        手动自检时无所谓；这里是常驻循环，掉一帧就等于丢一次打卡
        （0x90 一共才 7 字节，115200 下被读取切成两半太常见了）。
        所以这里改成累加缓冲 + 只消费完整帧，残帧留到下一轮。
    """

    def __init__(self):
        self.buf = bytearray()

    def feed(self, chunk):
        """喂一段新字节，返回 (frames, logs)。
        frames = [(cmd, data_bytes), ...]  校验通过的帧
        logs   = bytes                      判定为文本的字节
        """
        self.buf += chunk
        frames, text = [], bytearray()
        i, n = 0, len(self.buf)

        while i < n:
            if self.buf[i] != bt.FRAME_HEAD:
                text.append(self.buf[i])
                i += 1
                continue
            if i + 3 > n:
                break                       # 帧头/LEN/CMD 还没到齐，留着
            ln = self.buf[i + 1]
            end = i + 4 + ln                # 含校验字节的整帧结束位置
            if end > n:
                break                       # 帧身还没到齐，留着
            cmd = self.buf[i + 2]
            data = bytes(self.buf[i + 3:i + 3 + ln])
            if bt.checksum(cmd, data) == self.buf[i + 3 + ln]:
                frames.append((cmd, data))
                i = end
            else:
                text.append(self.buf[i])    # 0xA5 只是文本里的巧合
                i += 1

        del self.buf[:i]
        if len(self.buf) > 4096:            # 防呆：长期对不上帧头别把内存吃光
            del self.buf[:-64]
        return frames, bytes(text)


# ---------------------------------------------------------------------------
# 后端访问（只用标准库 urllib，少一个第三方依赖）
# ---------------------------------------------------------------------------
def http_json(method, url, body=None, timeout=5):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


# ---------------------------------------------------------------------------
# 后端卡片 → 板子字段
# ---------------------------------------------------------------------------
STATE_BY_NAME = {
    "future":  bt.HABIT_STATE_FUTURE,
    "pending": bt.HABIT_STATE_PENDING,
    "done":    bt.HABIT_STATE_DONE,
    "missed":  bt.HABIT_STATE_MISSED,
}


def card_to_item(entry):
    """把后端的一张习惯卡片翻成板子要的字段。

    注意这里**只做翻译，不做判定**：谁是 done/pending/missed 完全听后端的
    （habit_logic.py 是唯一判定源）。板子和手机才能显示得一模一样。
    """
    card = entry.get("card") or {}
    slots = list(card.get("slots") or [])
    if len(slots) > bt.HABIT_MAX_SLOT:
        slots = slots[:bt.HABIT_MAX_SLOT]

    states = [s.get("state") for s in slots]
    if card.get("mode") == "single":
        # 单次型：后端给的 card.state 就是展示态（可能落在昨晚，跨天确认用）
        state = STATE_BY_NAME.get(card.get("state"), bt.HABIT_STATE_FUTURE)
    elif card.get("met"):
        state = bt.HABIT_STATE_DONE
    elif "pending" in states:
        state = bt.HABIT_STATE_PENDING
    elif "future" in states:
        state = bt.HABIT_STATE_FUTURE
    else:
        state = bt.HABIT_STATE_MISSED

    mask, cur, done = 0, bt.HABIT_SLOT_NONE, 0
    open_max = bt.HABIT_OPEN_NONE       # 一格都没开窗（今天全是 future）
    for i, s in enumerate(slots):
        st = s.get("state")
        # slots 是后端按时间升序排的：最后一个"不是 future"的格子就是
        # "已开窗到第几格"。板子据此拒掉提前打卡（它自己不看时间，
        # 判定源永远只有后端一个）。
        if st != "future":
            open_max = i
        if st == "done":
            mask |= (1 << i)
            done += 1
        elif st == "pending" and cur == bt.HABIT_SLOT_NONE:
            cur = i

    return {
        "key":      entry.get("key"),
        "name":     entry.get("name") or "",
        "date":     card.get("date"),       # 打卡回写要用：睡觉这类跨天习惯记在播报那天
        "slots":    [s.get("slot") for s in slots],
        "mode":     bt.HABIT_MODE_MULTI if card.get("mode") == "multi" else bt.HABIT_MODE_SINGLE,
        "total":    len(slots) or 1,
        "done":     done,
        "state":    state,
        "cur_slot": cur,
        "open_max": open_max,
        "streak":   int(card.get("streak") or 0),
        "mask":     mask,
    }


def build_items(payload):
    """后端 /habits/today 的应答 → 板子习惯列表（停用的不发，别占位置）"""
    cards = (payload.get("data") or {}).get("cards") or []
    return [card_to_item(e) for e in cards if e.get("enabled") is not False]


def fingerprint(items):
    """内容指纹：变了才下发，避免每轮都刷屏（刷屏时用户正好在看卡片）

    open_max 也进指纹：它决定"哪些格子现在能点"。虽然它几乎总跟着
    state/cur_slot 一起变，但"前格已 miss、后格仍 future"这种局面里
    state 一直是 FUTURE —— 只靠 state 会漏掉这次"开窗"，屏幕上就白等。
    """
    return json.dumps([(i["name"], i["mode"], i["total"], i["done"], i["state"],
                        i["cur_slot"], i["open_max"], i["streak"], i["mask"])
                       for i in items],
                      ensure_ascii=False)


# ---------------------------------------------------------------------------
# TODAY 页日程（v1.5）：GET /api/v1/events → 0x14/0x15×N/0x16
# ---------------------------------------------------------------------------
# 上次"日程条数"告警时的数量：只在数量变化时打印，避免每轮刷屏
ctx_cap_seen = {}


def build_sched_items(events, today):
    """后端 events → 板子日程列表。只发「今天及以后」，按 (date,time) 升序。"""
    out = []
    for e in events:
        d = e.get("date") or ""
        t = e.get("time") or "00:00"
        if not d or d < today:
            continue                       # 过去的日程不上屏（屏小，只看未来）
        try:
            hh, mm = int(t[:2]), int(t[3:5])
            dt = datetime.strptime(d, "%Y-%m-%d")
        except ValueError:
            continue
        out.append({
            "id":     e.get("id"),
            "title":  e.get("title") or "",
            "date":   d,
            "mon":     dt.month,
            "day":     dt.day,
            "weekday": dt.isoweekday(),     # 1=周一..7=周日，与协议一致
            "hh":     hh, "mm": mm,
            "status": bt.SCHED_STATUS_DONE if e.get("status") == "done"
                      else bt.SCHED_STATUS_PENDING,
        })
    out.sort(key=lambda x: (x["date"], x["hh"], x["mm"]))
    if len(out) > bt.SCHED_MAX_ITEMS:
        # 静默截断最坑：屏幕少一条却什么都没说。只在数量变化时提示，避免刷屏。
        if len(out) != ctx_cap_seen.get("n"):
            ctx_cap_seen["n"] = len(out)
            print("[!] 日程有 %d 条，板子只存得下 %d 条（按时间升序留前面），"
                  "多出来的不会上屏" % (len(out), bt.SCHED_MAX_ITEMS), flush=True)
        out = out[:bt.SCHED_MAX_ITEMS]
    return out


def sched_fingerprint(items):
    return json.dumps([(i["date"], i["hh"], i["mm"], i["title"], i["status"])
                       for i in items], ensure_ascii=False)


# ---------------------------------------------------------------------------
# 提醒（v2.0）：习惯到点 / 日程提前 N 分钟 → 板子弹卡(0x17) + 语音播报
# 说话内容统一走话术池 app/phrases.py（内置手写池 + 预生成池，洗牌袋防重复）；
# 时机判定只在这里做，打卡状态仍只听后端的。
#
# ★ 整合改动（2026-11）：语音出口统一到 pc_baozi 的主链路。
#   原来这里自己起一个 TCP 8765 语音服务端（baozi_voice.py），板子连进来推 PCM。
#   整合后 pc_baozi/voice_server.py 就是那个服务端（VAD/ASR/LLM/TTS 全在它后面），
#   再起第二个会直接撞"地址已在使用"。所以：
#     · 删掉 baozi_voice 依赖与 _get_voice_link()
#     · 播报改成调用注入的 speaker（AppService 会把 pipeline.say 塞进来）
#     · 主链路忙（正在对话）或板子不在时，这一句只弹卡不出声，下一句照常
# ---------------------------------------------------------------------------
REMIND_AHEAD_MIN = 10              # 日程没配提前量时的兜底（分钟）
# 扫描周期。原来 30 秒：对"提前1分钟"这种短提前量太粗——一次扫描偏差就是半分钟，
# 加上判定只有 GRACE_SEC 兜底，短提前量容易踩在边界上。改成 10 秒：
# 提醒扫描只读内存缓存、不发串口帧，代价几乎为零，换来时序精度 3 倍。
REMIND_SCAN_SEC  = 10              # 扫描周期
SETTINGS_PULL_SEC = 60             # 多久回后端拉一次提醒设置（免打扰等）
SEEN_PATH        = os.path.join(HERE, "remind_seen.json")   # 已播标记（防重复）

_tts_lock = threading.Lock()       # 串行播报：上一条没播完前别合成下一条
_speaker = None                    # fn(text) -> bool，由 AppService 注入（见 set_speaker）


def set_speaker(fn):
    """注入播报函数（pc_baozi 的 pipeline.say）。传 None 表示撤回。

    返回 True = 这一句已经交给主链路播出去了；False = 主链路忙 / 板子不在。
    """
    global _speaker
    _speaker = fn


# ---------------------------------------------------------------------------
# 一键演示的播报出口（由 AppService 注入 DemoAnnouncer）
#
# 板子按演示步骤打 `DEMO:<NAME>:<IDX>` 上来，这里转给 announcer：
# 它放一段本地预合成的 WAV，放完往下发一行 DEMORUN 让板子开演。
# 没注入就不处理（板子那边的 2.5 秒兜底窗口会自己往下走，只是没有解说）。
# ---------------------------------------------------------------------------
_demo = None

# 当前打开的串口对象（write_line 用它；重连后 run() 会重新赋值）
_SER = [None]


def set_demo(announcer):
    global _demo
    _demo = announcer


def write_line(text):
    """往屏幕串口发一行纯文本（下行命令），如 "DEMORUN"。

    为什么单独一个函数：板子的 UART0 是"协议帧 + printf + 文本命令"三线共线，
    文本命令走的是 uart_hal 的行解析，不需要组帧/校验，直接写即可。
    没开串口（--dry）就只打印。
    """
    if _SER[0] is None:
        print("        [↓ 文本] %s（未开串口，仅打印）" % text)
        return False
    try:
        _SER[0].write((text + "\r\n").encode("ascii"))
        _SER[0].flush()
        return True
    except Exception as e:                  # noqa: BLE001
        print("[!] 下行文本发送失败：%s" % e)
        return False


def _load_seen():
    try:
        with open(SEEN_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _save_seen(seen):
    try:
        with open(SEEN_PATH, "w", encoding="utf-8") as f:
            json.dump(seen, f, ensure_ascii=False)
    except Exception as e:
        print("[!] 已播标记写盘失败（不影响运行）：%s" % e)


def speak(text):
    """播报一句提醒。放后台线程：串口轮询 loop 不能被几秒的播报卡住。

    出口只有一个：注入的 speaker（pc_baozi 主链路 TTS → ESP32 → 喇叭）。
    没有 speaker（比如单独跑本脚本）或主链路忙，就只弹卡不出声 ——
    屏幕上的提醒卡片照常显示，不影响打卡。
    """
    def _run():
        with _tts_lock:
            if _speaker is None:
                print("[i] 未接入主语音链路，本次提醒只弹卡不出声")
                return
            try:
                if not _speaker(text):
                    print("[i] 主语音链路忙/板子不在，本次提醒只弹卡不出声")
            except Exception as e:
                print("[!] 提醒播报失败（只弹卡）：%s" % e)
    threading.Thread(target=_run, daemon=True).start()


def send_remind(ser, args, rtype, hh, mm, title):
    """0x17 提醒弹卡帧（板子无论在哪个界面都会弹，番茄钟中会等它跑完再弹）。"""
    frame, _blen = bt.build_remind(rtype, hh, mm, title)
    _write(ser, frame, args.dry)


# ---------------------------------------------------------------------------
# 提醒策略：免打扰 / 播报冷却
#
# 默认值不在本地编，统一由后端 GET /api/v1/settings 给（小程序改完，桥接
# 一分钟内就跟着变）。这样"手机上显示免打扰 07:00 结束"和"实际几点出声"
# 不会各说各话。
# ---------------------------------------------------------------------------
def refresh_settings(ctx, args):
    """拉一次后端设置。拉失败就沿用上一次的 ——
    不要因为一次网络抖动退回默认值，那会表现为"刚设的免打扰突然不生效了"。"""
    try:
        payload = http_json("GET", args.api.rstrip("/") + "/api/v1/settings")
    except Exception:
        return False
    if isinstance(payload.get("data"), dict):
        ctx["settings"] = payload["data"]
        return True
    return False


def _setting(ctx, key, default):
    return (ctx.get("settings") or {}).get(key, default)


def _setting_int(ctx, key, default):
    try:
        return int(float(_setting(ctx, key, default)))
    except (TypeError, ValueError):
        return default


def _setting_bool(ctx, key, default):
    v = str(_setting(ctx, key, "1" if default else "0")).strip().lower()
    return v in ("1", "true", "yes", "on", "开")


def _hm_to_min(s, fallback):
    try:
        h, m = str(s).split(":")[:2]
        return (int(h) * 60 + int(m)) % (24 * 60)
    except Exception:
        return fallback


def in_quiet(now, ctx):
    """现在是否处于免打扰时段。支持跨零点（23:00~07:00 这种）。

    起止填成同一个时刻 = 不设免打扰。少数派写法，但比"变成整天静音"合理得多。
    """
    if not _setting_bool(ctx, "quiet_enabled", True):
        return False
    start = _hm_to_min(_setting(ctx, "quiet_start", "00:00"), 0)
    end = _hm_to_min(_setting(ctx, "quiet_end", "07:00"), 7 * 60)
    if start == end:
        return False
    cur = now.hour * 60 + now.minute
    if start < end:
        return start <= cur < end
    return cur >= start or cur < end


def _habit_text(ctx, hkey, htype, name):
    """习惯提醒说什么：专属池（句子直接点了"喝水"，最自然）与通用池取并集来抽。

    通用池永远是内置手写的，所以即使某个习惯还没生成过专属池，也一定有的说。
    睡觉类只用通用池：那类话术本来就与习惯名无关（"该睡了"就是"该睡了"），
    而按"作息"这个习惯名生成出来的句子反而别扭（"一起来做个作息吧"）。
    """
    book = ctx.get("phrase_book")
    if book is not None and rp is not None:
        if htype == "sleep":
            cats = ["sleep"]
        else:
            cats = [rp.habit_pool_key(hkey), rp.category_of_habit(htype)]
        t = book.pick_any(cats, {"name": name})
        if t:
            return t
    return "该「%s」打卡啦" % name


def _event_text(ctx, title, mins):
    """日程提醒说什么：从话术池抽一句；池子缺失时退回固定话术。

    ⚠️ 话术池里 event 类共 7 句，其中 6 句带 {mins}、1 句不带
    （「小主人，『X』马上开始啦，别迟到哦！」）。抽中那句时分钟数就被吞掉了。
    这在 mins >= 2 时会造成误导：明明还有3分钟，却只说"马上开始"。
    所以这里加一道兜底：抽到的句子**不含任何分钟信息**、而实际还剩 >= 2 分钟时，
    再抽一次（洗牌袋会给出不同的一句，不会原地重复）；仍不行就用固定模板。
    """
    book = ctx.get("phrase_book")
    if book is not None and rp is not None:
        payload = {"title": title, "mins": mins}
        for _ in range(2):                    # 最多抽两次，避免死循环
            t = book.pick("event", payload)
            if not t:
                break
            # 只有"剩2分钟以上"才要求句子必须提到分钟数；
            # 剩1分钟时"马上开始"是准确的，不必强求。
            if mins < 2 or ("%d" % mins) in t or "分" in t:
                return t
        # 两次都没抽到带时间的：宁可退到固定模板，也不要说假话
        return "再有%d分钟，就「%s」了" % (mins, title)
    return "再有%d分钟，就「%s」了" % (mins, title)


def check_reminders(ser, ctx, args):
    """扫一遍缓存的后端数据，该提醒的提醒。

    刻意分成两步：先**收集**这一轮到点的，再按策略**决定每条怎么办**。
    原因是三种处理对"已播标记"的影响完全不同，混在收集里最容易写错：

        播报      → 标记已播
        免打扰    → 也标记，但不出声也不弹卡
                    （否则静音时段一结束，会把这几个钟头攒下的一口气全念出来）
        冷却中    → **不标记**，留给下一轮
                    （宁可晚几秒再提醒，也别两条挨着念）

    判定规则：
      习惯：某个 slot 进入 pending（后端判定窗口开了）→ 该提醒
      日程：按它自己的 remind_offsets 提前量，每档各到点一次
    """
    now = datetime.now()
    seen = ctx["remind_seen"]
    today = now.strftime("%Y-%m-%d")

    # 扫描前强制拉一次最新日程。
    # 【为什么每轮都拉】refresh_sched 的"内容没变就不下发"只省串口流量，
    #   **内存里的 last_events 是它拉到之后才赋值的**。如果只在首次拉，
    #   用户在手机上新建的日程要等主循环轮到 refresh_sched 才进内存，
    #   而提前量短的（提前1~2分钟）在那一轮已经过期 → 判进 GRACE 之外 → 永不响。
    #   这就是"手机设了、到点没声"的根因。环回 HTTP 两条不到 2ms，
    #   扫描周期 10 秒一次，这点开销完全换得起"设了就生效"。
    try:
        refresh(ser, ctx, args)
        refresh_sched(ser, ctx, args)
    except Exception as e:
        print("[!] 提醒前拉数据失败（沿用上一份）：%s" % e)

    # 清理两天前的旧标记，文件不无限膨胀
    cutoff = (now - timedelta(days=2)).strftime("%Y-%m-%d")
    for k in [k for k, d in seen.items() if str(d) < cutoff]:
        del seen[k]

    # 收集上限放宽到 20：免打扰时得把攒下的**全部**消费掉，
    # 只收两条的话，静音结束后剩下的还会一条条冒出来
    COLLECT_CAP = 20
    cands = []

    # ---- 习惯：slot 变 pending ----
    # ⚠️ 这里必须读后端**原始卡片**（payload.data.cards），不能用 build_items() 的产物：
    #    build_items 是给"上屏"用的，它把每个 slot 的状态压成了一个总 state；
    #    逐位判定需要 card.slots 里的原始 state。之前误用它的结果，
    #    e.get("card") 恒为空 → 习惯提醒其实一次都没触发过。
    payload = ctx.get("last_habits_payload")
    if payload:
        for e in ((payload.get("data") or {}).get("cards") or []):
            if e.get("enabled") is False:
                continue
            card = e.get("card") or {}
            name = e.get("name") or ""
            htype = e.get("type") or "point"
            date = card.get("date") or today
            if card.get("mode") == "single":
                pairs = [("main", card.get("state"))]
            else:
                pairs = [(s.get("slot"), s.get("state"))
                         for s in (card.get("slots") or [])]
            for slot, st in pairs:
                if st != "pending":
                    continue
                k = "h|%s|%s|%s" % (e.get("key"), date, slot)
                if k in seen:
                    continue
                cands.append((bt.REMIND_TYPE_HABIT, now.hour, now.minute, name,
                              _habit_text(ctx, e.get("key"), htype, name), k, htype))
                if len(cands) >= COLLECT_CAP:
                    break
            if len(cands) >= COLLECT_CAP:
                break

    # ---- 日程：按它自己的提前量（remind_offsets）到点就报 ----
    # 以前这里写死"提前 10 分钟"，可日程的提前量小程序里是能设的
    # （remind_offsets，秒数组，还能配多个，如 [600,3600] = 提前10分钟 + 提前1小时）。
    # 现在每个提前量各自到点报一次，用户设多少就是多少。
    for ev in (ctx.get("last_events") or []):
        if len(cands) >= COLLECT_CAP:
            break
        if ev.get("status") == "done":
            continue
        d, t = ev.get("date") or "", ev.get("time") or ""
        try:
            start = datetime.strptime("%s %s" % (d, t[:5]), "%Y-%m-%d %H:%M")
        except ValueError:
            continue
        delta = (start - now).total_seconds()
        if delta <= 0:
            continue

        # 到点未播的补播宽限（秒）。
        # 【为什么需要】原来只看 "delta > off 就跳过"，而扫描是 30 秒一轮、
        #   拉数据是 5 秒一轮，两者不同步。如果某个提前量窗口（几分钟宽）
        #   正好扫不到，这一档就永久丢了 —— 用户在手机上设的提醒等于没设。
        #   加了宽限之后：只要越过这个提前量之后还没播过，且没超宽限，就补播。
        # 【为什么不能无限宽】否则早上开电脑会把昨晚的日程一条条念一遍。
        GRACE_SEC = 180

        offs = []
        for o in (ev.get("remind_offsets") or [REMIND_AHEAD_MIN * 60]):
            try:
                o = int(o)
            except (TypeError, ValueError):
                continue
            if o > 0:
                offs.append(o)
        hit = None
        hit_off = None
        for off in sorted(set(offs)):
            if delta > off:              # 还没走到这个提前量
                continue
            k = "e|%s|%s|%d" % (ev.get("id"), d, off)
            if k in seen:
                continue
            # delta 已经比 off 小超过宽限 = 这一档错过的太久了，不再补
            if (off - delta) > GRACE_SEC:
                continue
            hit = k
            hit_off = off
            break
        if not hit:
            continue
        # 文案里的分钟数：准点用**用户设的提前量**，补播用**实际剩余**。
        # 【为什么不能只用 off】准点时 off 最好（设3分钟就听3分钟，跟扫描时序无关）。
        #   但如果是延迟补播，用 off 就会说假话：设提前5分钟、迟到180秒才扫到，
        #   实际只剩3分钟，却对着用户念"还有5分钟" —— 差 2 分钟以上，明显误导。
        # 【为什么不能只用 delta】delta 会被串口写阻塞污染（见 2.8.2②），
        #   设 181 秒却播"还有2分钟"，用户设的和听到的对不上。
        # 【折中】延迟 <= 90 秒：差异最多 1 分钟，用 off（体验稳定）；
        #        延迟 >  90 秒：已失真，用 delta（说真话）。
        lag = hit_off - delta
        mins = max(1, int(round((hit_off if lag <= 90 else delta) / 60.0)))
        cands.append((bt.REMIND_TYPE_SCHED, start.hour, start.minute,
                      ev.get("title") or "",
                      _event_text(ctx, ev.get("title") or "", mins), hit, "event"))

    # ---- 策略：免打扰 / 冷却 ----
    quiet = in_quiet(now, ctx)
    exempt_sleep = _setting_bool(ctx, "quiet_exempt_sleep", True)
    cooldown = _setting_int(ctx, "remind_cooldown_min", 5) * 60
    # 冷却只用来管"接二连三"：同一轮里两条挨着说，或前后两条隔了不到几秒。
    # 原来是无条件按 remind_cooldown_min（默认 5 分钟）卡，后果很实在：
    #   提前10分钟的日程刚说完，2 分钟后真正的"该打卡了"就被吞掉，等到下一轮
    #   已经错过窗口 → 用户设的提醒等于没设。
    # 现在：距上次播报 >= cooldown 就直接放行；不足 cooldown 时，只有"这一轮
    # 已经说过一条"才拦，单独一条照播。既不会连珠炮，也不会漏。
    spoken_ago = time.time() - ctx.get("last_spoken_ts", 0.0)
    can_speak = spoken_ago >= cooldown
    # 【习惯的冷却必须单独记账 —— 这是"设了不响"的真凶】
    # 原来习惯和日程共用 last_spoken_ts。实证时间线：
    #   17:21:0x 一条日程响了 → last_spoken_ts 被刷新 → 5 分钟冷却重新计时；
    #   17:23:00 用户的习惯到点、后端判 pending，但 spoken_ago 只有 115 秒
    #            → 每轮都被 continue 推到下一轮（不标记、不丢）；
    #   17:24:12 用户在手机上点了完成 → 状态变 done；
    #   17:26:0x 冷却终于满了，可这时它已经不是 pending → 永不播报。
    # 用户看到的现象就是"设了 17:23，没响"，而且日志里连一条尝试都没有。
    # 日程是用户亲手定的时刻，它响过不该让习惯闭嘴，所以分开记。
    habit_ago = time.time() - ctx.get("last_habit_spoken_ts", 0.0)
    can_habit = habit_ago >= cooldown

    # 一轮最多说几条。原来 2，太紧：三条日程撞同一分钟（比如用户一次设了
    # "提前10分钟 + 提前5分钟"两档，再叠一条习惯），第三条被挤到下一轮，
    # 而下一轮它可能已经过了宽限 → 丢。放宽到 4，仍然有上限防"开电脑念一串"。
    MAX_FIRE = 4
    fired, swallowed = [], 0
    for rtype, hh, mm, title, text, key, htype in cands:
        exempt = exempt_sleep and htype == "sleep"
        if quiet and not exempt:
            seen[key] = today
            swallowed += 1
            continue
        if len(fired) >= MAX_FIRE:
            continue         # 本轮说满了：不标记，下一轮再来
        # 【冷却只管习惯，不管日程】
        # 日程是用户在小程序里亲手定的时刻，到点就该响 —— 这是"提醒"的本分。
        # 习惯（喝水/护眼等）频率高、重复性强，才需要冷却防打扰。
        # 原来两者一视同仁，出了个说不通的现象：
        #   22:10:35 报完一条日程 → 22:12:00 另一条日程到点 → 冷却还剩 3.5 分钟
        #   → 推迟 → 等到 22:15:35 冷却满了，可那时它已经过期 95 秒、
        #   超出 GRACE 180？不，是超出后判进"错过太久" → **永久丢失**。
        #   用户设了两条提醒，只听得到一条，而且是随机听得到哪条。
        # 现在：日程无条件放行；习惯照旧受冷却约束。
        if not can_habit and habit_ago > 0 and htype != "event":
            # 冷却未满：推迟到下一轮再说。**不标记**，所以不会丢。
            # 但下一轮同样可能还在冷却 —— 靠 GRACE_SEC 兜住：宽限内一定能等到。
            continue
        seen[key] = today
        fired.append((rtype, hh, mm, title, text, key, htype))

    _save_seen(seen)

    if swallowed:
        # 前缀故意不用 [提醒]：否则日志里"静默 N 条"会被当成一次真的播报，
        # 翻日志时很容易误判
        print("[免打扰] %s~%s 时段内静默 %d 条"
              % (_setting(ctx, "quiet_start", "00:00"),
                 _setting(ctx, "quiet_end", "07:00"), swallowed))
    if not fired:
        return

    ctx["last_spoken_ts"] = time.time()
    for rtype, hh, mm, title, text, key, htype in fired:
        # 只有习惯才刷新习惯冷却；日程响过不再拖累习惯（见 can_habit 处的说明）
        if htype != "event":
            ctx["last_habit_spoken_ts"] = ctx["last_spoken_ts"]
        print("[提醒] %s" % text)
        if not args.dry:
            speak(text)                   # 声音（后台线程，不卡串口）
        try:
            send_remind(ser, args, rtype, hh, mm, title)   # 板子弹卡
        except ValueError as e:
            print("[!] 提醒组帧失败：%s" % e)
        # 记住"屏幕上这张卡是谁"。板子点卡只回 0x95（不带 idx/slot），
        # 所以只能由这边记住 —— 用户点掉时顺手打卡，见 0x95 分支。
        ctx["pending_remind"] = _remind_target(key) if rtype == bt.REMIND_TYPE_HABIT else None
        time.sleep(0.2)


def push_sched_all(ser, items, ctx, args):
    """0x14 对时后跟 0x15×N / 0x16。成功后刷新 idx→event_id 对照表。"""
    now = datetime.now()
    _write(ser, bt.build_frame(bt.CMD["time"],
                               [now.month, now.day, now.isoweekday(),
                                now.hour, now.minute, now.second]), args.dry)
    _write(ser, bt.build_frame(bt.CMD["sched_begin"], [len(items)]), args.dry)
    for i, it in enumerate(items):
        frame, _blen = bt.build_sched_item(i, it["title"], it["mon"], it["day"],
                                           it["weekday"], it["hh"], it["mm"],
                                           it["status"])
        _write(ser, frame, args.dry)
        if not args.dry:
            time.sleep(0.05)
    _write(ser, bt.build_frame(bt.CMD["sched_end"], [len(items)]), args.dry)
    ctx["map_sched"] = items
    if not args.dry:
        ctx["ack_sched"] = {"deadline": time.time() + 2.0, "retried": False}


def refresh_sched(ser, ctx, args, force=False, reason=""):
    """拉一次后端日程，内容变了（或 force）就整批下发。返回是否真的发了。"""
    url = args.api.rstrip("/") + "/api/v1/events"
    try:
        payload = http_json("GET", url)
    except Exception:
        ctx["sched_fingerprint"] = None   # 恢复后必重发（板子可能也重启过）
        return False
    events = (payload.get("data") or {}).get("events") or []
    today = datetime.now().strftime("%Y-%m-%d")
    items = build_sched_items(events, today)
    ctx["last_events"] = events              # 提醒扫描复用这份数据

    fp = sched_fingerprint(items)
    if not force and fp == ctx.get("sched_fingerprint"):
        return False
    try:
        push_sched_all(ser, items, ctx, args)
    except ValueError as e:
        print("[!] 日程组帧失败：%s" % e)
        return False
    ctx["sched_fingerprint"] = fp
    print("[↓] %s下发 %d 条日程：%s" % (reason, len(items),
          "、".join("%s %s%s" % (i["date"][5:], i["hh"], i["title"]) for i in items)
          or "(空)"))
    return True


def handle_sched_check(ctx, args, idx, action):
    """板子 0x93：[idx][action] → PATCH /api/v1/events/{id}/status。"""
    table = ctx.get("map_sched") or []
    if idx >= len(table):
        print("[!] 板子报了未知的日程序号 %d，忽略" % idx)
        return False
    it = table[idx]
    status = "pending" if action else "done"
    url = args.api.rstrip("/") + "/api/v1/events/%s/status" % it["id"]
    try:
        http_json("PATCH", url, {"status": status})
    except Exception as e:
        print("[!] 日程状态写回失败（%s）：%s" % (it["title"], e))
        return False
    print("[↑] 包子屏幕 → 后端：日程「%s」%s" %
          (it["title"], "已完成" if action == 0 else "撤销完成"))
    return True


# ---------------------------------------------------------------------------
# 下发
# ---------------------------------------------------------------------------
def _write(ser, frame, dry):
    """往串口写一帧。**任何失败都只吞掉这一帧，绝不让异常冒到主循环。**

    ⚠️ 这里是整条链路的单点：以前没有 try，`ser.write()` 抛
    SerialException(PermissionError) 会直接把常驻进程打死，表现是
    "后端未连接 + 屏幕不再更新 + 语音也不响了"，而且要人肉重启。
    触发条件很常见：USB 线松一下 / 板子复位时 USB 重新枚举 /
    串口被串口助手或 CDK 终端抢走 —— 都只是一瞬间的事，不该让服务停摆。

    处理策略：
      - 记下失败，交给主循环判断要不要重开串口（见 ctx["serial_dead"]）
      - 这一帧丢掉：反正还有指纹比对 + --resync 定期重发兜底
    """
    if dry:
        return True
    try:
        ser.write(frame)
        ser.flush()
        return True
    except Exception as e:                  # SerialException / OSError / 超时
        ctx_dead = _WRITE_ERR[0]
        if not ctx_dead:
            _WRITE_ERR[0] = True
            print("[!] 串口写失败，本帧丢弃（不会退出，主循环会尝试重开串口）：%s" % e)
        return False


# 串口写失败标志（True = 这一轮已经报过，别刷屏）
_WRITE_ERR = [False]
# 串口读失败标志。读出错时置 True，主循环会尝试重开串口。
_READ_ERR = [False]


def send_time_sync(ser, args):
    """只发 0x10 对时帧，不带任何数据（板子顶部时钟专用）。

    为什么必须单独有这么一个函数：
      0x10 原来只在 push_all / push_sched 的开头顺带发一次。而那两个函数
      只在「后端内容变了」时才被调用 —— 也就是说**习惯和日程不变，就永远不
      对时**。板子于是靠自己的 SysTick 自由累加，跑几十分钟后能偏 8 分钟。
      对外表现就是用户看到的两件事：
        ① 顶部时间不准；
        ② 一滑动屏幕时间就"变"——其实不是变，是重绘时刚好读到又被对时
           修正过的值，看起来像滑动把时间改掉了。

    v1.9 起带秒（第 6 字节）。这是上面第 ② 条的根治：
      只发时分时，板子收到就把秒清零，于是永远比 PC 慢 0~59 秒，每次对时
      都"往回跳一下"。带上秒之后板子被钉死在 PC 时间上，误差 <1 秒，
      分钟数只会往前走，肉眼看不出任何变化。
    """
    now = datetime.now()
    _write(ser, bt.build_frame(bt.CMD["time"],
                               [now.month, now.day, now.isoweekday(),
                                now.hour, now.minute, now.second]),
           args.dry)


def push_all(ser, items, ctx, args):
    """0x10 对时 + 0x11/0x12×N/0x13 整批下发。成功后刷新 idx→key 对照表。"""
    now = datetime.now()
    _write(ser, bt.build_frame(bt.CMD["time"],
                               [now.month, now.day, now.isoweekday(),
                                now.hour, now.minute, now.second]),
           args.dry)
    _write(ser, bt.build_frame(bt.CMD["habit_begin"], [len(items)]), args.dry)

    for i, it in enumerate(items):
        frame, _blen = bt.build_habit_item(i, it["name"], it["mode"], it["total"],
                                           it["done"], it["state"], it["cur_slot"],
                                           it["streak"], it["mask"], it["open_max"])
        _write(ser, frame, args.dry)
        # 板子主循环会整批排空帧队列；这里留一点间隔更稳
        if not args.dry:
            time.sleep(0.05)

    _write(ser, bt.build_frame(bt.CMD["habit_end"], [len(items)]), args.dry)

    # 板子回调 0x90 时只有序号，靠这张表翻译回 habit_key + slot
    ctx["map"] = items
    if not args.dry:
        # 板子 END 后会回 0x91；2 秒没等到 = 有帧丢了，主循环里重发一次
        ctx["ack_habits"] = {"deadline": time.time() + 2.0, "retried": False}


def refresh(ser, ctx, args, force=False, reason=""):
    """拉一次后端，内容变了（或 force）就整批下发。返回是否真的发了。"""
    url = args.api.rstrip("/") + "/api/v1/habits/today"
    try:
        payload = http_json("GET", url)
    except Exception as e:
        if ctx.get("online", True):
            print("[!] 连不上后端 %s" % args.api)
            print("    %s" % e)
            print("    后端起了吗？(baozi-backend\\start.bat)  改过端口就加 --api")
        ctx["online"] = False
        ctx["fingerprint"] = None       # 恢复连接后必须重发一遍（板子那边可能也重启了）
        return False
    ctx["online"] = True

    items = build_items(payload)
    ctx["last_habits_payload"] = payload     # 提醒扫描复用这份数据，不再多发请求
    if len(items) > bt.HABIT_MAX_COUNT:
        print("[!] 启用中的习惯有 %d 个，板子只存得下 %d 个，先发前 %d 个"
              % (len(items), bt.HABIT_MAX_COUNT, bt.HABIT_MAX_COUNT))
        items = items[:bt.HABIT_MAX_COUNT]

    fp = fingerprint(items)
    if not force and fp == ctx.get("fingerprint"):
        return False                    # 没变化：别打扰正在看屏幕的人

    try:
        push_all(ser, items, ctx, args)
    except ValueError as e:
        print("[!] 组帧失败：%s" % e)
        return False

    ctx["fingerprint"] = fp
    names = "、".join(i["name"] for i in items) or "(空)"
    print("[↓] %s下发 %d 个习惯：%s" % (reason, len(items), names))
    for i, it in enumerate(items):
        print("      [%d] %-8s %s %d/%d  streak=%dd"
              % (i, it["name"], _state_name(it["state"]), it["done"], it["total"], it["streak"]))
    return True


def _state_name(s):
    return {bt.HABIT_STATE_FUTURE: "future", bt.HABIT_STATE_PENDING: "pending",
            bt.HABIT_STATE_DONE: "done", bt.HABIT_STATE_MISSED: "missed"}.get(s, "?")


# ---------------------------------------------------------------------------
# 上行处理
# ---------------------------------------------------------------------------
def take_demo_requests(ctx, text):
    """从板子的日志文本里切出完整的 `DEMO:<NAME>:<IDX>` 行。

    返回 [(name, idx), ...]；没收到完整的行就返回空表。

    ⚠️ 为什么要带 ctx["demo_tail"] 这种跨轮缓冲：
        pump 每轮只读 256 字节，"DEMO:HAPPY:1\r\n" 完全可能被切成
        "DEMO:HA" + "PPY:1\r\n" 两半 —— 按整行判断的话这一句就丢了，
        表现是"演示到这一步突然没有解说"，而且极难查（日志里啥也没有）。
        帧切分那边早就有同样的问题并且是这么解决的，这里照抄。
    """
    if not text or demo_bridge is None:
        return []
    s = (ctx.get("demo_tail") or "") + text
    lines = s.split("\n")
    ctx["demo_tail"] = lines[-1]        # 最后一段可能还没收全，留到下一轮
    out = []
    for raw in lines[:-1]:
        got = demo_bridge.parse_demo_line(raw)
        if got is not None:
            out.append(got)
    return out


def handle_checkin(ctx, args, idx, ord_slot, action):
    """板子 0x90：[idx][slot][action] → 写回后端。返回是否成功。"""
    table = ctx.get("map") or []
    if idx >= len(table):
        print("[!] 板子报了未知的习惯序号 %d（多半是刚改过习惯），忽略" % idx)
        return False

    it = table[idx]
    slots = it.get("slots") or []
    if ord_slot >= len(slots):
        print("[!] 习惯「%s」没有第 %d 格，忽略" % (it["name"], ord_slot))
        return False

    slot = slots[ord_slot]
    verb = "checkin" if action == 0 else "uncheck"
    url = args.api.rstrip("/") + "/api/v1/habits/%s/%s" % (it["key"], verb)
    body = {"date": it.get("date"), "slot": slot, "source": "device"}
    try:
        http_json("POST", url, body)
    except Exception as e:
        print("[!] 写回后端失败（%s）：%s" % (it["name"], e))
        return False

    print("[↑] 包子屏幕 → 后端：%s %s   (date=%s slot=%s)"
          % (it["name"], "完成" if action == 0 else "撤销", it.get("date"), slot))
    return True


def _remind_target(key):
    """把提醒标记 "h|<habit_key>|<date>|<slot>" 拆成打卡要用的三段。
    不是习惯标记（或格式不对）返回 None。"""
    parts = (key or "").split("|")
    if len(parts) != 4 or parts[0] != "h":
        return None
    return {"key": parts[1], "date": parts[2], "slot": parts[3]}


def _checkin_from_remind(ctx, args, pr):
    """用户点了提醒弹卡 → 直接给那条习惯打卡。

    为什么放在 PC 侧而不是板端：0x17 弹卡帧里只有 type/hh/mm/标题，没有
    idx/slot —— 板子根本不知道自己弹的是哪一条，只能回一句"卡关了"。
    而 PC 发卡时就知道是谁，记在自己这儿最省事：不改协议、不用重烧固件。

    这就是"到点弹卡 → 点一下 → 已打卡"闭环里缺的那一环。以前点卡只关卡，
    用户还得再进习惯页找那条点一次，提醒的意义废了一半。
    """
    key, date, slot = pr.get("key"), pr.get("date"), pr.get("slot")
    if not key or not date or slot in (None, ""):
        return False
    url = args.api.rstrip("/") + "/api/v1/habits/%s/checkin" % key
    try:
        http_json("POST", url, {"date": date, "slot": slot, "source": "device"})
    except Exception as e:
        print("[!] 弹卡打卡写回失败（%s）：%s" % (key, e))
        return False
    print("[↑] 点弹卡 → 已打卡：%s  (date=%s slot=%s)" % (key, date, slot))
    return True


def _ack_timeout(ctx, now, slot, label, args, resend):
    """0x91 超时处理：最多补发一次，再超时就放弃本轮。

    抽成函数是为了让习惯/日程共用同一套逻辑，又各用各的 ack 槽位
    （共用一个槽位时后下的批次会覆盖前一批的等待状态）。
    """
    ack = ctx.get(slot)
    if not ack or now <= ack["deadline"]:
        return
    if ack["retried"]:
        print("[!] %s补发后仍未确认，放弃本轮（等下个周期自动再对）" % label)
        ctx[slot] = None
        return

    ack["retried"] = True           # ← 必须在 resend 之前：resend 会换掉这个 dict
    print("[!] 没等到板子的 0x91 确认（可能丢帧），重发一遍%s" % label)
    ctx[slot] = None                # 先清掉，resend 时会建新的
    resend()
    fresh = ctx.get(slot)
    if fresh:                       # 新的这次也算「补发过」，别再补了
        fresh["retried"] = True


def pump(splitter, ser, ctx, args):
    """收一轮串口。返回收到几帧。

    --dry 模式下 ser 是 None（有意不开串口），这里直接返回。
    不加这层判断的话主循环会抛 'NoneType' object has no attribute 'read'，
    被外层当成"串口读失败"打红字 —— 用 dry 排障时会误以为串口坏了。
    """
    if ser is None:
        return 0
    chunk = ser.read(256)
    if not chunk:
        return 0

    frames, text = splitter.feed(chunk)
    if text:
        s = text.decode("ascii", "replace").replace("\r", "\n")

        for name, idx in take_demo_requests(ctx, s):
            print("[↑] 板子请求播报：%s%s" % (name, "（第 %d 步）" % (idx + 1) if idx >= 0 else ""))
            if _demo is None:
                # 没接播报器（单独跑本脚本 / 没起主链路）：板子会自己按 2.5s 兜底往下演
                print("    [i] 未接入演示播报器，本次只演示不出声")
                continue
            try:
                _demo.on_board_line(name, idx)
            except Exception as e:          # noqa: BLE001
                print("[!] 演示播报处理失败：%s" % e)

        # 【板子重启检测】板子的习惯/日程缓存都在内存里，一断电就空了。
        # 但它的 0x92/0x94「催数据」只在**用户去点那张空卡片**时才发，
        # 所以重启后习惯页会一直显示空白，最长要等到 --resync(5分钟) 才补上
        # （实测日志：22:52 重启，一直到用户 22:53 手点空卡片才恢复）。
        # 这里盯住启动横幅，一看见就立刻全量重推，不用等 resync。
        #
        # ⚠️ 关键：不能在看见 "BaoZi boot" 的**当场**就推。
        #    实测时序（当前固件，逐行摘自 link.log）：
        #        ===== BaoZi boot =====
        #        board_init done (console UART0 @115200)
        #        servo stage skipped
        #        --- ui_app_init() ---
        #        [UI] ui_app v3 (pomodoro)
        #        face ready
        #        --- icnt8952bn_init() ---     ← 触摸屏初始化，这里会卡好几秒
        #        touch ready
        #        --- uart_hal_init() ---       ← UART 协议栈这时才就绪
        #    在 icnt8952bn_init() 期间推过去的数据全被丢掉，也不回 0x91，
        #    白白触发两轮补发。所以必须等 "uart_hal_init" 这一行。
        #
        #    这版固件打完 "--- uart_hal_init() ---" 会紧跟一行结果：
        #        [BaoZi] uart ready (UART0 PA18/PA17 @115200, shared with printf)
        #    认这条最准（"uart_hal_init" 只是阶段标题，理论上是初始化**开始**）。
        #    但从阶段标题到 ready 之间只有毫秒级，两者都认也无妨。
        #
        #    启动总耗时实测约 11 秒（ui_app_init → face ready 之间就卡了 3.3 秒），
        #    所以兜底给 15 秒：宁可等久一点把数据推准，也不要推早了被丢掉
        #    —— 推早了不但白费，还会因为板子不回 0x91 触发两轮补发刷屏。
        _now = time.time()
        if "BaoZi boot" in s:
            ctx["boot_at"] = _now
            ctx["boot_ready"] = False
        if ctx.get("boot_at") and not ctx.get("boot_ready"):
            ready = ("uart ready" in s) or ("uart_hal_init" in s)
            if ready or (_now - ctx["boot_at"] >= 15.0):
                ctx["boot_ready"] = True
                print("[↑] 板子启动完成（%s）→ 立刻全量重推习惯 + 日程"
                      % ("UART 已就绪" if ready else "15 秒兜底"))
                ctx["need_push"] = True
                ctx["need_sched_push"] = True

        if args.show_log:
            for line in s.split("\n"):
                line = "".join(c for c in line if c == "\t" or 32 <= ord(c) < 127).strip()
                if line:
                    print("        [板子] %s" % line)

    for cmd, data in frames:
        if cmd == 0x90 and len(data) >= 3:
            if handle_checkin(ctx, args, data[0], data[1], data[2]):
                # 【回推要故意晚 1.2 秒，别立刻推】
                # 板子点完格子会马上整屏重绘（~100ms 的 DMA 刷屏），而我们收到
                # 0x90 后立刻回一整批 0x11~0x13，正好撞进那段刷屏里 ——
                # 实机表现就是"点一下就一条色带"。
                # 让板子先把反馈画完，我们再下发校正数据。
                ctx["need_push_at"] = time.time() + 1.2
        elif cmd == 0x91:
            # 0x91 只带「已存几个」，不带类型（习惯和日程共用这个应答码），
            # 所以两边都清掉。这也正是为什么要分成两个独立槽位：
            # 原来共用一个 ctx["ack"]，日程下发会覆盖习惯的等待状态，
            # 习惯那次就再也没人确认（丢帧也不会补发）。
            n = data[0] if data else 0
            kind = "习惯" if ctx.get("ack_habits") else ("日程" if ctx.get("ack_sched") else "")
            print("[·] 板子确认：已存 %d 个%s" % (n, ("（" + kind + "）") if kind else "（习惯/日程）"))
            ctx["ack_habits"] = None
            ctx["ack_sched"] = None
        elif cmd == 0x92:
            print("[↑] 板子催习惯数据（屏幕上点了空卡片）")
            ctx["need_push"] = True
        elif cmd == 0x93 and len(data) >= 2:
            if handle_sched_check(ctx, args, data[0], data[1]):
                ctx["need_sched_push"] = True
        elif cmd == 0x94:
            print("[↑] 板子催日程数据（TODAY 页点了空卡片）")
            ctx["need_sched_push"] = True
        elif cmd == 0x95 and len(data) >= 2:
            clicked = (data[1] == 0)
            print("[↑] 板子提醒弹卡已关闭（%s）"
                  % ("点掉了" if clicked else "60s 超时"))
            pr = ctx.pop("pending_remind", None)
            if clicked and pr:
                # 【点弹卡 = 打卡】
                # 板子 0x95 不带 idx，点的是哪一条只能靠发卡时记下的
                # pending_remind。超时关闭（reason=1）不算，别凭空打卡。
                if _checkin_from_remind(ctx, args, pr):
                    # 晚 1.2 秒再回推：板子关完卡正恢复界面重绘，
                    # 立刻灌数据会撞出"点一下就一条色带"。
                    ctx["need_push_at"] = time.time() + 1.2
        elif cmd == 0x8F:
            print("[!] 板子报错 code=0x%02X" % (data[0] if data else 0))
        elif args.show_log:
            print("[·] %s" % bt.RSP.get(cmd, "0x%02X" % cmd))
    return len(frames)


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
def _port_handshake(dev, timeout=0.8):
    """向 dev 发一帧 0x10 对时，看它是不是包子屏幕。

    真屏幕收到后会 printf "[UART] sync time ..."（uart_protocol.c 里那行），
    这就是最可靠的"你是谁"应答 —— 不依赖 COM 号，也不依赖插在哪个 USB 口。

    返回：True 确认是屏幕 / False 没回话 / None 打不开（被别的程序占用或已拔）

    为什么可以放心往候选口发这一帧：
      0x10 只改时钟，不动习惯/日程；候选若是 ESP32 的 CH340 烧录口，
      收到这串字节就是一堆无意义数据，它走 WiFi 干活，串口只用于烧录和日志。
    """
    try:
        ser = serial.Serial(dev, 115200, timeout=0.1)
    except Exception:
        return None
    try:
        now = datetime.now()
        frame = bt.build_frame(bt.CMD["time"],
                               [now.month, now.day, now.isoweekday(),
                                now.hour, now.minute, now.second])
        ser.reset_input_buffer()
        ser.write(frame)
        ser.flush()
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            buf += ser.read(256)
            if b"[UART]" in buf or b"sync time" in buf:
                return True
        return False
    except Exception:
        return False
    finally:
        try:
            ser.close()
        except Exception:
            pass


def _port_is_junk(p):
    """蓝牙虚拟口 / CKLink 调试口 —— 都不是屏幕的口。"""
    d = (p.description or "").lower()
    return ("蓝牙" in d or "bluetooth" in d or "bth" in d
            or "cklink" in d or "ck-link" in d or "ck link" in d)


def _port_is_ttl(p):
    """USB-TTL 常见芯片：认准它们。"""
    d = (p.description or "").lower()
    return any(k in d for k in ("ch340", "ch341", "cp210", "cp21",
                                "ftdi", "ft232", "pl2303",
                                "usb-serial", "usb serial"))


def pick_port(explicit, quiet=False):
    """选屏幕串口。quiet=True 时不打印排查长文（后台重试用，否则刷屏）。"""
    ports = list(list_ports.comports())
    if explicit:
        return explicit

    real = [p for p in ports if not _port_is_junk(p)]
    if len(real) == 1:
        print("[i] 自动选中串口 %s（%s）" % (real[0].device, real[0].description))
        return real[0].device
    ttl = [p for p in real if _port_is_ttl(p)]
    if len(ttl) == 1:
        print("[i] 自动选中串口 %s（%s）" % (ttl[0].device, ttl[0].description))
        return ttl[0].device

    # 多个 USB-TTL 同时挂着（最常见：ESP32-CAM 的 CH340 也在）。
    # 以前这里直接 sys.exit(1) 让人手动 -p —— 现场双击 bat 就闪退，
    # 屏幕一片空白，看起来像"屏幕坏了"。现在改成逐个握手认人。
    if len(ttl) > 1:
        if not quiet:
            print("[i] 发现 %d 个 USB-TTL 串口，逐个握手确认哪一个是屏幕……" % len(ttl))
        hits = []
        for p in ttl:
            r = _port_handshake(p.device)
            if not quiet:
                tag = {True: "← 是屏幕", False: "无回应", None: "打不开(可能被占用)"}[r]
                print("    %-8s %-26s %s" % (p.device, p.description, tag))
            if r:
                hits.append(p)
        if len(hits) == 1:
            print("[i] 握手成功：%s = 包子屏幕" % hits[0].device)
            return hits[0].device
        if len(hits) > 1:
            print("[!] %s 都回了屏幕的握手，无法唯一确定，请用 -p 指定"
                  % ", ".join(h.device for h in hits))

    if quiet:
        raise RuntimeError("暂未找到屏幕串口")

    print("选不出屏幕的串口。当前可用串口：")
    for p in ports:
        print("   %-8s %s" % (p.device, p.description))
    if not real:
        print("   （没有发现 USB-TTL 串口：USB-TTL 插好了吗？驱动装了吗？）")
    else:
        print("   （排除蓝牙/CKLink 后仍有 %d 个候选，且握手都没回话）" % len(real))
        print("   请依次确认：")
        print("     1) 屏幕是不是没上电？板子启动要 11 秒才开串口。")
        print("     2) 串口被占用？关掉 CDK 的串口监视器 / 别开第二个实例。")
        print("     3) 都不行就在 config.json 里写死 app_board_port（如 COM9）")
    # 不再 sys.exit(1)：整合进 pc_baozi 后是后台线程，退出整个进程会连带
    # 把语音链路干掉。交给 AppService 决定怎么降级/提示。
    raise RuntimeError("没有可用/可确认的 TD1601 屏幕串口")


def main():
    ap = argparse.ArgumentParser(
        description="包子设备链路桥接（后端 ↔ TD1601 屏幕）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="列出本机串口后退出")
    ap.add_argument("-p", "--port", help="串口号，如 COM5（只有一个串口时可省略）")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("--api", default=DEFAULT_API, help="后端地址，默认 %s" % DEFAULT_API)
    ap.add_argument("--interval", type=float, default=5.0, help="拉后端间隔秒数，默认 5")
    ap.add_argument("--resync", type=float, default=300.0,
                    help="到点强制重发一次（防丢帧/防板子重启），秒，默认 300；0 关闭")
    ap.add_argument("--dry", action="store_true", help="不开串口，只打印会发什么")
    ap.add_argument("--once", action="store_true", help="只同步一次就退出（冒烟测试）")
    ap.add_argument("--no-log", dest="show_log", action="store_false",
                    help="不打印板子的 printf 日志（只留协议交互）")
    ap.set_defaults(show_log=True)
    args = ap.parse_args()

    if args.list:
        bt.list_serial_ports()
        return

    print("=" * 68)
    print("包子设备链路  后端 %s  <->  TD1601 屏幕" % args.api)
    print("=" * 68)
    try:
        run(args)
    except RuntimeError as e:
        print("[!] %s" % e)
        sys.exit(1)


# 停止信号：AppService 要在后台线程里优雅退出（原来只能 Ctrl+C 或杀进程）
STOP = threading.Event()


def stop():
    """请求退出常驻循环（AppService.stop() 调）。"""
    STOP.set()


REOPEN_SEC = 3.0          # 串口异常后每隔几秒重试一次（含"从没打开成功"的情况）


def _open_serial(args, state):
    """尝试打开屏幕串口。成功返回 (ser, port)，失败返回 (None, None)。

    【为什么不抛异常了】
        以前打不开串口就 raise RuntimeError，AppService 收到后打个
        "屏幕桥接未启动"就把线程结束了 —— 于是现场最常见的一个软故障
        （串口助手 / CDK 终端 / 第二个实例先占住了 COM 口）会变成
        **永久故障**：用户关掉那个程序，屏幕也不会再收到任何数据，
        必须重启 main.py。而板子的习惯/日程只存在内存里，
        表现就是"屏幕一片空白、点了说收不到电脑的指令"。

        串口打不开几乎全是"过一会儿自己就好"的软故障，所以这里一律
        返回 None 交给主循环慢慢重试，让桥接线程一直活着。

    state 只用来控制日志：首次失败打印完整排查提示，之后静默，
    避免每 3 秒刷一屏。
    """
    first = not state.get("tried")
    state["tried"] = True
    try:
        port = pick_port(args.port, quiet=not first)
    except Exception as e:
        if first:
            print("[!] 现在找不到屏幕串口：%s" % e)
        return None, None
    try:
        ser = serial.Serial(port, args.baud, timeout=0.05)
    except Exception as e:
        if first:
            print("[!] 打开串口 %s 失败：%s" % (port, e))
            print("    多半是被别的程序占着（串口助手 / CDK 终端 / 另一个 main.py）。")
            print("    不用管它 —— 关掉那个程序后这里会自动连上并重推数据。")
        return None, None
    time.sleep(0.3)
    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    print("[i] 串口 %s 已打开 @%d（协议口与 printf 共线，日志会自动过滤）"
          % (port, args.baud))
    _SER[0] = ser                    # 演示播报的 DEMORUN 要用它下行
    return ser, port


def run(args):
    """常驻桥接主循环。

    args 需要这些字段（argparse 的 Namespace 或等价对象都行）：
        port / baud / api / interval / resync / dry / once / show_log

    不再因为串口问题退出：找不到 / 打不开 / 中途掉线，都留在循环里重试。
    """
    ser = None
    port = None                        # --dry 模式下没有串口；重连逻辑会判断
    open_state = {}                    # 只管"首次失败要不要打完整提示"
    # 这两个标志是模块级的，上一次运行可能留在 True —— 开局先清掉，
    # 否则新起来的桥接会被上一次的"串口已死"状态带偏
    _WRITE_ERR[0] = False
    _READ_ERR[0] = False
    if args.dry:
        print("[i] --dry：不开串口，只打印将要下发的帧")
    else:
        ser, port = _open_serial(args, open_state)

    ctx = {"map": [], "map_sched": [], "fingerprint": None, "sched_fingerprint": None,
           "online": None, "need_push": False, "need_sched_push": False,
           "ack_habits": None, "ack_sched": None,
           "last_habits_payload": None, "last_events": None,
           "remind_seen": _load_seen(), "last_remind_scan": 0.0,
           "settings": {}, "last_settings_pull": 0.0, "last_spoken_ts": 0.0,
           "boot_at": 0.0, "boot_ready": True, "last_reopen": 0.0,
           "demo_tail": "",
           "phrase_book": rp.PhraseBook() if rp else None}
    splitter = FrameSplitter()

    if ctx["phrase_book"] is not None:
        print("[i] 提醒话术池：%s" % ctx["phrase_book"].describe())

    if refresh_settings(ctx, args):
        print("[i] 提醒策略：免打扰 %s~%s（%s），睡觉类%s；两次播报至少隔 %d 分钟"
              % (_setting(ctx, "quiet_start", "00:00"), _setting(ctx, "quiet_end", "07:00"),
                 "开" if _setting_bool(ctx, "quiet_enabled", True) else "关",
                 "可穿透" if _setting_bool(ctx, "quiet_exempt_sleep", True) else "不穿透",
                 _setting_int(ctx, "remind_cooldown_min", 5)))
    else:
        print("[i] 没拉到提醒设置（后端没起来？），先用内置默认值；下面每分钟会重试")

    # 语音出口：整合后由主链路（pipeline.say）负责，这里不再建任何服务端。
    if _speaker is None:
        print("[i] 没有接入主语音链路 —— 提醒只弹卡不出声（单独跑本脚本时正常）")

    # 串口没打开时先别推：推了也是全丢，还会刷一屏"串口写失败"
    if ser is not None or args.dry:
        ok = refresh(ser, ctx, args, force=True, reason="首次")
        if not ok and args.dry is False:
            print("[i] 首轮没发出去（后端没起来？）。没关系，下面每 %.0fs 自动重试。"
                  % args.interval)
        refresh_sched(ser, ctx, args, force=True, reason="首次")
    else:
        print("[i] 串口暂不可用，先不推数据；等串口连上后会自动全量重推")

    if args.once:
        if ser:
            time.sleep(1.0)
            pump(splitter, ser, ctx, args)
            ser.close()
        print("[i] --once 结束。")
        return

    print()
    print("开始常驻。屏幕操作：长按包子 → 左右滑翻页（TODAY 日程 / TIMER / HABIT 习惯），")
    print("习惯页点行进详情、点圆圈打卡；上下滑滚动列表；点日程行切换完成。")
    print("Ctrl+C 或 AppService.stop() 退出。")
    print()

    last_pull = time.time()
    last_sched_pull = time.time()      # 日程轮询独立计时（不能和 last_pull 共用）
    last_resync = time.time()
    try:
        while not STOP.is_set():
            try:
                pump(splitter, ser, ctx, args)
            except Exception as e:
                # 读失败也要打标记：串口可能已经掉了（USB 重新枚举 / 被别的程序抢走）。
                # 只打印不重开的话，后面每一轮都会继续报同样的错，日志刷屏且永远不恢复。
                if not _READ_ERR[0]:
                    _READ_ERR[0] = True
                    print("[!] 串口读取出错（会尝试重开串口）：%s" % e)

            now = time.time()

            # 【串口还没打开成功：这一轮只做"重试打开"这一件事】
            # 以前这里没有任何处理 —— run() 开头打不开就直接抛 RuntimeError
            # 出去了，桥接线程死掉，屏幕永远等不到数据（见 _open_serial 的说明）。
            # 现在留在循环里每 REOPEN_SEC 秒试一次，试开成功就全量重推。
            if ser is None and not args.dry:
                if now - ctx.get("last_reopen", 0.0) >= REOPEN_SEC:
                    ctx["last_reopen"] = now
                    ser, port = _open_serial(args, open_state)
                    if ser is not None:
                        splitter = FrameSplitter()      # 半截帧作废
                        ctx["need_push"] = True
                        ctx["need_sched_push"] = True
                        ctx["boot_at"] = 0.0            # 重新等一次启动横幅
                    else:
                        open_state.setdefault("last_hint", now)
                        if now - open_state["last_hint"] >= 30.0:
                            open_state["last_hint"] = now
                            print("[i] 屏幕串口仍未可用（被占用 / 没插好 / 板子没上电？），"
                                  "每 %.0f 秒自动重试一次……" % REOPEN_SEC)
                time.sleep(0.2)
                continue

            # 【提醒扫描放最前 —— 这是整段循环的第一优先级】
            # 【为什么】refresh / refresh_sched / 定期重发都要往串口写十几帧，
            #   115200 下一帧几十毫秒，一轮重发实测能占掉一分多钟。原来提醒扫描
            #   排在它们后面，等于被堵在队尾：设 181 秒的提前量，等真扫到时只剩
            #   103 秒（日志实证）。提前量越短越容易被整档错过。
            #   提到最前面之后，提醒的时序只受"上一轮提醒本身"影响，
            #   不再被任何下行任务拖延。
            if now - ctx["last_remind_scan"] >= REMIND_SCAN_SEC:
                ctx["last_remind_scan"] = now
                try:
                    check_reminders(ser, ctx, args)
                except Exception as e:
                    print("[!] 提醒扫描出错（这一轮跳过，下一轮再来）：%s" % e)

            # 0x90 回写后的「延迟重推」到点了没（见 pump 里 0x90 分支的说明）
            if ctx.get("need_push_at") and now >= ctx["need_push_at"]:
                ctx["need_push_at"] = None
                ctx["need_push"] = True

            if ctx.get("need_push"):
                ctx["need_push"] = False
                try:
                    refresh(ser, ctx, args, force=True, reason="打卡后校正 ")
                except Exception as e:
                    print("[!] 下发习惯失败（这一轮跳过）：%s" % e)
            elif now - last_pull >= args.interval:
                last_pull = now
                try:
                    refresh(ser, ctx, args)
                except Exception as e:
                    print("[!] 下发习惯失败（这一轮跳过）：%s" % e)

            # 【必须用独立的计时器，不能和上面的 last_pull 共用】
            # 原来这里写 `elif now - last_pull >= args.interval`，而 last_pull
            # 刚在上面那个分支里被更新成 now 了 —— 判断恒为假，这一段等于死代码。
            # 后果：日程只在"打卡校正"或"定期重发(5分钟)"时才拉一次。
            # 用户在手机上新建的日程，最长要等 5 分钟才进到桥接的内存里，
            # 提前量短的（比如提前2分钟）在这一轮里已经过期 → 永远不响。
            # 这就是"手机设了、到点没声"的根因之一。
            if ctx.get("need_sched_push"):
                ctx["need_sched_push"] = False
                last_sched_pull = now
                try:
                    refresh_sched(ser, ctx, args, force=True, reason="日程校正 ")
                except Exception as e:
                    print("[!] 下发日程失败（这一轮跳过）：%s" % e)
            elif now - last_sched_pull >= args.interval:
                last_sched_pull = now
                try:
                    refresh_sched(ser, ctx, args)
                except Exception as e:
                    print("[!] 下发日程失败（这一轮跳过）：%s" % e)

            # 兜底重发：万一有帧丢了（校验失败会被丢），或者板子中途重启过，
            # 靠它把状态重新对齐，不用人工干预。
            if args.resync > 0 and now - last_resync >= args.resync:
                last_resync = now
                for _fn, _why in ((lambda: refresh(ser, ctx, args, force=True, reason="定期重发 "), "习惯"),
                                  (lambda: refresh_sched(ser, ctx, args, force=True, reason="定期重发 "), "日程")):
                    try:
                        _fn()
                    except Exception as e:
                        print("[!] 定期重发%s失败：%s" % (_why, e))

            # 提醒设置（免打扰 / 冷却）：每分钟回后端拉一次，改完一分钟内生效
            if now - ctx["last_settings_pull"] >= SETTINGS_PULL_SEC:
                ctx["last_settings_pull"] = now
                refresh_settings(ctx, args)

            # 【独立对时 —— 和"数据有没有变化"解耦】
            # v1.9 起 0x10 带秒，所以不再需要"卡在整分 ±2 秒"这种将就做法：
            # 随便什么时候发都行，板子收到就直接对齐到 PC 的 hh:mm:ss。
            # 每 10 秒一次，误差 <1 秒，分钟数只会向前，绝不会往回跳
            # —— 这才是"时间会变"的真正根治（旧做法让板子每 55 秒回退一次）。
            # 首次无条件发（板子可能是 --:--，或刚重启过）。
            _want_sync = ("last_time_sync" not in ctx) or \
                         (now - ctx["last_time_sync"] >= 10.0)
            if _want_sync:
                ctx["last_time_sync"] = now
                try:
                    send_time_sync(ser, args)
                except Exception as e:
                    print("[!] 对时失败（下一轮再来）：%s" % e)

            # 0x91 确认超时 → 重发一次（板子队列溢出丢帧的兜底）
            #
            # ⚠️ 这里有个极易踩的坑：必须先置 retried=True，再调 refresh*。
            #    因为 refresh*/push_* 内部会把 ack 槽位 **换成一个新的 dict**
            #    （retried 默认 False）。如果先重发后标记，标记就写到了被丢弃的
            #    旧 dict 上，新 dict 永远是 retried=False
            #    → 每 2 秒补发一次，无限刷屏（实测板子重启时连刷 30+ 轮）。
            #
            # 习惯和日程必须各用各的槽位：共用一个的话，后下的那批会覆盖前一批
            # 的等待状态，前一批就再也没人确认了。
            _ack_timeout(ctx, now, "ack_habits", "习惯", args,
                         lambda: refresh(ser, ctx, args, force=True, reason="补发 "))
            _ack_timeout(ctx, now, "ack_sched", "日程", args,
                         lambda: refresh_sched(ser, ctx, args, force=True, reason="补发 "))

            # 【串口自愈】写失败/读失败后立刻重开，最多每 3 秒试一次。
            # 触发场景：USB 线松一下、板子复位时 USB 重新枚举、串口被别的
            # 程序（串口助手 / CDK 终端）抢走。这些都是一瞬间的事，
            # 但原来会直接把常驻进程打死 —— 用户看到的是"后端未连接"。
            if port and (_WRITE_ERR[0] or _READ_ERR[0]):
                if now - ctx.get("last_reopen", 0.0) >= 3.0:
                    ctx["last_reopen"] = now
                    print("[!] 串口异常，尝试重新打开 %s ..." % port)
                    try:
                        try:
                            ser.close()
                        except Exception:
                            pass
                        time.sleep(0.3)
                        ser = serial.Serial(port, args.baud, timeout=0.05)
                        ser.reset_input_buffer()
                        _SER[0] = ser            # 演示播报的下行要跟着换新串口
                        splitter = FrameSplitter()      # 半截帧作废，别带着脏缓冲
                        ctx["demo_tail"] = ""           # 半行同理（演示播报的上行）
                        _WRITE_ERR[0] = False
                        _READ_ERR[0] = False
                        # 重连后板子可能重启过，强制重推一遍
                        ctx["need_push"] = True
                        ctx["need_sched_push"] = True
                        print("[i] 串口已重新打开，准备重推数据")
                    except Exception as e:
                        print("[!] 重开串口失败（%s），3 秒后再试" % e)

            time.sleep(0.02)
    except KeyboardInterrupt:
        print("\n退出。")
    finally:
        _SER[0] = None
        if ser:
            ser.close()


if __name__ == "__main__":
    main()
