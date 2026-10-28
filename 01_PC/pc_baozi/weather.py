"""简单的联网检索：目前只做「天气查询」。

为什么需要：
    包子（本地 qwen2.5:7b）**没有联网能力**，问它"武汉天气怎么样"只会编。
    桌面机器人报天气是很常见的诉求，所以在 PC 端用代码去查真实数据，
    再把结果作为**上下文**注入给模型，让它用自己的语气转述。

数据源：wttr.in（免费、无需注册、无需 API key）
    https://wttr.in/Wuhan?format=j1     返回 JSON（含当前天气 + 3 天预报）
    中文城市名直接可用（URL 编码后），例如 武汉 -> %E6%AD%A6%E6%B1%89

设计要点：
    · **只在问到天气时才联网**，平时零开销；
    · 超时给得短（默认 6s），失败就返回 ""，**绝不让它拖垮对话**；
    · 不引入新依赖 —— 用标准库 urllib（项目已有 requests，但 urllib 更省事）；
    · 城里名从中文话里粗提，提不到就用配置里的默认城市。
"""

from __future__ import annotations

import json
import re
import urllib.parse
import urllib.request
from typing import Optional

# 触发天气查询的关键词
_WEATHER_HINTS = ("天气", "气温", "下雨", "会不会冷", "冷不冷", "热不热",
                  "温度", "穿什么", "紫外线", "风大", "预报")

# "武汉天气怎么样" -> 武汉；"北京今天天气" -> 北京
# 城市名一般紧挨在"天气"前面，长度 2~8 个汉字
_CITY_RE = re.compile(
    r"([\u4e00-\u9fff]{2,8}?)(?:今天|明天|后天|的)*"
    r"(?:天气|气温|温度|预报|冷不冷|热不热|会不会冷|会不会下雨|下雨|要带伞)")
# 「北京冷不冷」这类省略句：城市名后面直接跟问法，中间没有"天气"二字
_CITY_ASK_RE = re.compile(
    r"([\u4e00-\u9fff]{2,8}?)(?:今天|明天|后天|那边)?\s*"
    r"(?:冷不冷|热不热|多少度|几度|会下雨吗|下雨吗|天气好吗)")

# "冷不冷""穿什么好"这类省略句：没有城市名但确实是在问天气
_QUESTION_HINTS = re.compile(r"[?？]|怎么样|如何|多少|几度|冷不冷|热不热|会不会|要穿|穿什么|带伞|下雨")

# 常见的非城市前缀词，避免把"今天天气"里的"今天"当成城市
_STOP_PREFIX = ("今天", "明天", "后天", "现在", "请问", "帮我", "查一下", "看看",
                "我们", "那边", "这边", "什么", "怎么样", "如何")

# wttr.in 的中文天气描述 -> 简短中文
_WEATHER_DESC = {
    "Sunny": "晴", "Clear": "晴", "Partly cloudy": "多云", "Cloudy": "多云",
    "Overcast": "阴", "Mist": "薄雾", "Fog": "雾", "Freezing fog": "冻雾",
    "Patchy rain possible": "可能有零星小雨", "Patchy rain nearby": "附近有零星小雨",
    "Light rain": "小雨", "Moderate rain": "中雨", "Heavy rain": "大雨",
    "Light drizzle": "毛毛雨", "Patchy light rain": "零星小雨",
    "Thundery outbreaks possible": "可能雷阵雨", "Moderate or heavy rain shower": "中到大雨",
    "Light rain shower": "小阵雨", "Torrential rain shower": "暴雨",
    "Light snow": "小雪", "Moderate snow": "中雪", "Heavy snow": "大雪",
    "Blizzard": "暴雪", "Sleet": "雨夹雪", "Patchy light snow": "零星小雪",
}


def looks_like_weather(text: str) -> bool:
    """这句话是不是在问天气？"""
    if not text:
        return False
    return any(h in text for h in _WEATHER_HINTS)


def extract_city(text: str) -> str:
    """从中文问句里粗提城市名；提不到返回 ""。

    >>> extract_city("武汉天气怎么样")
    '武汉'
    >>> extract_city("明天北京天气如何")
    '北京'
    >>> extract_city("今天天气不错")
    ''
    """
    if not text:
        return ""
    m = _CITY_RE.search(text) or _CITY_ASK_RE.search(text)
    if not m:
        return ""
    city = m.group(1)
    # 反复削掉"今天/请问/武汉"这类前缀（可能连着好几个，如"请问武汉"）。
    # 注意：削完**可以是空**——"今天天气不错"里的"今天"纯粹是时间词不是城市，
    # 必须削成空串（否则会拿"今天"去查天气，wttr.in 会猜出个莫名其妙的地点）。
    changed = True
    while changed:
        changed = False
        for p in _STOP_PREFIX:
            if city.startswith(p):
                city = city[len(p):]
                changed = True
    city = city.strip("的地了和与")
    return city if 2 <= len(city) <= 8 else ""


def _translate(desc: str) -> str:
    return _WEATHER_DESC.get(desc, desc)


def fetch_weather(city: str, timeout_s: float = 6.0) -> Optional[dict]:
    """查 wttr.in，返回整理好的天气字典；失败返回 None。"""
    if not city:
        return None
    url = f"https://wttr.in/{urllib.parse.quote(city)}?format=j1"
    try:
        req = urllib.request.Request(
            url, headers={"User-Agent": "curl/8.0",   # wttr.in 对 UA 有要求
                          "Accept-Language": "zh"})
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            data = json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001 —— 联网失败一律当"查不到"
        return None

    try:
        cur = data["current_condition"][0]
        area = None
        try:
            area = data["nearest_area"][0]["areaName"][0]["value"]
        except Exception:  # noqa: BLE001
            area = city

        desc = _translate(cur["weatherDesc"][0]["value"])
        out = {
            "城市": city,
            "地点": area or city,
            "天气": desc,
            "气温": f"{cur['temp_C']}°C",
            "体感": f"{cur['FeelsLikeC']}°C",
            "湿度": f"{cur['humidity']}%",
            "风": f"{cur.get('winddir16Point', '')}{cur.get('windspeedKmph', '')}km/h",
        }

        # 明天的最高/最低温（wttr.in 的 weather[1] 是明天）
        try:
            tomorrow = data["weather"][1]
            out["明天"] = (f"{_translate(tomorrow['hourly'][4]['weatherDesc'][0]['value'])}，"
                           f"{tomorrow['mintempC']}~{tomorrow['maxtempC']}°C")
        except Exception:  # noqa: BLE001
            pass
        return out
    except Exception:  # noqa: BLE001
        return None


def weather_hint(text: str, default_city: str = "", timeout_s: float = 6.0) -> str:
    """给模型的"天气上下文"；不需要或查不到返回 ""。

    返回形如：
        城市：武汉（实测于 wttr.in）
        现在：小雨，气温 18°C（体感 17°C），湿度 82%，风 东风8km/h
        明天：多云，15~24°C
    """
    if not looks_like_weather(text):
        return ""

    city = extract_city(text)
    if not city:
        # 提不到城市：可能是"冷不冷""穿什么好"这类省略句（用默认城市），
        # 也可能是"今天天气不错"这类纯陈述 —— 后者不该去联网，交给模型闲聊。
        if not _QUESTION_HINTS.search(text):
            return ""
        city = default_city
    if not city:
        return ""

    w = fetch_weather(city, timeout_s)
    if not w:
        return f"城市：{city}（联网查询失败，天气数据暂时拿不到）"

    lines = [f"城市：{w['城市']}（数据来自 wttr.in 实时查询）"]
    lines.append(f"现在：{w['天气']}，气温 {w['气温']}（体感 {w['体感']}），"
                 f"湿度 {w['湿度']}，风 {w['风']}")
    if "明天" in w:
        lines.append(f"明天：{w['明天']}")
    return "\n".join(lines)
