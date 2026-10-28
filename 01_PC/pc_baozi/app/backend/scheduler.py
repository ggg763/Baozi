# -*- coding: utf-8 -*-
"""提醒调度器：每隔 30 秒做三件事

  1. scan_events()    日程提醒（按 remind_offsets 提前量推）
  2. scan_habits()    习惯提醒（由 habits 表驱动，按打卡位逐位触发）
  3. settle_habits()  把过了确认窗口还没确认的 pending 结算成 missed

【两套幂等机制，别混用】
  - 日程走 remind_log 的唯一键 (event_id, offset_sec, send_date)
  - 习惯走 habit_log 的 pending 行本身：INSERT OR IGNORE 插得进去 = 这条提醒还没播报过。
    为什么不用 remind_log？它的唯一键里 event_id 是 NULL（习惯没有日程 id），
    而 SQLite 里 NULL 互不相等，压根去不了重。

【为什么习惯提醒走 habit_log 而不是另开一张表】
  播报本身就是"写一条待确认记录"这个动作，两件事是同一件事。
  顺带一个好处：界面上的待打卡圆点是**按时钟算的**，不依赖调度器有没有跑成功。
  调度器漏了一轮，用户照样能看到"现在该做这个"，只是没听到播报。
"""
import asyncio
import json
from datetime import datetime, timedelta

import db
import habit_logic

# 推送回调（由 main.py 注入，用于 WebSocket 推送给机器人）
push_handler = None

# 习惯提醒的触发窗口（秒）。比扫描周期宽，进程短暂卡顿也不会漏播；
# 重复触发由 pending 行的唯一索引挡掉。
HABIT_FIRE_WINDOW = 120


def set_push_handler(fn):
    global push_handler
    push_handler = fn


def _now():
    return datetime.now()


def _to_dt(s):
    s = s.replace("T", " ")[:16]
    return datetime.strptime(s, "%Y-%m-%d %H:%M")


def _today():
    return _now().strftime("%Y-%m-%d")


# ---------------- 日程提醒 ----------------
def _already_sent(event_id, offset_sec):
    conn = db.get_conn()
    row = conn.execute(
        "SELECT 1 FROM remind_log WHERE event_id=? AND offset_sec=? AND send_date=?",
        (event_id, offset_sec, _today()),
    ).fetchone()
    conn.close()
    return row is not None


def _mark_sent(event_id, offset_sec):
    conn = db.get_conn()
    try:
        conn.execute(
            "INSERT INTO remind_log(event_id, offset_sec, send_date) VALUES(?,?,?)",
            (event_id, offset_sec, _today()),
        )
        conn.commit()
    except Exception:
        pass  # 唯一索引冲突 = 已推送过，忽略
    conn.close()


async def _push(remind_type, title, text, emotion="remind_gentle"):
    payload = {
        "type": "remind",
        "remind_type": remind_type,
        "title": title,
        "text": text,
        "emotion": emotion,
    }
    if push_handler:
        await push_handler(payload)
    else:
        print("[提醒]", text)


def scan_events():
    """扫描日程提醒：event_time - offset 到点即推送，幂等去重"""
    now = _now()
    conn = db.get_conn()
    rows = conn.execute(
        "SELECT * FROM events WHERE status='pending' AND event_time >= ?",
        (now.strftime("%Y-%m-%d %H:%M"),),
    ).fetchall()
    conn.close()

    for r in rows:
        event_time = _to_dt(r["event_time"])
        offsets = json.loads(r["remind_offsets"] or "[600]")
        for off in offsets:
            trigger = event_time - timedelta(seconds=off)
            if 0 <= (now - trigger).total_seconds() <= 30 and not _already_sent(r["id"], off):
                if off >= 86400:
                    text = f"明天{event_time.strftime('%H:%M')}要{r['title']}，记得准备"
                elif off >= 3600:
                    text = f"还有{off // 3600}小时，{r['title']}抓紧了"
                else:
                    text = f"{r['title']}时间快到了"
                asyncio.create_task(_push("event", r["title"], text))
                _mark_sent(r["id"], off)


# ---------------- 习惯提醒 ----------------
def _habit_specs():
    """只取启用中的习惯 —— 关掉的习惯不该再提醒"""
    conn = db.get_conn()
    rows = conn.execute(
        "SELECT * FROM habits WHERE user_id=? AND enabled=1 ORDER BY sort_order, id",
        (db.DEFAULT_USER,),
    ).fetchall()
    conn.close()
    return [habit_logic.spec_of_row(r) for r in rows]


def _mark_pending(habit_key, date, slot):
    """写一条 pending 当作"这条提醒已播报"的幂等标记。

    返回 True 表示这次是新插进去的（= 还没播报过，该播）；
    False 说明这位置已经有记录（播报过了，或用户已经确认过），不要重复播。
    """
    conn = db.get_conn()
    try:
        cur = conn.execute(
            "INSERT OR IGNORE INTO habit_log(user_id,habit_key,log_date,slot,status,source) "
            "VALUES(?,?,?,?,'pending','device')",
            (db.DEFAULT_USER, habit_key, date, slot),
        )
        conn.commit()
        fresh = cur.rowcount > 0
    except Exception as e:
        print("[scheduler] 写 pending 失败:", e)
        fresh = False
    conn.close()
    return fresh


def scan_habits():
    """按 habits 表驱动扫描习惯提醒。

    时间点型（睡觉/锻炼）只有 1 个打卡位 'main'；
    间隔型（喝水）有 N 个位（09:00~21:00 每小时一个），每位各提醒一次。
    两类用同一段循环，所以新增习惯类型不用改这里。
    """
    now = _now()
    date = now.strftime("%Y-%m-%d")

    for h in _habit_specs():
        for slot in habit_logic.slots_of(h):
            # 用 remind_min（= 习惯时刻 - 提前量），不是习惯本身的时刻：
            # 用户要的是"提前一段时间提醒"，比如 23:30 睡觉 → 23:00 就开口
            tm = habit_logic.remind_min(h, slot)
            trigger = now.replace(hour=tm // 60, minute=tm % 60, second=0, microsecond=0)
            delta = (now - trigger).total_seconds()
            if 0 <= delta < HABIT_FIRE_WINDOW:
                if _mark_pending(h["key"], date, slot):
                    asyncio.create_task(
                        _push(
                            "habit",
                            h.get("name") or "",
                            habit_logic.remind_text(h),
                            habit_logic.remind_emotion(h),
                        )
                    )


# ---------------- 超时结算 ----------------
def settle_habits():
    """过了确认窗口还挂着 pending 的，结算成 missed

    这一步是"断签"的唯一来源：播报之后到了截止时间还没确认，才叫没做到。
    顺手把"习惯已被删除/停用，但还挂着 pending"的记录也结掉，
    否则它们永远停在待确认状态，界面上会一直有个点不掉。
    """
    now = _now()
    specs = {h["key"]: h for h in _habit_specs()}

    conn = db.get_conn()
    rows = conn.execute(
        "SELECT id, habit_key, log_date, slot FROM habit_log "
        "WHERE user_id=? AND status='pending'",
        (db.DEFAULT_USER,),
    ).fetchall()

    settled = 0
    for r in rows:
        h = specs.get(r["habit_key"])
        if h is None or habit_logic.window_closed(h, r["log_date"], r["slot"], now):
            conn.execute("UPDATE habit_log SET status='missed' WHERE id=?", (r["id"],))
            settled += 1
    conn.commit()
    conn.close()

    if settled:
        print("[scheduler] 已结算 %d 条超时未确认的打卡" % settled)


async def scheduler_loop():
    """主循环：每 30 秒扫一次"""
    while True:
        try:
            scan_events()
            scan_habits()
            settle_habits()
        except Exception as e:
            print("[scheduler] error:", e)
        await asyncio.sleep(30)
