/******************************************************************************
 * @file     demo_show.h
 * @brief    一键演示：把"屏幕能演什么 + 脖子能演什么"整套跑一遍
 *
 * 动机：答辩/展示时不可能让用户先去摸脸颊、再去点按钮、再滑到某一页才看到
 *       一个功能。需要**一个按钮把全部能力按顺序演一遍**，并且每一步开始时
 *       由喇叭先说清楚"现在演的是什么"（板子没有喇叭，只能请 PC 代播）。
 *
 * ★ 链路（三端接力，与 voice_link 同一套路）：
 *
 *      [TD1601]                     [PC]                      [ESP32]
 *   点 DEMO 页按钮
 *        │
 *        ├─ printf "DEMO:HAPPY:1" ──► 抓到这一行
 *        │                            │
 *        │                            ├─ 读本地预合成 WAV（assets/demo/HAPPY.wav）
 *        │                            └─ PCM ──推流──► 喇叭播报："开心的时候…"
 *        │                            │
 *        ├─（等着播报完，脸上先摆"在听"的表情）
 *        │                            └─ 播完回一行 "DEMORUN"
 *        ◄───────────────────────────────┘
 *        └─ 表情切到 happy + 请求 MOVE_HAPPY（走 emotion_fsm 正式路径）
 *
 *   板子不依赖 PC 也能演：等不到 DEMORUN 就按 DEMO_WAIT_DEFAULT_MS 自己往下走，
 *   只是没有语音。PC 也可以主动发 DEMOGO 让它开演（串口助手就能触发）。
 *
 * 协议（纯 ASCII 文本行，\r\n 结尾，与 printf 日志共线，按前缀过滤）
 *
 *   上行（板 → PC）：
 *     DEMO:<NAME>:<IDX>   请求播报第 IDX 步；板子随后进入等待
 *     DEMOEND             整套演示自然结束
 *     DEMOABORT           被用户（点屏幕）或命令打断
 *
 *   下行（PC → 板）：
 *     DEMOGO                开始整套（等价于在 DEMO 页点 START）
 *     DEMOQUIT              立刻停止
 *     DEMORUN               播报放完了，现在开始演这一步
 *     DEMOWAIT:<ms>         播报大概这么长（板子把等待窗口放宽，上限 12s）
 *
 * ⚠️ NAME 与 PC 侧 demo_voice.py 的 DEMO_SCRIPTS 必须一一对应：
 *    PC 是靠这个名字去找 assets/demo/<NAME>.wav 的，改一边就要改另一边。
 ******************************************************************************/
#ifndef _DEMO_SHOW_H_
#define _DEMO_SHOW_H_

#include <stdint.h>

/* 演示一共几步（= k_steps[] 的长度）*/
#define DEMO_STEPS          10

/* 等 PC 播报的默认窗口。PC 没接 / 没这个音频文件时，板子自己往下走，
 * 只是没有语音。刻意做得短：没人接 PC 时整套演示不该在这里干等。 */
#define DEMO_WAIT_DEFAULT_MS    2500U
/* DEMOWAIT 能放宽到的上限：PC 说 30 秒也只等 12 秒，防止一行坏数据把演示卡死 */
#define DEMO_WAIT_MAX_MS       12000U

/* 开始整套演示（DEMO 页的 START 按钮 / 串口 DEMOGO 都走这里）。
 * 已经在演示中则忽略（连点不会重头再来）。 */
void demo_show_start(void);

/* 立刻停止（用户点屏幕 / 串口 DEMOQUIT）。不在演示中则无动作。 */
void demo_show_stop(void);

/* 1 = 正在演示 */
int  demo_show_running(void);

/* 当前第几步（0..DEMO_STEPS-1）；没在演示时返回 -1 */
int  demo_show_index(void);

/* 当前这一步的名字（"HAPPY"…）；没在演示时返回 0 */
const char *demo_show_name(void);

/* 每拍推进（由 ui_app_tick 调，dt 是真实毫秒）。
 * 表情与动作都不在这里直接写硬件：表情走 face_app 的临时表情层，
 * 动作走 behavior_request_move() → emotion_fsm，与手动点按钮完全同一条路径。 */
void demo_show_tick(uint32_t dt_ms);

/* 串口命令拦截：认得这行就吃掉并返回 1（app_core 里放在 emotion_fsm 之前）。
 * 返回 0 = 不是演示的命令，交给别人。 */
int  demo_show_handle_line(const char *line);

/* ---- 卡片层第 7 页（DEMO 页）----
 * 与 play_card / face_picker 同款契约：外框由 ui_app 画好，这里只画内容。 */
void demo_card_draw(void);

/* 点击命中：1 = 点在 START/STOP 按钮上，0 = 没点中。
 * 只判命中、不做动作 —— "先收卡片还是先开演"这个顺序只有 ui_app 知道，
 * 由它拿到 1 之后再调 demo_show_start() / demo_show_stop()。 */
int  demo_card_tap(int x, int y);

#endif /* _DEMO_SHOW_H_ */
