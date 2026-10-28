# -*- coding: utf-8 -*-
"""提醒话术池 —— 让包子说话不只一句

【为什么要有话术池】
同一个习惯一天要提醒很多次（喝水 09:00~21:00 每小时一次 = 12 次），
如果每次都念"该喝水了，做完记得告诉我"，用户第三天就自动忽略了。
所以提醒的"话"必须换着说。

做法分两层：
  1. 内置兜底池（本文件 BUILTIN）：手写、离线可用、零延迟 —— 保证**永远有话说**
  2. Qwen 预生成池（remind_pool.json）：用本地千问批量生成，闲时跑一次落盘
     —— 只是"加料"，文件删了也不影响运行（自动退回内置池）

【为什么不实时调大模型】
qwen2.5:7b 在本机生成一句要 1~2 秒，而提醒是"到点就得说"。
所以生成放在闲时（手动/定时跑 gen_remind_phrases.py），播报时只做抽取。

【怎么保证不重复】
用"洗牌袋"：把整池洗一把，一个个往外抽，抽完才重新洗牌。
所以在抽完一整轮之前，绝不会重复同一句 —— 比"随机抽"更不容易撞句。
袋子状态落盘（同目录 remind_pool_state.json），桥接重启也不会从头再来。

【模板占位符】
  {name}  习惯名（喝水 / 锻炼 …）
  {title} 日程名
  {mins}  还有几分钟
只有"占位数据齐全"的模板才会被抽中 —— 所以不会出现把 {title} 念出来的尴尬。
"""
import json
import os
import random
import re

HERE = os.path.dirname(os.path.abspath(__file__))
POOL_PATH = os.path.join(HERE, "remind_pool.json")            # Qwen 生成池（可选）
STATE_PATH = os.path.join(HERE, "remind_pool_state.json")     # 洗牌袋状态

# 类别 -> 内置兜底话术。语气参照用户给的样例：俏皮、可爱、像撒娇的小助手。
#
# ⚠️⚠️ 通用池的硬规矩（踩过坑，别改回去）：
#   通用池要能套在**任意**习惯名上，而中文的名字（喝水/护眼/背单词/锻炼）是**动宾短语**，
#   它只能待在"能接动词短语"的位置。所以 {name} 后面只允许接这几类万能后缀：
#       时间到 / 啦 / 咯 / 了 / 哦 / 吧 / 的时候 / 的时间到了 / 后面直接跟标点
#   绝对禁止（都是真机上念出来过的病句）：
#       {name}一下      → "喝水一下"、"背单词一下"      ✗
#       {name}起来      → "喝水起来吧"                   ✗
#       别{name}忘了    → "别喝水忘了"（应为"别忘了喝水"） ✗
#       {name}…说再见   → "喝水时间到，跟包子说再见"      ✗
#   另外通用句里**不许出现只属于某一个习惯的名词**（水/杯子/眼睛/颈椎/屏幕/单词…），
#   否则"记得喝水"会被套到"护眼"上。这类专属句一律放进 habit:<key> 专属池。
#   —— 这两条由 _check_remind_phrases.py 自动检查，改坏了测试会红。
BUILTIN = {
    # 睡觉类：一天只提醒一次，不必带习惯名
    "sleep": [
        "夜深啦，该睡觉觉了，明天还要元气满满哦！",
        "你昨天晚上按时睡觉了吗？今晚也要乖乖早睡哦！",
        "晚安时间到！快去睡觉，梦里给你准备了好多小星星。",
        "再刷手机就要变成小熊猫啦，快闭上眼睛睡觉吧！",
        "被子已经帮你焐热啦，就差一个你，快去睡吧！",
        "早睡的人最好看啦，现在去睡觉，好不好？",
        "眼睛也该休息啦，我们明天早上见，晚安哦！",
        "一天辛苦啦，闭上眼睛，我陪着你，晚安。",
        "月亮都出来遛弯了，你也该钻进被窝咯～",
    ],
    # 间隔型（喝水 / 起身活动 …）：一天多次，最需要换着说
    "interval": [
        "{name}时间到啦，做完记得点一下哦！",
        "该{name}啦，别忘了顺手点个打卡！",
        "{name}的提醒又来啦，这次可不能再忘咯。",
        "叮咚，{name}时间到，我等你的打卡呢！",
        "来嘛来嘛，就一小会儿，把{name}做了吧！",
        "小主人，{name}啦，做完告诉我一声哦。",
        "嘿，{name}咯，快去快去！",
        "{name}时间到，包子在这儿等你打卡～",
        "别忘了{name}哦，我在这里给你加油！",
        "该{name}啦，做完记得告诉我哦！",
    ],
    # 时间点型（锻炼 / 吃药 …）：一天一次
    "point": [
        "该{name}啦，做完记得告诉我哦！",
        "到{name}的时间啦，出发出发！",
        "小主人，{name}时间到，就等你啦。",
        "{name}时间到啦，做完点一下打卡，好不好？",
        "嘿，{name}咯，快去快去！",
        "别忘了{name}哦，我在这里给你加油！",
        "{name}的时间到了，包子在等你哦～",
        "该{name}啦，别拖拉哦！",
    ],
    # 日程：提前 N 分钟播报（{title} 一律加「」括起来，免得和前后文黏成病句）
    "event": [
        "还有{mins}分钟就「{title}」啦，准备一下哦！",
        "提醒你一下，再过{mins}分钟就要「{title}」了！",
        "小主人，「{title}」马上开始啦，别迟到哦！",
        "还有{mins}分钟就「{title}」了，我在这里给你加油！",
        "「{title}」就要到啦，还有{mins}分钟，快准备吧！",
        "「{title}」还有{mins}分钟就开始咯，快准备一下～",
        "{mins}分钟后就是「{title}」啦，别忘了哦！",
    ],
}

# 通用池里禁止出现的"专属名词"：它们只属于某一个习惯，
# 一旦出现在通用句里，就会被错误地套到别的习惯上（"记得喝水"→"护眼"）。
GENERIC_FORBIDDEN_NOUNS = [
    "喝水", "水杯", "杯子", "解渴", "护眼", "眼睛", "屏幕", "颈椎", "肩颈",
    "单词", "锻炼", "跑步", "运动", "吃药",
]

# 通用池里 {name} 后面禁止紧跟这些成分（需要特定动词搭配，不是万能后缀）
GENERIC_FORBIDDEN_SLOT = [
    r"\{name\}一下",
    r"\{name\}起来",
    r"别\{name\}忘了",
    r"\{name\}第一件事",
    # 日程两条：{title} 必须被「」括起来（否则"准备一下开会要开始了"这种黏连句），
    # {mins} 后面必须跟"分钟"（否则念成"还有一个30就要开会了"）。
    r"(?<!「)\{title\}(?!」)",
    r"\{mins\}(?!分钟)",
]

# 上面两条禁令只对**多习惯共用**的池子生效。
# sleep 天生单用途（"该睡了"跟习惯名无关，池子里提"被窝"完全正确），
# event 只套日程标题；这两类不做名词检查。
GENERIC_CHECKED_CATS = ("interval", "point")


# 习惯 type -> 话术类别
HABIT_CATEGORY = {
    "sleep": "sleep",
    "interval": "interval",
    "point": "point",
}

_PH = re.compile(r"\{(\w+)\}")


def placeholders(tpl):
    """模板里用到的占位符集合，例如 '{name}' -> {'name'}"""
    return set(_PH.findall(tpl))


def category_of_habit(htype):
    """习惯类型 -> 话术类别；未知类型统一按时间点型处理"""
    return HABIT_CATEGORY.get(htype or "", "point")


def habit_pool_key(habit_key):
    """某个具体习惯的专属池子键，如 water -> 'habit:water'

    为什么要有专属池：泛化的"按类型"话术必须用 {name} 占位，
    而 7B 模型写不出这种泛指句（总会把"喝水""眼睛"写死）。
    所以更靠谱的做法是**挨个习惯生成**，句子里直接写"喝水"，
    存进 habit:<key>；抽话时优先用它，没有再退回按类型的通用池。
    """
    return "habit:%s" % (habit_key or "")


def _safe_format(tpl, ctx):
    """格式化模板；万一有占位符没给值，原样保留（而不是抛 KeyError 把提醒搞挂）"""
    class _D(dict):
        def __missing__(self, k):
            return "{%s}" % k
    try:
        return tpl.format_map(_D(ctx or {}))
    except Exception:
        return tpl


def _eligible(pool, ctx):
    """只挑"占位数据齐全"的模板；一条都不满足时退回整池（宁可念得不具体，也别没话说）"""
    keys = set((ctx or {}).keys())
    ok = [t for t in pool if placeholders(t) <= keys]
    return ok or list(pool)


class PhraseBook:
    """话术池。用 pick() 取一句；内部维护每类的洗牌袋，落盘防重启重置。"""

    def __init__(self, pool_path=POOL_PATH, state_path=STATE_PATH, seed=None):
        self.rng = random.Random(seed)
        self.state_path = state_path
        self.pools = self._merge(pool_path)
        self.state = self._load_state()

    # ---------- 载入 ----------
    @staticmethod
    def _load_json(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return None

    def _merge(self, pool_path):
        """内置池 + 生成池（去重、保序）"""
        pools = {k: list(v) for k, v in BUILTIN.items()}
        gen = self._load_json(pool_path) or {}
        generated = gen.get("pools") if isinstance(gen, dict) else None
        for cat, arr in (generated or {}).items():
            if not isinstance(arr, list):
                continue
            pools.setdefault(cat, [])
            for t in arr:
                if isinstance(t, str) and t.strip() and t.strip() not in pools[cat]:
                    pools[cat].append(t.strip())
        return pools

    def _load_state(self):
        st = self._load_json(self.state_path)
        return st if isinstance(st, dict) else {}

    def _save_state(self):
        try:
            with open(self.state_path, "w", encoding="utf-8") as f:
                json.dump(self.state, f, ensure_ascii=False, indent=0)
        except Exception:
            pass                       # 状态存不上不影响本次播报，下次从头洗牌而已

    # ---------- 查询 ----------
    def categories(self):
        return sorted(self.pools)

    def count(self, category):
        return len(self.pools.get(category) or [])

    def describe(self):
        return "、".join("%s %d 句" % (c, self.count(c)) for c in self.categories())

    # ---------- 抽取 ----------
    # 「最近说过」的窗口大小：光靠洗牌袋只能保证"一池之内不重复"，
    # 挡不住"袋子抽空了重洗、新袋子立刻又抽到刚说过的那句"——
    # 真机上就表现为"隔三四条又听到同一句"。所以额外记一个最近窗口，抽中就换一句。
    # 取池子的 1/3（上限 8）：既够挡住近处重复，又保证袋子里永远有 2/3 的候选可选，
    # 不会出现"剩下的全在窗口里、被迫重复"的死角。
    RECENT_CAP = 8

    def recent_window(self, pool_size):
        return max(1, min(max(pool_size, 1) // 3, self.RECENT_CAP))

    def _draw(self, bag_key, pool, ctx):
        """洗牌袋的核心：从 pool 里抽一句。bag_key 决定用哪个袋子。

        袋子的构造里**直接剔掉"最近 k 句"**，而不是"抽到了再想办法换一句"。
        区别很关键：后者在袋子快见底时（剩下的恰好都在最近窗口里）会陷入"换无可换"，
        只能被迫重复，保证就漏了；前者从构造上就让最近 k 句根本不在候选里。

        这样得到的是一条**能证明的**不变式：
            同一句话两次之间至少隔 k+1 条（池子大于 k 时）。
        因为抽中 X 的那一刻 X 不在最近 k 句里 ⇒ 上一次 X 至少在 k+1 次之前。
        注意袋子的一轮长度因此变成 pool-k（少了被排除的那几句），这是有意的代价。
        """
        k = self.recent_window(len(pool))
        recent = [t for t in (self.state.get(bag_key + "|recent") or []) if t in pool]
        bag = [t for t in (self.state.get(bag_key) or []) if t in pool]

        if not bag:
            blocked = set(recent[-k:])
            bag = [t for t in pool if t not in blocked]
            if not bag:                 # 池子比窗口还小：退化，只能整池上
                bag = list(pool)
            self.rng.shuffle(bag)

        tpl = bag.pop(0)
        self.state[bag_key] = bag
        self.state[bag_key + "|last"] = tpl
        self.state[bag_key + "|recent"] = (recent + [tpl])[-k:]
        self._save_state()
        return _safe_format(tpl, ctx)

    def pick(self, category, ctx=None):
        """从某类别抽一句（洗牌袋：整池抽完才重洗，保证短时间不重复）。

        返回已填好占位符的字符串；类别不存在时返回空串，调用方自行兜底。
        """
        pool = _eligible(self.pools.get(category) or [], ctx)
        if not pool:
            return ""
        return self._draw(category, pool, ctx)

    def pick_any(self, cats, ctx=None):
        """从多个类别的**并集**里抽，共用一个洗牌袋。

        典型用法（习惯提醒）：["habit:water", "interval"] ——
        专属池（句子直接点了"喝水"，最自然）和通用池（手写话术）都算候选，
        一句话要抽完整个并集才会重洗，所以轮换面更大、更不容易腻。
        某个类别没有（还没生成过 / 已停用）时，它就自然只剩另一个在供话。
        """
        seen, pool = set(), []
        for c in cats:
            for t in (self.pools.get(c) or []):
                if t not in seen:
                    seen.add(t)
                    pool.append(t)
        pool = _eligible(pool, ctx)
        if not pool:
            return ""
        return self._draw("union:" + "|".join(cats), pool, ctx)


if __name__ == "__main__":
    book = PhraseBook()
    print("话术池：%s" % book.describe())
    for cat in book.categories():
        ctx = {"name": "喝水", "title": "开组会", "mins": 10}
        print("\n[%s] 抽 6 句看看不重复：" % cat)
        for _ in range(6):
            print("   ", book.pick(cat, ctx))
