#pragma once
#include <stdint.h>

// =====================================================================
//  ESP32 <-> PC 的帧协议
//  （pc_baozi/protocol.py 是同一份协议的 Python 实现，两边必须保持一致）
//
//  ★ 2026-11 起传输介质改成【USB 串口】（USB CDC），不再是 WiFi/TCP：
//     · ESP32 通过 USB 线插在跑 pc_baozi 的那台电脑上；
//     · PC 的网络只用于访问外部服务（edge-tts / 天气 / Ollama …），
//       不再需要和 ESP32 处在同一个局域网，也不再需要知道对方 IP。
//    帧格式、magic、seq 全部沿用原来的定义，所以协议层几乎没变 ——
//    变的只是"字节从哪里来、往哪里去"（见 link.cpp 的 USE_USB_LINK）。
//
//  帧结构，全部小端：
//
//    偏移  长度  字段
//    0     1     0xA5            magic0
//    1     1     0x5A            magic1
//    2     1     type            见下面的 PKT_*
//    3     1     flags           保留，填 0
//    4     2     seq             uint16 递增序号（回绕）
//    6     2     payload_len     uint16，0 ~ PROTO_MAX_PAYLOAD
//    8     N     payload
//
//  串口是字节流但不保证"一次 read 拿到整帧"，所以两端都用累积缓冲 +
//  状态机切帧（link.cpp 的 feed() / protocol.py 的 FrameDecoder）。
//  payload_len 超限 = 流已经错位，逐字节重新同步。
// =====================================================================

#define PROTO_MAGIC0        0xA5
#define PROTO_MAGIC1        0x5A
#define PROTO_HEADER_LEN    8
#define PROTO_MAX_PAYLOAD   2048

// ---- type ----
#define PKT_MIC_PCM     0x01    // ESP32 -> PC  麦克风 PCM：16k / 16bit / mono / 小端
#define PKT_TTS_PCM     0x02    // PC -> ESP32  待播放 PCM：16k / 16bit / mono / 小端
#define PKT_CTRL        0x03    // 双向         控制/事件，payload[0] 是 CTRL_*
#define PKT_TEXT        0x04    // PC -> ESP32  调试文本（UTF-8，不保证以 \0 结尾）
// ESP32 -> PC 的日志文本（UTF-8）。为什么要走帧而不是直接写串口：
// 串口这根线现在同时承载二进制协议帧，裸文本会把帧流冲乱、两端都解析不了。
// PC 收到后按行打印，前缀 [esp]。
#define PKT_LOG         0x05
// ESP32 -> PC  摄像头 JPEG 分片（配合 CTRL_CAM_REQ / CTRL_CAM_DONE）。
// 一帧 JPEG 通常 20~60KB，按 PROTO_MAX_PAYLOAD 切片连续发送，
// 收完由 CTRL_CAM_DONE 标记结束（payload 里带总字节数，用来校验）。
#define PKT_CAM_JPEG    0x06

// ---- PKT_CTRL 的 payload[0] ----
#define CTRL_PLAY_BEGIN 0x01    // PC -> ESP32  下面开始推 TTS 音频了
#define CTRL_PLAY_END   0x02    // PC -> ESP32  本轮 TTS 推完了
#define CTRL_PLAY_ABORT 0x03    // PC -> ESP32  立刻停止播放并清空缓冲（用户打断）
#define CTRL_PING       0x04    // 双向         保活
#define CTRL_PONG       0x05    // 双向         保活应答
#define CTRL_HELLO      0x06    // ESP32 -> PC  上线问候，payload[1..] = 设备描述字符串
#define CTRL_HELLO_ACK  0x07    // PC -> ESP32  收到（USB 模式下 PC 打开串口就先发一帧它）
// ⚠️ 握手是【单向发起 + 单向应答】，两端都只答一次，答完就停：
//      ESP32 --HELLO--> PC --HELLO_ACK--> ESP32（收到 ACK 只清标记，不再回 HELLO）
//    任何一端"有问必答"都会变成 HELLO ↔ HELLO_ACK 无限回声，
//    把串口带宽吃满（2026-11 实测：PC 终端"设备上线"疯狂刷屏）。
#define CTRL_AUDIO_STATE 0x08   // ESP32 -> PC  播放状态：payload[1] = 0 空闲 / 1 正在放
#define CTRL_EMOTION    0x09    // PC -> ESP32  情绪：payload[1..] = "EMO:HAPPY" 之类的整行
#define CTRL_INTENT     0x0A    // PC -> ESP32  意图：payload[1..] = "INTENT:CHAT" 之类的整行
#define CTRL_TD_TEXT    0x0B    // PC -> ESP32  文本：payload[1..] = 任意一行，原样转发给 TD1601
#define CTRL_INTENSITY  0x0C    // PC -> ESP32  情感强度：payload[1..] = "INTENSITY:HIGH" 之类的整行
// PC -> ESP32  状态字节帧：payload[1] = 编码字节（0..215），原样透传 + 帧头给 TD1601。
// 为什么要这个（2026-09 实测的真实故障）：一轮对话原来要连发 3 条文本命令
// （INTENSITY + EMO + INTENT ≈ 47 字节），TD1601 的 UART3 FIFO 中途溢出，
// 命令被截断成 "INTENSITY:EMO:SAD" 之类。改成 1 个字节后 3 帧变 1 帧、47 字节变 3 字节。
// 编码表：code = (emo_id*8 + intent_id)*3 + intensity_id（与 td1601.py 一致）
#define CTRL_STATE      0x0D
// PC -> ESP32  请求抓一帧：payload[1] 可选 = 请求序号（0~255，原样带回）
#define CTRL_CAM_REQ    0x0E
// ESP32 -> PC  一帧传完：payload[1] = 0 成功 / 1 失败
//              payload[2..5] = 本帧总字节数（uint32 小端，失败时为 0）
//              payload[6]    = 请求序号（CTRL_CAM_REQ 带来的那个，没带就是 0）
#define CTRL_CAM_DONE   0x0F
