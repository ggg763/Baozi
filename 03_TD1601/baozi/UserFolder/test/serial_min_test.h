/******************************************************************************
 * @file     serial_min_test.h
 * @brief    最简串口自检（见 serial_min_test.c）
 ******************************************************************************/

#ifndef __SERIAL_MIN_TEST_H__
#define __SERIAL_MIN_TEST_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 最简串口自检：打印 banner/刻度行 + 回显收到的字节 + 每秒心跳，不返回 */
void serial_min_test(void);

#ifdef __cplusplus
}
#endif

#endif /* __SERIAL_MIN_TEST_H__ */
