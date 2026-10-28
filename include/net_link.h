/******************************************************************************
 * @file     net_link.h
 * @brief    配网文本命令的解析层（UART3 ← ESP32，或 UART0 ← PC 台架直连）
 *
 * 为什么用"文本行"而不是二进制帧：
 *   · UART3 这条线上 ESP32 侧本来就是 `Serial2.println("EMO:HAPPY")` 这种裸文本，
 *     没有 A5|LEN|CMD|DATA|CHK 信封（见 uart_esp.c 头注释）；为了配网再给它加一套
 *     帧格式，等于让 ESP32 同时维护两种编码，得不偿失。
 *   · 一条 WIFI 串最长 117 字节，115200 下 10ms 发完，不需要分帧。
 *   · 台架上用串口助手敲一行就能验，不用写打包脚本。
 *
 * 命令（行尾 \r 或 \n，大小写敏感，全部 ASCII）：
 *   QR:<payload>            设置二维码内容并显示。payload 就是二维码里装的东西，
 *                           通常是 `WIFI:T:WPA;S:<热点名>;P:<密码>;;`
 *   QR:OFF                  清掉二维码（回到"没有网络"）
 *   NET:<state>             只更新状态：0=空闲 1=热点已开 2=连接中 3=已连接 4=失败
 *   NET:<state>;<ssid>      带 SSID（ASCII；中文会被显示成 (non-ascii)）
 *   NET:<state>;<ssid>;<ip> 带 SSID 与 IP（已连接时用）
 *
 * 典型一轮（时间从上到下）：
 *   NET:1;Baozi-A1B2                          ← 热点起来了
 *   QR:WIFI:T:WPA;S:Baozi-A1B2;P:88888888;;   ← 屏幕跳到 NET 页显示二维码
 *   NET:2                                     ← 手机提交了家里 WiFi，正在连
 *   NET:3;MyHome;192.168.1.37                 ← 连上了，显示 SSID + IP
 *   （失败则 NET:4，屏幕提示重来，ESP32 自行重开热点）
 *
 * ⚠️ 单向上行的现实：实板只引出了 PB18（ESP32→TD1601），**TD1601 无法回话**。
 *    所以"扫完了吗""连上了吗"这些状态必须由 ESP32 主动推，板子只做展示。
 *    这也是为什么命令里没有 ACK —— 不是省掉了，是物理上没有回程线。
 ******************************************************************************/
#ifndef _NET_LINK_H_
#define _NET_LINK_H_

#include <stdint.h>

/* 上电初始化（清空二维码与状态）。 */
void net_link_init(void);

/* 处理一行文本。返回 1 = 这行是配网命令、已消费；0 = 不是，调用方继续按原逻辑处理。
 * 两个串口都可以喂进来：UART3 的真机链路，UART0 的台架直连。 */
int net_link_handle_line(const char *line);

#endif /* _NET_LINK_H_ */
