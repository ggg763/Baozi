"""算术兜底：在代码层精确计算，不依赖大模型算数。

为什么需要：
    qwen2.5:7b 这类 7B 模型做**多位数乘法**很不可靠（三位数乘三位数经常算错），
    而简单加减、甚至不定积分反而能对 —— 这是模型能力边界，不是 prompt 能修的。
    所以在 PC 端识别用户话里的算式、用 Python 精确算好，再把结果作为**上下文**
    交给模型，让它在回复里自然说出答案（而不是自己硬算）。

支持：
    "123乘456"、"123×456"、"123*456"、"12加上34"、"100除以8"、"12-5"、
    "123乘以456等于多少" —— 即"数字(运算符数字)+"的片段。
    括号、小数、多个运算符（按 Python 优先级）也支持。

安全：
    `safe_eval` 只放行 [0-9 + - * / ( ) . 空格]，且用空 __builtins__ 求值，
    不含任何名字/函数调用，因此不存在代码注入风险。

不做的（故意保守，避免误判）：
    · 把字母 x/X 当乘号（会和英文单词混淆）；
    · 数学函数（sin/sqrt/积分）—— 那些模型本身能算对，交给它就好；
    · 纯数字（"我有3个苹果"这种没有运算符，不会触发）。
"""

from __future__ import annotations

import re
from typing import Optional

# 中文/全角运算符 -> Python 运算符（长词在前，避免"加上"被"加"截断）
_OP_MAP = (
    ("加上", "+"), ("加", "+"),
    ("减去", "-"), ("减", "-"),
    ("乘以", "*"), ("乘", "*"),
    ("除以", "/"), ("除", "/"),
    ("×", "*"), ("·", "*"),
    ("÷", "/"),
)

# 形如： 123 * 456        （数字 (运算符 数字)+，允许括号/小数/空格）
# `\(?\s*` 前缀让 "(12+8)*5" 这类**以括号开头**的算式也能整体匹配，
# 否则正则会从 "12" 开始匹配，切出 "12+8)*5" 这种残句，safe_eval 直接判非法。
_EXPR_RE = re.compile(
    r"\(?\s*\d+(?:\.\d+)?(?:\s*[+\-*/]\s*\(?\s*\d+(?:\.\d+)?\s*\)?)+")
# safe_eval 的放行字符集
_SAFE_RE = re.compile(r"[0-9+\-*/().\s]+")


def extract_expression(text: str) -> str:
    """把用户话里的算式归一化成 Python 表达式；找不到返回 ""。

    >>> extract_expression("123乘456等于多少")
    '123*456'
    >>> extract_expression("今天天气不错")
    ''
    """
    if not text:
        return ""
    s = text
    for cn, sym in _OP_MAP:
        s = s.replace(cn, sym)
    m = _EXPR_RE.search(s)
    return m.group(0).strip() if m else ""


def safe_eval(expr: str) -> Optional[float]:
    """只允许四则运算与括号的求值；非法或出错返回 None。"""
    if not expr or not _SAFE_RE.fullmatch(expr):
        return None
    try:
        val = eval(expr, {"__builtins__": {}}, {})  # noqa: S307 —— 已用字符白名单锁死
    except Exception:  # noqa: BLE001
        return None
    if isinstance(val, (int, float)):
        return val
    return None


def _fmt(val: float) -> str:
    """整数就不带小数点；否则保留合理位数。"""
    if float(val).is_integer():
        return str(int(val))
    return f"{val:.6g}"


def calc_hint(text: str) -> str:
    """返回给模型的"计算器结果"上下文；不需要计算时返回 ""。

    例：calc_hint("123乘456等于多少") -> "123*456=56088"
    """
    expr = extract_expression(text)
    if not expr:
        return ""
    val = safe_eval(expr)
    if val is None:
        return ""
    return f"{expr}={_fmt(val)}"
