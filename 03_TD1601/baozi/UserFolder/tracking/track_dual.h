/******************************************************************************
 * @file     track_dual.h
 * @brief    红外双轴（YAW + PITCH）跟随 —— 引脚与可调参数
 *
 * YAW 单轴版（track_yaw.c）已实机验证稳定（2026-08-30）；本文件把 PITCH 加上，
 * 按 UserFolder/doc/ir_tracking_plan.md §1.3 的二维表**同时**驱动两个轴。
 *
 * DUAL 状态 -> 动作表（seen：1 = 看到目标）：
 *   seen L C R | YAW   | PITCH | 判定
 *   -----------+-------+-------+--------------------------------
 *   0 0 0      |  ——   |  ——   | 无目标（走丢失分档）
 *   1 0 0      |  左   |  ——   | 左（高度未知，不动 PITCH）
 *   1 1 0      |  左   |  抬   | 左上
 *   1 1 1      |  ——   |  ——   | 全覆盖（很近），不动
 *   1 0 1      |  ——   |  低   | 下方（下面两路都看到、上方 C 没看到）
 *   0 1 0      |  ——   |  抬   | 中上
 *   0 1 1      |  右   |  抬   | 右上
 *   0 0 1      |  右   |  ——   | 右
 *
 * 规则：
 *   YAW   ：只看到单侧才转（两侧都看到 / 都没看到 -> 水平方向无法判断）
 *   PITCH ：C 看到且未全覆盖 -> 目标偏高 -> 抬头；L+R 都看到但 C 没看到 -> 偏低 -> 低头
 *
 * 约定（本项目实测）：脉宽增大 = YAW 右转 / PITCH 抬头
 ******************************************************************************/
#ifndef _TRACK_DUAL_H_
#define _TRACK_DUAL_H_

/* ---- 三路红外输入（PORTB，位掩码） ---- */
#define TD_IR_LEFT_PIN      PIN22
#define TD_IR_CENTER_PIN    PIN23
#define TD_IR_RIGHT_PIN     PIN29
#define TD_IR_ALL_PIN       (TD_IR_LEFT_PIN | TD_IR_CENTER_PIN | TD_IR_RIGHT_PIN)
#define TD_IR_GPIO_IDX      1U      /* PORTB 的 GPIO 控制器 idx */

/* seen 位序：bit2 = L，bit1 = C，bit0 = R（1 = 看到目标） */
#define TD_SEEN_L(seen)     (((seen) >> 2) & 1U)
#define TD_SEEN_C(seen)     (((seen) >> 1) & 1U)
#define TD_SEEN_R(seen)     (((seen) >> 0) & 1U)

/* ---- 采样与去抖（沿用单轴已验证值） ---- */
#define TD_SAMPLE_MS        5U      /* IR 采样周期 */
#define TD_DEBOUNCE_N       12U     /* 连续 N 次一致才认可（12 × 5ms = 60ms） */

/* ---- 控制节拍（★ 2026-10 丝滑化：100ms → 25ms 细分）----
 * 原来是"每 100ms 跳 15us"，舵机看着一格一格挪、有阶梯感；
 * 现在控制周期切细 4 倍、每拍位移按比例减小（巡航速度不变），台阶小 4 倍 → 连续。
 * 去抖/判定仍按 TD_SAMPLE_MS(5ms) 采样，不受影响。 */
#define TD_CTRL_MS          25U     /* 每 25ms 走一步（原 100ms；细分 4 倍） */

/* ---- 追踪速度：每拍位移（us/拍）----
 * 与 TD_CTRL_MS 一起决定巡航速度：yaw 4us/25ms = 16us/100ms ≈ 原 15us/100ms。
 * 想更跟手就调大（可串口 TRK:SPD 实时改）。 */
#define TD_YAW_STEP_US      4U      /* YAW 每拍位移（原 15us/100ms） */
#define TD_PITCH_STEP_US    3U      /* PITCH 每拍位移（原 10us/100ms；行程更小） */
#define TD_CENTER_STEP_US   5U      /* 丢失回中步长（两轴共用） */

/* ---- ★ 丝滑化（2026-10）：加速度斜坡 + 死区滑行 ---- */
#define TD_ACCEL_US         2U      /* 每拍速度增量：0→满速只要 2 拍（起步/反向不"弹射"）*/
#define TD_GLIDE_US         28U     /* YAW 到位后的滑行总量（≈2.5°，用减速段收尾，不再硬停）*/
#define TD_GLIDE_PITCH_US   22U     /* PITCH 滑行总量（行程更小，按比例给）*/
#define TD_GLIDE_TICKS      14U     /* 滑行分几拍走完（14 × 25ms = 350ms 的减速尾巴）*/

/* ---- ★ 丢失后的"扫一圈"（2026-10）----
 * 丢失目标 → 保持 / 慢搜 / 缓慢回中，一直撑到 TD_LOST_SWEEP_MS 还没有信号，
 * 就主动左右扫一圈找目标；一圈扫完仍然什么都没有 → `track_dual_sweep_done()` 置位，
 * 由状态机决定退出跟随（比"丢 3s 就退"从容得多）。 */
#define TD_LOST_SWEEP_MS    5000U   /* 丢失这么久后开始"扫一圈"（原 3s 直接退，太急）*/
#define TD_SWEEP_STEP_US    15U     /* 扫视步长（比追踪慢，看得清；一圈约 4~5s）*/

/* ---- 目标丢失分档（ms） ---- */
#define TD_LOST_HOLD_MS     200U    /* < 200ms：区域过渡，保持不动 */
#define TD_LOST_SEARCH_MS   1000U   /* 200ms~1s：按最后方向慢速搜；> 1s：缓慢回中位 */

/* ---- 调试打印 ---- */
#define TD_PRINT_MS         1000U   /* 无变化时每 1s 打一行心跳 */

/* ==================== 供情感状态机调用（非阻塞接口）====================
 *
 * 设计意图：红外跟随是**情感状态机的一个"互动模式"**，由特定指令呼起、
 * 退出后交还动作库（movements/）。因此跟踪不能是死循环，必须能被状态机
 * 在主循环里"推着走"：
 *
 *     track_dual_init();                     // 进入跟随模式时调一次
 *     while (mode == MODE_TRACK) {
 *         track_dual_step();                 // 每 TD_SAMPLE_MS(5ms) 调一次
 *         mdelay(TD_SAMPLE_MS);
 *         if (track_dual_sweep_done()) {     // 丢失 5s → 自动扫一圈；扫完仍没目标才退
 *             mode = MODE_IDLE; break;
 *         }
 *     }
 *     track_dual_exit();                     // 退出：两轴回中并复位内部状态
 *
 * ⚠️ 与动作库的关系：本文件与 movements/ 都会写舵机，**不能并行**。
 *    进入跟随前先 moves_interrupt() 停掉正在播的动作；退出后再交还动作库。
 *    模式互斥是状态机的职责（见 UserFolder/doc/README.md §5.3）。
 */
void track_dual_init(void);                  /* 进入：引脚 + 舵机初始化 + 复位内部状态 */
void track_dual_step(void);                  /* 推进一次（采样/去抖/按节拍步进/打印） */
void track_dual_exit(void);                  /* 退出：两轴回中位 + 复位内部状态 */
uint32_t track_dual_lost_ms(void);           /* 当前"无目标"持续时长（ms），供状态机判断自动退出 */
uint8_t  track_dual_ready(void);             /* 1 = init 全部走完（0 = 跟随循环不会跑）*/

/* 自测入口：init + 死循环 step（main.c 目前调的仍是它；状态机接入后改调上面三件套） */
void track_dual_test(void);

/* ==================== 运行时调参（串口实时改，不用烧录）====================
 * 状态机在跟随模式下用这些接口现场调手感，改完立即生效（下次 step 起效）。
 * `track_dual_reset_params()` 一键回到上面的编译默认值。 */
void track_dual_set_speed(uint32_t yaw_us, uint32_t pitch_us);  /* 追踪速度（每拍位移 us）*/
void track_dual_set_accel(uint32_t accel_us);                   /* 加速度（每拍速度增量 us）*/
void track_dual_set_glide(uint32_t yaw_us, uint32_t pitch_us, uint16_t ticks); /* 死区滑行量/拍数 */
void track_dual_set_ctrl_ms(uint32_t ms);                       /* 控制周期（默认 25ms）*/
void track_dual_reset_params(void);                             /* 恢复编译默认 */
void track_dual_show_params(void);                              /* 打印当前全部参数 */

/* ==================== 丢失后的"扫一圈"==================== */
/* 扫完一圈仍没找到目标 -> 返回 1（状态机据此退出跟随）；未触发/正在扫 -> 0 */
uint8_t track_dual_sweep_done(void);
/* 调"扫一圈"的参数：丢失多久开始扫（ms）、扫视步长（us/拍）*/
void track_dual_set_sweep(uint32_t wait_ms, uint32_t step_us);

#endif /* _TRACK_DUAL_H_ */
