/**
 * main.c - TD1601 机器人主程序
 *
 * 行为层核心：PC 只下发语义状态（emotion），
 * TD1601 自主完成 情绪 -> 行为 -> 动作 -> 硬件 的决策与执行。
 *
 * 主循环：
 *   UART 收消息 -> Emotion FSM 更新 -> Behavior 决策 -> Motion/Screen/LED 执行
 *   传感器周期扫描（触摸/红外）直接驱动 FSM
 *   空闲超时自动降级（IDLE -> BORED -> SLEEPING）
 */

#include "robot_types.h"

#include "uart_comm.h"
#include "emotion_state.h"
#include "behavior.h"
#include "sensor_manager.h"

#include "hal/hal_gpio.h"   /* HAL_DelayMs */

/* 主循环节拍（毫秒） */
#define TICK_MS          (20U)

/* 简易毫秒时钟：真机可替换为定时器/RTOS 时钟源 */
static uint32_t s_now_ms = 0U;

/* ---------- 系统时钟 ---------- */
static uint32_t sys_now_ms(void)
{
    /* TODO(真机): 返回系统 tick（如 SysTick 毫秒计数）。
     * PC 调试期使用简单递增计数（每次主循环 +TICK_MS）。 */
    return s_now_ms;
}

/* ---------- 初始化 ---------- */
static void system_init(void)
{
    UART_Init();

    /* 行为层内部会初始化 Screen/LED/Servo 并复位姿态 */
    Behavior_Init();

    /* 情绪 FSM：注册行为回调 */
    Emotion_Init(Behavior_OnEmotionChanged);

    /* 传感器 */
    Sensor_Init();

    /* 建立待机基准：开机视为一次互动，避免立即降级 */
    Emotion_Touch(sys_now_ms());

    UART_SendLine("BOOT: BAOZI TD1601 READY");
}

/* ---------- 主循环 ---------- */
int main(void)
{
    system_init();

    while (1) {
        uint32_t now = sys_now_ms();

        /* 1. 处理 PC 下发消息（UART 中断已写入缓冲，此处拆行解析） */
        UART_Process(now);

        /* 2. 传感器扫描（触摸/红外） */
        Sensor_Scan(now);

        /* 3. 情绪 FSM 节拍（空闲降级检查） */
        Emotion_Tick(now);

        /* 4. 行为层节拍（动作插值/动画推进，当前为空） */
        Behavior_Tick(now);

        /* 节拍控制：PC 调试期推进虚拟时钟 + 真实延时 */
        s_now_ms += TICK_MS;
        HAL_DelayMs(TICK_MS);
    }

    /* 不可达 */
    return 0;
}
