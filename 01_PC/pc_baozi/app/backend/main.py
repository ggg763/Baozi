# -*- coding: utf-8 -*-
"""包子（BaoZi）后端主程序：FastAPI + 接口 + WebSocket + 提醒调度"""
import asyncio
import json
from contextlib import asynccontextmanager
from datetime import datetime
from typing import Optional

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware

import db
import habit_logic
import scheduler

DEFAULT_USER = "demo-user"

# 提醒策略的默认值。
# 【为什么放在后端而不是桥接里】
#   这两套默认值小程序（设置页）和桥接（真正播报的那一环）都要用。
#   各写一份迟早对不上（"手机上显示免打扰 07:00 结束，实际 08:00 才出声"）。
#   所以统一在后端：GET /api/v1/settings 会把默认值和库里已有的值合并后返回，
#   两边都不用自己编默认值。
#   0/1 表示开关；时间一律 "HH:MM"。
DEFAULT_SETTINGS = {
    "sleep_time": "23:30",              # 老字段，作息提醒时间
    "water_interval": "60",             # 老字段，喝水提醒间隔（分钟）
    "quiet_enabled": "1",               # 免打扰总开关
    "quiet_start": "00:00",             # 免打扰开始
    "quiet_end": "07:00",               # 免打扰结束
    "quiet_exempt_sleep": "1",          # 睡觉类提醒可以穿透免打扰
    "remind_cooldown_min": "5",         # 两次播报之间至少隔几分钟
}


# ---------- WebSocket 连接管理（推送给机器人） ----------
class ConnectionManager:
    def __init__(self):
        self.connections = {}

    async def connect(self, device_id, ws):
        self.connections[device_id] = ws
        print("[ws] 设备上线:", device_id)

    def disconnect(self, device_id):
        self.connections.pop(device_id, None)
        print("[ws] 设备下线:", device_id)

    async def broadcast(self, payload):
        for ws in list(self.connections.values()):
            try:
                await ws.send_json(payload)
            except Exception:
                pass


manager = ConnectionManager()


# ---------- 一句话摘要（模板拼接，不用大模型） ----------
def build_summary(events):
    if not events:
        return "今天没有安排，好好休息。"
    names = "、".join(f"{e['event_time'][11:16]}{e['title']}" for e in events[:3])
    head = f"今天有 {len(events)} 件事：{names}"
    if len(events) > 3:
        head += f" 等 {len(events)} 件事"
    return head + "。"


def _to_event_dict(row):
    e = dict(row)
    e["remind_offsets"] = json.loads(e["remind_offsets"] or "[600]")
    e["date"] = e["event_time"][:10]
    e["time"] = e["event_time"][11:16]
    return e


# ---------- 生命周期 ----------
@asynccontextmanager
async def lifespan(app):
    db.init_db()
    scheduler.set_push_handler(manager.broadcast)
    task = asyncio.create_task(scheduler.scheduler_loop())
    print("[main] 后端启动完成，监听 8000 端口")
    yield
    task.cancel()


app = FastAPI(title="包子 BaoZi 后端", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ---------- 日程接口 ----------
@app.get("/api/v1/events")
async def list_events(date: Optional[str] = None, status: Optional[str] = None):
    conn = db.get_conn()
    sql = "SELECT * FROM events WHERE user_id=?"
    params = [DEFAULT_USER]
    if date:
        sql += " AND event_time LIKE ?"
        params.append(date + "%")
    if status:
        sql += " AND status=?"
        params.append(status)
    sql += " ORDER BY event_time"
    rows = conn.execute(sql, params).fetchall()
    conn.close()

    events = [_to_event_dict(r) for r in rows]
    today = datetime.now().strftime("%Y-%m-%d")
    today_events = [e for e in events if e["date"] == today]
    return {"code": 0, "data": {"summary": build_summary(today_events), "events": events}}


@app.post("/api/v1/events")
async def create_event(payload: dict):
    title = payload.get("title")
    event_time = payload.get("event_time")
    if not title or not event_time:
        raise HTTPException(400, "title 和 event_time 必填")
    conn = db.get_conn()
    cur = conn.execute(
        "INSERT INTO events(user_id,title,event_type,event_time,remind_offsets,repeat_rule,remind_way,source) "
        "VALUES(?,?,?,?,?,?,?,?)",
        (
            DEFAULT_USER,
            title,
            payload.get("event_type", "custom"),
            event_time,
            json.dumps(payload.get("remind_offsets", [600])),
            payload.get("repeat_rule", "none"),
            payload.get("remind_way", "voice_emotion"),
            payload.get("source", "applet"),
        ),
    )
    conn.commit()
    eid = cur.lastrowid
    conn.close()
    return {"code": 0, "data": {"id": eid}}


@app.put("/api/v1/events/{event_id}")
async def update_event(event_id: int, payload: dict):
    fields = []
    values = []
    for k in ("title", "event_type", "event_time", "repeat_rule", "remind_way", "status"):
        if k in payload:
            fields.append(f"{k}=?")
            values.append(payload[k])
    if "remind_offsets" in payload:
        fields.append("remind_offsets=?")
        values.append(json.dumps(payload["remind_offsets"]))
    if not fields:
        return {"code": 0}
    values.append(event_id)
    conn = db.get_conn()
    conn.execute(f"UPDATE events SET {', '.join(fields)} WHERE id=?", values)
    conn.commit()
    conn.close()
    return {"code": 0}


@app.delete("/api/v1/events/{event_id}")
async def delete_event(event_id: int):
    conn = db.get_conn()
    conn.execute("DELETE FROM events WHERE id=?", (event_id,))
    conn.commit()
    conn.close()
    return {"code": 0}


@app.patch("/api/v1/events/{event_id}/status")
async def set_event_status(event_id: int, payload: dict):
    conn = db.get_conn()
    conn.execute("UPDATE events SET status=? WHERE id=?", (payload.get("status", "done"), event_id))
    conn.commit()
    conn.close()
    return {"code": 0}


# ---------- 设置接口 ----------
@app.get("/api/v1/settings")
async def get_settings():
    """返回"默认值 + 库里已存的值"合并后的结果。

    ⚠️ 不合并的话，从没保存过设置的用户拿到的是空 dict，
    小程序设置页会一片空白、桥接只能自己编默认值 —— 两边就分叉了。
    """
    conn = db.get_conn()
    rows = conn.execute(
        "SELECT item_key, item_value FROM settings WHERE user_id=?", (DEFAULT_USER,)
    ).fetchall()
    conn.close()
    out = dict(DEFAULT_SETTINGS)
    out.update({r["item_key"]: r["item_value"] for r in rows})
    return {"code": 0, "data": out}


@app.put("/api/v1/settings")
async def update_settings(payload: dict):
    settings = payload.get("settings", payload)
    conn = db.get_conn()
    for k, v in settings.items():
        conn.execute(
            "INSERT INTO settings(user_id,item_key,item_value) VALUES(?,?,?) "
            "ON CONFLICT(user_id,item_key) DO UPDATE SET item_value=excluded.item_value",
            (DEFAULT_USER, k, str(v)),
        )
    conn.commit()
    conn.close()
    return {"code": 0}


# ---------- 习惯定义 CRUD ----------
def _to_habit_dict(row):
    """数据库行 -> 接口对象

    同时给出两套字段名：habit_key / key、confirm_mode / confirmMode ...
    小程序用 camelCase，设备端与旧代码用 snake_case，两边都能直接读，
    省得以后再为命名对不齐返工。
    """
    h = dict(row)
    h["config"] = json.loads(h["config"] or "{}")
    h["enabled"] = bool(h["enabled"])
    # 前端直接展开 config，省去一层嵌套
    h.update(h["config"])

    h["key"] = h["habit_key"]
    h["confirmMode"] = h.get("confirm_mode") or "on_time"
    h["windowMin"] = h.get("window_min") if h.get("window_min") is not None else 30
    h["deadline"] = h.get("deadline") or "12:00"
    h["minRatio"] = h["min_ratio"] if h.get("min_ratio") is not None else 0.6
    # 提前量：老库的 config 里没有这字段，这里按类型补默认值再回给前端，
    # 免得小程序编辑页拿到 undefined 把提前量显示成空白
    h["leadMin"] = habit_logic.lead_min(h)
    h["lead_min"] = h["leadMin"]
    return h


def _load_log(conn, habit_key, from_date, to_date):
    """一次把区间内的打卡记录捞成字典，避免逐天查库"""
    rows = conn.execute(
        "SELECT log_date, slot, status FROM habit_log "
        "WHERE user_id=? AND habit_key=? AND log_date BETWEEN ? AND ?",
        (DEFAULT_USER, habit_key, from_date, to_date),
    ).fetchall()
    return {(r["log_date"], r["slot"]): {"status": r["status"]} for r in rows}


def _recent_log(conn, habit_key, now=None):
    """算连续天数要往前看一段历史，一次捞 400 天"""
    now = now or datetime.now()
    today = now.strftime("%Y-%m-%d")
    return _load_log(
        conn, habit_key, habit_logic.shift_date(today, -habit_logic.MAX_LOOKBACK), today
    )


def _habit_spec(conn, habit_key):
    row = conn.execute(
        "SELECT * FROM habits WHERE user_id=? AND habit_key=?", (DEFAULT_USER, habit_key)
    ).fetchone()
    return habit_logic.spec_of_row(row) if row else None


def _card_payload(conn, habit_key, date=None):
    """指定日期这张卡片的完整打卡模型 + 天数，供前端直接渲染"""
    spec = _habit_spec(conn, habit_key)
    if not spec:
        return None
    now = datetime.now()
    d = date or now.strftime("%Y-%m-%d")
    log = _recent_log(conn, habit_key, now)
    return {
        "key": habit_key,
        "card": habit_logic.build_card(spec, d, now, log),
        "streak": habit_logic.streak_of(spec, now, log),
    }


@app.get("/api/v1/habits")
async def list_habits():
    conn = db.get_conn()
    rows = conn.execute(
        "SELECT * FROM habits WHERE user_id=? ORDER BY sort_order, id",
        (DEFAULT_USER,),
    ).fetchall()
    now = datetime.now()
    out = []
    for r in rows:
        h = _to_habit_dict(r)
        spec = habit_logic.spec_of_row(r)
        streak = habit_logic.streak_of(spec, now, _recent_log(conn, h["habit_key"], now))
        h["streak"] = streak
        h["days"] = streak      # days 改成派生值：列还在，但不再是"播报一次加一天"的假数据
        out.append(h)
    conn.close()
    return {"code": 0, "data": {"habits": out}}


@app.post("/api/v1/habits")
async def create_habit(payload: dict):
    name = (payload.get("name") or "").strip()
    if not name:
        raise HTTPException(400, "name 必填")

    habit_key = payload.get("habit_key") or "h%d" % int(datetime.now().timestamp() * 1000)
    htype = payload.get("type", "point")

    # config：优先用显式传入的 config，否则从扁平字段里挑出属于本类型的键
    config = payload.get("config")
    if config is None:
        keys = ("time", "repeat", "interval", "start", "end", "sleep", "wake", "lead_min")
        config = {k: payload[k] for k in keys if k in payload}

    conn = db.get_conn()

    # 打卡规格：没传就按类型推导，规则与 habit_logic.normalize_habit / 迁移脚本保持一致。
    # 否则新建的 sleep 型习惯会拿到表默认的 on_time，和迁移来的作息规则不一样。
    confirm_mode = payload.get("confirm_mode") or payload.get("confirmMode")
    if not confirm_mode:
        confirm_mode = "after_ward" if htype == "sleep" else "on_time"
    window_min = payload.get("window_min", payload.get("windowMin"))
    if window_min is None:
        window_min = 30
    deadline = payload.get("deadline") or "12:00"
    ratio = payload.get("min_ratio", payload.get("minRatio"))
    if ratio is None:
        # 单次型（sleep/point）本身就只有一次，"完成率"等价于"做没做"，达标线取满
        ratio = 0.6 if htype == "interval" else 1.0

    try:
        cur = conn.execute(
            "INSERT INTO habits(user_id,habit_key,name,type,config,enabled,days,sort_order,"
            "confirm_mode,window_min,deadline,min_ratio) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                DEFAULT_USER,
                habit_key,
                name,
                htype,
                json.dumps(config, ensure_ascii=False),
                1 if payload.get("enabled", True) else 0,
                int(payload.get("days", 0)),
                int(payload.get("sort_order", 99)),
                confirm_mode,
                int(window_min),
                deadline,
                float(ratio),
            ),
        )
        conn.commit()
        hid = cur.lastrowid
    except Exception as e:
        conn.close()
        raise HTTPException(409, "创建失败（habit_key 重复？）：%s" % e)
    conn.close()
    return {"code": 0, "data": {"id": hid, "habit_key": habit_key}}


@app.patch("/api/v1/habits/{habit_id}")
async def update_habit(habit_id: int, payload: dict):
    conn = db.get_conn()
    row = conn.execute("SELECT * FROM habits WHERE id=?", (habit_id,)).fetchone()
    if not row:
        conn.close()
        raise HTTPException(404, "习惯不存在")

    fields, values = [], []
    for k in ("name", "type"):
        if k in payload:
            fields.append(f"{k}=?")
            values.append(payload[k])
    if "enabled" in payload:
        fields.append("enabled=?")
        values.append(1 if payload["enabled"] else 0)
    if "days" in payload:
        fields.append("days=?")
        values.append(int(payload["days"]))
    if "sort_order" in payload:
        fields.append("sort_order=?")
        values.append(int(payload["sort_order"]))

    # 参数类字段合并进 config：
    # 既支持前端传 {"config": {...}}，也支持直接传扁平键 {"time": "22:00"}
    # lead_min = 提前量（分钟），也是 config 成员，改它和改时间走同一条路
    CONFIG_KEYS = ("time", "repeat", "interval", "start", "end", "sleep", "wake", "lead_min")
    patch_cfg = {}
    if "config" in payload:
        patch_cfg.update(payload["config"])
    patch_cfg.update({k: payload[k] for k in CONFIG_KEYS if k in payload})
    if patch_cfg:
        merged = dict(json.loads(row["config"] or "{}"))
        merged.update(patch_cfg)
        fields.append("config=?")
        values.append(json.dumps(merged, ensure_ascii=False))

    # 打卡规格字段（camelCase 与 snake_case 都收，前端传哪种都能生效）
    SPEC_KEYS = (
        ("confirm_mode", "confirm_mode"),
        ("confirmMode", "confirm_mode"),
        ("window_min", "window_min"),
        ("windowMin", "window_min"),
        ("deadline", "deadline"),
        ("min_ratio", "min_ratio"),
        ("minRatio", "min_ratio"),
    )
    for src, col in SPEC_KEYS:
        if src in payload:
            fields.append("%s=?" % col)
            values.append(payload[src])

    if not fields:
        conn.close()
        return {"code": 0}

    fields.append("updated_at=datetime('now','localtime')")
    values.append(habit_id)
    conn.execute(f"UPDATE habits SET {', '.join(fields)} WHERE id=?", values)
    conn.commit()
    conn.close()
    return {"code": 0}


@app.delete("/api/v1/habits/{habit_id}")
async def delete_habit(habit_id: int):
    """删除习惯定义，同时清掉它的打卡记录（否则重新添加同名习惯会继承旧天数）"""
    conn = db.get_conn()
    row = conn.execute("SELECT habit_key FROM habits WHERE id=?", (habit_id,)).fetchone()
    if row:
        conn.execute(
            "DELETE FROM habit_log WHERE user_id=? AND habit_key=?",
            (DEFAULT_USER, row["habit_key"]),
        )
    conn.execute("DELETE FROM habits WHERE id=?", (habit_id,))
    conn.commit()
    conn.close()
    return {"code": 0}


# ---------- 习惯打卡接口 ----------
#
# 【为什么天数不落库】
# 老实现是"播报一次 days+1"，那只证明包子提醒过你，证明不了你做到了 —— 假天数。
# 现在 habit_log 只记事实（pending / done / missed），天数由 habit_key 的日志
# 现算（habit_logic.streak_of），所以"播报"永远不可能加天数。
#
# 【为什么习惯的提醒去重也走 habit_log】
# remind_log 的唯一键是 (event_id, offset_sec, send_date)，而 SQLite 里 NULL 互不相等，
# 习惯的 event_id 是 NULL 就完全去不了重。所以习惯改用"pending 行本身"当幂等标记：
# INSERT OR IGNORE 插入 pending，插得进去 = 这条提醒还没播报过。

@app.get("/api/v1/habits/today")
@app.get("/api/v1/habits/cards")
async def habit_cards(date: Optional[str] = None):
    """今日（或指定日期）所有习惯的打卡面板：卡片 + 待确认清单

    /today 是给小程序读的语义名；/cards 是"补卡任意日期"时用的，
    两个路径等价，date 传过去哪天就返回哪天的卡片。
    """
    now = datetime.now()
    d = date or now.strftime("%Y-%m-%d")
    conn = db.get_conn()
    rows = conn.execute(
        "SELECT * FROM habits WHERE user_id=? ORDER BY sort_order, id", (DEFAULT_USER,)
    ).fetchall()
    out = []
    for r in rows:
        spec = habit_logic.spec_of_row(r)
        log = _recent_log(conn, spec["key"], now)
        out.append({
            "key": spec["key"],
            "id": spec["id"],
            "name": spec["name"],
            "type": spec["type"],
            "enabled": spec["enabled"],
            "card": habit_logic.build_card(spec, d, now, log),
        })
    conn.close()
    return {
        "code": 0,
        "data": {"date": d, "cards": out, "pending": habit_logic.pending_items(out)},
    }


@app.post("/api/v1/habits/{habit_key}/checkin")
async def habit_checkin(habit_key: str, payload: dict = None):
    """打卡。带 date 就是补卡/次日确认，slot 用于一天多次的习惯。

    返回重算后的 card，前端拿到就能直接刷新，不用再请求一次。
    """
    payload = payload or {}
    now = datetime.now()
    date = payload.get("date") or now.strftime("%Y-%m-%d")
    slot = payload.get("slot") or habit_logic.SLOT_MAIN
    source = payload.get("source") or "app"

    conn = db.get_conn()
    if not _habit_spec(conn, habit_key):
        conn.close()
        raise HTTPException(404, "习惯不存在：%s" % habit_key)
    conn.execute(
        "INSERT INTO habit_log(user_id,habit_key,log_date,slot,status,source,confirmed_at) "
        "VALUES(?,?,?,?,'done',?,datetime('now','localtime')) "
        "ON CONFLICT(user_id,habit_key,log_date,slot) DO UPDATE SET "
        "status='done', source=excluded.source, confirmed_at=datetime('now','localtime')",
        (DEFAULT_USER, habit_key, date, slot, source),
    )
    conn.commit()
    data = _card_payload(conn, habit_key, date)
    conn.close()
    return {"code": 0, "data": data}


@app.post("/api/v1/habits/{habit_key}/uncheck")
async def habit_uncheck(habit_key: str, payload: dict = None):
    """撤销打卡（把记录删掉，而不是留个 undone —— 否则"到底做没做"要查两种状态）"""
    payload = payload or {}
    now = datetime.now()
    date = payload.get("date") or now.strftime("%Y-%m-%d")
    slot = payload.get("slot") or habit_logic.SLOT_MAIN

    conn = db.get_conn()
    conn.execute(
        "DELETE FROM habit_log WHERE user_id=? AND habit_key=? AND log_date=? AND slot=?",
        (DEFAULT_USER, habit_key, date, slot),
    )
    conn.commit()
    data = _card_payload(conn, habit_key, date)
    conn.close()
    return {"code": 0, "data": data}


@app.get("/api/v1/habits/{habit_key}/log")
async def habit_log_range(
    habit_key: str,
    start: Optional[str] = None,
    end: Optional[str] = None,
    days: int = 30,
):
    """逐日记录（日历格子用）：每天是 met / missed / pending / future"""
    now = datetime.now()
    today = now.strftime("%Y-%m-%d")
    end_d = end or today
    days = max(1, min(int(days or 30), habit_logic.MAX_LOOKBACK))
    start_d = start or habit_logic.shift_date(end_d, -(days - 1))

    conn = db.get_conn()
    spec = _habit_spec(conn, habit_key)
    if not spec:
        conn.close()
        raise HTTPException(404, "习惯不存在：%s" % habit_key)
    log = _load_log(conn, habit_key, start_d, end_d)

    slots = habit_logic.slots_of(spec)
    out = []
    d = start_d
    for _ in range(habit_logic.MAX_LOOKBACK):
        if d > end_d:
            break
        done = sum(1 for s in slots if habit_logic.state_of(spec, d, s, now, log) == "done")
        out.append({
            "date": d,
            "status": habit_logic.day_status(spec, d, now, log),
            "done": done,
            "total": len(slots),
        })
        d = habit_logic.shift_date(d, 1)

    streak = habit_logic.streak_of(spec, now, _recent_log(conn, habit_key, now))
    conn.close()
    return {
        "code": 0,
        "data": {"habit_key": habit_key, "days": out, "streak": streak},
    }


@app.get("/api/v1/habits/{habit_key}/streak")
async def habit_streak(habit_key: str):
    """连续天数（由 habit_log 现算，不读 habits.days）

    - done 的一天且达标 = +1
    - pending 的一天（今天还没确认 / 窗口还开着）**不计数也不中断**  ← 铁律 3
    """
    now = datetime.now()
    today = now.strftime("%Y-%m-%d")
    conn = db.get_conn()
    spec = _habit_spec(conn, habit_key)
    if not spec:
        conn.close()
        raise HTTPException(404, "习惯不存在：%s" % habit_key)
    log = _recent_log(conn, habit_key, now)
    data = {
        "habit_key": habit_key,
        "streak": habit_logic.streak_of(spec, now, log),
        "todayStatus": habit_logic.day_status(spec, today, now, log),
        "total": sum(1 for k in log if log[k]["status"] == "done"),
    }
    conn.close()
    return {"code": 0, "data": data}


# ---------- 设备接口 ----------
@app.get("/api/v1/devices/{device_id}")
async def get_device(device_id: str):
    conn = db.get_conn()
    row = conn.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()
    conn.close()
    if not row:
        return {"code": 0, "data": {"device_id": device_id, "online": 0}}
    return {"code": 0, "data": dict(row)}


# ---------- WebSocket 通道（机器人连接，接收提醒下发） ----------
@app.websocket("/ws/device")
async def ws_device(ws: WebSocket, device_id: str = "baozi-001"):
    await ws.accept()
    await manager.connect(device_id, ws)
    try:
        while True:
            data = await ws.receive_json()
            if data.get("type") == "heartbeat":
                await ws.send_json({"type": "pong"})
    except WebSocketDisconnect:
        manager.disconnect(device_id)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
