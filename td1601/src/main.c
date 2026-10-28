/******************************************************************************
 * @file     main.c
 * @brief    BaoZi desktop robot main (TD1601)
 *
 * 本文件 = 【合并版】入口：把两块开发合到一个固件里
 *   ① LCD 分支（同事交付的 baozi_LCD）：ICNT8952BN 触摸 + 卡片 UI（ui/ui_app/
 *      pomo/habit_store/sched_store）+ 参数化脸 + PC 串口协议 + ESP32 情绪链路
 *   ② 本机工程：servo_core 舵机 + movements 动作库 + emotion 情感三层 +
 *      tracking 红外跟随 + 参数化脸渲染（带总线仲裁）
 *
 * ★ 合并后的两条"所有权"约定（改动前务必先读）：
 *   · 串口 UART0 归 src/uart_hal.c 独占（异步中断 + 字节级分流：帧 / 文本命令）。
 *     emotion_fsm.c 的 poll_uart 已默认关闭（EMO_FSM_OWN_UART = 0），文本命令
 *     由 UserFolder/app_core.c 取出后喂 emotion_fsm_feed_line()。
 *   · 屏幕归 UserFolder/ui_app.c 独占（表情态 + 卡片层）。
 *     face_ui_* 由 UserFolder/anim/face_ui_card.c 实现并转发到 ui_app，
 *     所以 face_emo_test.c 不再参与编译（它自带主循环 + 自己读串口）。
 *
 * ★ 主循环节拍：emotion_fsm_step() 负责舵机/动作/心情（moves 播放是阻塞的，
 *   期间靠 moves 的 20ms 钩子回调 face_ui_tick → app_core_step 保活屏幕与触摸），
 *   app_core_step() 负责触摸/串口/分片刷屏。
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <drv/common.h>     /* mdelay / udelay */
#include "board_config.h"
#include "board_init.h"
#include "video.h"
#include "ui.h"             /* ui_set_bus_callback / ui_flush_busy */
#include "app_core.h"       /* app_core_init / app_core_step */
#include "behavior.h"       /* behavior_init：红外近距 + 安全抑制注册 */
#include "emotion/emotion_fsm.h"
#include "servo_test/servo_core.h"   /* servo_bus_event（总线仲裁）*/

/* video.c 依赖的全局帧计数（官方模板同款定义） */
int frame_count = 0;

/* video.c 依赖的毫秒延时（官方模板同款实现：udelay(1000) 累加） */
void HAL_Delay(uint32_t ms)
{
    while (ms--) {
        udelay(1000);
    }
}

/* ===== 测试入口切换区 ===== */
extern void anim_demo(void);       /* UserFolder/anim/anim_player.c（ANM3 位图管线，默认不编译）*/
extern void video_tetra_test(void);/* UserFolder/QSPI_test/video_tetra_test.c 屏幕 3D 四面体 */
extern void face_emo_test(void);   /* UserFolder/anim/face_emo_test.c 参数化脸·自带串口（合并后默认不编译）*/
extern void emotion_test(void);    /* UserFolder/test/emotion_test.c 情感状态机·串口交互测试 */
extern void ir_test(void);
extern void track_yaw_test(void);
extern void track_dual_test(void);

/* ===== 入口选择（只改这一个数字）=====
 *   4 = 【正式】合并版：卡片 UI + 触摸 + 参数化脸 + 舵机动作 + 情感状态机
 *   3 = 纯情感状态机（只跑 emotion_fsm_run，不启用卡片 UI/触摸）
 *       ⚠️ 走这一档必须把 emotion/emotion_fsm.c 的 EMO_FSM_OWN_UART 改回 1，
 *          否则串口命令没人收（它已把 UART0 让给 uart_hal.c）。
 *   2 = 3D 旋转正四面体（判断"屏幕本身"是否正常的基准）
 *   1 = 位图管线（需在 package.yaml 打开 anim_player.c + libanimres.a）
 *   0 = 参数化脸自带调试入口（需重新登记 face_emo_test.c）
 */
#define TEST_ENTRY 4

/* 合并版：舵机/情感 + 卡片 UI 一起跑 */
static void app_run(void)
{
    printf("\r\n===== BaoZi boot (merged: UI + touch + servo + emotion) =====\r\n");

    /* ① 屏幕 + 触摸 + 两根串口先就绪（emotion_fsm_init 依赖屏幕已初始化）*/
    app_core_init();

    /* ② 卡片层刷屏的总线仲裁：舵机写 PWM 与分段 DMA 抢 AHB 会导致白块。
     *    表情层由 emotion_fsm_init() 内部的 face_set_bus_callback 注册；
     *    卡片层（ui.c）必须在这里补一次同样的注册。 */
    ui_set_bus_callback(servo_bus_event);

    /* ③ 舵机 + 动作库 + 情感三层 +（其内部）屏幕表情后端 */
    printf("[app] --- emotion_fsm_init() ---\r\n");
    if (emotion_fsm_init() != 0) {
        printf("[app] emotion_fsm_init FAILED (servo/moves init fail)\r\n");
    } else {
        printf("[app] emotion + servo ready\r\n");
    }

    /* ④ 红外近距 + 安全抑制：手指在屏上 / 贴太近 → FSM 立刻停手。
     *    必须在 emotion_fsm_init() 之后（FSM 内部状态要先就绪）。 */
    behavior_init();

    printf("[app] serial: 115200 8N1. binary frames + text cmds (EMO:/INTENT:/help)\r\n");

    for (;;) {
        /* 舵机/动作/心情：FSM 模式下 moves_play_variant() 会阻塞几秒，
         * 期间由 moves 的 20ms 钩子调 face_ui_tick() → app_core_step()，
         * 屏幕与触摸不会冻住。 */
        emotion_fsm_step();

        /* 触摸 / 串口 / UI / 分片刷屏 */
        app_core_step();

        /* 刷屏没完就不睡：让下一片尽快接上；空闲时让出 CPU 给中断 */
        if (!ui_flush_busy() && !app_core_touch_down()) {
            mdelay(5);
        }
    }
}

int main(void)
{
    board_init();

#if TEST_ENTRY == 4
    app_run();
#elif TEST_ENTRY == 3
    emotion_fsm_run();       /* 纯情感状态机（EMO_FSM_OWN_UART 需为 1）*/
#elif TEST_ENTRY == 2
    video_tetra_test();      /* 3D 旋转四面体（测屏幕本身）*/
#elif TEST_ENTRY == 1
    anim_demo();             /* 位图管线（需登记 anim_player.c + libanimres.a）*/
#else
    face_emo_test();         /* 参数化脸自带串口调试（需登记 face_emo_test.c）*/
#endif
    return 0;
}
