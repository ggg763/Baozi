/******************************************************************************
 * @file     face_ui_card.c
 * @brief    face_ui.h 的【卡片 UI 版】实现 —— 把情感状态机的屏幕输出接到 ui_app
 *
 * 背景（为什么需要这个文件）：
 *   本机工程原来有两套互斥的屏幕驱动：
 *     · UserFolder/anim/face_emo_test.c  自带主循环 + 自己读串口，提供 face_ui_*
 *       （emotion_fsm 调的就是它）
 *     · UserFolder/ui_app.c（合并自 LCD 栈）卡片 UI + 参数化脸，自带触摸/分片刷屏
 *   二者都调 face_init()/眨眼状态机，只能留一个。合并后屏幕归 ui_app 所有，
 *   所以【不再编译 face_emo_test.c】，改由本文件实现同一组 face_ui_* 符号：
 *
 *     face_ui_init()          → 屏幕已由 app_core_init() 初始化好；这里只兜底
 *     face_ui_tick()          → 调 app_core_step()：动作播放期间仍推进屏幕与触摸
 *     face_ui_emotion(name)   → ui_app_set_emotion()（卡片层里只记录不抢屏）
 *     face_ui_mood(name)      → 心情优先映射（sleeping→asleep / bored→neutral）
 *     face_ui_intent(name)    → 有专用表情就切（think / listen / question / greet），
 *                               没有返回 -1 让 emotion_fsm 回落情绪表情
 *
 * 注意（2026-11 扩展）：表情现在是"pose = 模型 + 动作"，实际由 anim/face_app.c
 * 渲染（切表情走 300ms 参数过渡，表情自己会动）。本文件只做"转发 + 意图判定"。
 *
 * 于是 emotion_fsm.c 一行都不用改语义：它以为在驱动一块"只有脸"的屏，
 * 实际屏幕是"脸 + 可触摸的卡片层"。
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "face_ui.h"        /* UserFolder/anim/face_ui.h */
#include "ui_app.h"         /* ui_app_set_emotion / ui_app_ready / ui_app_init */
#include "app_core.h"       /* app_core_step */
#include "emotion/emotion_manager.h"   /* emo_state / EMO_STIM_* */

/* 表情控制层（anim/face_app.c）—— 与 ui_app.c 一样用 extern 声明，
 * 用来查"这个意图/名字有没有专用表情"。 */
extern int  face_app_has_pose(const char *name);
/* 情绪 + 强度 -> 具体表情的挑选在 face_app.c 里做（表在那边），
 * 这里只负责把连续状态推过去 —— anim 层因此不必 include emotion 层。 */
extern void face_app_set_state(uint16_t happiness, uint16_t energy, uint16_t curiosity);
extern int  face_app_play(const char *name, uint32_t ms);

/* 把当前连续状态推给表情层。每次切换表情前都要推：
 * 同一情绪在不同强度下是【不同的脸】（happy 攒够 -> 大笑，sad 掉下去 -> 哭）。*/
static void push_state(void)
{
    emo_state_t st = emo_state();

    face_app_set_state((uint16_t)st.happiness, (uint16_t)st.energy,
                       (uint16_t)st.curiosity);
}

/* 当前情绪（心情只是"临时覆盖"，回 active/idle 时用回它）*/
static const char *s_emo = "neutral";

void face_ui_init(void)
{
    /* 正常路径：app_core_init() 已经跑过 ui_app_init()。
     * 兜底路径：有人（旧测试入口）直接调 emotion_fsm_init()，屏幕还没初始化。 */
    if (!ui_app_ready()) {
        printf("[fui] lazy ui_app_init()\r\n");
        ui_app_init();
    }
    printf("[fui] screen owner = ui_app (cards + parametric face)\r\n");
}

void face_ui_tick(void)
{
    /* 这就是"动作在播、屏幕别冻住"的那个钩子：moves 播放期间每 20ms 回调一次。
     * app_core_step() 内部按真实时间算 dt，与主循环共用不会重复计时。 */
    app_core_step();
}

int face_ui_emotion(const char *name)
{
    if (name == 0 || name[0] == 0) {
        return -1;
    }
    s_emo = name;                   /* emotion_manager 给的是静态串，可长期持有 */
    push_state();
    ui_app_set_emotion(name);
    return 0;
}

int face_ui_mood(const char *name)
{
    if (name == 0) {
        return -1;
    }
    push_state();
    if (strcmp(name, "sleeping") == 0) {
        ui_app_set_emotion("asleep");
    } else if (strcmp(name, "bored") == 0) {
        /* v9 起有专门的"无聊"脸（v9 之前降级成 neutral）。
         * face_app 会在此基础上每隔 9~18s 自动冒一个哈欠。 */
        ui_app_set_emotion("bored");
    } else {
        ui_app_set_emotion(s_emo);  /* active / idle：回到当前情绪 */
    }
    return 0;
}

/* 意图专用表情（think / listen / question / greet）。
 * 返回 0 = 有该表情并已切换；-1 = 没有 —— emotion_fsm 据此回落到情绪表情。
 * 名字来自 emotion_fsm 的 s_intent[]（静态缓冲，满足 ui_app_set_emotion
 * 对"指针长期有效"的要求）。
 *
 * ⚠️ 只能走 ui_app_set_emotion()：它在卡片层/番茄钟里只记录不刷屏。
 * 直接调 face_app_* 会绕过这层判断，把正在看的卡片界面覆盖掉。 */
int face_ui_intent(const char *name)
{
    if (name == 0 || name[0] == 0) {
        return -1;
    }
    {
        /* 意图名（PC 协议）→ 表情名。以前这里是直接拿 PC 的名字去查表，
         * 于是 INTENT:GREETING 永远查不到（表里叫 greet），白白回落到情绪。
         * 协议名与美术命名解耦，映射只放这一处。 */
        static const struct {
            const char *intent;
            const char *pose;
        } map[] = {
            { "chat",     "listen"   },   /* 在跟你说话：安静地听 */
            { "greeting", "wink"     },   /* 打招呼：眨个眼比咧嘴笑更像"认识你" */
            { "greet",    "wink"     },
            { "question", "question" },   /* 疑问：偏头瞪眼 */
            { "command",  "think"    },   /* 收到指令：先"思考"一下 */
            { "listen",   "listen"   },
            { "think",    "think"    },
            { "look",     "look"     },
            { 0, 0 }
        };
        int i;
        const char *pose = name;

        for (i = 0; map[i].intent != 0; i++) {
            const char *a = map[i].intent;
            const char *b = name;
            while ((*a != '\0') && (*b != '\0')) {
                char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
                if (ca != cb) {
                    break;
                }
                a++;
                b++;
            }
            if (*a == '\0' && *b == '\0') {
                pose = map[i].pose;
                break;
            }
        }
        if (!face_app_has_pose(pose)) {
            return -1;              /* 没有专用表情：调用方回落到情绪 */
        }
        push_state();
        ui_app_set_emotion(pose);
    }
    return 0;
}

/* 刺激 -> 一次性表情（夸奖/责备/玩耍）。
 * 与"情绪表情"不冲突：先改连续状态（可能漂移情绪），再盖 2 秒的瞬时表情，
 * 到期自动回落到新情绪。抚摸不在这里 —— 它需要"每次移动续期"，
 * 由 ui_app.c 的抚摸判定自己调 face_app_play。 */
void face_ui_reaction(uint8_t kind)
{
    switch (kind) {
    case EMO_STIM_PRAISE:  (void)face_app_play("smug", 2000);     break;  /* 被夸：得意 */
    case EMO_STIM_SCOLD:   (void)face_app_play("nervous", 2000);  break;  /* 被凶：紧张 */
    case EMO_STIM_PLAY:    (void)face_app_play("mischief", 2200); break;  /* 一起玩：坏笑 */
    default: break;
    }
}
