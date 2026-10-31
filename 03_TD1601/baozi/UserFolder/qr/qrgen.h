/******************************************************************************
 * @file     qrgen.h
 * @brief    屏幕二维码生成器（纯计算，无外设依赖）—— 可在 PC 上单独编译验证
 *
 * 为什么自己写一个而不是让 PC/ESP32 下发点阵：
 *   1. 屏幕要显示的是"这台设备当前的热点名/密码"，内容随时会变；下发点阵
 *      意味着每换一次网就要传 480×480×1bpp ≈ 28 KB —— UART3 只有 115200 且
 *      实板上还是单向的，没必要。
 *   2. 下发字符串 = 一条 QWERTY 长度的文本（< 120 字节），TD1601 自己编码，
 *      想画多大画多大（整数倍缩放，模块边界永远锐利），不受传输尺寸限制。
 *   3. 编码器是纯整数运算，能在 PC 上跟 segno / OpenCV 逐位比对 —— 与表情层
 *      同一套"先离线验证再上板"的做法。
 *
 * 实现范围（够用就好，不追求全集）：
 *   · 只做 **Byte 模式**（WIFI: 串、URL 都是 ASCII/UTF-8 字节流，正好）
 *   · 版本 1..10（1..213 字节 @L / 163 字节 @M，远超一条 WIFI 串）
 *   · ECC L / M / Q / H 四档表都在，默认 M
 *   · 8 种掩模全做，按标准罚分选最优（否则某些内容在屏上会出现大片同色，
 *     手机对焦困难）
 *
 * 单位与布局：
 *   modules[ y * size + x ]，一个字节一个模块，1 = 黑。**不含静区**，
 *   调用方自己留白（标准要求 ≥4 模块）。
 *
 * ⚠️ 与 face_* 一样，本文件不允许出现 float/double —— 板上没有 FPU。
 ******************************************************************************/
#ifndef _QRGEN_H_
#define _QRGEN_H_

#include <stdint.h>

/* 版本上限。10 版 = 57×57 模块，480px 屏上一模块 8px 还剩 24px 静区；
 * 再大就得缩到 6px，观感反而更差，而且容量早就够用了。 */
#define QRGEN_MAX_VERSION   10
#define QRGEN_MAX_SIZE      (17 + 4 * QRGEN_MAX_VERSION)      /* 57 */

typedef enum {
    QRGEN_ECC_L = 0,   /* 约 7%  纠错，容量最大 */
    QRGEN_ECC_M = 1,   /* 约 15% 纠错 —— **默认**，手机上最好扫 */
    QRGEN_ECC_Q = 2,   /* 约 25% */
    QRGEN_ECC_H = 3    /* 约 30% */
} qrgen_ecc_t;

typedef struct {
    const uint8_t *modules;   /* 值 0/1；**行跨度为 stride 而不是 size** */
    int            stride;    /* 每行字节数（= QRGEN_MAX_SIZE，内部是定长二维数组） */
    int            size;      /* 有效模块边长（= 17 + 4*version），<= stride */
    int            version;   /* 1..QRGEN_MAX_VERSION */
    int            mask;      /* 实际使用的掩模 0..7 */
    int            ecc;       /* qrgen_ecc_t */
} qrgen_result_t;

/* 编码。成功返回 0，并把结果写进 *out（data 指向内部静态缓冲，
 * 下一次调用会被覆盖）。失败返回负数：
 *     -1 文本为空 / 含不可编码字符
 *     -2 内容超出版本 10 在该 ECC 档下的容量
 *     -3 内部参数错误（不该发生） */
int qrgen_encode(const char *text, qrgen_ecc_t ecc, qrgen_result_t *out);

/* 给定字节数，算出最少需要第几版（0 = 连最大版都装不下）。
 * NET 页面用它决定要不要把 SSID 截短。 */
int qrgen_min_version(int byte_len, qrgen_ecc_t ecc);

/* 该版本 + ECC 档下的**数据**字节容量（已扣掉模式头/长度域/纠错码）。 */
int qrgen_capacity(int version, qrgen_ecc_t ecc);

#endif /* _QRGEN_H_ */
