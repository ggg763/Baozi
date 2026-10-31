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
    · 超时给得短（默认 6s），失败就放弃，**绝不让它拖垮对话**；
    · 不引入新依赖 —— 用标准库 urllib；
    · 城市名从中文话里粗提，提不到就用配置里的默认城市。

2026-10-31 实测修的四处（原来问天气经常答非所问）：
    1. **描述没翻译**：wttr.in 只返回英文（"Smoky haze"），字典漏词 + 原值带
       尾随空格（"Overcast "），于是包子会念出英文。→ 补全字典 + 归一化匹配。
    2. **风向是英文**："SE11km/h"。→ 十六方位转中文，风速换算成蒲福风级。
    3. **把非城市词当城市**："外面冷不冷" 会拿"外面"去查，wttr.in 猜出一个
       毫不相关的地点，输出却写"城市：外面"—— 包子就会照着念一个假地名。
       → 扩充停用词；另外输出用 wttr.in 回传的实际地点名做交叉校验。
    4. **问明天却报现在**：问"明天会下雨吗"给的是当前实况 + 明天一行，
       50 字上限下模型只能挑一半说。→ 按问法给对应的那一天，并补降水概率。
    另外加了 10 分钟缓存：连着问两次不用再等 3 秒。
"""

from __future__ import annotations

import json
import re
import threading
import time
import urllib.parse
import urllib.request
from typing import Optional

# 触发天气查询的关键词
_WEATHER_HINTS = ("天气", "气温", "下雨", "会不会冷", "冷不冷", "热不热",
                  "温度", "穿什么", "紫外线", "风大", "预报", "带伞",
                  "几度", "多少度", "刮风", "闷热", "凉快", "下雨吗")

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

# 常见的非城市前缀词，避免把"今天天气"里的"今天"当成城市。
# ⚠️ 漏一个就会拿这个词去 wttr.in 查，它会"贴心"地猜一个地点回来 ——
#    于是包子一本正经地报一个完全不相干的城市，比答不上来更糟。
#    （实测踩到的就是"外面冷不冷"）
_STOP_PREFIX = ("今天", "明天", "后天", "现在", "请问", "帮我", "查一下", "看看",
                "我们", "那边", "这边", "什么", "怎么样", "如何", "外面", "这里",
                "那里", "家里", "室内", "室外", "待会", "一会儿", "出去", "出门",
                "我想", "知道", "一下")

# wttr.in 的英文天气描述 -> 简短中文。
# 匹配前会 strip + 转小写，所以键统一写小写、不带尾随空格。
_WEATHER_DESC = {
    # 晴 / 云
    "sunny": "晴", "clear": "晴", "partly cloudy": "多云", "cloudy": "多云",
    "overcast": "阴", "partly clear": "少云",
    # 雾 / 霾 / 尘
    "mist": "薄雾", "fog": "雾", "freezing fog": "冻雾", "haze": "霾",
    "smoke": "烟霾", "smoky haze": "烟霾", "sand": "扬沙", "dust": "浮尘",
    "widespread dust": "大范围浮尘",
    # 毛毛雨 / 雨
    "patchy light drizzle": "零星毛毛雨", "light drizzle": "毛毛雨",
    "frozen drizzle": "冻毛毛雨", "drizzle": "毛毛雨",
    "patchy rain possible": "可能有零星小雨", "patchy rain nearby": "附近有零星小雨",
    "patchy light rain": "零星小雨", "light rain": "小雨",
    "moderate rain at times": "间歇性中雨", "moderate rain": "中雨",
    "heavy rain at times": "间歇性大雨", "heavy rain": "大雨",
    "light rain shower": "小阵雨", "moderate or heavy rain shower": "中到大雨",
    "torrential rain shower": "暴雨", "moderate or heavy rain": "中到大雨",
    # 雷
    "thundery outbreaks possible": "可能有雷阵雨",
    "patchy light rain with thunder": "零星小雨伴雷",
    "moderate or heavy rain with thunder": "中到大雨伴雷",
    "patchy light snow with thunder": "零星小雪伴雷",
    "moderate or heavy snow with thunder": "中到大雪伴雷",
    # 雨夹雪 / 冻雨
    "light sleet": "小雨夹雪", "moderate or heavy sleet": "中到大雨夹雪",
    "sleet": "雨夹雪", "light sleet showers": "小阵雨夹雪",
    "moderate or heavy sleet showers": "中到大阵雨夹雪",
    "light freezing rain": "轻度冻雨", "moderate or heavy freezing rain": "中到强冻雨",
    "patchy freezing drizzle possible": "可能有零星冻毛毛雨",
    "freezing drizzle": "冻毛毛雨", "heavy freezing drizzle": "强冻毛毛雨",
    # 雪
    "patchy light snow": "零星小雪", "light snow": "小雪",
    "patchy moderate snow": "零星中雪", "moderate snow": "中雪",
    "patchy heavy snow": "零星大雪", "heavy snow": "大雪",
    "light snow showers": "小阵雪", "moderate or heavy snow showers": "中到大阵雪",
    "blizzard": "暴雪", "blowing snow": "风吹雪", "ice pellets": "冰粒",
    "patchy moderate snow showers": "零星中阵雪",
}

# 十六方位 -> 中文（wttr.in 的 winddir16Point 是英文缩写）
_WIND_DIR = {
    "N": "北", "NNE": "东北偏北", "NE": "东北", "ENE": "东北偏东",
    "E": "东", "ESE": "东南偏东", "SE": "东南", "SSE": "东南偏南",
    "S": "南", "SSW": "西南偏南", "SW": "西南", "WSW": "西南偏西",
    "W": "西", "WNW": "西北偏西", "NW": "西北", "NNW": "西北偏北",
}

# 蒲福风级上界（km/h）：索引即级别
_BEAUFORT = (1, 5, 11, 19, 28, 38, 49, 61, 74, 88, 102, 117)

# 结果缓存：city -> (时间戳, 结果字典)。天气十分钟粒度足够，
# 免得连着问两句各等 3 秒。
_CACHE: "dict[str, tuple[float, dict]]" = {}
_CACHE_TTL_S = 600.0
_CACHE_LOCK = threading.Lock()


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
    >>> extract_city("外面冷不冷")     # 曾经会把"外面"当城市
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
    """英文天气描述 -> 中文。查不到就原样返回（总比编一个强）。"""
    if not desc:
        return ""
    key = desc.strip().lower()
    return _WEATHER_DESC.get(key, desc.strip())


def _wind_str(point: str, kmh: str) -> str:
    """'SE' + '11' -> '东南风，2级'。

    为什么不直接给 "SE 11km/h"：那串英文会被模型原样念出来（"S E 十一公里
    每小时"），听着像故障。蒲福风级是中文语境里更自然的说法。
    """
    try:
        v = float(kmh)
    except (TypeError, ValueError):
        return ""
    level = sum(1 for b in _BEAUFORT if v > b)
    dir_cn = _WIND_DIR.get((point or "").strip().upper(), "")
    if level <= 0:
        return f"{dir_cn}风，微风" if dir_cn else "基本无风"
    if not dir_cn:
        return f"{level}级风"
    return f"{dir_cn}风，{level}级"


def _pick_day(text: str) -> int:
    """用户问的是哪一天？0=今天（实况）, 1=明天, 2=后天。"""
    if "后天" in text:
        return 2
    if "明天" in text or "明儿" in text:
        return 1
    return 0


def fetch_weather(city: str, timeout_s: float = 6.0,
                  use_cache: bool = True) -> Optional[dict]:
    """查 wttr.in，返回整理好的天气字典；失败返回 None。"""
    if not city:
        return None

    if use_cache:
        with _CACHE_LOCK:
            hit = _CACHE.get(city)
        if hit and time.monotonic() - hit[0] < _CACHE_TTL_S:
            return dict(hit[1])     # 拷贝一份，别让调用方改到缓存

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

        out = {
            "城市": city,
            "地点": area or city,
            "天气": _translate(cur["weatherDesc"][0]["value"]),
            "气温": f"{cur['temp_C']}°C",
            "体感": f"{cur['FeelsLikeC']}°C",
            "湿度": f"{cur['humidity']}%",
            "风": _wind_str(cur.get("winddir16Point", ""),
                            cur.get("windspeedKmph", "")),
            "降水概率": f"{cur.get('chanceofrain', '')}%" if cur.get(
                "chanceofrain", "") != "" else "",
        }

        # 未来两天（wttr.in 的 weather[0] 是今天、[1] 明天、[2] 后天）
        for i, label in ((1, "明天"), (2, "后天")):
            try:
                day = data["weather"][i]
                # hourly 每 3 小时一档：index 4 是正午，最能代表这一天
                hourly = day["hourly"][4]
                # 降水概率取当天最大值——问"会下雨吗"要的是"今天有没有可能下"，
                # 不是正午那一刻的概率
                chance = max((int(h.get("chanceofrain", 0) or 0)
                              for h in day["hourly"]), default=0)
                out[label] = {
                    "天气": _translate(hourly["weatherDesc"][0]["value"]),
                    "气温": f"{day['mintempC']}~{day['maxtempC']}°C",
                    "降水概率": f"{chance}%",
                }
            except Exception:  # noqa: BLE001
                pass
        out["_at"] = time.time()
    except Exception:  # noqa: BLE001
        return None

    with _CACHE_LOCK:
        _CACHE[city] = (time.monotonic(), dict(out))
    return out


def weather_hint(text: str, default_city: str = "", timeout_s: float = 6.0) -> str:
    """给模型的"天气上下文"；不需要或查不到返回 ""。

    返回形如：
        城市：武汉（wttr.in 实时查询）
        现在：小雨，26°C（体感 27°C），湿度 61%，东南风，2级，降水概率 20%
        明天：毛毛雨，12~24°C，降水概率 60%

    查不到时返回 "" —— 交给提示词里的红线（"没看到就说暂时查不到"），
    比塞一句"联网查询失败"更好：后者会被模型原样念给用户听。
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
        return ""

    # wttr.in 认不出这个地名时会猜一个附近的点，回传的"地点"是它实际报的地方。
    # 只有中文城市名才有意义（英文的 areaName 多半是拼音，念出来怪）；
    # 和输入对不上时以它为准，避免包子报一个假地名。
    place = city
    area = str(w.get("地点") or "")
    if area and not area.isascii() and area != city:
        place = f"{city}（{area}）"

    day = _pick_day(text)
    lines = [f"城市：{place}（wttr.in 实时查询，刚刚查到）"]

    if day == 0:
        cur = [f"现在：{w['天气']}", f"{w['气温']}（体感 {w['体感']}）",
               f"湿度 {w['湿度']}"]
        if w.get("风"):
            cur.append(w["风"])
        if w.get("降水概率"):
            cur.append(f"降水概率 {w['降水概率']}")
        lines.append("，".join(cur))
        # 问的是"今天"，顺带把明天的也带上（用户常接着问），但只给一行
        if "明天" in w:
            tm = w["明天"]
            lines.append(f"明天：{tm['天气']}，{tm['气温']}，降水概率 {tm['降水概率']}")
    else:
        label = "明天" if day == 1 else "后天"
        d = w.get(label)
        if not d:
            return ""
        lines.append(f"{label}：{d['天气']}，{d['气温']}，降水概率 {d['降水概率']}")

    return "\n".join(lines)


def clear_cache() -> None:
    """清掉缓存（测试用，或者想强制刷新时）。"""
    with _CACHE_LOCK:
        _CACHE.clear()
