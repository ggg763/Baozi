"""报时 / 报日期：把真实时间交给模型，别让它自己编。

为什么需要单独一个模块：
    系统提示词里已经注入了「今天的日期」「现在的时间」两行，但 7B 模型
    **经常会忽略状态行** —— 问"几点了"它照样回一句"我猜大概下午吧"。
    日历/时钟是确定性事实，跟算式一样，应该由代码算好再塞给它（参考
    calculator.py 的做法），而不是指望模型从提示词里"注意到"。

    另外"还有多久到六点"这类**需要算**的问题，模型根本算不对：
    它不知道现在是几点，只能胡编一个剩余分钟数。

只做三件事：
    1. 现在几点（含上午/下午等时段）；
    2. 今天几号、星期几；
    3. 距离某个整点还有多久（"还有多久到六点""离下班还有多久"）。
"""

from __future__ import annotations

import datetime
import re

_WEEKDAY_CN = "一二三四五六日"

# 触发：问时刻
_TIME_HINTS = ("几点", "什么时间", "现在时间", "时刻", "上午还是下午",
               "早上还是晚上", "几点钟", "现在多久", "现在多会")

# 触发：问日期
_DATE_HINTS = ("几号", "几月几号", "什么日期", "今天是什么日子", "星期几",
               "周几", "礼拜几", "今天是哪天", "什么节日")

# 触发：算还有多久到某个点。
# 目标点既接受阿拉伯数字（"还有多久到 18 点"）也接受中文数字（"到六点"）——
# 实测口语里中文数字才是主流，只写 \d{1,2} 会让"还有多久到六点"完全不触发。
_COUNTDOWN_RE = re.compile(
    r"(?:还有|还剩|距离|离|距)\s*(?:多久|多长时间|多少时间|多少分钟|几分钟)?\s*"
    r"(?:到|至)?\s*(\d{1,2}|[一二三四五六七八九十]{1,3})\s*[点時时:]")

_CN_NUM = {"零": 0, "一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5,
           "六": 6, "七": 7, "八": 8, "九": 9, "十": 10, "十一": 11, "十二": 12}


def _span_cn(h: int) -> str:
    """小时 -> 中文时段。直接给模型，省得它把 14 点换算成"上午"。"""
    if h < 5:
        return "凌晨"
    if h < 9:
        return "早上"
    if h < 12:
        return "上午"
    if h < 13:
        return "中午"
    if h < 18:
        return "下午"
    if h < 19:
        return "傍晚"
    return "晚上"


def now_str() -> str:
    """'14:24（下午）'"""
    n = datetime.datetime.now()
    return f"{n.hour:02d}:{n.minute:02d}（{_span_cn(n.hour)}）"


def date_str() -> str:
    """'2026年10月31日 星期六'"""
    n = datetime.datetime.now()
    return f"{n.year}年{n.month}月{n.day}日 星期{_WEEKDAY_CN[n.weekday()]}"


def looks_like_time(text: str) -> bool:
    """这句话是不是在问时间/日期？"""
    if not text:
        return False
    return (any(h in text for h in _TIME_HINTS)
            or any(h in text for h in _DATE_HINTS)
            or _COUNTDOWN_RE.search(text) is not None)


def _countdown(text: str) -> str:
    """'还有多久到六点' -> '距离 18:00 还有 3 小时 36 分钟'。

    只认整点（"六点""18点"），不认"六点半"——半小时的用户少，
    正则一放宽就会把"三点开会"这类陈述也当成倒计时。
    """
    m = _COUNTDOWN_RE.search(text)
    if not m:
        return ""
    tok = m.group(1)
    if tok.isdigit():
        target = int(tok)
    else:
        # 中文数字：'六' -> 6，'十一' -> 11。表里有 '十一' '十二'，
        # 所以从长到短试，别把 '十二' 拆成 '十' + '二'
        target = _CN_NUM.get(tok)
        if target is None:
            return ""
    if not 0 <= target <= 24:
        return ""
    if target < 8:            # "还有多久到3点" 大概率指下午 3 点
        target += 12
    target %= 24

    now = datetime.datetime.now()
    delta = (target - now.hour) * 60 - now.minute
    if delta <= 0:            # 今天的这个点已经过了，算到明天
        delta += 24 * 60
    h, mi = divmod(delta, 60)
    if h <= 0:
        return f"距离 {target}:00 还有 {mi} 分钟"
    return f"距离 {target}:00 还有 {h} 小时 {mi} 分钟"


def time_hint(text: str) -> str:
    """给模型的"时间上下文"；不问时间时返回 ""。

    例：
        问"几点了"    -> "现在是 14:24（下午）"
        问"今天几号"  -> "今天是 2026年10月31日 星期六"
        问"还有多久到六点" -> "现在是 14:24（下午），距离 18:00 还有 3 小时 36 分钟"
    """
    if not looks_like_time(text):
        return ""

    parts = []
    if any(h in text for h in _TIME_HINTS):
        parts.append(f"现在是 {now_str()}")
    if any(h in text for h in _DATE_HINTS):
        parts.append(f"今天是 {date_str()}")
    cd = _countdown(text)
    if cd:
        # 倒计时必须知道"现在几点"才能理解，一并给上
        if not parts:
            parts.append(f"现在是 {now_str()}")
        parts.append(cd)
    return "；".join(parts)
