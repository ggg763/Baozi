// =====================================================================
//  wifi_secrets.example.h —— 模板，可入库
//
//  用法：把本文件复制成同目录的 wifi_secrets.h，填上自己的热点。
//        wifi_secrets.h 已在 .gitignore 里，不会被提交。
//
//      Windows:  copy wifi_secrets.example.h wifi_secrets.h
//      macOS/Linux: cp wifi_secrets.example.h wifi_secrets.h
//
//  没有 wifi_secrets.h 也能编译：config.h 会退回占位值并打一条 #warning，
//  烧进去的固件连不上任何热点（现象是串口一直重连失败）。
//
//  ⚠️ 两个数组必须严格等长、顺序一一对应（同一个下标是同一组凭据）。
//     顺序 = 开机尝试顺序，每个热点最多等 WIFI_PER_NET_TIMEOUT_MS（默认 12 秒）。
//
//  ⚠️ 硬性约束（违反连不上）：
//     · 只支持 2.4GHz（ESP32-S3 无 5GHz 射频）
//     · 只支持 WPA2-个人（WPA3 会报 status=6）
//     · SSID 用纯英文（中文 SSID 实测被对端断开，status=6）
// =====================================================================
#pragma once

static const char* const WIFI_SSID_LIST[] = {
  "YOUR_WIFI_SSID",        // [0] 主用热点（现场用的放最前）
  "YOUR_BACKUP_SSID",      // [1] 备选；不需要就删掉这一行（两个数组一起删）
};

static const char* const WIFI_PASSWORD_LIST[] = {
  "YOUR_WIFI_PASSWORD",
  "YOUR_BACKUP_PASSWORD",
};

// AP 模式（USE_AP_MODE=1）才用；≥8 位，WPA2
#define AP_PASSWORD "YOUR_AP_PASSWORD"
