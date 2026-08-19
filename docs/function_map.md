# 包子（BaoZi）项目函数地图

> 自动生成：`tools/gen_function_map.py`，生成时间 2026-08-19 11:37
> 用途：快速查询每个函数的基础功能与调用关系。
> 覆盖：PC 端 Python（13 个模块）+ TD1601 端 C（14 个模块，不含 tests/）。
>
> **Notion 兼容说明**：本文档全部使用列表格式（Notion 对 markdown 表格支持不稳定，列表可 100% 正常显示）。

---

## 一、模块总览


- **`main.py`**（py）— 主控入口：音频管线 + 状态机 + TD1601 通信
- **`baozis_ear.py`**（py）— 耳朵（ASR）：SenseVoiceSmall 语音识别
- **`baozis_mouth.py`**（py）— 嘴巴（TTS）：edge-tts 语音合成
- **`communication.py`**（py）— TD1601 通信层：PC→TD1601 语义状态下发
- **`brain/brain.py`**（py）— 大脑编排：BaoZiBrain 对话流水线
- **`brain/json_parser.py`**（py）— JSON 解析：LLM 输出三级解析
- **`brain/ollama_client.py`**（py）— Ollama 客户端：LLM 网络请求
- **`memory/memory_manager.py`**（py）— 记忆管理：短期历史 + 长期画像 + 压缩
- **`emotion/emotion_manager.py`**（py）— 情绪管理：情绪归一化 + 内部状态
- **`emotion/state_machine.py`**（py）— 情绪状态机：IDLE/ACTIVE/BORED/SLEEPING
- **`robot/command_mapper.py`**（py）— 命令映射：LLM 命令 → 标准动作
- **`robot/hardware_interface.py`**（py）— 硬件抽象：RobotHardware 契约 + FakeTD1601
- **`robot/visual_robot.py`**（py）— 虚拟机器人：Tkinter 可视化硬件
- **`src/main.c`**（c）— TD1601 主程序：主循环串联 UART/FSM/行为/传感器
- **`src/emotion_state.c`**（c）— 情绪 FSM：情绪状态与空闲降级
- **`src/behavior.c`**（c）— 行为规划层：情绪 → {表情,LED,动作} 映射
- **`src/motion.c`**（c）— 动作组合层：高层动作（点头/摇摆/睡姿）
- **`src/servo_control.c`**（c）— 舵机控制：角度限幅与 PWM 输出
- **`src/screen_display.c`**（c）— LCD 表情：情绪 → 屏幕动画
- **`src/led_control.c`**（c）— LED 控制：颜色/亮度
- **`src/sensor_manager.c`**（c）— 传感器管理：触摸/红外 → FSM
- **`src/uart_comm.c`**（c）— UART 通信：协议解析（轻量 + JSON）
- **`hal/hal_pwm.c`**（c）— HAL-PWM：舵机脉宽输出（占位）
- **`hal/hal_lcd.c`**（c）— HAL-LCD：屏幕显示（占位）
- **`hal/hal_led.c`**（c）— HAL-LED：RGB 输出（占位）
- **`hal/hal_gpio.c`**（c）— HAL-GPIO：触摸/红外/延时（占位）
- **`hal/hal_uart.c`**（c）— HAL-UART：串口收发（占位）

---

## 二、逐函数详解


## main.py — 主控入口

*音频管线 + 状态机 + TD1601 通信*

**函数清单**（共 4 个）：

- `_create_hardware()` — 创建硬件接口：（入口/回调）
- `_load_system_prompt()` — 读取包子人设提示词（prompts/system_prompt.txt）（入口/回调）
- `process_audio_file()` — 处理单个音频文件，返回完整交互结果。（入口/回调）
- `warmup()` — 预热三个核心模块，避免首次调用卡顿。（入口/回调）

---


### `_create_hardware()` — 创建硬件接口：

- **签名**：`_create_hardware(gui)`
- **功能**：创建硬件接口：
- **调用**：—
- **被调用**：—（入口/回调）


### `_load_system_prompt()` — 读取包子人设提示词（prompts/system_prompt.txt）

- **签名**：`_load_system_prompt()`
- **功能**：读取包子人设提示词（prompts/system_prompt.txt）
- **调用**：—
- **被调用**：—（入口/回调）


### `process_audio_file()` — 处理单个音频文件，返回完整交互结果。

- **签名**：`process_audio_file(audio_path, clear_context, state_machine, comm)`
- **功能**：处理单个音频文件，返回完整交互结果。
- **调用**：`baozis_ear()`, `baozis_mouth()`, `chat()`, `clear_history()`, `send_emotion()`, `update_interaction()`
- **被调用**：—（入口/回调）


### `warmup()` — 预热三个核心模块，避免首次调用卡顿。

- **签名**：`warmup()`
- **功能**：预热三个核心模块，避免首次调用卡顿。
- **调用**：`baozis_ear()`, `baozis_mouth()`, `chat()`, `clear_history()`
- **被调用**：—（入口/回调）

## baozis_ear.py — 耳朵（ASR）

*SenseVoiceSmall 语音识别*

**函数清单**（共 2 个）：

- `_get_model()` — 懒加载模型：首次调用时加载，后续复用（被 `baozis_ear` 调用）
- `baozis_ear()` — 包子的耳朵 - 语音识别（被 `process_audio_file`、`warmup` 调用）

---


### `_get_model()` — 懒加载模型：首次调用时加载，后续复用

- **签名**：`_get_model()`
- **功能**：懒加载模型：首次调用时加载，后续复用
- **调用**：—
- **被调用**：`baozis_ear()`


### `baozis_ear()` — 包子的耳朵 - 语音识别

- **签名**：`baozis_ear(audio_path, language, use_itn)`
- **功能**：包子的耳朵 - 语音识别
- **调用**：`_get_model()`
- **被调用**：`process_audio_file()`, `warmup()`

## baozis_mouth.py — 嘴巴（TTS）

*edge-tts 语音合成*

**函数清单**（共 2 个）：

- `_tts_async()` — 异步生成 TTS 音频，返回输出文件路径（被 `baozis_mouth` 调用）
- `baozis_mouth()` — 同步入口：生成语音文件，返回文件路径。（被 `process_audio_file`、`warmup` 调用）

---


### `_tts_async()` — 异步生成 TTS 音频，返回输出文件路径

- **签名**：`_tts_async(text, voice, rate, pitch)`
- **功能**：异步生成 TTS 音频，返回输出文件路径
- **调用**：`save()`
- **被调用**：`baozis_mouth()`


### `baozis_mouth()` — 同步入口：生成语音文件，返回文件路径。

- **签名**：`baozis_mouth(text, emotion, voice)`
- **功能**：同步入口：生成语音文件，返回文件路径。
- **调用**：`_tts_async()`, `close()`
- **被调用**：`process_audio_file()`, `warmup()`

## communication.py — TD1601 通信层

*PC→TD1601 语义状态下发*

**函数清单**（共 8 个）：

- `__init__()` — Args:（入口/回调）
- `_connect()` — —（入口/回调）
- `_send_line()` — —（被 `send_emotion`、`send_intent`、`send_line` 调用）
- `close()` — —（被 `baozis_mouth` 调用）
- `mode()` — —（入口/回调）
- `send_emotion()` — 下发情绪状态（TD1601 据此自主决策行为）。（被 `process_audio_file` 调用）
- `send_intent()` — 下发意图（预留，TD1601 当前优先响应 emotion）。（入口/回调）
- `send_line()` — 直接发送一行原始消息（调试用）。（入口/回调）

---


### `__init__()` — Args:

- **签名**：`__init__(self, port, baudrate, use_json)`
- **功能**：Args:
- **调用**：`_connect()`
- **被调用**：—（入口/回调）


### `_connect()` — （无注释）

- **签名**：`_connect(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `_send_line()` — （无注释）

- **签名**：`_send_line(self, line)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`send_emotion()`, `send_intent()`, `send_line()`


### `close()` — （无注释）

- **签名**：`close(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`baozis_mouth()`


### `mode()` — （无注释）

- **签名**：`mode(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `send_emotion()` — 下发情绪状态（TD1601 据此自主决策行为）。

- **签名**：`send_emotion(self, emotion)`
- **功能**：下发情绪状态（TD1601 据此自主决策行为）。
- **调用**：`_send_line()`
- **被调用**：`process_audio_file()`


### `send_intent()` — 下发意图（预留，TD1601 当前优先响应 emotion）。

- **签名**：`send_intent(self, intent)`
- **功能**：下发意图（预留，TD1601 当前优先响应 emotion）。
- **调用**：`_send_line()`
- **被调用**：—（入口/回调）


### `send_line()` — 直接发送一行原始消息（调试用）。

- **签名**：`send_line(self, line)`
- **功能**：直接发送一行原始消息（调试用）。
- **调用**：`_send_line()`
- **被调用**：—（入口/回调）

## brain/brain.py — 大脑编排

*BaoZiBrain 对话流水线*

**函数清单**（共 4 个）：

- `__init__()` — —（入口/回调）
- `_build_messages()` — —（入口/回调）
- `chat()` — 包子聊天入口（被 `process_audio_file`、`test`、`warmup` 调用）
- `clear_history()` — 清空短期聊天（被 `process_audio_file`、`warmup` 调用）

---


### `__init__()` — （无注释）

- **签名**：`__init__(self, system_prompt)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `_build_messages()` — （无注释）

- **签名**：`_build_messages(self, user_input)`
- **功能**：（无注释）
- **调用**：`get_history()`, `get_profile()`
- **被调用**：—（入口/回调）


### `chat()` — 包子聊天入口

- **签名**：`chat(self, user_input)`
- **功能**：包子聊天入口
- **调用**：`_build_messages()`, `add_message()`, `compress_async()`, `get_emotion()`, `map()`, `need_compress()`, `parse()`, `update()`
- **被调用**：`process_audio_file()`, `test()`, `warmup()`


### `clear_history()` — 清空短期聊天

- **签名**：`clear_history(self)`
- **功能**：清空短期聊天
- **调用**：—
- **被调用**：`process_audio_file()`, `warmup()`

## brain/json_parser.py — JSON 解析

*LLM 输出三级解析*

**函数清单**（共 6 个）：

- `__init__()` — —（入口/回调）
- `_extract_json()` — 从混杂文本中寻找JSON对象（被 `parse` 调用）
- `_fallback()` — —（被 `parse` 调用）
- `_normalize()` — 保证输出字段稳定（被 `parse` 调用）
- `_try_json()` — —（被 `parse` 调用）
- `parse()` — 解析LLM输出（入口/回调）

---


### `__init__()` — （无注释）

- **签名**：`__init__(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `_extract_json()` — 从混杂文本中寻找JSON对象

- **签名**：`_extract_json(self, text)`
- **功能**：从混杂文本中寻找JSON对象
- **调用**：—
- **被调用**：`parse()`


### `_fallback()` — （无注释）

- **签名**：`_fallback(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`parse()`


### `_normalize()` — 保证输出字段稳定

- **签名**：`_normalize(self, data)`
- **功能**：保证输出字段稳定
- **调用**：—
- **被调用**：`parse()`


### `_try_json()` — （无注释）

- **签名**：`_try_json(self, text)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`parse()`


### `parse()` — 解析LLM输出

- **签名**：`parse(self, raw)`
- **功能**：解析LLM输出
- **调用**：`_extract_json()`, `_fallback()`, `_normalize()`, `_try_json()`
- **被调用**：—（入口/回调）

## brain/ollama_client.py — Ollama 客户端

*LLM 网络请求*

**函数清单**（共 3 个）：

- `__init__()` — 初始化 Ollama 客户端（入口/回调）
- `chat()` — 调用 Ollama（被 `process_audio_file`、`test`、`warmup` 调用）
- `test()` — 简单测试 Ollama 是否可用（入口/回调）

---


### `__init__()` — 初始化 Ollama 客户端

- **签名**：`__init__(self, url, model, temperature, top_p, timeout)`
- **功能**：初始化 Ollama 客户端
- **调用**：—
- **被调用**：—（入口/回调）


### `chat()` — 调用 Ollama

- **签名**：`chat(self, messages)`
- **功能**：调用 Ollama
- **调用**：—
- **被调用**：`process_audio_file()`, `test()`, `warmup()`


### `test()` — 简单测试 Ollama 是否可用

- **签名**：`test(self)`
- **功能**：简单测试 Ollama 是否可用
- **调用**：`chat()`
- **被调用**：—（入口/回调）

## memory/memory_manager.py — 记忆管理

*短期历史 + 长期画像 + 压缩*

**函数清单**（共 12 个）：

- `__init__()` — —（入口/回调）
- `_compress_history()` — —（入口/回调）
- `add_message()` — —（入口/回调）
- `clear_history()` — 清空短期记忆（被 `process_audio_file`、`warmup` 调用）
- `compress_async()` — 后台启动压缩（入口/回调）
- `get_context()` — 提供给Brain的上下文（入口/回调）
- `get_history()` — —（被 `_build_messages`、`get_context` 调用）
- `get_profile()` — —（被 `_build_messages` 调用）
- `load()` — 从文件加载长期记忆（入口/回调）
- `need_compress()` — —（入口/回调）
- `save()` — 保存长期记忆（被 `_compress_history`、`_tts_async`、`update_profile` 调用）
- `update_profile()` — —（入口/回调）

---


### `__init__()` — （无注释）

- **签名**：`__init__(self, memory_file, max_history, ollama_url, model_name)`
- **功能**：（无注释）
- **调用**：`load()`
- **被调用**：—（入口/回调）


### `_compress_history()` — （无注释）

- **签名**：`_compress_history(self)`
- **功能**：（无注释）
- **调用**：`save()`
- **被调用**：—（入口/回调）


### `add_message()` — （无注释）

- **签名**：`add_message(self, role, content)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `clear_history()` — 清空短期记忆

- **签名**：`clear_history(self)`
- **功能**：清空短期记忆
- **调用**：—
- **被调用**：`process_audio_file()`, `warmup()`


### `compress_async()` — 后台启动压缩

- **签名**：`compress_async(self)`
- **功能**：后台启动压缩
- **调用**：—
- **被调用**：—（入口/回调）


### `get_context()` — 提供给Brain的上下文

- **签名**：`get_context(self)`
- **功能**：提供给Brain的上下文
- **调用**：`get_history()`
- **被调用**：—（入口/回调）


### `get_history()` — （无注释）

- **签名**：`get_history(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_build_messages()`, `get_context()`


### `get_profile()` — （无注释）

- **签名**：`get_profile(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_build_messages()`


### `load()` — 从文件加载长期记忆

- **签名**：`load(self)`
- **功能**：从文件加载长期记忆
- **调用**：—
- **被调用**：—（入口/回调）


### `need_compress()` — （无注释）

- **签名**：`need_compress(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `save()` — 保存长期记忆

- **签名**：`save(self)`
- **功能**：保存长期记忆
- **调用**：—
- **被调用**：`_compress_history()`, `_tts_async()`, `update_profile()`


### `update_profile()` — （无注释）

- **签名**：`update_profile(self, profile)`
- **功能**：（无注释）
- **调用**：`save()`
- **被调用**：—（入口/回调）

## emotion/emotion_manager.py — 情绪管理

*情绪归一化 + 内部状态*

**函数清单**（共 7 个）：

- `__init__()` — —（入口/回调）
- `_update_state()` — 将离散情绪转换为内部状态（被 `update` 调用）
- `get_emotion()` — 返回当前情绪标签（入口/回调）
- `get_state()` — 返回内部状态（入口/回调）
- `normalize()` — Return a TTS-compatible emotion label for a raw LLM value.（被 `update` 调用）
- `reset()` — 恢复默认状态（入口/回调）
- `update()` — 更新情绪（被 `_draw_face` 调用）

---


### `__init__()` — （无注释）

- **签名**：`__init__(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `_update_state()` — 将离散情绪转换为内部状态

- **签名**：`_update_state(self, emotion)`
- **功能**：将离散情绪转换为内部状态
- **调用**：—
- **被调用**：`update()`


### `get_emotion()` — 返回当前情绪标签

- **签名**：`get_emotion(self)`
- **功能**：返回当前情绪标签
- **调用**：—
- **被调用**：—（入口/回调）


### `get_state()` — 返回内部状态

- **签名**：`get_state(self)`
- **功能**：返回内部状态
- **调用**：—
- **被调用**：—（入口/回调）


### `normalize()` — Return a TTS-compatible emotion label for a raw LLM value.

- **签名**：`normalize(self, emotion)`
- **功能**：Return a TTS-compatible emotion label for a raw LLM value.
- **调用**：—
- **被调用**：`update()`


### `reset()` — 恢复默认状态

- **签名**：`reset(self)`
- **功能**：恢复默认状态
- **调用**：—
- **被调用**：—（入口/回调）


### `update()` — 更新情绪

- **签名**：`update(self, emotion)`
- **功能**：更新情绪
- **调用**：`_update_state()`, `normalize()`
- **被调用**：`_draw_face()`

## emotion/state_machine.py — 情绪状态机

*IDLE/ACTIVE/BORED/SLEEPING*

**函数清单**（共 5 个）：

- `__init__()` — 参数:（入口/回调）
- `_heartbeat_loop()` — 后台循环：每 5 秒检查一次，如果太久没理包子，触发无聊/睡觉。（入口/回调）
- `shutdown()` — 程序退出时调用（入口/回调）
- `update_interaction()` — 当用户和包子说话、LLM返回结果时，调用此方法。（被 `process_audio_file` 调用）
- `wake_up()` — 手动唤醒（例如触摸传感器触发时调用）（入口/回调）

---


### `__init__()` — 参数:

- **签名**：`__init__(self, hardware)`
- **功能**：参数:
- **调用**：`set_expression()`, `set_led()`
- **被调用**：—（入口/回调）


### `_heartbeat_loop()` — 后台循环：每 5 秒检查一次，如果太久没理包子，触发无聊/睡觉。

- **签名**：`_heartbeat_loop(self)`
- **功能**：后台循环：每 5 秒检查一次，如果太久没理包子，触发无聊/睡觉。
- **调用**：`set_expression()`, `set_led()`, `set_servo()`
- **被调用**：—（入口/回调）


### `shutdown()` — 程序退出时调用

- **签名**：`shutdown(self)`
- **功能**：程序退出时调用
- **调用**：—
- **被调用**：—（入口/回调）


### `update_interaction()` — 当用户和包子说话、LLM返回结果时，调用此方法。

- **签名**：`update_interaction(self, emotion)`
- **功能**：当用户和包子说话、LLM返回结果时，调用此方法。
- **调用**：`set_expression()`, `set_led()`, `set_servo()`
- **被调用**：`process_audio_file()`


### `wake_up()` — 手动唤醒（例如触摸传感器触发时调用）

- **签名**：`wake_up(self)`
- **功能**：手动唤醒（例如触摸传感器触发时调用）
- **调用**：`set_expression()`, `set_led()`, `set_servo()`
- **被调用**：—（入口/回调）

## robot/command_mapper.py — 命令映射

*LLM 命令 → 标准动作*

**函数清单**（共 4 个）：

- `__init__()` — —（入口/回调）
- `_number()` — 安全数字解析（被 `map` 调用）
- `map()` — 将LLM命令转换成标准格式（入口/回调）
- `none()` — 空动作（被 `map` 调用）

---


### `__init__()` — （无注释）

- **签名**：`__init__(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `_number()` — 安全数字解析

- **签名**：`_number(self, parts, default)`
- **功能**：安全数字解析
- **调用**：—
- **被调用**：`map()`


### `map()` — 将LLM命令转换成标准格式

- **签名**：`map(self, command)`
- **功能**：将LLM命令转换成标准格式
- **调用**：`_number()`, `none()`
- **被调用**：—（入口/回调）


### `none()` — 空动作

- **签名**：`none(self)`
- **功能**：空动作
- **调用**：—
- **被调用**：`map()`

## robot/hardware_interface.py — 硬件抽象

*RobotHardware 契约 + FakeTD1601*

**函数清单**（共 10 个）：

- `__init__()` — —（入口/回调）
- `reset_pose()` — 复位姿态（默认空实现，硬件可实现覆盖）（入口/回调）
- `reset_pose()` — —（入口/回调）
- `run()` — 阻塞式主循环（GUI 模式需要；模拟硬件默认为空）（入口/回调）
- `set_expression()` — 切换表情（happy/sad/angry/surprised/loving/neutral/bored/sleep）（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）
- `set_expression()` — —（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）
- `set_led()` — 切换 LED 颜色（red/green/blue/yellow/white/purple/dim）（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）
- `set_led()` — —（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）
- `set_servo()` — 控制舵机（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）
- `set_servo()` — —（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）

---


### `__init__()` — （无注释）

- **签名**：`__init__(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `reset_pose()` — 复位姿态（默认空实现，硬件可实现覆盖）

- **签名**：`reset_pose(self)`
- **功能**：复位姿态（默认空实现，硬件可实现覆盖）
- **调用**：—
- **被调用**：—（入口/回调）


### `reset_pose()` — （无注释）

- **签名**：`reset_pose(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `run()` — 阻塞式主循环（GUI 模式需要；模拟硬件默认为空）

- **签名**：`run(self)`
- **功能**：阻塞式主循环（GUI 模式需要；模拟硬件默认为空）
- **调用**：—
- **被调用**：—（入口/回调）


### `set_expression()` — 切换表情（happy/sad/angry/surprised/loving/neutral/bored/sleep）

- **签名**：`set_expression(self, emotion)`
- **功能**：切换表情（happy/sad/angry/surprised/loving/neutral/bored/sleep）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`


### `set_expression()` — （无注释）

- **签名**：`set_expression(self, emotion)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`


### `set_led()` — 切换 LED 颜色（red/green/blue/yellow/white/purple/dim）

- **签名**：`set_led(self, color)`
- **功能**：切换 LED 颜色（red/green/blue/yellow/white/purple/dim）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`


### `set_led()` — （无注释）

- **签名**：`set_led(self, color)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`


### `set_servo()` — 控制舵机

- **签名**：`set_servo(self, servo_id, angle, duration)`
- **功能**：控制舵机
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`


### `set_servo()` — （无注释）

- **签名**：`set_servo(self, servo_id, angle, duration)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`

## robot/visual_robot.py — 虚拟机器人

*Tkinter 可视化硬件*

**函数清单**（共 9 个）：

- `__init__()` — —（入口/回调）
- `_draw_base()` — —（被 `__init__` 调用）
- `_draw_face()` — —（被 `_update_loop` 调用）
- `_update_loop()` — 每帧刷新表情、舵机、LED（被 `__init__` 调用）
- `reset_pose()` — —（入口/回调）
- `run()` — 启动GUI主循环（会阻塞，建议放在主线程最后）（入口/回调）
- `set_expression()` — —（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）
- `set_led()` — —（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）
- `set_servo()` — —（被 `_heartbeat_loop`、`update_interaction`、`wake_up` 调用）

---


### `__init__()` — （无注释）

- **签名**：`__init__(self, window_size)`
- **功能**：（无注释）
- **调用**：`_draw_base()`, `_update_loop()`
- **被调用**：—（入口/回调）


### `_draw_base()` — （无注释）

- **签名**：`_draw_base(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`__init__()`


### `_draw_face()` — （无注释）

- **签名**：`_draw_face(self)`
- **功能**：（无注释）
- **调用**：`update()`
- **被调用**：`_update_loop()`


### `_update_loop()` — 每帧刷新表情、舵机、LED

- **签名**：`_update_loop(self)`
- **功能**：每帧刷新表情、舵机、LED
- **调用**：`_draw_face()`
- **被调用**：`__init__()`


### `reset_pose()` — （无注释）

- **签名**：`reset_pose(self)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `run()` — 启动GUI主循环（会阻塞，建议放在主线程最后）

- **签名**：`run(self)`
- **功能**：启动GUI主循环（会阻塞，建议放在主线程最后）
- **调用**：—
- **被调用**：—（入口/回调）


### `set_expression()` — （无注释）

- **签名**：`set_expression(self, emotion)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`


### `set_led()` — （无注释）

- **签名**：`set_led(self, color)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`


### `set_servo()` — （无注释）

- **签名**：`set_servo(self, servo_id, angle, duration)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`_heartbeat_loop()`, `update_interaction()`, `wake_up()`

## src/main.c — TD1601 主程序

*主循环串联 UART/FSM/行为/传感器*

**函数清单**（共 3 个）：

- `main()` — —（入口/回调）
- `sys_now_ms()` — —（被 `main`、`system_init` 调用）
- `system_init()` — —（被 `main` 调用）

---


### `main()` — （无注释）

- **签名**：`int main(void)`
- **功能**：（无注释）
- **调用**：`Behavior_Tick()`, `Emotion_Tick()`, `HAL_DelayMs()`, `Sensor_Scan()`, `UART_Process()`, `sys_now_ms()`, `system_init()`
- **被调用**：—（入口/回调）


### `sys_now_ms()` — （无注释）

- **签名**：`uint32_t sys_now_ms(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`main()`, `system_init()`


### `system_init()` — （无注释）

- **签名**：`void system_init(void)`
- **功能**：（无注释）
- **调用**：`Behavior_Init()`, `Emotion_Init()`, `Emotion_Touch()`, `Sensor_Init()`, `UART_Init()`, `UART_SendLine()`, `sys_now_ms()`
- **被调用**：`main()`

## src/emotion_state.c — 情绪 FSM

*情绪状态与空闲降级*

**函数清单**（共 7 个）：

- `Emotion_Get()` — 获取当前情绪（入口/回调）
- `Emotion_Init()` — 初始化情绪状态机（置为 EMO_IDLE，重置计时）（被 `system_init` 调用）
- `Emotion_Tick()` — 心跳节拍（周期性调用，建议 10ms~100ms 一次） 内部根据空闲时长自动降级 IDLE→BORED→SLEEPING（被 `main` 调用）
- `Emotion_Touch()` — 记录一次互动（重置空闲计时） 主循环在"收到 PC 消息 / 传感器事件"时调用 （被 `Emotion_Update`、`Emotion_WakeUp`、`Sensor_Scan`、`system_init` 调用）
- `Emotion_Update()` — 更新情绪（外部输入：PC 下发 / 传感器触发） （被 `Sensor_Scan`、`parse_json_emotion`、`parse_lightweight` 调用）
- `Emotion_WakeUp()` — 手动唤醒（如触摸唤醒）（入口/回调）
- `notify()` — —（被 `Emotion_Tick`、`Emotion_Update`、`Emotion_WakeUp` 调用）

---


### `Emotion_Get()` — 获取当前情绪

- **签名**：`EmotionState Emotion_Get(void)`
- **功能**：获取当前情绪
- **调用**：—
- **被调用**：—（入口/回调）


### `Emotion_Init()` — 初始化情绪状态机（置为 EMO_IDLE，重置计时）

- **签名**：`void Emotion_Init(EmotionChangeCb on_change)`
- **功能**：初始化情绪状态机（置为 EMO_IDLE，重置计时）
- **调用**：—
- **被调用**：`system_init()`


### `Emotion_Tick()` — 心跳节拍（周期性调用，建议 10ms~100ms 一次） 内部根据空闲时长自动降级 IDLE→BORED→SLEEPING

- **签名**：`void Emotion_Tick(uint32_t now_ms)`
- **功能**：心跳节拍（周期性调用，建议 10ms~100ms 一次） 内部根据空闲时长自动降级 IDLE→BORED→SLEEPING
- **调用**：`notify()`
- **被调用**：`main()`


### `Emotion_Touch()` — 记录一次互动（重置空闲计时） 主循环在"收到 PC 消息 / 传感器事件"时调用 

- **签名**：`void Emotion_Touch(uint32_t now_ms)`
- **功能**：记录一次互动（重置空闲计时） 主循环在"收到 PC 消息 / 传感器事件"时调用 
- **调用**：—
- **被调用**：`Emotion_Update()`, `Emotion_WakeUp()`, `Sensor_Scan()`, `system_init()`


### `Emotion_Update()` — 更新情绪（外部输入：PC 下发 / 传感器触发） 

- **签名**：`bool Emotion_Update(EmotionState emo)`
- **功能**：更新情绪（外部输入：PC 下发 / 传感器触发） 
- **调用**：`Emotion_Touch()`, `notify()`
- **被调用**：`Sensor_Scan()`, `parse_json_emotion()`, `parse_lightweight()`


### `Emotion_WakeUp()` — 手动唤醒（如触摸唤醒）

- **签名**：`void Emotion_WakeUp(void)`
- **功能**：手动唤醒（如触摸唤醒）
- **调用**：`Emotion_Touch()`, `notify()`
- **被调用**：—（入口/回调）


### `notify()` — （无注释）

- **签名**：`void notify(EmotionState new_state)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Emotion_Tick()`, `Emotion_Update()`, `Emotion_WakeUp()`

## src/behavior.c — 行为规划层

*情绪 → {表情,LED,动作} 映射*

**函数清单**（共 3 个）：

- `Behavior_Init()` — behavior.h - TD1601 行为规划层（Behavior Layer） 职责： 接收 Emotion FSM 的情绪变化， 决策"当前情绪该做什么"——表情、动作、灯光（被 `system_init` 调用）
- `Behavior_OnEmotionChanged()` — 情绪变化回调（由 Emotion_Init 传入 FSM）（入口/回调）
- `Behavior_Tick()` — 周期驱动（可选，用于动作的插值推进） （被 `main` 调用）

---


### `Behavior_Init()` — behavior.h - TD1601 行为规划层（Behavior Layer） 职责： 接收 Emotion FSM 的情绪变化， 决策"当前情绪该做什么"——表情、动作、灯光

- **签名**：`void Behavior_Init(void)`
- **功能**：behavior.h - TD1601 行为规划层（Behavior Layer） 职责： 接收 Emotion FSM 的情绪变化， 决策"当前情绪该做什么"——表情、动作、灯光
- **调用**：`LED_Init()`, `Motion_ResetPose()`, `Screen_Init()`, `Servo_Init()`
- **被调用**：`system_init()`


### `Behavior_OnEmotionChanged()` — 情绪变化回调（由 Emotion_Init 传入 FSM）

- **签名**：`void Behavior_OnEmotionChanged(EmotionState new_state)`
- **功能**：情绪变化回调（由 Emotion_Init 传入 FSM）
- **调用**：`LED_SetColor()`, `Screen_ShowEmotion()`
- **被调用**：—（入口/回调）


### `Behavior_Tick()` — 周期驱动（可选，用于动作的插值推进） 

- **签名**：`void Behavior_Tick(uint32_t now_ms)`
- **功能**：周期驱动（可选，用于动作的插值推进） 
- **调用**：`Motion_Tick()`
- **被调用**：`main()`

## src/motion.c — 动作组合层

*高层动作（点头/摇摆/睡姿）*

**函数清单**（共 11 个）：

- `Motion_AngryTilt()` — 生气：歪头 + 快速复位（入口/回调）
- `Motion_BoredSlouch()` — 无聊：缓慢歪头（入口/回调）
- `Motion_HappySway()` — 开心：轻微点头（2 次）（入口/回调）
- `Motion_IdlePose()` — 待机：回到正中（入口/回调）
- `Motion_Init()` — motion.h - TD1601 动作组合层（Motion Layer） 职责： 用 Servo_Move 等原语组合出"高层动作"（点头、摇摆、歪头、睡姿...）， 每个动作由（入口/回调）
- `Motion_LovingLean()` — 亲昵：靠近（前倾）（入口/回调）
- `Motion_ResetPose()` — 复位到正中（90/90）（被 `Behavior_Init`、`Motion_Init` 调用）
- `Motion_SadNod()` — 难过：头部下垂（入口/回调）
- `Motion_SleepPose()` — 睡眠：低头垂眼（入口/回调）
- `Motion_SurprisedLean()` — 惊讶：头部后仰（入口/回调）
- `Motion_Tick()` — 周期驱动（推进正在执行的动作序列/缓动插值） （被 `Behavior_Tick` 调用）

---


### `Motion_AngryTilt()` — 生气：歪头 + 快速复位

- **签名**：`void Motion_AngryTilt(void)`
- **功能**：生气：歪头 + 快速复位
- **调用**：`HAL_DelayMs()`, `Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_BoredSlouch()` — 无聊：缓慢歪头

- **签名**：`void Motion_BoredSlouch(void)`
- **功能**：无聊：缓慢歪头
- **调用**：`Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_HappySway()` — 开心：轻微点头（2 次）

- **签名**：`void Motion_HappySway(void)`
- **功能**：开心：轻微点头（2 次）
- **调用**：`HAL_DelayMs()`, `Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_IdlePose()` — 待机：回到正中

- **签名**：`void Motion_IdlePose(void)`
- **功能**：待机：回到正中
- **调用**：`Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_Init()` — motion.h - TD1601 动作组合层（Motion Layer） 职责： 用 Servo_Move 等原语组合出"高层动作"（点头、摇摆、歪头、睡姿...）， 每个动作由

- **签名**：`void Motion_Init(void)`
- **功能**：motion.h - TD1601 动作组合层（Motion Layer） 职责： 用 Servo_Move 等原语组合出"高层动作"（点头、摇摆、歪头、睡姿...）， 每个动作由
- **调用**：`Motion_ResetPose()`
- **被调用**：—（入口/回调）


### `Motion_LovingLean()` — 亲昵：靠近（前倾）

- **签名**：`void Motion_LovingLean(void)`
- **功能**：亲昵：靠近（前倾）
- **调用**：`HAL_DelayMs()`, `Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_ResetPose()` — 复位到正中（90/90）

- **签名**：`void Motion_ResetPose(void)`
- **功能**：复位到正中（90/90）
- **调用**：`Servo_Move()`
- **被调用**：`Behavior_Init()`, `Motion_Init()`


### `Motion_SadNod()` — 难过：头部下垂

- **签名**：`void Motion_SadNod(void)`
- **功能**：难过：头部下垂
- **调用**：`HAL_DelayMs()`, `Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_SleepPose()` — 睡眠：低头垂眼

- **签名**：`void Motion_SleepPose(void)`
- **功能**：睡眠：低头垂眼
- **调用**：`Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_SurprisedLean()` — 惊讶：头部后仰

- **签名**：`void Motion_SurprisedLean(void)`
- **功能**：惊讶：头部后仰
- **调用**：`HAL_DelayMs()`, `Servo_Move()`
- **被调用**：—（入口/回调）


### `Motion_Tick()` — 周期驱动（推进正在执行的动作序列/缓动插值） 

- **签名**：`void Motion_Tick(uint32_t now_ms)`
- **功能**：周期驱动（推进正在执行的动作序列/缓动插值） 
- **调用**：—
- **被调用**：`Behavior_Tick()`

## src/servo_control.c — 舵机控制

*角度限幅与 PWM 输出*

**函数清单**（共 4 个）：

- `Servo_GetAngle()` — 立即读取当前角度（被 `Sensor_Scan` 调用）
- `Servo_Init()` — servo_control.h - TD1601 舵机控制层 职责： - PWM 生成与角度控制（通过 HAL 层） - 角度限幅 0-180 - 缓动（easing）计算骨架 /（被 `Behavior_Init` 调用）
- `Servo_Move()` — 控制单个舵机 （被 `Motion_AngryTilt`、`Motion_BoredSlouch`、`Motion_HappySway`、`Motion_IdlePose`、`Motion_LovingLean`、`Motion_ResetPose`、`Motion_SadNod`、`Motion_SleepPose`、`Motion_SurprisedLean`、`Sensor_Scan` 调用）
- `clamp_angle()` — —（被 `Servo_Move` 调用）

---


### `Servo_GetAngle()` — 立即读取当前角度

- **签名**：`uint8_t Servo_GetAngle(uint8_t servo_id)`
- **功能**：立即读取当前角度
- **调用**：—
- **被调用**：`Sensor_Scan()`


### `Servo_Init()` — servo_control.h - TD1601 舵机控制层 职责： - PWM 生成与角度控制（通过 HAL 层） - 角度限幅 0-180 - 缓动（easing）计算骨架 /

- **签名**：`void Servo_Init(void)`
- **功能**：servo_control.h - TD1601 舵机控制层 职责： - PWM 生成与角度控制（通过 HAL 层） - 角度限幅 0-180 - 缓动（easing）计算骨架 /
- **调用**：`HAL_PWM_SetServoAngle()`
- **被调用**：`Behavior_Init()`


### `Servo_Move()` — 控制单个舵机 

- **签名**：`void Servo_Move(uint8_t servo_id, uint8_t angle, uint16_t duration_ms, EasingType easing)`
- **功能**：控制单个舵机 
- **调用**：`HAL_PWM_SetServoAngle()`, `clamp_angle()`
- **被调用**：`Motion_AngryTilt()`, `Motion_BoredSlouch()`, `Motion_HappySway()`, `Motion_IdlePose()`, `Motion_LovingLean()`, `Motion_ResetPose()`, `Motion_SadNod()`, `Motion_SleepPose()`, `Motion_SurprisedLean()`, `Sensor_Scan()`


### `clamp_angle()` — （无注释）

- **签名**：`uint8_t clamp_angle(int angle)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Servo_Move()`

## src/screen_display.c — LCD 表情

*情绪 → 屏幕动画*

**函数清单**（共 3 个）：

- `Screen_Init()` — screen_display.h - TD1601 LCD 表情显示层 职责： 根据 EmotionState 显示对应表情动画（被 `Behavior_Init` 调用）
- `Screen_ShowEmotion()` — 显示情绪表情 （被 `Behavior_OnEmotionChanged` 调用）
- `Screen_Tick()` — 周期驱动（帧动画推进） （入口/回调）

---


### `Screen_Init()` — screen_display.h - TD1601 LCD 表情显示层 职责： 根据 EmotionState 显示对应表情动画

- **签名**：`void Screen_Init(void)`
- **功能**：screen_display.h - TD1601 LCD 表情显示层 职责： 根据 EmotionState 显示对应表情动画
- **调用**：`HAL_LCD_Clear()`, `HAL_LCD_Init()`
- **被调用**：`Behavior_Init()`


### `Screen_ShowEmotion()` — 显示情绪表情 

- **签名**：`void Screen_ShowEmotion(EmotionState emo, const char *anim_name)`
- **功能**：显示情绪表情 
- **调用**：`HAL_LCD_ShowText()`
- **被调用**：`Behavior_OnEmotionChanged()`


### `Screen_Tick()` — 周期驱动（帧动画推进） 

- **签名**：`void Screen_Tick(uint32_t now_ms)`
- **功能**：周期驱动（帧动画推进） 
- **调用**：—
- **被调用**：—（入口/回调）

## src/led_control.c — LED 控制

*颜色/亮度*

**函数清单**（共 3 个）：

- `LED_Init()` — led_control.h - TD1601 LED 灯效控制层 职责： 设置 LED 颜色/亮度（RGB），按情绪映射（被 `Behavior_Init` 调用）
- `LED_SetBrightness()` — 设置亮度（0-100%）（入口/回调）
- `LED_SetColor()` — 设置 LED 颜色 （被 `Behavior_OnEmotionChanged`、`LED_SetBrightness` 调用）

---


### `LED_Init()` — led_control.h - TD1601 LED 灯效控制层 职责： 设置 LED 颜色/亮度（RGB），按情绪映射

- **签名**：`void LED_Init(void)`
- **功能**：led_control.h - TD1601 LED 灯效控制层 职责： 设置 LED 颜色/亮度（RGB），按情绪映射
- **调用**：`HAL_LED_Init()`, `HAL_LED_SetRgb()`
- **被调用**：`Behavior_Init()`


### `LED_SetBrightness()` — 设置亮度（0-100%）

- **签名**：`void LED_SetBrightness(uint8_t percent)`
- **功能**：设置亮度（0-100%）
- **调用**：`LED_SetColor()`
- **被调用**：—（入口/回调）


### `LED_SetColor()` — 设置 LED 颜色 

- **签名**：`void LED_SetColor(RgbColor color)`
- **功能**：设置 LED 颜色 
- **调用**：`HAL_LED_SetRgb()`
- **被调用**：`Behavior_OnEmotionChanged()`, `LED_SetBrightness()`

## src/sensor_manager.c — 传感器管理

*触摸/红外 → FSM*

**函数清单**（共 2 个）：

- `Sensor_Init()` — sensor_manager.h - TD1601 传感器管理 职责： 周期扫描机器人本体传感器（触摸、红外追踪等）， 事件直接驱动情绪状态机——不经过 PC（被 `system_init` 调用）
- `Sensor_Scan()` — 周期扫描（主循环调用，建议 10ms~50ms 一次） （被 `main` 调用）

---


### `Sensor_Init()` — sensor_manager.h - TD1601 传感器管理 职责： 周期扫描机器人本体传感器（触摸、红外追踪等）， 事件直接驱动情绪状态机——不经过 PC

- **签名**：`void Sensor_Init(void)`
- **功能**：sensor_manager.h - TD1601 传感器管理 职责： 周期扫描机器人本体传感器（触摸、红外追踪等）， 事件直接驱动情绪状态机——不经过 PC
- **调用**：`HAL_GPIO_Init()`, `HAL_GPIO_ReadTouch()`
- **被调用**：`system_init()`


### `Sensor_Scan()` — 周期扫描（主循环调用，建议 10ms~50ms 一次） 

- **签名**：`void Sensor_Scan(uint32_t now_ms)`
- **功能**：周期扫描（主循环调用，建议 10ms~50ms 一次） 
- **调用**：`Emotion_Touch()`, `Emotion_Update()`, `HAL_GPIO_ReadIRDirection()`, `HAL_GPIO_ReadTouch()`, `Servo_GetAngle()`, `Servo_Move()`
- **被调用**：`main()`

## src/uart_comm.c — UART 通信

*协议解析（轻量 + JSON）*

**函数清单**（共 8 个）：

- `UART_Init()` — uart_comm.h - TD1601 UART 通信层 职责： 接收并解析 PC 端下发的"语义状态"消息， 转换后驱动情绪状态机（被 `system_init` 调用）
- `UART_OnByteReceived()` — 供 HAL 层中断回调：收到一个字节（入口/回调）
- `UART_Process()` — 周期处理：从接收缓冲解析完整消息 （被 `main` 调用）
- `UART_SendLine()` — 向 PC 发送一行文本（调试/心跳用）（被 `system_init` 调用）
- `handle_line()` — —（被 `UART_OnByteReceived` 调用）
- `name_to_emotion()` — —（被 `parse_json_emotion`、`parse_lightweight` 调用）
- `parse_json_emotion()` — —（被 `handle_line` 调用）
- `parse_lightweight()` — —（被 `handle_line` 调用）

---


### `UART_Init()` — uart_comm.h - TD1601 UART 通信层 职责： 接收并解析 PC 端下发的"语义状态"消息， 转换后驱动情绪状态机

- **签名**：`void UART_Init(void)`
- **功能**：uart_comm.h - TD1601 UART 通信层 职责： 接收并解析 PC 端下发的"语义状态"消息， 转换后驱动情绪状态机
- **调用**：`HAL_UART_Init()`
- **被调用**：`system_init()`


### `UART_OnByteReceived()` — 供 HAL 层中断回调：收到一个字节

- **签名**：`void UART_OnByteReceived(uint8_t byte)`
- **功能**：供 HAL 层中断回调：收到一个字节
- **调用**：`handle_line()`
- **被调用**：—（入口/回调）


### `UART_Process()` — 周期处理：从接收缓冲解析完整消息 

- **签名**：`void UART_Process(uint32_t now_ms)`
- **功能**：周期处理：从接收缓冲解析完整消息 
- **调用**：—
- **被调用**：`main()`


### `UART_SendLine()` — 向 PC 发送一行文本（调试/心跳用）

- **签名**：`void UART_SendLine(const char *line)`
- **功能**：向 PC 发送一行文本（调试/心跳用）
- **调用**：`HAL_UART_Send()`
- **被调用**：`system_init()`


### `handle_line()` — （无注释）

- **签名**：`void handle_line(char *line)`
- **功能**：（无注释）
- **调用**：`parse_json_emotion()`, `parse_lightweight()`
- **被调用**：`UART_OnByteReceived()`


### `name_to_emotion()` — （无注释）

- **签名**：`EmotionState name_to_emotion(const char *name)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`parse_json_emotion()`, `parse_lightweight()`


### `parse_json_emotion()` — （无注释）

- **签名**：`bool parse_json_emotion(const char *line)`
- **功能**：（无注释）
- **调用**：`Emotion_Update()`, `name_to_emotion()`
- **被调用**：`handle_line()`


### `parse_lightweight()` — （无注释）

- **签名**：`bool parse_lightweight(const char *line)`
- **功能**：（无注释）
- **调用**：`Emotion_Update()`, `name_to_emotion()`
- **被调用**：`handle_line()`

## hal/hal_pwm.c — HAL-PWM

*舵机脉宽输出（占位）*

**函数清单**（共 2 个）：

- `HAL_PWM_Init()` — —（入口/回调）
- `HAL_PWM_SetServoAngle()` — —（被 `Servo_Init`、`Servo_Move` 调用）

---


### `HAL_PWM_Init()` — （无注释）

- **签名**：`void HAL_PWM_Init(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：—（入口/回调）


### `HAL_PWM_SetServoAngle()` — （无注释）

- **签名**：`void HAL_PWM_SetServoAngle(uint8_t servo_id, uint8_t angle)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Servo_Init()`, `Servo_Move()`

## hal/hal_lcd.c — HAL-LCD

*屏幕显示（占位）*

**函数清单**（共 3 个）：

- `HAL_LCD_Clear()` — —（被 `Screen_Init` 调用）
- `HAL_LCD_Init()` — —（被 `Screen_Init` 调用）
- `HAL_LCD_ShowText()` — —（被 `Screen_ShowEmotion` 调用）

---


### `HAL_LCD_Clear()` — （无注释）

- **签名**：`void HAL_LCD_Clear(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Screen_Init()`


### `HAL_LCD_Init()` — （无注释）

- **签名**：`void HAL_LCD_Init(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Screen_Init()`


### `HAL_LCD_ShowText()` — （无注释）

- **签名**：`void HAL_LCD_ShowText(const char *text)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Screen_ShowEmotion()`

## hal/hal_led.c — HAL-LED

*RGB 输出（占位）*

**函数清单**（共 2 个）：

- `HAL_LED_Init()` — —（被 `LED_Init` 调用）
- `HAL_LED_SetRgb()` — —（被 `LED_Init`、`LED_SetColor` 调用）

---


### `HAL_LED_Init()` — （无注释）

- **签名**：`void HAL_LED_Init(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`LED_Init()`


### `HAL_LED_SetRgb()` — （无注释）

- **签名**：`void HAL_LED_SetRgb(uint8_t r, uint8_t g, uint8_t b)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`LED_Init()`, `LED_SetColor()`

## hal/hal_gpio.c — HAL-GPIO

*触摸/红外/延时（占位）*

**函数清单**（共 4 个）：

- `HAL_DelayMs()` — —（被 `Motion_AngryTilt`、`Motion_HappySway`、`Motion_LovingLean`、`Motion_SadNod`、`Motion_SurprisedLean`、`main` 调用）
- `HAL_GPIO_Init()` — —（被 `Sensor_Init` 调用）
- `HAL_GPIO_ReadIRDirection()` — —（被 `Sensor_Scan` 调用）
- `HAL_GPIO_ReadTouch()` — —（被 `Sensor_Init`、`Sensor_Scan` 调用）

---


### `HAL_DelayMs()` — （无注释）

- **签名**：`void HAL_DelayMs(uint32_t ms)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Motion_AngryTilt()`, `Motion_HappySway()`, `Motion_LovingLean()`, `Motion_SadNod()`, `Motion_SurprisedLean()`, `main()`


### `HAL_GPIO_Init()` — （无注释）

- **签名**：`void HAL_GPIO_Init(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Sensor_Init()`


### `HAL_GPIO_ReadIRDirection()` — （无注释）

- **签名**：`int8_t HAL_GPIO_ReadIRDirection(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Sensor_Scan()`


### `HAL_GPIO_ReadTouch()` — （无注释）

- **签名**：`bool HAL_GPIO_ReadTouch(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`Sensor_Init()`, `Sensor_Scan()`

## hal/hal_uart.c — HAL-UART

*串口收发（占位）*

**函数清单**（共 2 个）：

- `HAL_UART_Init()` — —（被 `UART_Init` 调用）
- `HAL_UART_Send()` — —（被 `UART_SendLine` 调用）

---


### `HAL_UART_Init()` — （无注释）

- **签名**：`void HAL_UART_Init(void)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`UART_Init()`


### `HAL_UART_Send()` — （无注释）

- **签名**：`void HAL_UART_Send(const uint8_t *data, uint16_t len)`
- **功能**：（无注释）
- **调用**：—
- **被调用**：`UART_SendLine()`


---

*重新生成：在项目根目录运行 `python tools/gen_function_map.py`。*
