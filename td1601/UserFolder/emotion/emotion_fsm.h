/******************************************************************************
 * @file     emotion_fsm.h
 * @brief    情感状态机（TD1601 行为层）—— 情感状态 → 舵机 + 屏幕 的编排层
 *
 * 架构（与 PC 端 Baozi_PC/emotion/ 对齐，但把硬件输出收敛到一处）：
 *
 *   PC 端                                        TD1601 端（本文件）
 *   ─────────────────────────────────────────    ──────────────────────────────────────
 *   EmotionManager   离散情绪 → 连续状态          emotion/emotion_manager.c
 *   MoodStateMachine ACTIVE/IDLE/BORED/SLEEPING   emotion/mood_fsm.c
 *   （两者内部各自调 hardware.set_*()）           ★ 统一收敛到本文件：选动作 + 调参数
 *                                                  ↓
 *                                  ┌───────────────┴───────────────┐
 *                                  ▼                               ▼
 *                    movements/ 动作库（舵机）            anim/face_ui（屏幕表情）
 *                                  ▼
 *                    servo_test/servo_core.c（限位 + PWM）
 *
 *      PC / ESP32 只下发【语义】（EMO:HAPPY / INTENT:CHAT / TRACK:ON）
 *            │  UART0 115200（本文件是唯一串口所有者）
 *            ▼
 *   ┌────────────────────────────────────────────┐
 *   │ emotion_fsm                                │
 *   │  ① 解析命令 → emotion_manager / mood_fsm    │
 *   │  ② 由主循环推进心情计时（降级阈值见 mood_fsm.h：60s/180s）│
 *   │  ③ (mood, emotion) → 动作 ID（语义层）       │
 *   │  ④ (happiness, energy, curiosity) → 增益     │
 *   │     （强度层，调制动作幅度/速度）             │
 *   └────────────────────────────────────────────┘
 *
 * ⚠️ 舵机互斥（**已实现**）：本文件与红外跟随（tracking/track_dual.c）都会写舵机，
 *    用【模式】互斥，同一时刻只有一个在写：
 *
 *      EMO_MODE_FSM   : 情绪动作（movements）+ 屏幕表情（face_ui）—— 默认
 *      EMO_MODE_TRACK : 红外跟随独占舵机（track_dual）+ 屏幕表情继续跑
 *
 *    切换：串口 `TRACK:ON` / `TRACK:OFF`（裸名 `track` / `follow` / `stop`）；
 *          收到任何 EMO: / INTENT: 命令会**自动退出跟随**（那是"要它做表情动作"）。
 *    自动退出：跟随中丢失目标超过 3s（`track_dual_lost_ms()`）。
 *    进跟随前 moves_interrupt() 让动作库停手；退出时 track_dual_exit() 回中并交还。
 ******************************************************************************/
#ifndef _EMOTION_FSM_H_
#define _EMOTION_FSM_H_

#include <stdint.h>
#include "movements/moves.h"                /* move_id_t */
#include "emotion/emotion_manager.h"        /* emo_id_t */
#include "emotion/mood_fsm.h"               /* mood_id_t */

/* 状态机主循环节拍（ms）—— 10ms 足够跟得上串口命令与空闲降级判断。
 * 跟随模式（MODE_TRACK）内部会切到 TD_SAMPLE_MS(5ms)，因为 track_dual_step()
 * 按 5ms 累加采样/去抖/控制节拍。 */
#define EMO_FSM_TICK_MS     10U

/* 初始化：动作库（含舵机）+ 情绪管理 + 心情状态机 + 屏幕表情 + 总线仲裁 + 播放期钩子。
 * 返回 0 成功。 */
int emotion_fsm_init(void);

/* 推进一次：轮询串口命令 → 推进心情计时 → 按模式推进
 *   · FSM  模式：选动作 → 播一个周期（阻塞；期间靠 tick 钩子保持响应）
 *   · TRACK 模式：track_dual_step() + face_ui_tick()，不碰动作库
 * 【非阻塞】：两种模式下命令都随时可打断/切换。由主循环按 EMO_FSM_TICK_MS(或
 * 跟随时的 TD_SAMPLE_MS) 反复调用。 */
void emotion_fsm_step(void);

/* 自测入口：init + 死循环 step（src/main.c 调用；不返回）*/
void emotion_fsm_run(void);

/* ==================== 供测试/其它入口复用的接口 ====================
 * UserFolder/test/emotion_test.c（串口交互测试）用的就是这三个。这样设计的用意是：
 * 测试与正式运行【共用同一份】协议解析、动作映射与调制算法 —— 测出来的行为就是上机
 * 跑的行为，不会出现"测试通过、正式不一样"的两套逻辑。 */
void      emotion_fsm_feed_line(const char *line);               /* 喂一行命令（EMO:/INTENT:/TRACK:/裸名）*/
move_id_t emotion_fsm_move_for(mood_id_t mood, emo_id_t emo);    /* 语义层：(心情,情绪) → 动作 ID */
void      emotion_fsm_apply_gain(mood_id_t mood, emo_id_t emo);  /* 强度层：把调制增益写进动作库 */

/* 下发一个【显式情绪/心情】名字（= EMO:xxx 的值部分，大小写不敏感）：
 *   "happy"/"sad"/"angry"/"surprised"/"loving"/"neutral" → 改情绪（emo_apply）
 *   "idle"/"bored"/"sleeping"(= "sleep"/"asleep")        → 改心情（mood_fsm）
 *   其它                                                 → 归一化为 neutral
 * 会先退出红外跟随、清掉上一次 INTENT，下一拍由状态机同步到屏幕与舵机。
 *
 * ★ 协议/ESP32 链路必须走这条，不能只调 ui_app_force_emotion()：
 *   后者只把一张脸画上屏，情绪层不变 —— 而 emotion_fsm_step() 每拍都会用
 *   emo_current_name() 重新同步屏幕，于是"闪一下目标表情又被拉回原情绪"
 *   （触摸手势用 emo_force 定过的 loving/angry 尤其明显）。 */
void      emotion_fsm_command_emotion(const char *value);

/* 当前运行模式：0 = 情绪动作（FSM），1 = 红外跟随（TRACK）。
 * 切换用串口命令 TRACK:ON / TRACK:OFF；本函数供测试/上位机查询。 */
uint8_t   emotion_fsm_mode(void);

/* ==================== 合并版新增（v2.0 · 卡片 UI / 安全抑制）====================
 *
 * 这三条接口是给"屏幕 UI 层"用的，让 UI 既能驱动行为、又不会绕过安全边界：
 *
 *   emotion_fsm_set_inhibit_cb(cb)
 *       app 层注册一个"该停手了吗"的查询（手指按在屏上；红外近距那条
 *       距离保护已按用户要求关闭，见 behavior.c 的 BEHAVIOR_IR_INHIBIT）。
 *       返回 1 时本层立刻 moves_interrupt()，且本拍不启动任何新动作 —— 包括
 *       红外跟随模式。cb 传 0 取消注册。
 *
 *   emotion_fsm_request_action(move_id)
 *       UI 按钮请求播一个动作（取值见 movements/moves.h 的 move_id_t）。
 *       请求进入槽位，下一拍由本层按正式流程播放（限位、变体池、增益调制、
 *       总线仲裁全部照旧）；播放是阻塞的，期间仍可被触摸/近距打断。
 *
 *   emotion_fsm_stimulus(kind, amount)
 *       "影响情绪"而不是"直接改情绪"：只微调连续状态（happiness/energy/
 *       curiosity），情绪由阈值漂移决定（见 emotion/emotion_manager.h 的
 *       EMO_STIM_*）。同时视为一次互动（打断无聊/睡眠）并同步屏幕表情。
 *
 *   emotion_fsm_note_interaction()
 *       上报一次"人类交互"（触摸屏上任何操作）。只重置心情计时 ——
 *       3 分钟没人碰才会进 SLEEPING（阈值见 mood_fsm.h）。睡着时被摸会唤醒。
 *
 *   emotion_fsm_request_track(on)
 *       请求切换红外跟随（on=1 进 / 0 退）。UI 的 PLAY 页（第 4 页）开关调它，
 *       和 emotion_fsm_request_action() 同一条纪律：只排队，下一拍由本层按正式
 *       流程切模式 —— moves_interrupt() / track_dual_init() 只能在状态机自己
 *       那一拍里做，不能从触摸分支（可能被 moves 钩子重入）直接调。
 */
void emotion_fsm_set_inhibit_cb(int (*cb)(void));
void emotion_fsm_request_action(uint8_t move_id);
void emotion_fsm_stimulus(uint8_t kind, uint8_t amount);
void emotion_fsm_note_interaction(void);
void emotion_fsm_request_track(int on);

#endif /* _EMOTION_FSM_H_ */
