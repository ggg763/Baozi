#pragma once
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include "protocol.h"

// =====================================================================
//  ESP32 侧的链路层。
//
//  两种实现，由 config.h 的 USE_USB_LINK 选择：
//    · USE_USB_LINK=1：USB 串口（USB CDC）直连 PC。
//        PC 通过 USB 线给板子供电 + 通信，不涉及 WiFi，也不需要 IP。
//    · USE_USB_LINK=0（当前用这个）：WiFi + TCP 客户端。
//        WiFi 段支持多热点自动切换 + UDP 自动发现 PC 地址。
//
//  两种实现对外接口完全一致，只负责"字节进、帧出"，
//  不关心帧里装的是音频、图像还是控制消息。
// =====================================================================

typedef std::function<void(uint8_t type, uint16_t seq,
                           const uint8_t* payload, uint16_t len)> FrameHandler;

bool linkBegin();                 // USB：初始化串口；WiFi：连 WiFi + 连服务器
void linkLoop();                  // 主循环里反复调用，负责收帧（WiFi 模式还负责重连）
bool linkConnected();             // 是否可以收发（USB：PC 是否在跟我们说话）

bool linkSendFrame(uint8_t type, const uint8_t* payload, uint16_t len);
bool linkSendCtrl(uint8_t ctrl_cmd, const uint8_t* extra = NULL, uint16_t extra_len = 0);
void linkSetHandler(FrameHandler h);   // 收到完整一帧时回调

// WiFi 掉线重连后 IP 可能变了 -> HTTP 服务器要重开。
// linkLoop() 里置 true，.ino 主循环看到后关掉旧 server 再 begin()。
extern volatile bool s_http_need_restart;
