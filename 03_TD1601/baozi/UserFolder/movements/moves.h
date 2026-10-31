/******************************************************************************
 * @file     moves.h
 * @brief    动作库：把"情感状态 → 舵机动作"的代码集中到 movements/ 文件夹
 *
 * 设计：
 *   - moves.c  提供【轨迹原语】（平滑插值移动）+ 动作注册表 + 播放引擎 + 可打断
 *   - moves_xxx.c  每个状态一个文件，用轨迹原语写该状态的动作（当前已实现 IDLE）
 *   后续再加状态时：新建 moves_<状态>.c，在里面实现 moves_<状态>_run()，并在
 *   moves.c 的 s_moves[] 表里登记一行即可（见 movements/README.md）。
 *
 * 语义（重要）：这两个舵机就是机器人的【脖子】——
 *   YAW   = 脖子左右转（转头 / 摇脑袋）
 *   PITCH = 脖子抬头 / 低头（点头）
 *   所有动作读起来都是"转头多少度、点头多少"的拟人表达，而不是冷冰冰的脉宽。
 *
 * 坐标约定：所有动作都用【相对中位的偏移 us】表达（yaw_off / pitch_off），
 *   正负号含义（实测）：脉宽增大 → YAW 右转 / PITCH 抬头。
 *   动作代码不需要自己关心绝对脉宽 —— moves.c 会加上 servo_core.h 里的实测中位，
 *   并走 servo_set_pulse() 的限位钳制（YAW 300~1440 / PITCH 800~1320，2026-11 收窄正向），
 *   超范围自动截断。
 ******************************************************************************/
#ifndef _MOVES_H_
#define _MOVES_H_

#include <stdint.h>

/* ==================== 待机（IDLE）"缓慢巡视四周" 参数 ====================
 * 2026-09 改版：原来的"V 字摇摆点头"（±80us、300ms 一步）节奏偏快、幅度偏小，
 * 看着像在"抖"，不像在"看"。现在待机动作改成【缓慢巡视】：幅度开到 ±300us
 * （原来的近 4 倍），每段 1.5~7s（原来的十几倍），中途会在两侧停住张望。
 *
 * 待机有 3 个变体（见 moves_idle.c），呼出时随机挑一个（moves_play_variant），
 * 避免"每 20 秒一模一样地重播"。三个变体的"性格"：
 *   变体1 缓慢环视   —— 慢慢从左扫到右，两侧各停一下           ≈20s
 *   变体2 远望俯视   —— 抬头看远、左右张望、低头看桌面        ≈23s
 *   变体3 扁 8 字巡游 —— 用 move_figure8 慢慢画两圈扁 8 字     ≈18s
 * 幅度还会再乘【整体基准 1.2】与【调制增益】（amp 700~1200，中性再 ×1.15 好奇系数）：
 *   ±300 × 1.2 × 1.38 ≈ ±497us → YAW 503~1497us（中位 1000），
 *   距实测上限 1700 仍有约 200us 余量。
 *   【2026-10-31 备注】当天曾把限位收到 1440/1320，这套幅度立刻顶死（卡顿+堵转），
 *   已回退。apply_off() 仍会留 10us 余量并打 CLIP 告警，是给"以后又有人想收限位"兜底的。
 *   【2026-11 现状】正向限位确实收到了 1440/1320，但这次按【各情绪实际 amp】重算过：
 *   待机巡视 1393、开心 1324、八字 1337 都还在 1440 以内，唯一贴顶的是下面这个
 *   SURPRISED_PITCH_UP（180×1.32=238 → 1337 > 1320，会被截到 1310）。
 *   所以"收到 1440/1320 就一定顶死"这个说法不成立 —— 关键是算的时候别拿待机那个
 *   1.38 当所有情绪的系数（curiosity 只乘在待机动作上，而待机动作幅度本来就小）。*/

/* ---- 变体1「缓慢环视」---- */
#define IDLEP1_YAW          300     /* 左右张望幅度（us，±）—— 2026-09 由 80 开到 300 */
#define IDLEP1_PITCH_UP      25     /* 张望时略抬头（us）*/
#define IDLEP1_PITCH_DOWN    15     /* 扫过时略低头（us，向下，让弧线自然）*/
#define IDLEP1_SWEEP_MS     4000U   /* 第一次扫到一侧的时间 */
#define IDLEP1_CROSS_MS     7000U   /* 从一侧慢慢扫到另一侧（最慢的一段）*/
#define IDLEP1_HOLD_MS      1500U   /* 到一侧后停住张望 */
#define IDLEP1_RETURN_MS    3000U   /* 收尾缓慢回中 */

/* ---- 变体2「远望与俯视」---- */
#define IDLEP2_YAW          280     /* 左右张望幅度（us，±）*/
#define IDLEP2_PITCH_UP     130     /* 抬头看远处（us，向上）*/
#define IDLEP2_PITCH_SIDE    60     /* 侧看时保持的抬头量（us）*/
#define IDLEP2_PITCH_DOWN   140     /* 低头看桌面（us，向下）*/
#define IDLEP2_LOOK_MS      2500U   /* 抬头/低头的时间 */
#define IDLEP2_SIDE_MS      3500U   /* 侧看的时间 */
#define IDLEP2_HOLD_MS      1200U   /* 每个姿势停住的时间 */
#define IDLEP2_RETURN_MS    2800U   /* 收尾回中 */

/* ---- 变体3「扁 8 字巡游」----
 * 用动作库的曲线原语 move_figure8（yaw=A·sinθ、pitch=B·sin2θ）慢慢画∞：
 * 视野像"扫过四周一圈"，比直线来回更自由。第二圈更慢、上下更开。 */
#define IDLEP3_YAW_AMP      280     /* 第一圈横向半宽（us）*/
#define IDLEP3_PITCH_AMP     70     /* 第一圈纵向半高（us）*/
#define IDLEP3_LAP_MS       5500U   /* 第一圈用时 */
#define IDLEP3_HOLD_MS      2000U   /* 两圈之间停一下 */
#define IDLEP3_YAW_AMP2     200     /* 第二圈横向半宽（us）*/
#define IDLEP3_PITCH_AMP2   110     /* 第二圈纵向半高（us）*/
#define IDLEP3_LAP2_MS      6500U   /* 第二圈用时（更慢）*/
#define IDLEP3_RETURN_MS    2000U   /* 收尾回中 */

/* ==================== 开心（HAPPY）"摇头晃脑" 参数 ====================
 * 轨迹：先扬头 → 左右摆动 4 次（每次越到一侧时脑袋高度不同 → 晃出波浪感）
 *       → 抬头定格（得意）→ 回中
 *
 * 【2026-09 修正"看起来幅度偏小"】根因不是名义幅度，而是舵机【没走到位就反向】
 * （开环舵机：脉宽一换向就反向，上一段没走完就被"削平"）。三处修正：
 *   ① 放慢摆动 SWING_MS 160→220ms（需求速度 3.3 → 2.6us/ms；舵机极限约 6.7）
 *   ② 每个极点加 SETTLE_MS(40ms) 停顿，让这一侧真正走完再反向
 *   ③ 摆幅 YAW 220→240
 * 需求速度 = 2×240×1.2(MOVE_AMP_BASE) / 220ms ≈ 2.6us/ms。
 * 幅度仍在实测行程内：YAW 1000 ±240×1.2×1.125 ≈ ±324 → 676~1324。 */
#define HAPPY_YAW_SWING     240     /* 左右摆幅（us，±）—— 2026-09 由 220 加大 */
#define HAPPY_PITCH_HI      80      /* 摆动高点（抬头，us） */
#define HAPPY_PITCH_LO      40      /* 摆动低点（us） */
#define HAPPY_LIFT_MS       150U    /* 起始扬头时间 */
#define HAPPY_SWING_MS      220U    /* 单次摆动时间（2026-09 由 160 放慢：给舵机到位的时间）*/
#define HAPPY_SETTLE_MS      15U    /* 每个极点停顿：等舵机到位再反向
                                     * （2026-11 由 40 缩到 15：梯形剖面已让端点速度为 0，
                                     *   再加 40ms 硬停只剩"到点站住"的顿感）*/
#define HAPPY_HOLD_MS       250U    /* 结尾抬头定格（得意） */
#define HAPPY_RETURN_MS     200U    /* 回中时间 */

/* ==================== 八字环绕（EIGHT）参数 ====================
 * 脖子画一个横 8 字（∞）：左右转一整圈（sin θ），同时上下点两圈（sin 2θ）：
 *     yaw   = EIGHT_YAW_AMP   × sin(θ)
 *     pitch = EIGHT_PITCH_AMP × sin(2θ)
 * 两者合成就是一笔横着的 ∞。幅度（±250us 宽 / ±120us 高）都在实测行程内。 */
#define EIGHT_YAW_AMP       250     /* 8 字的横向半宽（us）*/
#define EIGHT_PITCH_AMP     120     /* 8 字的纵向半高（us）*/
#define EIGHT_LAP_MS        1800U   /* 画一圈 8 字的时间 */
#define EIGHT_LAPS          3U      /* 一次动作画几圈 */
#define EIGHT_ENTER_MS      200U    /* 起笔前回中时间 */
#define EIGHT_RETURN_MS     200U    /* 收笔回中时间 */

/* ==================== 难过（SAD）"低头 + 缓慢左右摇" 参数 ====================
 * 轨迹：慢慢低头 → 在低位左右各摆一次（每段行程 2×摆幅）→ 回中
 * 节奏明显比 IDLE/HAPPY 慢，读起来就是"垂头丧气地晃"。 */
#define SAD_PITCH_DOWN      150     /* 低头量（us，向下）*/
#define SAD_YAW_SWING       70      /* 左右摆幅（us，±）*/
#define SAD_DOWN_MS         500U    /* 低头时间 */
#define SAD_SWAY_MS         700U    /* 单次摆动时间 */
#define SAD_RETURN_MS       400U    /* 回中时间 */
#define SAD_HOLD_MS         400U    /* 结尾停顿 */

/* ==================== 生气（ANGRY）"昂头 + 快速甩头" 参数 ====================
 * 和 SAD 的慢相反：抬头挺着 + 左右甩头，幅度大、时间短。
 *
 * 【2026-09 两轮修正】
 *   ① 摆幅 110 → 265（上机看"甩头不够狠"）
 *   ② 修"没甩到位就反向"：原来 90ms/段 = 需求速度 240×1.2×2/90 ≈ 6.4us/ms，
 *      贴着舵机极限（约 6.7us/ms）→ 实际被拉回来，看着小。
 *      现在 265×1.2×2/130 ≈ 4.9us/ms，并在极点加 SETTLE_MS(40ms) 停顿。
 *
 * 限位余量（YAW 中位 1000、限位 300~1700；angry 的 amp≈975、上限 1200）：
 *     常态 265×1.2×0.975 ≈ 310us → 690~1310us；极端 265×1.2×1.2 ≈ 382us → 618~1382us。
 * 想再狠只改 ANGRY_YAW_SWING；想先试手感用 test/emotion_test 的
 * `play 4` 配 `gain 1300 1200`（或 `gain 1000 500` 放慢看"不缩水"的幅度）。 */
#define ANGRY_PITCH_UP      90      /* 昂头量（us，向上）*/
#define ANGRY_YAW_SWING     265     /* 左右甩头幅度（us，±）—— 2026-09 由 240 加大 */
#define ANGRY_LIFT_MS       120U    /* 昂头时间 */
#define ANGRY_SWING_MS      130U    /* 单次甩头时间（2026-09 由 90 放慢：给舵机到位的时间）*/
#define ANGRY_SETTLE_MS      15U    /* 每个极点停顿：等舵机到位再反向
                                     * （2026-11 由 40 缩到 15：端点速度已由梯形剖面归零）*/
#define ANGRY_RETURN_MS     150U    /* 回中时间 */

/* ==================== 惊讶（SURPRISED）"猛抬头 + 定格" 参数 ====================
 * 一次性大动作：极快抬头到最高（行程内），定格一会儿，再回落 —— 拟人版"一愣"。 */
#define SURPRISED_PITCH_UP      180 /* 抬头量（us，向上）—— 本工程最大的单次抬升 */
#define SURPRISED_UP_MS         120U/* 抬头时间（要"猛"）*/
#define SURPRISED_HOLD_MS       900U/* 定格时间 */
#define SURPRISED_RETURN_MS     200U/* 回落时间 */

/* ==================== 爱意（LOVING）"微抬头 + 缓慢歪头摆动" 参数 ====================
 * 幅度中等、速度很慢、弧线长；来回写在两侧之间（行程 2×摆幅），显得温柔。 */
#define LOVING_PITCH_UP     60      /* 微抬头量（us，向上）*/
#define LOVING_PITCH_WARM   30      /* 摆动时保持的抬头量（us）*/
#define LOVING_YAW_SWING    90      /* 歪头摆幅（us，±）*/
#define LOVING_LIFT_MS      400U    /* 抬头时间 */
#define LOVING_SWAY_MS      700U    /* 单次摆动时间（慢）*/
#define LOVING_NEAR_MS      600U    /* 靠近中位的那一段（幅度小一些）*/
#define LOVING_RETURN_MS    400U    /* 回中时间 */
#define LOVING_HOLD_MS      300U    /* 结尾停顿 */

/* ==================== 无聊（BORED）"歪头 + 慢摆 + 发呆" 参数 ====================
 * 心情层（MOOD_BORED）专用。2026-09 拆成 2 个变体（随机呼出）：
 *   变体1 歪头发呆 —— 歪到一侧、发很久的呆、换边、回正     ≈13s（被 spd×0.7 拉到 ≈19s）
 *   变体2 垂头发呆 —— 直接垂下头，极慢地小幅晃，再回正     ≈13s（同上）
 * 心情修正会让整体速度再 ×0.7（emotion_fsm 的 policy_apply），所以这里的时间参数
 * 看着不长，实际播起来还要慢一档。 */

/* ---- 变体1「歪头发呆」---- */
#define BORED_YAW_TILT      90      /* 歪头幅度（us，±）*/
#define BORED_PITCH_DOWN    80      /* 低头量（us，向下）*/
#define BORED_TILT_MS       900U    /* 歪过去的时间 */
#define BORED_SWAY_MS       1200U   /* 从一侧摆到另一侧的时间（2×摆幅，慢）*/
#define BORED_HOLD_MS       2600U   /* 每侧的"发呆"停顿（2026-09 由 1200 拉长）*/
#define BORED_RETURN_MS     800U    /* 回中时间 */
#define BORED_REST_MS       600U    /* 结尾停顿 */

/* ---- 变体2「垂头发呆」---- */
#define BORED2_YAW_TILT      70     /* 歪头幅度（us，±）*/
#define BORED2_PITCH_DOWN   150     /* 低头量（us，向下）—— 比变体1 更垂 */
#define BORED2_DOWN_MS      1400U   /* 垂头时间 */
#define BORED2_SWAY_MS      2500U   /* 极慢地歪一点 */
#define BORED2_HOLD_MS      2500U   /* 长时间不动 */
#define BORED2_RETURN_MS    1800U   /* 懒懒回中 */

/* ==================== 睡眠（SLEEP）"低头 + 呼吸起伏" 参数 ====================
 * 心情层（MOOD_SLEEPING）专用：慢慢垂下头，然后在小范围内极缓慢地起伏
 * （模拟呼吸/打盹），一个周期结束后由状态机继续重播 —— 期间任何命令都能
 * 立刻打断（靠 moves 的 tick 钩子轮询串口，见 moves_set_tick_hook）。 */
#define SLEEP_PITCH_DROP    200     /* 低头量（us，向下）—— 行程内（1100-200=900）*/
#define SLEEP_BREATH_AMP     25     /* 呼吸起伏幅度（us，±）*/
#define SLEEP_DROP_MS       800U    /* 垂头时间 */
#define SLEEP_BREATH_MS     1500U   /* 单次呼吸（吸/呼各半个周期）时间 */
#define SLEEP_BREATHS       3       /* 一个周期呼吸几次 */

/* ==================== 动作 ID ==================== */
/* 前三个是【心情/通用】动作，后六个与【情绪】一一对应（顺序与 emo_id_t 无关，
 * 映射表在 emotion_fsm.c 的 policy_move()）*/
typedef enum {
    MOVE_IDLE = 0,      /* 待机：V 字摇摆点头（= 情绪 neutral 的动作）*/
    MOVE_HAPPY,         /* 开心：快速摇头晃脑 */
    MOVE_EIGHT,         /* 八字环绕：脖子画横 8 字（∞）*/
    MOVE_SAD,           /* 难过：低头 + 缓慢左右摇 */
    MOVE_ANGRY,         /* 生气：昂头 + 快速摇头 */
    MOVE_SURPRISED,     /* 惊讶：猛抬头 + 定格 */
    MOVE_LOVING,        /* 爱意：微抬头 + 缓慢歪头摆动 */
    MOVE_BORED,         /* 无聊（心情）：歪头 + 慢摆 + 发呆 */
    MOVE_SLEEP,         /* 睡眠（心情）：低头 + 呼吸起伏 */
    MOVE_MAX
} move_id_t;

/* ==================== 生命周期 ==================== */
/* 初始化动作库（内部会调用 servo_core_init()：mux + pwm + 回中位）。返回 0 成功 */
int  moves_init(void);

/* 动作名（用于日志/串口命令） */
const char *moves_name(move_id_t id);

/* 播放一个周期的动作（内部遇到中断标志会提前返回） */
void moves_play_once(move_id_t id);

/* 请求中断当前动作（下一个轨迹点生效）；播放循环里应定期查询 */
void moves_interrupt(void);

/* 查询并清除中断标志：返回 1 表示播放期间有人请求过中断 */
int  moves_interrupted(void);

/* ==================== 动作调制（情感状态 → 动作参数）====================
 * 解决"固定轨迹"与"连续情绪"的矛盾：
 *   固定轨迹（moves_<情绪>.c）负责【怎么动】—— 波形、节奏、姿态语义，由人手调优；
 *   这里的两个增益负责【动多大、动多快】—— 由上层按连续情绪状态实时给出。
 *
 * 增益是【千分比】：1000 = 名义值（即动作文件里写的偏移），1200 = 放大到 1.2 倍，700 = 缩到 0.7 倍。
 *   amp：缩放所有动作原语的目标偏移（幅度）；
 *   spd：缩放所有动作原语的时间参数（>1000 更快、<1000 更慢）。
 * 生效范围：move_to / move_by / move_center / move_figure8（底层 move_line 不缩放，
 * 因为它是"绝对目标"原语，保留给需要精确落点的地方）。
 * 超出行程也没关系 —— 最终仍会被 servo_set_pulse() 的实测限位钳制。
 *
 * 上层用法（emotion_fsm.c 的 policy_apply()）：
 *     moves_set_gain(amp, spd);  moves_play_once(MOVE_HAPPY); */
#define MOVE_GAIN_UNIT      1000

/* ==================== 整体幅度基准（2026-09 由 1000 提到 1200）====================
 * 上机看下来【所有动作】的幅度都偏小，于是把幅度整体放大 20%。
 * 放在这里（而不是逐个改动作参数）的好处：一处生效、manual / fsm / demo 全部路径
 * 都跟着变大，各动作之间的相对大小关系不变，以后再整体调也只改这一行。
 *
 * 实际输出偏移 = 动作文件里的偏移 × MOVE_AMP_BASE/1000 × amp_gain/1000
 * 例：待机巡视 ±300us ×1.2 ×1.17 ≈ ±421us（YAW 579~1421，限位 300~1700）。 */
#define MOVE_AMP_BASE       1200

/* 轨迹插值步长（ms）：动作库内部按这个粒度走点，也是 tick 钩子的调用周期
 * （emotion_fsm.c 的动作播放期轮询就挂在这个节拍上）*/
#define MOVE_TICK_MS        20U

/* 【2026-10-31】每个插值步允许的最大脉宽变化（us）—— 舵机速度保护。
 *
 * 为什么需要：动作的幅度会被 amp_gain(700~1200，中性再 ×1.15) 放大、
 * 时长会被 spd_gain(700~1400) 压缩，两头一夹，最快的动作能算到 9.5us/ms，
 * 而舵机空载约 6.7us/ms（0.15s/60°），带负载还要更低 —— 指令跑在前面，
 * 舵机追不上就是"一顿一顿"的卡顿，严重时全程全力硬顶（看着像堵转）。
 *
 * 90us / 20ms = 4.5us/ms，约等于 0.15s/60° 舵机带载的实际能力。
 * 超速的动作会被自动拉长时长（形状不变，只是变慢）。
 * 嫌动作太温吞就调大这个值；调到 134 以上等于关掉保护（空载极限）。*/
#define MOVE_MAX_STEP_US    90

/* 【2026-10-31】速度剖面：线性 → 梯形（起停各留一段加减速）
 *
 *   1 = 梯形：位置按"加速 → 匀速 → 减速"走，起止速度为 0。
 *       舵机起步不顿、到点不冲，观感明显更顺；代价是同样行程里中段速度快
 *       1/(1-RAMP/1000) 倍（RAMP=200 时 1.25 倍），限速已按这个折算，
 *       所以触发限速的动作总时长会比线性多约 25%。
 *   0 = 线性（原来的行为，整段匀速，起停有速度阶跃）。
 *
 * 想对比手感：把这个改成 0 重编一版，同一动作播两次即可。*/
#define MOVE_EASE_MODE      1
#define MOVE_EASE_RAMP      200     /* 千分比：加减速各占时长的 20%（必须 < 1000）*/

void     moves_set_gain(uint16_t amp_permille, uint16_t spd_permille);
uint16_t moves_amp_gain(void);
uint16_t moves_spd_gain(void);

/* ==================== 播放期步进钩子（给状态机"插队"用）====================
 * 播放一个动作会阻塞（内部 mdelay 逐步走轨迹）。状态机需要在【动作播放期间】
 * 也能响应串口命令 / 推进心情计时，因此提供这个钩子：
 *
 *     moves_set_tick_hook(emotion_fsm_tick_hook);
 *
 * 动作库每走一个 20ms 插值步（以及 move_hold 的每个 20ms 小块）就调用一次，
 * 钩子里【只允许】做轻量、非重入的事（轮询串口、累加时间、必要时调用
 * moves_interrupt() 请求打断），不允许再调 moves_play_* / move_*。
 * 传 NULL 表示不挂钩子。 */
typedef void (*move_tick_hook_t)(void);
void moves_set_tick_hook(move_tick_hook_t fn);

/* ==================== 变体池（同一状态多个动作，随机呼出）====================
 * 要解决的问题：同一个动作一遍遍循环播，几十秒就能看出"机械感"。
 *
 * 做法：一个状态挂 1~MOVE_MAX_VARIANTS 个【变体】，每个变体本身就是一条
 * 15~23s 的【多段长链】（幅度、方向、停留都不同）；呼出时随机挑一个，
 * 并且【不与上一次相同】。这样同一个状态连看几轮，序列也不会重复。
 *
 *   moves_play_once(id)     —— 固定播主变体（调试用：想看确定的那一个）
 *   moves_play_variant(id)  —— 随机播一个变体（正式状态机用这个）
 *
 * 变体池在 moves.c 的 s_variants[] 里登记（顺序与 move_id_t 一致）；
 * 新增一个变体 = 写实现函数 + 在表里加一行，见 movements/README.md。
 *
 * 【2026-09 现状】除 MOVE_EIGHT 外（8 字有堵转待定位，先不给它叠负载），
 * 其余 8 个状态都挂满 3 个变体 —— 清单见 moves.c 的 s_variants[] 与 movements/README.md。
 *
 * 【参数放哪】主变体①的幅度/基础参数留在本文件（HAPPY_* / SAD_* / ...）；
 * 变体②③以及①里新增段落的参数【就近定义在各自的 moves_<状态>.c 顶部】
 * （如 HAPPY2_* / LOVING3_* / BORED3_*），否则本文件会被几十组宏撑爆。
 * 想调某个变体，去它自己的文件顶部改。 */

#define MOVE_MAX_VARIANTS   3U      /* 每个状态最多几个变体（数组容量）*/

void     moves_play_variant(move_id_t id);      /* 随机播一个变体（不与上次相同）*/
void     moves_play_variant_at(move_id_t id, uint8_t idx);  /* 固定播第 idx 个变体（调试用）*/
uint8_t  moves_variant_count(move_id_t id);     /* 该状态有几个变体（1 = 只有主实现）*/
uint8_t  moves_last_variant(move_id_t id);      /* 上次随机到的变体下标（0 起）*/
uint32_t moves_rand(void);                      /* 小伪随机（动作内部想做轻微变化时用）*/

/* ==================== 轨迹原语（供 moves_xxx.c 使用）====================
 *
 * 【摆动行程的重要约定】——摆幅 n 与行程 2n 不是一回事：
 *   · 摆幅 n（如 HAPPY_YAW_SWING = 240）指"偏离中心的量"，即左 -n / 右 +n；
 *   · 从左(-n) 摆到右(+n)，实际要走 **2n**（220 → 440us），时间也是按 2n 算的；
 *   · 所以来回摆动应该"在一侧和另一侧之间"写目标，而不是每次都从中心出发
 *     （从中心出发的那一段只走 n，会让第一下比后面几下短 —— 动作会不均匀）。
 *   写法二选一，都不会算错：
 *     move_to(+n, ...);  move_to(-n, ...);  move_to(+n, ...);   // 直接写目标侧
 *     move_by(-2n, ...); move_by(+2n, ...);                     // 相对当前位置移动
 *
 * 【到位与速度的约定】——【2026-09 新增，写快速往复动作必读】
 *   舵机是【开环】的：没有位置/速度反馈，指令脉宽一换向它就开始往反方向走。
 *   所以"一段还没走完就切到反向目标"时，实际摆幅会被【削平】—— 现象是"幅度看着很小"，
 *   而不是名义幅度写小了。三条经验：
 *     · 需求速度 = 行程 / 段长 = (2×摆幅 × MOVE_AMP_BASE/1000) / 段长ms，**别超过 5us/ms**
 *       （常见 9g 舵机极限约 6.7us/ms ≈ 0.1s/60°；超了既削幅又嗡嗡响）
 *     · 每个【换向】极点后加一次短停顿 `move_hold(*_SETTLE_MS)`，让这一侧先走完、
 *       咬住位置，再反向。**2026-11：15ms 就够** —— 梯形剖面已经让端点速度为 0，
 *       原来那 40ms 只是"到点站住"的顿感（要更快可试 10ms；削幅了就加回来）。
 *     · 【同向】的连续段不要走 move_to（它两端都会减速到 0）：用 move_to_ex(...,
 *       0/1) 让段间接续，见下面的"连续弧线"。
 *   样例：ANGRY 原来 90ms 甩 240us 摆幅（6.4us/ms，削幅）→ 现在 130ms 甩 265us（4.9us/ms）+ 15ms 极点停顿。
 *   排查手法：同一动作 `gain 1000 500`（放慢一倍）再看 —— 摆幅明显变大就说明是速度问题，不是幅度问题。
 *
 * 【连续弧线 / 多段同向（2026-11）】
 *   多段 move_to 串起来的"绕圈"会在每个接缝处减速到 0，走起来是一段一段挪。
 *   改成 move_to_ex(target, ms, ease_in, ease_out)：
 *     · 首段 (1,0)  起步缓动、带速度交棒；
 *     · 中间段 (0,0) 匀速直连（不在接缝处刹车）；
 *     · 末段 (0,1)  带速度进场、收尾缓动。
 *   限速会自动按"峰值速度"折算（两端都缓动时峰值 = 1/(1-RAMP/1000)），
 *   所以同一段路的时长比 move_to 略长是正常的。
 */

/* 从 (yaw0,pitch0) 平滑插值移动到 (yaw1,pitch1)，耗时 ms 毫秒（线性、20ms 一步）*/
void move_line(int16_t yaw0, int16_t pitch0, int16_t yaw1, int16_t pitch1, uint16_t ms);

/* 同上，但可以指定两端是否缓动（1 = 该端速度为 0；0 = 带速度接续）。
 * 一般只在"多段同向"的中间段传 0，直接写动作的请用 move_to_ex。 */
void move_line_ex(int16_t yaw0, int16_t pitch0, int16_t yaw1, int16_t pitch1,
                  uint16_t ms, uint8_t ease_in, uint8_t ease_out);

/* 从【当前偏移】平滑移动到目标偏移 */
void move_to(int16_t yaw_off, int16_t pitch_off, uint16_t ms);

/* 同上，但指定两端是否缓动（1 = 该端速度为 0，0 = 带速度接续）。
 * 用途：把多段【同方向】的轨迹串成一条连续弧线，段间不减速：
 *     move_to_ex( 200,   0, 900, 1, 0);   // 起步缓动，带速度交棒
 *     move_to_ex(   0, -90, 900, 0, 0);   // 匀速接续
 *     move_to_ex(-200,   0, 900, 0, 1);   // 带速度进场，收尾缓动
 * 换向（往复摆动）不要用它 —— 那种情况端点就该是 0 速。 */
void move_to_ex(int16_t yaw_off, int16_t pitch_off, uint16_t ms,
                uint8_t ease_in, uint8_t ease_out);

/* 从【当前偏移】相对移动 (dyaw, dpitch)（例：从一侧摆到另一侧就是 ±2×摆幅）*/
void move_by(int16_t d_yaw, int16_t d_pitch, uint16_t ms);

/* 画"8 字"（∞）曲线轨迹：yaw = yaw_amp×sin(θ)、pitch = pitch_amp×sin(2θ)，
 * θ 从 0 走 laps 圈，每圈 ms_per_lap；内部逐点算、可中断，画完自动回中。 */
void move_figure8(int16_t yaw_amp, int16_t pitch_amp, uint16_t ms_per_lap, uint8_t laps);

/* 平滑回中位 */
void move_center(uint16_t ms);

/* 保持当前位置 ms 毫秒（可被中断打断）*/
void move_hold(uint16_t ms);

/* 当前偏移（us）*/
int16_t move_yaw_off(void);
int16_t move_pitch_off(void);

#endif /* _MOVES_H_ */
