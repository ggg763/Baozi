# -*- coding: utf-8 -*-
"""
gen_function_map.py - 生成《包子项目函数地图》docs/function_map.md

原理：
  - Python：用 ast 提取函数定义（名称/参数/docstring）与函数体内的调用；
  - C：用正则提取函数定义（返回类型/函数名/参数）与调用，并读取头文件声明注释；
  - 跨模块调用通过"全局函数名集合"匹配（含 self.xxx / 类方法调用）。

用法（项目根目录）：
    python tools/gen_function_map.py
"""

import os
import re
import ast
import sys
import io
import datetime

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ------------------------------------------------------------------
# 1. Python 解析
# ------------------------------------------------------------------

def py_funcs(path):
    """返回 [{name, args, doc, body_calls, defined_in, class_name}]"""
    with open(path, "r", encoding="utf-8") as f:
        tree = ast.parse(f.read())

    funcs = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            args = [a.arg for a in node.args.args]
            if node.args.vararg:
                args.append("*" + node.args.vararg.arg)
            if node.args.kwarg:
                args.append("**" + node.args.kwarg.arg)
            doc = ast.get_docstring(node)
            doc1 = (doc or "").strip().split("\n")[0][:80]

            calls = set()
            for sub in ast.walk(node):
                if isinstance(sub, ast.Call):
                    f = sub.func
                    if isinstance(f, ast.Name):
                        calls.add(f.id)
                    elif isinstance(f, ast.Attribute):
                        # self.xxx / obj.xxx -> 取属性名（xxx）
                        calls.add(f.attr)
            funcs.append({
                "name": node.name,
                "args": args,
                "doc": doc1,
                "calls": calls,
                "defined_in": os.path.basename(path),
            })
    return funcs

# ------------------------------------------------------------------
# 2. C 解析
# ------------------------------------------------------------------

C_KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "int", "void",
              "uint8_t", "uint16_t", "uint32_t", "char", "bool", "float", "double",
              "static", "const", "typedef", "struct", "enum", "extern"}

C_FUNC_RE = re.compile(
    r'(?m)^\s*(?:static\s+)?(?:const\s+)?'
    r'(?P<ret>[A-Za-z_][\w\s\*]*?)\s+'
    r'(?P<name>[A-Za-z_]\w*)\s*'
    r'\((?P<args>[^)]*)\)\s*\{'
)

def c_funcs(path):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    funcs = []
    for m in C_FUNC_RE.finditer(content):
        name = m.group("name")
        ret = m.group("ret").strip()
        args = m.group("args").strip()
        if name in C_KEYWORDS or name == "main" and "main" not in content:
            continue

        # 函数体（粗略取到下一个同级 { 结束 —— 用花括号计数）
        start = m.end()
        depth = 1
        i = start
        while i < len(content) and depth > 0:
            if content[i] == "{":
                depth += 1
            elif content[i] == "}":
                depth -= 1
            i += 1
        body = content[start:i]

        calls = set()
        for cm in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', body):
            cname = cm.group(1)
            if cname not in C_KEYWORDS and cname != name:
                calls.add(cname)

        funcs.append({
            "name": name,
            "args": args,
            "ret": ret,
            "doc": "",
            "calls": calls,
            "defined_in": os.path.basename(path),
        })
    return funcs

def c_doc_map(inc_dir):
    """从头文件提取函数声明上方的注释 -> {函数名: 首行说明}"""
    docmap = {}
    for fn in os.listdir(inc_dir):
        if not fn.endswith(".h"):
            continue
        with open(os.path.join(inc_dir, fn), "r", encoding="utf-8") as f:
            content = f.read()
        # 匹配 注释块 + 函数声明
        for m in re.finditer(
                r'/\*\*(.*?)\*/\s*(?:extern\s+)?'
                r'(?:[A-Za-z_][\w\s\*]*?)\s*([A-Za-z_]\w*)\s*\(', content, re.S):
            comment = re.sub(r'[\s\*]+', ' ', m.group(1)).strip()
            comment = re.sub(r'\s+', ' ', comment)
            comment = re.split(r'[。@]', comment)[0]  # 只保留第一句
            name = m.group(2)
            docmap.setdefault(name, comment[:90])
    return docmap

# ------------------------------------------------------------------
# 3. 汇总
# ------------------------------------------------------------------

def collect():
    """返回 {module_label: {"lang": "py"/"c", "funcs": [...], "doc": 模块说明}}"""
    modules = []

    # Python 模块
    py_files = [
        ("main.py", "主控入口", "音频管线 + 状态机 + TD1601 通信"),
        ("baozis_ear.py", "耳朵（ASR）", "SenseVoiceSmall 语音识别"),
        ("baozis_mouth.py", "嘴巴（TTS）", "edge-tts 语音合成"),
        ("communication.py", "TD1601 通信层", "PC→TD1601 语义状态下发"),
        ("brain/brain.py", "大脑编排", "BaoZiBrain 对话流水线"),
        ("brain/json_parser.py", "JSON 解析", "LLM 输出三级解析"),
        ("brain/ollama_client.py", "Ollama 客户端", "LLM 网络请求"),
        ("memory/memory_manager.py", "记忆管理", "短期历史 + 长期画像 + 压缩"),
        ("emotion/emotion_manager.py", "情绪管理", "情绪归一化 + 内部状态"),
        ("emotion/state_machine.py", "情绪状态机", "IDLE/ACTIVE/BORED/SLEEPING"),
        ("robot/command_mapper.py", "命令映射", "LLM 命令 → 标准动作"),
        ("robot/hardware_interface.py", "硬件抽象", "RobotHardware 契约 + FakeTD1601"),
        ("robot/visual_robot.py", "虚拟机器人", "Tkinter 可视化硬件"),
    ]
    for rel, title, desc in py_files:
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            modules.append({"label": rel, "title": title, "desc": desc,
                            "lang": "py", "funcs": py_funcs(p)})

    # C 模块
    c_files = [
        ("src/main.c", "TD1601 主程序", "主循环串联 UART/FSM/行为/传感器"),
        ("src/emotion_state.c", "情绪 FSM", "情绪状态与空闲降级"),
        ("src/behavior.c", "行为规划层", "情绪 → {表情,LED,动作} 映射"),
        ("src/motion.c", "动作组合层", "高层动作（点头/摇摆/睡姿）"),
        ("src/servo_control.c", "舵机控制", "角度限幅与 PWM 输出"),
        ("src/screen_display.c", "LCD 表情", "情绪 → 屏幕动画"),
        ("src/led_control.c", "LED 控制", "颜色/亮度"),
        ("src/sensor_manager.c", "传感器管理", "触摸/红外 → FSM"),
        ("src/uart_comm.c", "UART 通信", "协议解析（轻量 + JSON）"),
        ("hal/hal_pwm.c", "HAL-PWM", "舵机脉宽输出（占位）"),
        ("hal/hal_lcd.c", "HAL-LCD", "屏幕显示（占位）"),
        ("hal/hal_led.c", "HAL-LED", "RGB 输出（占位）"),
        ("hal/hal_gpio.c", "HAL-GPIO", "触摸/红外/延时（占位）"),
        ("hal/hal_uart.c", "HAL-UART", "串口收发（占位）"),
    ]
    docmap = c_doc_map(os.path.join(ROOT, "td1601_code", "inc"))
    for rel, title, desc in c_files:
        p = os.path.join(ROOT, "td1601_code", rel)
        if os.path.exists(p):
            funcs = c_funcs(p)
            for f in funcs:
                f["doc"] = docmap.get(f["name"], "")
            modules.append({"label": rel, "title": title, "desc": desc,
                            "lang": "c", "funcs": funcs})
    return modules

# ------------------------------------------------------------------
# 4. 生成 markdown
# ------------------------------------------------------------------

def clean_doc(text):
    """清洗功能描述，避免破坏 Notion 渲染：-> 换成箭头，去掉竖线/尖括号"""
    if not text:
        return text
    text = text.replace("->", "→")
    text = text.replace("|", "／")
    text = text.replace("<", "＜").replace(">", "＞")
    return text

def build_md(modules):
    # 全局函数名 -> (模块, 函数) 索引，用于被调用统计
    index = {}
    for mod in modules:
        for f in mod["funcs"]:
            index[f["name"]] = (mod["label"], f)

    L = []
    L.append(f"""# 包子（BaoZi）项目函数地图

> 自动生成：`tools/gen_function_map.py`，生成时间 {datetime.datetime.now().strftime("%Y-%m-%d %H:%M")}
> 用途：快速查询每个函数的基础功能与调用关系。
> 覆盖：PC 端 Python（13 个模块）+ TD1601 端 C（14 个模块，不含 tests/）。
>
> **Notion 兼容说明**：本文档全部使用列表格式（Notion 对 markdown 表格支持不稳定，列表可 100% 正常显示）。

---

## 一、模块总览

""")
    for mod in modules:
        L.append(f"- **`{mod['label']}`**（{mod['lang']}）— {mod['title']}：{clean_doc(mod['desc'])}")

    L.append("""
---

## 二、逐函数详解

""")

    for mod in modules:
        L.append(f"## {mod['label']} — {mod['title']}\n\n*{clean_doc(mod['desc'])}*\n")

        # 函数总表（列表格式，Notion 友好）
        L.append("**函数清单**（共 %d 个）：\n" % len(mod["funcs"]))
        for f in sorted(mod["funcs"], key=lambda x: x["name"]):
            caller_list = []
            for nm, (ml, mf) in index.items():
                if f["name"] in mf["calls"] and not (ml == mod["label"] and mf["name"] == f["name"]):
                    caller_list.append(mf["name"])
            doc = clean_doc(f["doc"]) or "—"
            callers = "被 `" + "`、`".join(sorted(set(caller_list))) + "` 调用" if caller_list else "入口/回调"
            L.append(f"- `{f['name']}()` — {doc}（{callers}）")

        L.append("\n---\n")

        # 逐函数详情
        for f in sorted(mod["funcs"], key=lambda x: x["name"]):
            if mod["lang"] == "py":
                sig = f"{f['name']}({', '.join(f['args'])})"
            else:
                sig = f"{f['ret']} {f['name']}({f['args']})"
            # 被谁调用
            callers = [mf["name"] for nm, (ml, mf) in index.items()
                       if f["name"] in mf["calls"]]
            callers = [c for c in callers if c != f["name"]]
            # 调用了谁（只保留"存在于项目中的函数"）
            internal_calls = sorted(
                nm for nm in f["calls"] if nm in index and nm != f["name"])
            doc = clean_doc(f["doc"]) or "（无注释）"
            L.append(f"""
### `{f['name']}()` — {doc}

- **签名**：`{sig}`
- **功能**：{doc}
- **调用**：{', '.join(f'`{c}()`' for c in internal_calls) if internal_calls else '—'}
- **被调用**：{', '.join(f'`{c}()`' for c in sorted(set(callers))) if callers else '—（入口/回调）'}
""")

    L.append("""
---

*重新生成：在项目根目录运行 `python tools/gen_function_map.py`。*
""")
    return "\n".join(L)

if __name__ == "__main__":
    modules = collect()
    md = build_md(modules)
    out = os.path.join(ROOT, "docs", "function_map.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(md)
    total = sum(len(m["funcs"]) for m in modules)
    print(f"OK 已生成: {out} | 模块 {len(modules)} 个 | 函数 {total} 个")
