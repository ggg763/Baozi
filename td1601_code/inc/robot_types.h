/**
 * robot_types.h - TD1601 机器人公共类型定义
 *
 * 架构定位：TD1601 是机器人的"行为层"核心控制器。
 * PC 端只下发"语义状态"（emotion / intent），
 * 具体行为（表情、动作、灯光）由 TD1601 自主决策。
 *
 * 本头文件定义全工程共享的枚举与数据结构，不依赖任何硬件。
 */

#ifndef ROBOT_TYPES_H
#define ROBOT_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 情绪状态（Emotion State）
 *
 * 来源：
 *   1. PC 端通过 UART 下发（EMO:HAPPY 等轻量协议，或 JSON）
 *   2. TD1601 本地传感器触发（触摸 → LOVING）
 *   3. TD1601 空闲超时自动降级（IDLE → BORED → SLEEPING）
 * ================================================================== */
typedef enum {
    EMO_IDLE = 0,      /**< 初始/待机基准态 */
    EMO_HAPPY,         /**< 开心 */
    EMO_SAD,           /**< 难过 */
    EMO_ANGRY,         /**< 生气 */
    EMO_SURPRISED,     /**< 惊讶 */
    EMO_LOVING,        /**< 亲昵/爱意 */
    EMO_BORED,         /**< 无聊（TD1601 空闲超时自触发） */
    EMO_SLEEPING,      /**< 睡眠（TD1601 空闲超时自触发） */
    EMO_COUNT          /**< 状态总数（哨兵，勿作真实状态使用） */
} EmotionState;

/* ==================================================================
 * 意图（Intent）
 *
 * PC 端 LLM 语义理解的可选输出，辅助 TD1601 决定互动节奏。
 * 当前为预留，TD1601 至少应响应 emotion。
 * ================================================================== */
typedef enum {
    INTENT_NONE = 0,   /**< 无特别意图 */
    INTENT_CHAT,       /**< 闲聊 */
    INTENT_GREETING,   /**< 问候 */
    INTENT_QUESTION,   /**< 提问/求助 */
    INTENT_COMMAND,    /**< 命令式（如"转个头"） */
    INTENT_COUNT
} IntentType;

/* ==================================================================
 * LED 颜色（RGB 各 8bit，0-255）
 * ================================================================== */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RgbColor;

/* ==================================================================
 * 舵机角度/动作参数
 * ================================================================== */
#define SERVO_HEAD_HORIZONTAL   (1)   /**< 舵机1：头部水平（左右） */
#define SERVO_HEAD_VERTICAL     (2)   /**< 舵机2：头部垂直（俯仰） */
#define SERVO_ANGLE_MIN         (0)
#define SERVO_ANGLE_MAX         (180)
#define SERVO_CENTER            (90)

/* 舵机缓动（easing）曲线类型 */
typedef enum {
    EASE_LINEAR = 0,   /**< 匀速 */
    EASE_IN_OUT,       /**< 缓入缓出（默认，动作更自然） */
    EASE_STEP          /**< 瞬间到位 */
} EasingType;

/* ==================================================================
 * 动作请求（Behavior 层 → Motion 层）
 * ================================================================== */
typedef struct {
    uint8_t  servo_id;             /**< 舵机编号 */
    uint8_t  target_angle;         /**< 目标角度 0-180 */
    uint16_t duration_ms;          /**< 动作时长（毫秒） */
    EasingType easing;             /**< 缓动方式 */
} ServoMoveCmd;

/* ==================================================================
 * 通信协议结构（UART 接收后解析出的消息）
 * ================================================================== */
typedef struct {
    bool     has_emotion;          /**< 本次消息是否携带 emotion */
    EmotionState emotion;          /**< 情绪（has_emotion 为 true 时有效） */
    bool     has_intent;           /**< 本次消息是否携带 intent */
    IntentType intent;             /**< 意图（has_intent 为 true 时有效） */
} PcMessage;

#ifdef __cplusplus
}
#endif

#endif /* ROBOT_TYPES_H */
