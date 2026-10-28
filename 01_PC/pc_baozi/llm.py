"""大模型对话后端（OpenAI 兼容接口）。

默认指向本地 Ollama：``http://localhost:11434/v1``，模型 ``qwen2.5:3b``
（换成 DeepSeek / 通义 / OpenAI 只需改 config 里的 base_url + api_key + model）。

注意：**不要用带思维链的模型**（qwen3 系列、deepseek-r1 等）。它们会把
max_tokens 全花在内部推理上，content 返回空字符串，语音助手直接哑掉。
本项目检测到这种情况会抛出明确的错误提示，而不是静默失败。

情感整合（任务3）：``chat_ex()`` 在返回回复文本的同时返回一个情绪标签
（happy/sad/angry/surprised/loving/neutral）。该情绪：
    - 经 TTS 之外的另一条链路下发 TD1601（见 td1601.py）驱动舵机/表情；
    - 同时写回 PC 侧的情绪内存（EmotionMemory），短期调控后续对话的语气。
"""

from __future__ import annotations

import datetime
import json
import os
import re
import time
from typing import Dict, List, Optional, Tuple

from emotion import (
    EmotionMemory,
    normalize_emotion,
    normalize_intent,
    normalize_intensity,
    guess_emotion_from_reply,
    DEFAULT_INTENSITY,
)

# 固定人设模板（包子的完整人格）；`{占位符}` 在每次请求前填充。
# 模板文件：pc_baozi/prompts/system_prompt.txt
_PROMPT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "prompts", "system_prompt.txt")


def load_system_prompt_template() -> str:
    """读取固定人设模板；读不到就返回一个最小可用版本。"""
    try:
        with open(_PROMPT_FILE, "r", encoding="utf-8") as f:
            return f.read().strip()
    except Exception as exc:  # noqa: BLE001
        print(f"[llm] 读取人设模板失败（{exc}），使用最小默认人设")
        return "你叫包子，是一只桌面陪伴机器人，说话简短、口语化、友好。"


_WEEKDAY_CN = "一二三四五六日"


def current_date_str() -> str:
    """今天的中文日期 + 星期，供 prompt 注入（包子应能回答"今天几号/星期几"）。"""
    now = datetime.datetime.now()
    return f"{now.year}年{now.month}月{now.day}日 星期{_WEEKDAY_CN[now.weekday()]}"


def parse_memorize(raw) -> Dict[str, str]:
    """解析模型输出的 memorize 字段 → {键: 值}。

    约定格式（容错）：`"名字=小明；生日=3月5日"`，也接受 `,` `|` `、` 分隔，
    键值之间接受 `=` `：` `:`。空串/无内容 → {}。
    """
    if not raw:
        return {}
    text = str(raw).strip()
    if not text or text.lower() in ("none", "null", "无", "没有", "-"):
        return {}

    out: Dict[str, str] = {}
    for seg in re.split(r"[;；|,，、\n]+", text):
        seg = seg.strip()
        if not seg:
            continue
        m = re.match(r"^([^=:：]{1,20})\s*[=:：]\s*(.+)$", seg)
        if not m:
            continue
        key = m.group(1).strip()
        val = m.group(2).strip()
        if key and val:
            out[key] = val
    return out


def parse_people_memo(raw) -> Dict[str, Dict[str, str]]:
    """解析 memorize 的**带归属**格式 → {人名: {字段: 值}}。

    人名为 "user"（或空）表示用户本人。约定格式：

        "user: 名字=小明；生日=3月5日 || 陈若语: 关系=同学；生日=7月20日"

    这样"我朋友的生日"就不会挤占用户自己的「生日」槽位 —— 从结构上防串味。
    容错：段落里没有 `人名:` 前缀的，一律归给 "user"（旧格式向后兼容）。
    """
    if not raw:
        return {}
    text = str(raw).strip()
    if not text or text.lower() in ("none", "null", "无", "没有", "-"):
        return {}

    out: Dict[str, Dict[str, str]] = {}
    # `||` 或 `|` 分段（每段一个人）
    for seg in re.split(r"\|\||\|", text):
        seg = seg.strip()
        if not seg:
            continue
        m = re.match(r"^([^=:：；;,，]{1,20}?)\s*[:：]\s*(.+)$", seg)
        if m:
            who = m.group(1).strip()
            rest = m.group(2).strip()
        else:
            who, rest = "user", seg      # 没写归属 -> 当作用户本人
        fields = parse_memorize(rest)
        if not fields:
            continue
        if who in ("", "user", "本人", "我", "用户"):
            who = "user"
        out.setdefault(who, {}).update(fields)
    return _fix_user_misattribution(_drop_non_people(out))


#: 明显不是"人"的名字（城市/地点等）——记忆库里不该出现这些
_NOT_PERSON_NAMES = {
    "武汉", "北京", "上海", "广州", "深圳", "杭州", "南京", "成都", "西安",
    "重庆", "天津", "苏州", "长沙", "郑州", "青岛", "大连", "厦门", "福州",
    "合肥", "昆明", "贵阳", "南昌", "哈尔滨", "沈阳", "长春", "石家庄",
    "太原", "济南", "兰州", "海口", "乌鲁木齐", "呼和浩特", "西宁", "银川",
    "拉萨", "南宁", "今天", "明天", "后天", "现在", "这里", "那里", "本地",
}


def _fix_user_misattribution(people: Dict[str, Dict[str, str]]) -> Dict[str, Dict[str, str]]:
    """纠正"把别人错记成用户本人"的情况。

    实测（2026-09）：
        "我同学是陈若语"  -> 模型输出 `user: 关系=同学；名字=陈若语`
        "陈若羽是我的同学" -> 模型输出 `user: 名字=陈若羽`
    也就是模型会**把别人的名字写进 user**，导致包子真的以为用户叫陈若语
    —— 这正是用户最初抱怨的"误以为我是 B"。

    兜底规则：user 里出现「同学/同事/朋友/室友/老师…」这类**关系词**，
    或出现「班级/学院/公司」等**只属于他人的字段**时，说明这份信息其实属于
    那个"关系对象"或别人，应该挪到 others 里对应人名下。
    """
    user = people.get("user") or {}
    if not user:
        return people

    # 关系字段：值是人名 -> 那个名字才是"其他人"
    REL_KEYS = ("同学", "同事", "同桌", "室友", "朋友", "好友", "老师",
                "导师", "老板", "队友", "搭档", "女友", "男友")
    # 这些字段本身说明"这份档案属于别人，不是用户"
    NON_USER_KEYS = ("关系", "所在学院", "班级", "年级", "公司", "部门",
                     "职位", "学号", "工号")
    # 身份词：出现这些说明是在描述"另一个人"，不是用户自己
    IDENTITY_WORDS = ("同学", "同事", "同桌", "室友", "朋友", "好友", "老师",
                      "导师", "老板", "队友", "搭档", "女友", "男友", "妈妈",
                      "爸爸", "姐姐", "妹妹", "哥哥", "弟弟", "爷爷", "奶奶")

    moved: Dict[str, Dict[str, str]] = {}
    new_user = dict(user)

    # ① user 里直接有 "名字" 是别人的人名
    rel_val = ""
    for rk in REL_KEYS:
        if rk in new_user:
            rel_val = str(new_user[rk]).strip()
            break

    looks_like_other = bool(rel_val) or any(k in new_user for k in NON_USER_KEYS)
    if looks_like_other and new_user.get("名字"):
        name = str(new_user.get("名字")).strip()
        rel_field_val = str(new_user.get("关系", "")).strip()
        # 判据：名字**等于关系字段的值**（"同学=陈若语"+"名字=陈若语"），
        # 或者关系字段的值是个**身份词**（"关系=同学" 配 "名字=陈若语"）
        #   —— 后者是实测常见形态：模型写 `user: 关系=同学；名字=陈若语`，
        #   意思是"用户有个同学叫陈若语"，却整段塞进了 user。
        is_other = bool(
            (rel_val and name == rel_val)
            or (rel_field_val and rel_field_val in IDENTITY_WORDS)
            or any(k in new_user for k in NON_USER_KEYS)
        )
        # 但如果 user 里已经有一个"不像身份词"的名字，且这次没有身份线索，
        # 就别乱动（那是用户自己在报名字）
        if is_other:
            moved.setdefault(name, {}).update(
                {k: v for k, v in new_user.items()
                 if k not in ("名字", "关系", "同学") and k not in NON_USER_KEYS})
            moved[name].setdefault("名字", name)
            # 把身份词转成"关系"，一并挪到那个人名下
            if rel_field_val in IDENTITY_WORDS:
                moved[name].setdefault("关系", rel_field_val)
            elif rel_val and rel_val in IDENTITY_WORDS:
                moved[name].setdefault("关系", rel_val)
            new_user = {k: v for k, v in new_user.items()
                        if k not in ("名字", "关系", "同学")
                        and k not in NON_USER_KEYS}

    # ② user 里 "同学=陈若语" 这种，只是线索，不算用户自己的信息
    #    -> 保留在 user（无害），同时不建档案（等用户介绍时再确认）

    if not moved:
        return people

    # ⚠️ 返回**扁平的人名映射**，与 parse_people_memo 的约定一致。
    #    早先这里错误地返回了 {"user":..., "others":...} 嵌套结构，
    #    结果 chat_ex 把 "others" 本身当成人名去 remember（实测 trace
    #    里看到 `remember('others', {...})`），归属修正等于失效。
    out = dict(new_user and {"user": new_user} or {})
    for nm, flds in (people.get("others") or {}).items():
        out.setdefault(nm, {}).update(flds)
    for nm, flds in moved.items():
        out.setdefault(nm, {}).update(flds)
    if not new_user:
        out.pop("user", None)
    return out


def _drop_non_people(people: Dict[str, Dict[str, str]]) -> Dict[str, Dict[str, str]]:
    """挡掉被误当成人名的地名/时间词，以及**结构保留字**。

    实测（2026-09）：问完天气后，模型会把「武汉」当成一个人写进 memorize，
    于是记忆库里出现 {"武汉": {"现在": "小阵雨"}} 这种垃圾条目。

    更糟的一次：包子人设里有「【其他人】」这个人物档案标题，模型就照着
    输出 `others: 名字=陈若语`，于是记忆库里出现一个名叫 **"others"** 的人，
    名字确认流程还傻傻地去跟用户核对"others"是哪几个字（日志里的
    `放弃核对名字「others」`）。所以结构保留字必须一并挡掉。
    """
    out = {}
    for who, fields in people.items():
        if who == "user":
            out[who] = fields
            continue
        if who in _NOT_PERSON_NAMES or who in _RESERVED_KEYS:
            continue
        # 去掉中文书名号/括号等包裹（模型可能写「其他人」、【他人】）
        bare = who.strip("【】「」<>《》()（）[]:： 　")
        if bare in _NOT_PERSON_NAMES or bare in _RESERVED_KEYS:
            continue
        out[who] = fields
    return out


#: 结构化保留字：这些是记忆库的**内部键名/标记**，不是人名。
#: 模型看到 prompt 里的档案结构后会照着输出（"others: 名字=陈若语"），必须挡掉。
_RESERVED_KEYS = {
    "others", "user", "本人", "其他人", "其他", "人物", "人物档案", "档案",
    "people", "person", "pending", "待确认", "profile", "画像", "名字",
    "关系", "字段", "key", "value", "null", "none", "true", "false",
    # 内部标记：历史里绝不该出现，模型若照抄过来必须拦掉
    "forget", "forget:", "memorize", "忘记", "遗忘", "删除", "FORGET",
}


def parse_forget(raw):
    """解析 forget 字段（用户纠错/遗忘）→ 要删的条目列表。

    约定格式：`"user: 生日"`（删本人某个字段）、`"user"`（删本人整档）、
    `"陈若语"`（删掉这个人整档）、`"陈若语: 生日"`。空 → []。
    返回 [{"who": "user"|人名, "field": "字段"|""}, ...]
    """
    if not raw:
        return []
    text = str(raw).strip()
    if not text or text.lower() in ("none", "null", "无", "没有", "-"):
        return []

    out = []
    for seg in re.split(r"[;；|,，、\n]+", text):
        seg = seg.strip()
        if not seg:
            continue
        if ":" in seg or "：" in seg:
            who, field = re.split(r"[:：]", seg, maxsplit=1)
        else:
            who, field = seg, ""
        who = who.strip()
        field = field.strip()
        # 容错：模型偶尔写成 `"user: 名字=宋泽纬"`（带上旧值），
        # 那不是字段名 —— 只取 `=` 左边，否则 memory.forget() 会去找一个
        # 叫"名字=宋泽纬"的字段，永远删不掉（静默失效）。
        if field and ("=" in field or "：" in field or ":" in field):
            field = re.split(r"[=:：]", field, maxsplit=1)[0].strip()
        if who in ("", "本人", "我", "用户"):
            who = "user"
        if who:
            out.append({"who": who, "field": field})
    return out


def _iter_json_objects(text: str):
    """按「括号配平」从文本里依次切出候选 JSON 对象（从后往前，JSON 通常在末尾）。

    为什么不用正则``\\{[^{}]*\\}``：那个字符类排除了花括号，只要 reply 字符串里
    出现 ``{`` 或 ``}`` 就匹配不上，静默降级成 neutral —— 表现为「情绪永远不切换」。
    这里按字符扫描并跟踪「是否在字符串内 / 是否被反斜杠转义」，
    能正确处理 ``{"reply":"{哈哈}","emotion":"happy"}`` 这类内容。
    """
    out = []
    depth = 0
    start = -1
    in_str = False
    esc = False
    for i, ch in enumerate(text):
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
        elif ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            if depth > 0:
                depth -= 1
                if depth == 0 and start >= 0:
                    out.append(text[start:i + 1])
                    start = -1
    # JSON 一般在末尾，优先从后往前试
    return list(reversed(out))


# 小模型常见"复读病"：把 prompt 里的示例占位字当标准答案，输出一堆
# Markdown 代码块 + "- reply：" "- emotion：" 标签，原样吐回来。
# 这些字不该进喇叭，统一识别并清掉。
_PLACEHOLDER_MARKERS = (
    "给用户的话",
    "这里写你真正要说的那句话",
    "这里写…",
    "<这里写",
    "要念出来的话",
)


def _looks_like_placeholder(reply: str) -> bool:
    """回复是不是模型在复读占位/格式垃圾，而非真实回答。"""
    r = reply.strip()
    if not r:
        return True
    # 直接命中占位字
    for m in _PLACEHOLDER_MARKERS:
        if m in r:
            return True
    # 里面混了 Markdown 代码块 / "- reply：" / "- emotion：" 之类格式标签
    if "```" in r:
        return True
    if any(k in r for k in ("- reply", "- emotion", "- intent", "- intensity")):
        return True
    return False


def _strip_format_junk(reply: str) -> str:
    """把 Markdown 代码块围栏、"- reply：" 标签等格式垃圾剥掉，尽量留下正文。"""
    r = reply.strip()
    # 去掉成对的 ``` 代码块围栏（含语言标注）
    r = re.sub(r"```[a-zA-Z]*", "", r)
    # 去掉 "- reply：" / "reply：" 这类字段标签行
    r = re.sub(r"(?m)^\s*[-*]?\s*(reply|emotion|intent|intensity|emotion|回复)\s*[：:]\s*", "", r)
    return r.strip()


class Llm:
    name = "base"

    def chat(self, user_text: str) -> str:
        """送入一句话，返回要朗读的回复文本。"""
        raise NotImplementedError

    def chat_ex(self, user_text: str, vision_context: str = "",
                calc_context: str = "", web_context: str = "") -> Tuple[str, str, str, str]:
        """返回 (回复文本, 情绪, 意图, 情感强度)。

        vision_context: 若本轮已经调用过视觉（用户在看图），把图像描述传进来，
            作为额外上下文交给模型，让它结合实际看到的东西回答。
        calc_context:   若本轮话里有算式，把代码算好的精确结果传进来
            （如 "123*456=56088"），避免 7B 自己做多位数乘法算错。
        web_context:    若本轮需要联网检索（问天气），把查到的实时数据传进来。
        情感强度 intensity: low / medium / high，用于 TD1601 调制动作幅度与速度。
        """
        raise NotImplementedError

    def need_vision(self, user_text: str) -> bool:
        """判断这句话是否需要"看一眼"（视觉）。默认不判断 → False。"""
        return False

    def reset(self) -> None:
        """清空对话历史。"""


# ---------------------------------------------------------------------
class OpenAiCompatLlm(Llm):
    name = "openai"

    # 输出格式要求：固定 JSON，含 reply / emotion / intent 三个字段
    OUTPUT_INSTRUCTION = (
        "\n\n# ====================\n"
        "【输出格式 - 必须严格遵守，违反会导致机器人故障】\n"
        "你的回复有且只有一个 JSON 对象，除此之外不要输出任何字符。\n"
        "⚠️ 绝对不要先说话再贴 JSON：不要说“好的”“明白了”之类，不要写解释，\n"
        "   不要用 Markdown 或代码块，第一个字符就必须是 `{`，最后一个字符是 `}`。\n"
        "JSON 示例（字段名照抄，值换成你的真实内容）：\n"
        '{"reply": "<这里写你真正要说的那句话>", "emotion": "<六选一>", '
        '"intensity": "<三选一>", "intent": "<七选一>"}\n'
        "- reply：要念出来的话（口语化纯文本，别写 Markdown/表情符号/旁白）。\n"
        "    ⚠️ reply 里必须写你这一轮真正想对用户说的话，绝对禁止照抄\n"
        "    “给用户的话”“<这里写…>”这类占位字，或输出空的 reply。\n"
        "- emotion：只能是 happy / sad / angry / surprised / loving / neutral 之一\n"
        "- intensity：情感强度，只能是 low / medium / high 之一\n"
        "    · low    = 淡淡的一点（随口一提、轻微共情）\n"
        "    · medium = 正常程度（默认，多数情况用它）\n"
        "    · high   = 强烈（用户情绪很激动，或明确要求你「非常」生气/开心/伤心）\n"
        "- intent：只能是 GREETING / QUESTION / CHAT / COMMAND / THANKS / "
        "GOODBYE / UNKNOWN 之一\n"
        "- 四个字段必须全部出现，一个都不能漏！\n"
        "- 禁止输出任何动作/硬件指令（如 SERVO、SCREEN、LED、action）\n"
    )

    def __init__(
        self,
        base_url: str,
        api_key: str,
        model: str,
        temperature: float = 0.7,
        max_tokens: int = 512,
        timeout_s: float = 120.0,
        system_prompt: str = "",
        history_turns: int = 8,
        emotion_memory: Optional[EmotionMemory] = None,
        num_ctx: int = 2048,
        memory=None,
    ) -> None:
        try:
            from openai import OpenAI  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "需要 openai 客户端库：pip install openai"
            ) from exc

        self._client = OpenAI(base_url=base_url, api_key=api_key or "none",
                              timeout=timeout_s)
        self._base_url = base_url          # 预热失败时要打出来（见 warmup）
        self._model = model
        self._temperature = temperature
        self._max_tokens = max_tokens
        # 固定人设：优先用模板文件，参数里的 system_prompt 作为兜底
        self._system = system_prompt or load_system_prompt_template()
        self._history_turns = max(0, int(history_turns))
        self._messages: List[Dict[str, str]] = []
        self.emotion_memory = emotion_memory
        self._num_ctx = int(num_ctx)
        # 记忆系统（短期历史 + 长期画像）。给了就用它，否则退回内置 _messages。
        self.memory = memory
        print(f"[llm] OpenAI 兼容后端: {base_url} / {model}")
        print(f"[llm] 固定人设已加载（{len(self._system)} 字符）")
        if self.memory is not None:
            print("[llm] 记忆系统已接入（短期历史 + 长期画像）")

    def warmup(self) -> None:
        """预热：发一个最短的请求，让 Ollama 把模型加载进显存并常驻。

        否则**第一句话**要等模型冷加载（7B 约 10 秒以上）。

        ⚠️ 失败时的措辞要准：原来写"不影响使用，只是首句会慢"，实测打脸 ——
        Ollama 没起来时**每一轮**都会 Connection error，包子完全不会回话。
        所以这里区分"连不上服务"和"其它错误"，前者直接给出该敲什么命令。
        """
        try:
            t0 = time.time()
            self._client.chat.completions.create(
                model=self._model,
                messages=[{"role": "user", "content": "你好"}],
                max_tokens=1,
            )
            print(f"[llm] 预热完成，模型已加载（{time.time() - t0:.1f}s）")
        except Exception as exc:  # noqa: BLE001
            text = f"{type(exc).__name__}: {exc}"
            unreachable = any(k in text for k in (
                "Connection error", "ConnectionError", "APIConnectionError",
                "ConnectError", "Connection refused", "timed out", "Timeout"))
            if unreachable:
                print(f"[llm] !! 连不上大模型服务：{self._base_url}")
                print("[llm]    Ollama 没在运行 —— 现在包子【不会回话】，不是麦克风问题。")
                print("[llm]    解决：开始菜单打开 Ollama（或命令行执行 ollama serve），")
                print(f"[llm]          再确认模型存在：ollama list | 需要 {self._model}")
                print("[llm]    （服务起来后不用重启本程序，下一句话就会好）")
            else:
                print(f"[llm] 预热失败（不影响使用，只是首句会慢）：{exc}")

    # 判断"这句话要不要看图"的轻量提示词（只要一个词，省 token/时间）
    VISION_JUDGE_PROMPT = (
        "你要判断用户这句话是否在询问摄像头能看到的东西"
        "（例如：你看到了什么、这是什么、我手里拿着什么、前面有什么、"
        "帮我看看、描述一下画面）。\n"
        "只回答一个词：YES 或 NO。不要输出任何其它内容。\n"
    )

    def need_vision(self, user_text: str) -> bool:
        """轻量判断：这句话是否需要调用摄像头看一眼。

        只让它回一个 YES/NO（max_tokens=4），很快、也不影响上下文历史。
        出错或不明确时一律返回 False（宁可不看图，也不误触发开销）。
        """
        try:
            resp = self._client.chat.completions.create(
                model=self._model,
                messages=[
                    {"role": "system", "content": self.VISION_JUDGE_PROMPT},
                    {"role": "user", "content": user_text},
                ],
                temperature=0.0,          # 判断要确定，别随机
                max_tokens=4,
            )
            ans = (resp.choices[0].message.content or "").strip().upper()
            return ans.startswith("YES")
        except Exception as exc:  # noqa: BLE001
            print(f"[llm] 视觉判断失败（按“不看图”处理）：{exc}")
            return False

    # ------------------------------------------------------------------
    def _render_system(self, user_text: str, vision_context: str = "",
                       calc_context: str = "", web_context: str = "") -> str:
        """把固定人设模板里的占位符填上当前状态（动态调整情感/意图）。

        模板里的占位符（见 prompts/system_prompt.txt）：
            {robot_emotion}    当前情绪
            {robot_energy}     精力 0~100
            {friendship_level} 亲密度（暂无长期记忆，给固定值 50）
            {current_date}     今天的日期 + 星期
            {user_input}       本轮用户输入
        同时也兼容 Baozi_PC 原模板的 {{xxx}} 写法。
        """
        emo = "neutral"
        energy = 50
        if self.emotion_memory is not None:
            emo = self.emotion_memory.get_emotion()
            energy = int(round(self.emotion_memory.get_state().get("energy", 0.5) * 100))

        text = self._system
        mapping = {
            "robot_emotion": emo,
            "robot_energy": str(energy),
            "friendship_level": "50",
            "current_date": current_date_str(),
            "user_input": user_text,
        }
        for key, val in mapping.items():
            text = text.replace("{{" + key + "}}", val)   # Baozi_PC 原写法
            text = text.replace("{" + key + "}", val)     # 简化写法

        # 长期记忆（事实库 + 画像）：让包子记得用户是谁、生日、家人…
        if self.memory is not None:
            text += self.memory.prompt_block()

        # 本轮调用过视觉 -> 把"看到的东西"作为上下文注入，让回答结合实际画面
        if vision_context:
            text += (
                "\n\n# 你刚刚看到的内容（由摄像头画面分析得到）\n"
                f"{vision_context}\n"
                "请结合上面的画面内容回答用户，不要凭空编造画面里没有的东西。\n"
            )

        # 本轮检测到算式 -> 注入精确计算结果（7B 自己算多位数乘法不可靠）
        if calc_context:
            text += (
                "\n\n# 计算器（本轮算式已经精确算好，直接用这个结果）\n"
                f"{calc_context}\n"
                "不要自己重算，直接把上面的结果写进 reply 里。\n"
            )

        # 本轮查了天气 -> 注入实时数据（包子自己没有联网能力，必须靠这个）
        if web_context:
            text += (
                "\n\n# 天气查询结果（实时数据，来自 wttr.in）\n"
                f"{web_context}\n"
                "请用这些数据如实回答，不要编造或改动数字。\n"
            )

        return text + self.OUTPUT_INSTRUCTION

    def _build_messages(self, user_text: str, vision_context: str = "",
                        calc_context: str = "", web_context: str = "") -> List[Dict[str, str]]:
        messages: List[Dict[str, str]] = [
            {"role": "system",
             "content": self._render_system(user_text, vision_context,
                                            calc_context, web_context)}
        ]
        # ★ 看图时不带历史（2026-11 修）：视觉是"即时感知"，跟过去的聊天
        #   历史不该互相串。原实现把最近 20 轮历史全带上，其中残留上一轮
        #   的视觉回答（如"我看到纸张"），和本轮新的视觉描述（"人+遥控器"）
        #   同时喂给 LLM —— 两处矛盾，7B 偏向历史里自己刚说过的话，导致
        #   YOLO 认对了、回复却说成上一张图的内容。
        #   看图时清空历史，LLM 只能依据本轮 vision_context 回答，不再串。
        if vision_context:
            # 直接跳过历史（memory 或内置 _messages 都不要）
            pass
        elif self.memory is not None:
            messages.extend(self.memory.get_history())
        else:
            messages.extend(self._messages)
        messages.append({"role": "user", "content": user_text})
        return messages

    def _extract(self, raw: str):
        """从模型输出里抽出 (reply, emotion, intent, intensity, memorize, forget)。

        memorize：本轮"学到的信息"，**带人分区** → {人名: {字段: 值}}，
            "user" 表示用户本人（见 parse_people_memo）。
        forget：  用户纠错/要求遗忘的条目 → [{"who":.., "field":..}]。

        容错：不是 JSON（或字段缺失/非法）就降级 —— 整段当 reply，emotion=neutral，
        intent=none。这样即使模型不听话，包子也不会哑掉。
        """
        text = raw.strip()
        data = None

        # ① 整段就是 JSON（最理想）
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            pass

        # ② 从文本里找 JSON 对象。
        #    ⚠️ 关键坑：不能只匹配「内部不含花括号」的 {[^{}]*}。模型常先输出
        #    一句自由文本、再跟 JSON（如 `好吧…{"reply":"好吧…","emotion":"sad"}`）；
        #    一旦 reply 字符串里带花括号，正则就匹配失败 → 静默降级成 neutral，
        #    表现为「情绪永远不切换」。改用括号配平扫描。
        if data is None:
            for cand in _iter_json_objects(text):
                try:
                    d = json.loads(cand)
                except json.JSONDecodeError:
                    continue
                if isinstance(d, dict):
                    data = d
                    break

        if isinstance(data, dict):
            reply = str(data.get("reply", "")).strip()
            raw_emo = data.get("emotion")
            emotion = normalize_emotion(raw_emo)
            intent = normalize_intent(data.get("intent", "none"))
            intensity = normalize_intensity(data.get("intensity"))
            memorize = parse_people_memo(data.get("memorize"))
            forget = parse_forget(data.get("forget"))
            # 模型漏了 emotion 字段时（实测会发生：它模仿历史里"只有 reply"的
            # JSON），normalize_emotion(None) 会返回 neutral —— 此时用回复文本
            # 做一次保守的关键词推测，避免情绪永远卡在 neutral。
            if reply and not raw_emo:
                guessed = guess_emotion_from_reply(reply)
                if guessed != "neutral":
                    emotion = guessed
            # ★ 兜底：模型复读了占位字/格式垃圾（如 reply="给用户的话" 或
            #   带 ``` 代码块），宁可整句丢弃也不进喇叭 —— 否则包子会念出
            #   "给用户的话 - emotion：neutral" 这种没人能听懂的东西。
            if reply and _looks_like_placeholder(reply):
                cleaned = _strip_format_junk(reply)
                if cleaned and not _looks_like_placeholder(cleaned):
                    reply = cleaned
                else:
                    reply = ""   # 洗不干净就丢弃，让上层当作"没内容"处理
            if reply:
                return reply, emotion, intent, intensity, memorize, forget

        # 降级：整段当回复（连 JSON 都没有）
        if text:
            cleaned = _strip_format_junk(text)
            if _looks_like_placeholder(cleaned):
                return ("", "neutral", "none", DEFAULT_INTENSITY, {}, [])
            return (cleaned, guess_emotion_from_reply(cleaned), "none",
                    DEFAULT_INTENSITY, {}, [])
        return "", "neutral", "none", DEFAULT_INTENSITY, {}, []

    # ------------------------------------------------------------------
    def chat_ex(self, user_text: str, vision_context: str = "",
                calc_context: str = "", web_context: str = "") -> Tuple[str, str, str, str]:
        """返回 (回复文本, 情绪, 意图, 情感强度)。

        vision_context: 本轮视觉描述（见基类说明）
        calc_context:   本轮算式的精确计算结果（如 "123*456=56088"）
        web_context:    本轮联网检索结果（如天气）
        """
        resp = self._client.chat.completions.create(
            model=self._model,
            messages=self._build_messages(user_text, vision_context,
                                          calc_context, web_context),
            temperature=self._temperature,
            max_tokens=self._max_tokens,
            extra_body={"options": {"num_ctx": self._num_ctx}},
        )
        choice = resp.choices[0]
        msg = choice.message
        raw = (msg.content or "").strip()

        # 有些模型（Qwen3 / DeepSeek-R1 这类带思维链的）会把内容放进
        # reasoning 字段，content 是空的 —— 明确报错，别丢空话给 TTS。
        if not raw:
            reasoning = getattr(msg, "reasoning", None) or getattr(
                msg, "reasoning_content", None
            )
            if reasoning:
                if choice.finish_reason == "length":
                    raise RuntimeError(
                        f"模型 '{self._model}' 是思维链模型：{self._max_tokens} tokens "
                        f"全用在推理上，没输出正文。请换纯文本模型"
                        f"（如 qwen2.5:7b），或把 llm_max_tokens 调大。"
                    )
                raise RuntimeError(
                    f"模型 '{self._model}' 只返回了推理内容、没有正文，"
                    f"换一个非思维链模型（如 qwen2.5:7b）试试。"
                )
            raise RuntimeError(
                f"模型 '{self._model}' 返回了空内容 (finish_reason={choice.finish_reason})"
            )

        reply, emotion, intent, intensity, memorize, forget = self._extract(raw)

        # ⚠️ 情绪命令兜底（2026-09 实测）：模型会把"你生气一下"理解成
        #    "用户生气了要安慰"（6 次只中 1 次），这是陪伴人设的强先验，
        #    prompt 压不住。这里用**用户原话**做代码层识别并覆盖 emotion。
        #    只在识别到明确的"你+情绪词+命令语气"时才动手，不会误伤普通对话。
        try:
            from emotion import parse_emotion_command
            cmd = parse_emotion_command(user_text)
            if cmd:
                cmd_emo, cmd_int = cmd
                if cmd_emo != emotion:
                    print(f"[llm] 情绪命令纠正：模型给 {emotion}，"
                          f"按用户命令改为 {cmd_emo}")
                emotion = cmd_emo
                intensity = cmd_int
        except Exception as exc:  # noqa: BLE001
            print(f"[llm] 情绪命令识别出错（忽略）：{exc!r}")

        # 长期记忆：把本轮"学到的信息"按**人**归档（"user" = 用户本人）。
        # 结构和历史里都带上人名前缀，模型据此学会"谁的信息归谁"。
        hist_memo = ""
        if self.memory is not None and (memorize or forget):
            # 先处理纠错（用户说"不对/忘掉"），再写入新信息
            for item in forget:
                try:
                    self.memory.forget(item.get("who", "user"), item.get("field", ""))
                except Exception as exc:  # noqa: BLE001
                    print(f"[llm] 遗忘失败：{exc!r}")
            for who, fields in memorize.items():
                try:
                    # 用户本人和别人的信息都直接记入人物档案（按人分区，不会串味）。
                    # 早期版本让"别人的名字"先走待确认流程（同音字核对），
                    # 但那套拆字交互在真实语音链路里太啰嗦、状态机也容易乱，
                    # 已按用户要求整体移除；现在靠 _fix_known_names 做同音纠正，
                    # 记错了直接对话纠正（forget）即可。
                    self.memory.remember(who, fields)
                except Exception as exc:  # noqa: BLE001
                    print(f"[llm] 记忆写入失败：{exc!r}")

        # ⚠️ 历史里 assistant 的 memorize 只放**干净的人名信息**，绝不写
        #    "FORGET: ..." 这类内部标记。
        #    为什么（2026-09 实测的真实故障）：早先我把遗忘标记也拼进历史
        #    （`"名字=宋泽伟 || FORGET: user: 名字"`），模型下一轮就**照抄**这个
        #    格式，输出 `memorize: "FORGET: user: 名字"` —— 于是记忆库里出现一个
        #    名叫 **"FORGET"** 的人。历史是模型模仿的范本，只能放"正确的样子"。
        if memorize:
            hist_memo = " || ".join(
                f"{who}: " + "；".join(f"{k}={v}" for k, v in fields.items())
                for who, fields in memorize.items())
        else:
            hist_memo = ""

        # ⚠️ 历史里 assistant 存【完整 JSON】（reply + emotion + intent + intensity
        #    + memorize）。为什么不能存裸文本、也不能只存 {"reply": ...}（2026-09 实测）：
        #      · 存裸文本：模型模仿历史格式，输出退化成"正文 + 末尾一个情绪词"
        #        （如 `好的，我陪你难过。sad`）→ 抽不到 JSON → 情绪卡死 neutral。
        #      · 只存 {"reply": ...}：模型照抄这个"精简"格式，也只输出 reply，
        #        → emotion 字段缺失 → 又回 neutral。
        #    存完整字段后模型照抄即可拿到正确字段（实测 3/3 正确）。
        #    memorize 也带上：让模型知道"这个字段每轮都要给"（没有就留空串），
        #    并且能看到"人分区"的写法，从而学会区分"我的"和"朋友的"。
        hist_assistant = json.dumps(
            {"reply": reply, "emotion": emotion,
             "intent": intent, "intensity": intensity, "memorize": hist_memo},
            ensure_ascii=False,
        )

        if self.memory is not None:
            # 记忆系统：记入短期历史；太长了就后台压缩成长期画像
            self.memory.add_message("user", user_text)
            self.memory.add_message("assistant", hist_assistant)
            if self.memory.need_compress():
                self.memory.compress_async()
        elif self._history_turns > 0:
            # 退回内置历史（同样存最小 JSON，理由见上）
            self._messages.append({"role": "user", "content": user_text})
            self._messages.append({"role": "assistant", "content": hist_assistant})
            keep = self._history_turns * 2
            if len(self._messages) > keep:
                self._messages = self._messages[-keep:]

        return reply, emotion, intent, intensity

    def chat(self, user_text: str) -> str:
        return self.chat_ex(user_text)[0]

    def reset(self) -> None:
        self._messages.clear()
        if self.memory is not None:
            self.memory.clear_history()


# ---------------------------------------------------------------------
class EchoLlm(Llm):
    """不接大模型，直接把听到的话回读一遍，用来验证“耳朵->嘴”闭环。"""

    name = "echo"

    def __init__(self, prefix: str = "你说的是：") -> None:
        self._prefix = prefix

    def chat(self, user_text: str) -> str:
        return f"{self._prefix}{user_text}"

    # ⚠️ 流水线走的是 chat_ex()（要情绪/意图/强度），不是 chat()。
    #    这里以前没重写 chat_ex，于是拿到的还是基类的 NotImplementedError ——
    #    用 echo 后端跑链路时 ASR 之后直接报"大模型调用失败"，
    #    整条 TTS/播放都测不到（离线自测和无模型调试都踩过）。
    #    返回值必须与基类契约一致：(回复, 情绪, 意图, 强度)。
    def chat_ex(self, user_text: str, vision_context: str = "",
                calc_context: str = "", web_context: str = ""):
        return (f"{self._prefix}{user_text}", "neutral", "chat", DEFAULT_INTENSITY)

    def reset(self) -> None:
        pass


class MockLlm(Llm):
    """固定回复。"""

    name = "mock"

    def __init__(self, reply: str = "收到，我是包子。") -> None:
        self._reply = reply

    def chat(self, user_text: str) -> str:
        return self._reply

    def reset(self) -> None:
        pass


class BrokenLlm(Llm):
    """总是抛异常，用来验证流水线不会因为大模型出错而卡死。"""

    name = "broken"

    def __init__(self, msg: str = "模拟大模型故障") -> None:
        self._msg = msg

    def chat(self, user_text: str) -> str:
        raise RuntimeError(self._msg)

    def reset(self) -> None:
        pass


# ---------------------------------------------------------------------
def make_llm(cfg, emotion_memory: Optional[EmotionMemory] = None, memory=None) -> Llm:
    backend = (cfg.llm_backend or "openai").lower()

    if backend == "openai":
        return OpenAiCompatLlm(
            base_url=cfg.llm_base_url,
            api_key=cfg.llm_api_key,
            model=cfg.llm_model,
            temperature=cfg.llm_temperature,
            max_tokens=cfg.llm_max_tokens,
            timeout_s=cfg.llm_timeout_s,
            system_prompt=cfg.system_prompt,
            history_turns=cfg.history_turns,
            emotion_memory=emotion_memory,
            num_ctx=getattr(cfg, "llm_num_ctx", 2048),
            memory=memory,
        )
    if backend == "echo":
        return EchoLlm()
    if backend == "mock":
        return MockLlm()

    raise ValueError(f"未知的 llm_backend: {cfg.llm_backend}")
