# -*- coding: utf-8 -*-
"""习惯打卡纯逻辑层 —— 与小程序 utils/habitStore.js 一一对应

【为什么要单独一层】
调度器（什么时候提醒、什么时候结算）和接口（前端要显示的卡片）用的是同一套判定。
如果两边各写一份，迟早出现"手机显示已完成、包子屏幕显示未完成"这种对不上的情况。
所以这里每个函数都与 JS 版本逐行对应，**改动时两边必须同步**。

【三条铁律】
  1. 提醒 ≠ 打卡 —— 天数只由 done 产生；播报只写 pending，绝不加天数
  2. 跨天记录记在"播报那天" —— 23:30 的睡觉提醒记当天，次日确认时把 date 传回来
  3. 今天没确认 ≠ 断签 —— pending（窗口还开着）既不计数也不中断

【habit_log.status 状态机】
  pending  提醒已播报，等确认（调度器写入；同时充当"这条提醒是否已播报"的幂等标记）
  done     用户确认（含事后补卡）
  missed   超窗口未确认，由调度器结算
"""
import json
from datetime import datetime, timedelta

SLOT_MAIN = "main"
MAX_LOOKBACK = 400          # streak 最多往回看这么多天
RECENT_DAYS = 7             # 判断"这是不是一个在用的习惯"


# ---------------- 时间小工具（全整数运算，避免浮点误差） ----------------
def to_min(hm):
    """'09:30' -> 570"""
    parts = str(hm if hm is not None else "0:0").split(":")
    h = int(parts[0]) if parts[0].strip() else 0
    m = int(parts[1]) if len(parts) > 1 and parts[1].strip() else 0
    return h * 60 + m


def to_hm(minutes):
    minutes = int(minutes) % (24 * 60)
    return "%02d:%02d" % (minutes // 60, minutes % 60)


def fmt_date(d):
    return d.strftime("%Y-%m-%d")


def shift_date(s, delta):
    d = datetime.strptime(s, "%Y-%m-%d")
    return fmt_date(d + timedelta(days=delta))


def now_min(now):
    return now.hour * 60 + now.minute


def _round_half_up(x):
    """JS 的 Math.round 是四舍五入（0.5 进位），Python 的 round 是银行家舍入，这里对齐 JS"""
    return int(x + 0.5)


# ---------------- 习惯规格 ----------------
def spec_of_row(row):
    """数据库行（或 dict）-> 逻辑层用的习惯规格：config 展开成扁平键 + 打卡规格"""
    d = dict(row)
    cfg = d.get("config") or "{}"
    if isinstance(cfg, str):
        try:
            cfg = json.loads(cfg)
        except Exception:
            cfg = {}

    ratio = d.get("min_ratio")
    out = {
        "id": d.get("id"),
        "key": d.get("habit_key") or d.get("key"),
        "name": d.get("name"),
        "type": d.get("type") or "point",
        "enabled": bool(d.get("enabled", 1)),
        "confirm_mode": d.get("confirm_mode") or "on_time",
        "window_min": d.get("window_min") if d.get("window_min") is not None else 30,
        "deadline": d.get("deadline") or "12:00",
        "min_ratio": ratio if ratio is not None else 0.6,
    }
    out.update(cfg)
    return out


def normalize_habit(h):
    """和 JS 的 normalizeHabit 对齐：补齐缺失的确认模式/窗口/达标线"""
    o = dict(h)
    if o.get("confirm_mode") is None:
        o["confirm_mode"] = "after_ward" if o.get("type") == "sleep" else "on_time"
    if o["confirm_mode"] == "on_time" and not o.get("window_min"):
        o["window_min"] = 30
    if o["confirm_mode"] == "after_ward" and not o.get("deadline"):
        o["deadline"] = "12:00"
    if o.get("min_ratio") is None:
        o["min_ratio"] = 0.6
    o["lead_min"] = lead_min(o)          # 把提前量补成实值，下游只管读
    return o


def slots_of(h):
    """一天有几个打卡位。interval 型按 开始/结束/间隔 实时算出（结束时刻独占）"""
    if h.get("type") != "interval":
        return [SLOT_MAIN]
    step = int(h.get("interval") or 60)
    if step <= 0:
        step = 60
    s = to_min(h.get("start") or "09:00")
    e = to_min(h.get("end") or "21:00")
    out = []
    t = s
    while t < e and len(out) < 48:
        out.append(to_hm(t))
        t += step
    return out or [SLOT_MAIN]


def slot_short(slot):
    """点阵下方的短标签：整点只显示小时，非整点显示 HH:MM"""
    if slot == SLOT_MAIN:
        return ""
    return slot[0:2] if slot[3:] == "00" else slot


def main_time_of(h):
    """单次型习惯的提醒时刻"""
    if h.get("type") == "sleep":
        return h.get("sleep") or "23:30"
    return h.get("time") or "20:00"


# ---------------- 提前量（lead_min） ----------------
# 用户要的是"提前一段时间提醒"，不是一个准点闹钟。
# 提前量存在习惯 config 里的 lead_min（分钟），缺省按类型给：
#   作息 sleep   提前 30 分钟（23:30 睡 → 23:00 就催）
#   喝水 interval 不提前（一天十几次，再提前就成骚扰了）
#   单次 point   提前 15 分钟（锻炼/吃药，给点准备时间）
# 提醒时刻 = 习惯时刻 - 提前量；**打卡窗口同样跟着提前**，
# 这样"包子一开口就能打卡"，不用干等到点。
LEAD_DEFAULT = {"sleep": 30, "interval": 0, "point": 15}


def lead_min(h):
    """这个习惯提前多少分钟提醒"""
    v = h.get("lead_min")
    if v is None:
        v = LEAD_DEFAULT.get(h.get("type") or "point", 0)
    try:
        v = int(v)
    except (TypeError, ValueError):
        v = 0
    return max(0, min(v, 24 * 60))          # 夹在 0 ~ 1440，防手滑填出负数


def trigger_min(h, slot):
    """某个打卡位的**习惯时刻**（分钟）"""
    return to_min(slot) if slot != SLOT_MAIN else to_min(main_time_of(h))


def remind_min(h, slot):
    """某个打卡位的**提醒时刻**（分钟）= 习惯时刻 - 提前量。

    夹到 >= 0：0 点附近的习惯再往前提前就会翻到前一天，那是没有意义的。
    """
    return max(0, trigger_min(h, slot) - lead_min(h))


def remind_text(h):
    t = h.get("type")
    if t == "sleep":
        return "该睡了，别熬夜"
    if t == "exercise":
        return "到锻炼时间了，今天练什么？"
    return "该%s了，做完记得告诉我" % (h.get("name") or "打卡")


def remind_emotion(h):
    return {"sleep": "sleepy", "interval": "remind_warm", "point": "remind_warm"}.get(
        h.get("type"), "remind_gentle"
    )


# ---------------- 单点状态判定 ----------------
# done    已确认
# pending 还在确认窗口内，可以点（也包含调度器写入的显式待确认）
# missed  窗口已关且没确认（仍可点补卡）
# future  还没到时间
def state_of(h, date, slot, now, log):
    rec = log.get((date, slot))
    if rec:
        st = rec.get("status")
        if st == "done":
            return "done"
        if st == "pending":
            return "pending"        # 调度器写入的待确认，不看时钟（由调度器负责结算）
        if st == "missed":
            return "missed"

    today = fmt_date(now)
    if date > today:
        return "future"
    if date < today:
        # 跨天型（睡觉/锻炼）：窗口延续到次日 deadline
        if h.get("confirm_mode") == "after_ward" and date == shift_date(today, -1):
            if now_min(now) <= to_min(h.get("deadline") or "12:00"):
                return "pending"
        return "missed"

    # 同一天
    cur = now_min(now)
    start = remind_min(h, slot)          # 提前量把"开窗"一起提前：包子一开口就能打卡
    if cur < start:
        return "future"
    if h.get("confirm_mode") == "after_ward":
        return "pending"
    win = int(h.get("window_min") or 30)
    return "pending" if cur <= start + win else "missed"


def window_closed(h, date, slot, now):
    """确认窗口是否已关（调度器结算 missed 用）。

    on_time   窗口 = 提醒时刻 + window_min（不跨天，最多到 23:59）
    after_ward 窗口 = 次日 deadline
    """
    if h.get("confirm_mode") == "after_ward":
        close = datetime.strptime(
            shift_date(date, 1) + " " + (h.get("deadline") or "12:00"), "%Y-%m-%d %H:%M"
        )
    else:
        close = datetime.strptime(
            date + " " + to_hm(remind_min(h, slot)), "%Y-%m-%d %H:%M"
        ) + timedelta(minutes=int(h.get("window_min") or 30))
        eod = datetime.strptime(date + " 23:59", "%Y-%m-%d %H:%M")
        if close > eod:
            close = eod
    return now > close


# ---------------- 一天的整体状态（日粒度结算） ----------------
# met     当天已达标（单次型：做了；多次型：完成率 >= 达标线）
# pending 当天还没结算（窗口还开着 / 还有没到的打卡位）
# missed  当天已定型且未达标
def day_status(h, date, now, log):
    slots = slots_of(h)
    states = [state_of(h, date, s, now, log) for s in slots]
    done = states.count("done")
    met = (done == 1) if len(slots) == 1 else (done / len(slots) >= h.get("min_ratio", 0.6))
    if met:
        return "met"
    if "pending" in states or "future" in states:
        return "pending"
    return "missed"


def streak_of(h, now, log):
    """连续天数：pending 的那天「不中断也不计入」—— 这就是"今天待打卡不算断签" """
    today = fmt_date(now)
    n = 0
    if day_status(h, today, now, log) == "met":
        n += 1
    d = shift_date(today, -1)
    for _ in range(MAX_LOOKBACK):
        st = day_status(h, d, now, log)
        if st == "met":
            n += 1
            d = shift_date(d, -1)
        elif st == "pending":
            d = shift_date(d, -1)     # 不中断、不计入
        else:
            break
    return n


# ---------------- 单次型：挑「最该处理」的那一条展示 ----------------
def has_recent_record(h, from_date, log):
    """往前 7 天内有没有任何记录（判断"这是不是一个在用的习惯"）"""
    for i in range(RECENT_DAYS):
        if (shift_date(from_date, -i), SLOT_MAIN) in log:
            return True
    return False


def single_view(h, now, log):
    """优先级：昨晚待确认 > 今天窗口内 > 昨晚已错过（且是长期习惯，方便补卡）> 今天"""
    today = fmt_date(now)
    y = shift_date(today, -1)
    for d in (y, today):
        if state_of(h, d, SLOT_MAIN, now, log) == "pending":
            return {"date": d, "state": "pending", "hero": True}
    if state_of(h, y, SLOT_MAIN, now, log) == "missed" and has_recent_record(h, y, log):
        return {"date": y, "state": "missed", "hero": False}
    return {"date": today, "state": state_of(h, today, SLOT_MAIN, now, log), "hero": False}


def rel_label(h, date, today):
    if date == today:
        return "今晚" if h.get("type") == "sleep" else "今天"
    if date == shift_date(today, -1):
        return "昨晚" if h.get("type") == "sleep" else "昨天"
    return date[5:]


# ---------------- 对外主接口：一张卡片的打卡模型 ----------------
def build_card(h, date, now, log):
    """输出字段与小程序 buildCard() 完全一致，前端可以直接 h.ck = card"""
    h = normalize_habit(h)
    today = fmt_date(now)
    slots = slots_of(h)
    need = h.get("min_ratio", 0.6)

    items = [{"slot": s, "short": slot_short(s), "state": state_of(h, date, s, now, log)} for s in slots]
    done = sum(1 for x in items if x["state"] == "done")
    total = len(items)
    met = (done == 1) if total == 1 else (done / total >= need)

    card = {
        "date": date,
        "isToday": date == today,
        "dateText": "今天" if date == today else date[5:],
        "mode": "multi" if total > 1 else "single",
        "slots": items,
        "total": total,
        "done": done,
        "ratioText": "%d%%" % _round_half_up(done / total * 100),
        "met": met,
        "minRatioText": "%d%%" % _round_half_up(need * 100),
        "streak": streak_of(h, now, log),
        "todayMet": day_status(h, today, now, log) == "met",
        "hero": False,
        "state": "",
        "text": "",
        "sub": "",
    }

    if card["mode"] == "single":
        v = single_view(h, now, log)
        card["date"] = v["date"]
        card["isToday"] = v["date"] == today
        card["dateText"] = "今天" if v["date"] == today else v["date"][5:]
        card["state"] = v["state"]
        card["hero"] = v["hero"]
        card["done"] = 1 if v["state"] == "done" else 0
        card["met"] = v["state"] == "done"
        rel = rel_label(h, v["date"], today)
        if v["state"] == "done":
            card["text"] = rel + " · 已完成"
            card["sub"] = "长按可撤销"
        elif v["state"] == "pending":
            card["text"] = rel + " · 待确认"
            card["sub"] = "点一下完成"
        elif v["state"] == "missed":
            card["text"] = rel + " · 未完成"
            card["sub"] = "点一下补上"
        else:
            card["text"] = rel + " · 还没到"
            card["sub"] = to_hm(remind_min(h, SLOT_MAIN)) + " 提前提醒"
    return card


def pending_items(habits_cards):
    """待确认清单（页面顶部横幅用）。停用的习惯不进来，否则关掉的习惯还在催你"""
    out = []
    for item in habits_cards or []:
        ck = item.get("card") or {}
        if item.get("enabled") is False:
            continue
        if ck.get("mode") == "single" and ck.get("hero") and ck.get("state") == "pending":
            out.append({"key": item.get("key"), "name": item.get("name"), "text": ck.get("text")})
    return out
