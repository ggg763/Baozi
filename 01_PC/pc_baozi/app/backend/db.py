# -*- coding: utf-8 -*-
"""数据库初始化 + 结构升级（SQLite，标准库，无额外依赖）

【为什么 init_db 里要带迁移】
这个库已经在用了（有真实数据），不能删库重建。所以结构变更全部走
`_migrate()` 里的幂等升级：先 PRAGMA 看列在不在，缺什么补什么。

两处历史问题由迁移修掉：
  1. habits 表原本没有打卡规格字段（confirm_mode / window_min / deadline / min_ratio）
  2. habit_log 用 habit_type 关联，而习惯的稳定标识是 habit_key
     —— 后果是同类型的第二个习惯会串台（删掉"喝水"会连删"起身活动"的记录）
"""
import json
import os
import sqlite3

# ⚠️ 整合改动：组员原版是 DB_PATH = "baozi.db"（相对【当前工作目录】）。
#    整合进 pc_baozi 后，主程序从 pc_baozi/ 启动，相对路径会在 pc_baozi/ 下
#    新建一个空库 —— 表现是"手机和屏幕全空"，而 app/backend/baozi.db 里的
#    演示数据一动不动。所以改成基于本文件位置的绝对路径。
#    也可以用环境变量 BAOZI_DB 覆盖（换库/备份时不用改代码）。
DB_PATH = os.environ.get(
    "BAOZI_DB", os.path.join(os.path.dirname(os.path.abspath(__file__)), "baozi.db"))
DEFAULT_USER = "demo-user"

# 打卡记录的建表语句单独抽出来：迁移时要重建这张表，避免两处 DDL 不一致
# ⚠️ 索引必须和建表分开：老库的 habit_log 还没有 habit_key 列，
#    如果在 SCHEMA 里一起建索引，会撞上"no such column: habit_key"。
#    所以索引统一放在迁移之后执行（见 init_db）。
HABIT_LOG_TABLE_DDL = """
CREATE TABLE IF NOT EXISTS habit_log (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id      TEXT NOT NULL,
  habit_key    TEXT NOT NULL,
  log_date     TEXT NOT NULL,
  slot         TEXT NOT NULL DEFAULT 'main',
  status       TEXT NOT NULL DEFAULT 'done',
  source       TEXT NOT NULL DEFAULT 'app',
  note         TEXT,
  confirmed_at TEXT,
  created_at   TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  UNIQUE(user_id, habit_key, log_date, slot)
);
"""

HABIT_LOG_INDEX_DDL = """
CREATE INDEX IF NOT EXISTS idx_habit_log_key_date ON habit_log(user_id, habit_key, log_date);
"""

HABIT_LOG_DDL = HABIT_LOG_TABLE_DDL + HABIT_LOG_INDEX_DDL

SCHEMA = """
CREATE TABLE IF NOT EXISTS events (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id        TEXT NOT NULL,
  title          TEXT NOT NULL,
  event_type     TEXT NOT NULL DEFAULT 'custom',
  event_time     TEXT NOT NULL,
  remind_offsets TEXT NOT NULL DEFAULT '[600]',
  repeat_rule    TEXT NOT NULL DEFAULT 'none',
  remind_way     TEXT NOT NULL DEFAULT 'voice_emotion',
  status         TEXT NOT NULL DEFAULT 'pending',
  source         TEXT NOT NULL DEFAULT 'applet',
  created_at     TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  updated_at     TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);
CREATE INDEX IF NOT EXISTS idx_events_user_time ON events(user_id, event_time);

CREATE TABLE IF NOT EXISTS settings (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id    TEXT NOT NULL,
  item_key   TEXT NOT NULL,
  item_value TEXT NOT NULL,
  updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  UNIQUE(user_id, item_key)
);

-- 习惯定义表
--   type          sleep(作息) / interval(间隔型) / point(时间点型)
--   config        JSON，新增习惯类型无需改表结构
--   habit_key     该习惯的稳定标识，habit_log.habit_key 与它对应
--   confirm_mode  on_time   当场确认（窗口 = 提醒后 window_min 分钟）
--                 after_ward 事后确认（截止到次日 deadline，如睡觉）
--   min_ratio     多次型习惯的日达标线（当天完成率 >= 它 → 这一天算坚持住了）
--   days          ⚠️ 已废弃的展示缓存。天数现在由 habit_log 现算（见 main.list_habits），
--                 这个列仅为兼容老代码/老库保留，不再参与任何逻辑
CREATE TABLE IF NOT EXISTS habits (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id      TEXT NOT NULL,
  habit_key    TEXT NOT NULL,
  name         TEXT NOT NULL,
  type         TEXT NOT NULL DEFAULT 'point',
  config       TEXT NOT NULL DEFAULT '{}',
  confirm_mode TEXT NOT NULL DEFAULT 'on_time',
  window_min   INTEGER NOT NULL DEFAULT 30,
  deadline     TEXT NOT NULL DEFAULT '12:00',
  min_ratio    REAL NOT NULL DEFAULT 0.6,
  enabled      INTEGER NOT NULL DEFAULT 1,
  days         INTEGER NOT NULL DEFAULT 0,
  sort_order   INTEGER NOT NULL DEFAULT 0,
  created_at   TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  updated_at   TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  UNIQUE(user_id, habit_key)
);

CREATE TABLE IF NOT EXISTS remind_log (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id   INTEGER,
  habit_type TEXT,
  offset_sec INTEGER,
  send_date  TEXT NOT NULL,
  sent_at    TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  UNIQUE(event_id, offset_sec, send_date)
);

CREATE TABLE IF NOT EXISTS devices (
  device_id  TEXT PRIMARY KEY,
  user_id    TEXT,
  online     INTEGER DEFAULT 0,
  last_seen  TEXT,
  fw_version TEXT
);
""" + HABIT_LOG_TABLE_DDL

# 首次建库时的默认习惯（与小程序原本的假数据保持一致，避免界面"变空"）
# 注意：不再给 days 赋值 —— 天数由 habit_log 现算
DEFAULT_HABITS = [
    {
        "habit_key": "sleep",
        "name": "作息",
        "type": "sleep",
        "config": {"sleep": "23:30", "wake": "07:00", "lead_min": 30},
        "confirm_mode": "after_ward",
        "window_min": 30,
        "deadline": "12:00",
        "min_ratio": 1.0,
    },
    {
        "habit_key": "water",
        "name": "喝水",
        "type": "interval",
        "config": {"interval": 60, "start": "09:00", "end": "21:00", "lead_min": 0},
        "confirm_mode": "on_time",
        "window_min": 30,
        "deadline": "12:00",
        "min_ratio": 0.6,
    },
    {
        "habit_key": "exercise",
        "name": "锻炼",
        "type": "point",
        "config": {"time": "19:00", "repeat": "每天", "lead_min": 15},
        "confirm_mode": "after_ward",
        "window_min": 30,
        "deadline": "12:00",
        "min_ratio": 1.0,
    },
]


def get_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def _table_columns(conn, table):
    return [r["name"] for r in conn.execute("PRAGMA table_info(%s)" % table).fetchall()]


def _seed_habits(conn):
    """习惯表为空时播种默认习惯（幂等：只在完全没有数据时执行）"""
    row = conn.execute(
        "SELECT COUNT(*) AS c FROM habits WHERE user_id=?", (DEFAULT_USER,)
    ).fetchone()
    if row["c"] > 0:
        return
    for i, h in enumerate(DEFAULT_HABITS):
        conn.execute(
            "INSERT INTO habits(user_id,habit_key,name,type,config,confirm_mode,"
            "window_min,deadline,min_ratio,enabled,sort_order) VALUES(?,?,?,?,?,?,?,?,?,1,?)",
            (
                DEFAULT_USER,
                h["habit_key"],
                h["name"],
                h["type"],
                json.dumps(h["config"], ensure_ascii=False),
                h["confirm_mode"],
                h["window_min"],
                h["deadline"],
                h["min_ratio"],
                i,
            ),
        )
    print("[db] 已播种 %d 条默认习惯" % len(DEFAULT_HABITS))


def _migrate(conn):
    """把老库升级到当前结构（幂等，可反复执行）"""
    # ---- habits：补打卡规格字段 ----
    hcols = _table_columns(conn, "habits")
    if hcols and "confirm_mode" not in hcols:
        conn.execute("ALTER TABLE habits ADD COLUMN confirm_mode TEXT NOT NULL DEFAULT 'on_time'")
        conn.execute("ALTER TABLE habits ADD COLUMN window_min INTEGER NOT NULL DEFAULT 30")
        conn.execute("ALTER TABLE habits ADD COLUMN deadline TEXT NOT NULL DEFAULT '12:00'")
        conn.execute("ALTER TABLE habits ADD COLUMN min_ratio REAL NOT NULL DEFAULT 0.6")
        # 老数据补默认规格：作息/锻炼 = 事后确认；这两类一天一次，达标线就是 100%
        conn.execute("UPDATE habits SET confirm_mode='after_ward' WHERE type IN ('sleep','point')")
        conn.execute("UPDATE habits SET min_ratio=1 WHERE type IN ('sleep','point')")
        print("[db] habits 已升级：+confirm_mode/window_min/deadline/min_ratio")

    # ---- habit_log：habit_type -> habit_key，并补 slot / source / confirmed_at ----
    lcols = _table_columns(conn, "habit_log")
    if lcols and "habit_key" not in lcols:
        src_col = "habit_type" if "habit_type" in lcols else "habit_key"
        note_expr = "note" if "note" in lcols else "NULL"
        conn.execute("ALTER TABLE habit_log RENAME TO habit_log_legacy")
        conn.executescript(HABIT_LOG_DDL)
        conn.execute(
            "INSERT INTO habit_log(user_id,habit_key,log_date,slot,status,source,note,"
            "confirmed_at,created_at) "
            "SELECT user_id, %s, log_date, 'main', status, 'device', %s, "
            "CASE WHEN status='done' THEN created_at ELSE NULL END, created_at "
            "FROM habit_log_legacy" % (src_col, note_expr)
        )
        conn.execute("DROP TABLE habit_log_legacy")
        print("[db] habit_log 已升级：habit_type -> habit_key，+slot/source/confirmed_at")

    # ---- remind_log：历史遗留的 habit_type 列留着不用了，习惯的提醒去重改走
    #      habit_log 的 pending 行（见 scheduler._mark_pending），不再依赖这张表 ----


def init_db():
    conn = get_conn()
    conn.executescript(SCHEMA)
    _migrate(conn)                       # 先补列/换表，索引才有得建
    conn.executescript(HABIT_LOG_INDEX_DDL)
    _seed_habits(conn)
    conn.commit()
    conn.close()
    print("[db] 数据库初始化完成：", DB_PATH)


if __name__ == "__main__":
    init_db()
