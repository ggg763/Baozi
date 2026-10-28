/******************************************************************************
 * @file     emotion_manager.h
 * @brief    情绪管理（TD1601 行为层）—— PC 端 emotion/emotion_manager.py 的 C 移植
 *
 * 对齐 PC 架构（Baozi_PC/emotion/）：
 *   PC: EmotionManager.update(emotion)  →  归一化情绪标签 + 更新连续内部状态
 *   C : emo_apply(emo_id_t)            →  归一化（由 emo_parse 完成）+ 更新连续状态
 *
 * 职责（与 PC 完全一致）：
 *   - 保存当前情绪
 *   - 接收 PC 下发的情绪标签（EMO:xxx）
 *   - 把【离散情绪】映射成【连续状态】(happiness / energy / curiosity)
 *
 * 不负责：生成情绪、解析 LLM、驱动硬件（驱动在 emotion_fsm.c / movements/）。
 *
 * ⚠️ 数据类型差异：PC 用 float（0.0~1.0），本工程 minilibc 浮点不可靠（见
 *    UserFolder/doc/README.md §3.7），因此全部改成【千分整数】（0~1000，
 *    1000 等价于 PC 的 1.0）；PC 的 0.9 → 这里的 900。
 ******************************************************************************/
#ifndef _EMOTION_MANAGER_H_
#define _EMOTION_MANAGER_H_

#include <stdint.h>

/* 连续状态量的满量程（千分制）：1000 = PC 端的 1.0 */
#define EMO_SCALE           1000

/* ==================== 基础情绪（与 PC VALID_EMOTIONS 一一对应）====================
 * PC: happy / sad / angry / surprised / loving / neutral
 * 顺序即协议顺序，emotion_fsm 的情绪→动作映射表按同一顺序书写。 */
typedef enum {
    EMO_NEUTRAL = 0,    /* 中立（PC 协议里下发的 "IDLE" 也归到这里）*/
    EMO_HAPPY,          /* 开心 */
    EMO_SAD,            /* 难过 */
    EMO_ANGRY,          /* 生气 */
    EMO_SURPRISED,      /* 惊讶 */
    EMO_LOVING,         /* 爱意 */
    EMO_MAX             /* 非法/结束标记（emo_parse 失败时返回它）*/
} emo_id_t;

/* 连续内部状态（千分制 0~1000），为将来的连续情绪模型留的接口（PC 同款） */
typedef struct {
    int16_t happiness;  /* 愉悦度 */
    int16_t energy;     /* 精力 */
    int16_t curiosity;  /* 好奇 */
} emo_state_t;

/* 情绪名（小写，与 PC 一致）；非法 id 返回 "?" */
const char *emo_name(emo_id_t e);

/* 名字 → 情绪 id（大小写不敏感，接受 PC VALID_EMOTIONS 的 6 个名字）。
 * 不认识返回 EMO_MAX（由调用方决定怎么处理）。 */
emo_id_t emo_parse(const char *name);

/* 更新当前情绪并刷新连续状态（= PC 的 EmotionManager.update()）*/
void emo_apply(emo_id_t e);

/* 当前情绪 */
emo_id_t emo_current(void);
const char *emo_current_name(void);

/* 当前连续状态（值拷贝，调用方不用关心内部存储）*/
emo_state_t emo_state(void);

/* 恢复默认（neutral + 500/500/500，= PC 的 EmotionManager.reset()）*/
void emo_reset(void);

/* ==================== 对外刺激（v2.0 · 合并版新增）====================
 *
 * 设计约定（用户 2026-11 明确要求）：UI 操作与抚摸**不能直接改情绪**，
 * 只能作为"刺激"去微调连续状态（happiness / energy / curiosity），
 * 情绪本身由连续状态越阈值后自然漂移。这样：
 *   · 摸一下就"变开心"不会瞬间跳变，而是攒够愉悦度才漂；
 *   · 后端（PC）下发的 EMO:xxx 仍然可以随时直接覆盖（emo_apply）。
 *
 * 调用方：UserFolder/behavior.c（抚摸手势 / 卡片 UI 的动作与情绪按钮）。
 */
#define EMO_STIM_TOUCH    0     /* 普通点触（每点一下的微量正反馈）*/
#define EMO_STIM_PET      1     /* 被抚摸（屏幕区域连续滑动）*/
#define EMO_STIM_PRAISE   2     /* 被夸奖（UI 按钮）*/
#define EMO_STIM_SCOLD    3     /* 被责备（UI 按钮）*/
#define EMO_STIM_PLAY     4     /* 一起玩（UI 触发动作）*/

/* 施加一次刺激。amount = 强度 1..100（100 = 完整增量）。
 * 返回施加后的当前情绪（可能因跨阈值而漂移，也可能不变）。*/
emo_id_t emo_stimulus(uint8_t kind, uint8_t amount);

#endif /* _EMOTION_MANAGER_H_ */
