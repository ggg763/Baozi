/******************************************************************************
 * @file     servo_motion_test.h
 * @brief    动作库测试台（串口直接播放 moves 里的动作）—— 接口
 *
 * 与另外两个舵机工具的定位差别：
 *   · servo_range_test  ：单步点动，用来【标定】中位与极限（一次 20us）
 *   · servo_dual_test   ：固定轨迹的双路回归（开环往复）
 *   · servo_motion_test ：本文件，【播动作库】—— 走的是正式固件同一套
 *     moves_* → apply_off → servo_set_pulse 路径，限位/限速/总线仲裁全都一致，
 *     所以这里能跑通，正式固件里就一定跑得通。
 *
 * 入口：src/main.c 里把 TEST_ENTRY 设成 5
 ******************************************************************************/
#ifndef _SERVO_MOTION_TEST_H_
#define _SERVO_MOTION_TEST_H_

/* 动作库测试台入口（被 src/main.c 调用，内部死循环不返回） */
void servo_motion_test(void);

#endif /* _SERVO_MOTION_TEST_H_ */
