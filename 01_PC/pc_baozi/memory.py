"""包子的记忆系统（移植自 Baozi_PC/memory/memory_manager.py）。

职责（= 📋 任务3 里"情感状态机的第一部分：PC 上作为大语言模型的 memory"）：
  1. 短期对话历史（最近 N 轮）——让包子记得刚才聊了什么；
  2. 长期用户画像（LLM 压缩得到的一段话）——记住用户是谁、偏好什么；
  3. 后台异步压缩：历史太长时，把旧对话"总结"进画像并清空旧历史，
     这样长期聊下去既不会丢关键信息，也不会让 prompt 无限膨胀（省 token、更快）。

不负责：调用 LLM 对话、情绪判断、硬件、TTS。

持久化：长期画像存到 memory.json（重启后仍在）。
"""

from __future__ import annotations

import json
import os
import threading
from typing import Dict, List


class MemoryManager:
    """短期历史 + 长期画像 + 后台压缩。"""

    def __init__(
        self,
        memory_file: str = "memory.json",
        max_history: int = 20,
        llm_base_url: str = "http://localhost:11434/v1",
        llm_api_key: str = "ollama",
        model_name: str = "qwen2.5:7b",
        log=print,
        enabled: bool = True,
    ) -> None:
        self.memory_file = memory_file
        self.max_history = int(max_history)      # 保留多少轮（1 轮 = 用户+助手 2 条）
        self.llm_base_url = llm_base_url
        self.llm_api_key = llm_api_key
        self.model_name = model_name
        self.log = log
        self.enabled = enabled

        self.conversation_history: List[Dict[str, str]] = []
        self.user_profile = ""
        # ==================== 长期记忆：按「人」分区 ====================
        # 为什么要分区（2026-09 实测的真实故障）：
        #   旧结构是一个扁平字典 {"名字": "龚泽安", "生日": "12月4号", "同学": "陈若语"}。
        #   用户说"我朋友陈若语，生日是7月20日"时，模型**无处可放** —— `生日` 这个键
        #   已经被用户自己占了，于是要么覆盖（把用户的生日改成朋友的）要么丢弃，
        #   之后包子就会把两个人的信息串在一起，误以为用户是陈若语。
        #
        # 新结构：每个人一个独立的"文档"（字典），信息不会跨人串味。
        #   {
        #     "user":  {"名字": "龚泽安", "生日": "12月4号"},        <- 用户本人，唯一槽位
        #     "others": {                                          <- 其他人，按名字索引
        #         "陈若语": {"关系": "同学", "生日": "7月20日"}
        #     }
        #   }
        # `user` 永远只有一个，绝不可能被"别人"的信息污染 —— 这是结构上的保证，
        # 不依赖模型是否听话。
        self.people: Dict[str, Dict[str, str]] = {"user": {}}
        self.compressing = False
        self._lock = threading.RLock()

        self.load()

    # ========================== 长期记忆 ==========================
    def load(self) -> None:
        if not os.path.exists(self.memory_file):
            return
        try:
            with open(self.memory_file, "r", encoding="utf-8") as f:
                data = json.load(f)
            with self._lock:
                self.user_profile = str(data.get("profile", "") or "")
                self.people = self._normalize_people(data.get("people"),
                                                     data.get("facts"))
                self._ensure_shape()
            n_people = len(self.people.get("others", {}))
            if self.user_profile or self.people.get("user") or n_people:
                self.log(f"[mem] 已加载长期记忆（本人 {len(self.people.get('user', {}))} 条，"
                         f"其他人 {n_people} 位，画像 {len(self.user_profile)} 字）")
        except Exception as exc:  # noqa: BLE001
            self.log(f"[mem] 加载记忆失败：{exc}")

    @staticmethod
    def _normalize_people(people, legacy_facts=None) -> Dict[str, Dict[str, str]]:
        """把读到的数据整理成规范的 people 结构，并**迁移旧版扁平 facts**。

        旧版（{"名字":.., "生日":.., "同学":..}）一律归到 "user" 名下 ——
        这是当时唯一的解读方式，迁移后至少不会丢数据，之后模型纠错时会修正。
        """
        out: Dict[str, Dict[str, str]] = {"user": {}, "others": {}}

        def _clean(d) -> Dict[str, str]:
            if not isinstance(d, dict):
                return {}
            return {str(k).strip(): str(v).strip()
                    for k, v in d.items() if str(k).strip() and str(v).strip()}

        if isinstance(people, dict):
            out["user"] = _clean(people.get("user"))
            others = people.get("others")
            if isinstance(others, dict):
                for name, fields in others.items():
                    nm = str(name).strip()
                    if nm and nm != "user":
                        cf = _clean(fields)
                        if cf:
                            out["others"][nm] = cf

        # 旧版扁平 facts -> 全归到"本人"名下
        if not out["user"] and not out["others"] and legacy_facts:
            out["user"] = _clean(legacy_facts)

        return out

    def _ensure_shape(self) -> None:
        """保证 people 结构完整（user / others 两个键都在）。

        为什么需要：`get_people()`、`list_people()` 等调用方默认这两个键存在。
        一旦某个路径把 people 换成缺键的 dict，就会 KeyError 崩掉整条对话链路。
        """
        with self._lock:
            if not isinstance(self.people, dict):
                self.people = {"user": {}, "others": {}}
            self.people.setdefault("user", {})
            self.people.setdefault("others", {})

    def save(self) -> None:
        try:
            with self._lock:
                profile = self.user_profile
                people = json.loads(json.dumps(self.people, ensure_ascii=False))
            with open(self.memory_file, "w", encoding="utf-8") as f:
                json.dump({"profile": profile, "people": people},
                          f, ensure_ascii=False, indent=2)
        except Exception as exc:  # noqa: BLE001
            self.log(f"[mem] 保存记忆失败：{exc}")

    # ---------------------- 长期记忆：按人分区读写 ----------------------
    #: 绝不能当【其他人】名字的字符串（模型输出的结构标记/保留字）。
    #: ⚠️ 注意 "user" **不**在这里 —— 它是"用户本人"的合法键。
    _BAD_PERSON = {
        "others", "forget", "memorize", "pending", "people", "profile",
        "forget:", "null", "none", "true", "false", "undefined",
        "其他人", "本人", "人物", "档案", "待确认", "忘记", "遗忘", "删除",
    }

    @classmethod
    def _valid_person_key(cls, who: str) -> bool:
        """判断一个字符串能不能当"人名"存进记忆库。

        允许：``user`` / ``本人`` / ``我``（= 用户本人），以及 1~12 字的普通字符串。
        拒绝：空串、内部标记（FORGET/others…）、带分隔符的串
              （``"user: 名字"`` 这种明显是模型把整行塞进来了）。
        """
        if who in ("user", "本人", "我", ""):
            return True                      # 用户本人的常规写法
        if not who or len(who) > 12:
            return False
        if who.lower() in cls._BAD_PERSON:
            return False
        # 带 : = ； | 的串不是人名（是把整条记录当人名了）
        if any(ch in who for ch in ":：=；;|｜\n"):
            return False
        return True

    @staticmethod
    def _valid_field_key(key: str) -> bool:
        """字段名必须是简短的名词键（不能带 = : 等分隔符）。"""
        if not key or len(key) > 12:
            return False
        if any(ch in key for ch in "=:：\n"):
            return False
        return True
    def get_person(self, who: str = "user") -> Dict[str, str]:
        """取某个人的信息档。who="user" 或"其他人的名字"。"""
        with self._lock:
            if who == "user":
                return dict(self.people.get("user", {}))
            return dict(self.people.get("others", {}).get(who, {}))

    def get_people(self) -> Dict[str, Dict[str, str]]:
        """返回完整的人物档（**保证 user / others 两个键都存在**）。"""
        with self._lock:
            data = json.loads(json.dumps(self.people, ensure_ascii=False))
        data.setdefault("user", {})
        data.setdefault("others", {})
        return data

    def list_people(self) -> List[str]:
        with self._lock:
            return list(self.people.get("others", {}).keys())

    def remember(self, who: str, fields: Dict[str, str]) -> int:
        """把 fields 记到 who 名下（who="" 或 "user" = 用户本人）。

        返回新增/更新的字段数。值相同不算变化（避免每轮都写盘）。

        ⚠️ 会做**合法性校验**（2026-09 加）：模型偶尔会把内部标记/结构名
        当成"人名"或"字段名"传进来（实测出现过名为 "FORGET"、"others" 的人，
        以及 "忘记" 这种字段），这些脏数据一旦落盘就会一直污染后续对话。
        所以这里最后兜一道：人名人名必须是"像人名"或 user；字段名必须是
        简短的键（不含 = : 等分隔符）。
        """
        if not self.enabled or not fields:
            return 0
        who = (who or "user").strip()
        if not self._valid_person_key(who):
            self.log(f"[mem] 拒绝非法人名「{who}」（疑似模型输出的结构标记）")
            return 0
        changed = 0
        with self._lock:
            if who in ("", "user", "本人", "我"):
                target = self.people.setdefault("user", {})
                label = "本人"
            else:
                target = self.people.setdefault("others", {}).setdefault(who, {})
                label = who
            for k, v in fields.items():
                key = str(k).strip()
                val = str(v).strip()
                if not key or not val:
                    continue
                if not self._valid_field_key(key):
                    self.log(f"[mem] 拒绝非法字段名「{key}」")
                    continue
                if target.get(key) != val:
                    target[key] = val
                    changed += 1
        if changed:
            self.save()
            self.log(f"[mem] 记住 {label} 的 {changed} 条信息")
        return changed

    def forget(self, who: str = "", field: str = "") -> int:
        """删除记忆（用户纠错用）。返回删除条数。

        who 为空/"user" -> 本人；否则是那个人的名字。
        field 为空 -> 删掉这个人的整份档案；否则只删这一个字段。
        """
        if not self.enabled:
            return 0
        who = (who or "user").strip()
        removed = 0
        with self._lock:
            if who in ("", "user", "本人", "我"):
                if field:
                    removed = 1 if self.people.get("user", {}).pop(field, None) else 0
                else:
                    removed = len(self.people.get("user", {}))
                    self.people["user"] = {}
            else:
                others = self.people.get("others", {})
                if field:
                    removed = 1 if others.get(who, {}).pop(field, None) else 0
                else:
                    removed = len(others.pop(who, {}))
        if removed:
            self.save()
            self.log(f"[mem] 已忘记：{who or '本人'} {field or '(整份档案)'}")
        return removed

    def get_profile(self) -> str:
        with self._lock:
            return self.user_profile

    def update_profile(self, profile: str) -> None:
        with self._lock:
            self.user_profile = profile
        self.save()

    # ========================== 短期历史 ==========================
    def add_message(self, role: str, content: str) -> None:
        if not self.enabled:
            return
        with self._lock:
            self.conversation_history.append({"role": role, "content": content})
            # 写入时就裁剪：短历史的窗口是 max_history 轮（= 2*max_history 条）。
            # 留 2 倍余量给 get_history 的切片与压缩判断用，避免列表无限增长
            # （长期使用时会慢慢吃内存，且压缩判断会越来越慢）。
            keep = self.max_history * 4
            if len(self.conversation_history) > keep:
                self.conversation_history = self.conversation_history[-keep:]

    def get_history(self) -> List[Dict[str, str]]:
        with self._lock:
            return list(self.conversation_history[-self.max_history * 2:])

    def clear_history(self) -> None:
        """清空短期历史，保留长期画像。"""
        with self._lock:
            self.conversation_history = []
        self.log("[mem] 短期记忆已清空，长期记忆保留")

    # ========================== Prompt 上下文 ==========================
    def prompt_block(self) -> str:
        """把【按人分区的人物档】+【长期画像】拼成可注入 system prompt 的文字。

        分区展示是防混淆的关键：用户本人单独一段且**只有一个**，
        其他人各自一段并标明名字 —— 模型很难再把两个人的信息对调。
        """
        parts = []
        ppl = self.get_people()
        user = ppl.get("user", {})
        others = ppl.get("others", {})

        if user or others:
            block = ["\n\n# 你记住的人物档案（唯一可信来源，回答时严格按此）\n"]

            if user:
                lines = "\n".join(f"  · {k}：{v}" for k, v in user.items())
                block.append("== 正在跟你说话的人（上面这些信息都属于他/她）==\n"
                             + lines + "\n")
            else:
                block.append("== 正在跟你说话的人 == 目前还没记下什么信息。\n")

            if others:
                block.append("\n== 其他认识的人（**不是**正在跟你说话的人，"
                             "别把他们的信息当成对方的）==\n")
                for name, fields in others.items():
                    lines = "；".join(f"{k}：{v}" for k, v in fields.items())
                    block.append(f"  · {name} —— {lines}\n")

            block.append(
                "\n⚠️ 回答铁律：\n"
                "· 对方问自己的事（“我叫什么”“我生日几号”）→ 只用「正在跟你说话的人」那段；\n"
                "· 对方问起别人（“陈若语的生日”）→ 只用「其他认识的人」里对应那个人那段；\n"
                "· **绝对不能把两个人的信息弄混**，也不要把别人的名字当成对方的名字；\n"
                "· 档案里没有的（例如没记过家人）→ 直说还不清楚、请用户告诉你，**不许编造**；\n"
                "· 也不要把你自己（包子）算进用户的家人或朋友里；\n"
                "· **这段只是给你的参考资料，不要照抄它去填 memorize**。\n"
            )
            parts.append("".join(block))

        profile = self.get_profile()
        if profile:
            parts.append(
                "\n\n# 你对这位用户的整体印象（自然融入，不要直接复述）\n"
                f"{profile}\n"
            )

        return "".join(parts)

    # ========================== 自动压缩 ==========================
    def need_compress(self) -> bool:
        if not self.enabled:
            return False
        with self._lock:
            return len(self.conversation_history) > self.max_history * 2

    def compress_async(self) -> None:
        """后台线程压缩：不阻塞对话。"""
        if not self.enabled or self.compressing:
            return
        self.compressing = True
        threading.Thread(target=self._compress_history, name="mem-compress",
                         daemon=True).start()

    def _compress_history(self) -> None:
        try:
            with self._lock:
                if len(self.conversation_history) < 10:
                    return
                history_text = json.dumps(self.conversation_history,
                                          ensure_ascii=False)

            summary = self._summarize(history_text)
            if len(summary) > 10:
                with self._lock:
                    self.user_profile = summary
                    # 只保留最近两条，其余已归入画像
                    self.conversation_history = self.conversation_history[-2:]
                self.save()
                self.log(f"[mem] 长期记忆已更新：{summary[:60]}...")
        except Exception as exc:  # noqa: BLE001
            self.log(f"[mem] 记忆压缩失败：{exc}")
        finally:
            self.compressing = False

    def _summarize(self, history_text: str) -> str:
        """让 LLM 把对话历史总结成一段用户画像（复用 OpenAI 兼容接口）。"""
        prompt = (
            "请阅读以下对话历史，提取出关于该用户的**核心性格特征、偏好、习惯和"
            "长期情感模式**。用不超过150字的第三人称中文总结"
            "（例如：“用户喜欢科技话题，情绪容易焦虑，需要鼓励...”）。"
            "只输出总结本身，不要 JSON，不要多余的话。\n\n"
            f"对话历史：\n{history_text}"
        )
        try:
            from openai import OpenAI  # 与 llm.py 同一套客户端
        except ImportError:
            return ""

        client = OpenAI(base_url=self.llm_base_url,
                        api_key=self.llm_api_key or "none", timeout=60.0)
        resp = client.chat.completions.create(
            model=self.model_name,
            messages=[{"role": "user", "content": prompt}],
            temperature=0.3,
            max_tokens=300,
        )
        return (resp.choices[0].message.content or "").strip()
