/******************************************************************************
 * @file     face_poses_v9.h   【自动生成，勿手改】
 * @brief    v9 表情表：30 个姿势（含 1 个别名）
 *
 * 来源：_probe/faces_v9.json（模拟器导出的定稿规格）
 * 生成：_probe/gen_poses.py   —— 改表情请改模拟器，然后重跑脚本
 *
 * 字段含义见 face_draw.h 的 fd_part_t 注释。坐标 1/2 像素、rx/ry 是半径。
 ******************************************************************************/

#ifndef _FACE_POSES_V9_H_
#define _FACE_POSES_V9_H_

#include "face_draw.h"

static const fd_part_t fd_parts_neutral[] = {
    { FD_SUP   , FR_EYE   , 1,  20, 255,  -199,     0,  155,  155,    0,     0,     0,    0 },
};

static const fd_part_t fd_parts_happy[] = {
    { FD_SUP   , FR_EYE   , 1,  20, 255,  -199,     0,  155,  155,    0,     0,     0,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,     0,   196,   84,   30,    0,   -16,     0,    0 },
};

static const fd_part_t fd_parts_laugh[] = {
    { FD_CRES  , FR_EYE   , 1,   0, 255,  -214,   -16,  168,   80,    0,    76,     0,    0 },
    { FD_CUT   , FR_MOUTH , 0,   0, 255,     0,   200,  140,  104,    0,    90,   -68,    0 },
};

static const fd_part_t fd_parts_shy[] = {
    { FD_SUP   , FR_EYE   , 1,  28, 255,  -199,    32,  156,   92,   -5,     0,     0,    0 },
    { FD_CAP   , FR_DECO  , 1,   0, 255,  -163,   248,    7,    7,    0,  -135,   200,    0 },
    { FD_CAP   , FR_DECO  , 1,   0, 255,  -205,   262,    7,    7,    0,  -177,   214,    0 },
    { FD_CAP   , FR_DECO  , 1,   0, 255,  -263,   276,    6,    6,    0,  -235,   228,    0 },
};

static const fd_part_t fd_parts_sad[] = {
    { FD_SUP   , FR_EYE   , 1,  22, 255,  -199,   -30,  155,  106,    0,     0,     0,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,     0,   210,   70,   26,    0,    14,     0,    0 },
};

static const fd_part_t fd_parts_sad_anguish[] = {
    { FD_CUT   , FR_EYE   , 1,   0, 255,  -204,    12,  176,  176,    0,    60,   -44,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,     0,   214,   64,   30,    0,    16,     0,    0 },
};

static const fd_part_t fd_parts_angry_low[] = {
    { FD_CUT   , FR_EYE   , 1,   0, 255,  -199,   -12,  156,  156,    0,    90,   -52,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,   -52,   196,    8,    8,    0,    52,   196,    0 },
};

static const fd_part_t fd_parts_angry_high[] = {
    { FD_CUT   , FR_EYE   , 1,   0, 255,  -235,   -24,  164,  164,    0,   124,   -60,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,   -52,   196,    8,    8,    0,    52,   196,    0 },
};

static const fd_part_t fd_parts_surprised[] = {
    { FD_SUP   , FR_EYE   , 1,  20, 255,  -199,   -22,  170,  170,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   232,   38,   46,    0,     0,     0,    0 },
};

static const fd_part_t fd_parts_loving[] = {
    { FD_CRES  , FR_EYE   , 1,   0, 255,  -199,   -20,  124,   72,    0,    60,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   192,   44,   32,    0,     0,     0,    0 },
    { FD_HEART , FR_DECO  , 0,   0, 255,   268,  -306,   88,   84,    0,     0,     0,    0 },
    { FD_HEART , FR_DECO  , 0,   0, 255,   366,  -188,   48,   46,    0,     0,     0,    0 },
};

static const fd_part_t fd_parts_asleep[] = {
    { FD_SUP   , FR_EYE   , 1,  30, 255,  -199,     0,  155,   26,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   196,   34,   20,    0,     0,     0,    0 },
    { FD_RING  , FR_DECO  , 0,   0, 255,   300,  -236,   44,   44,    0,     0,     0,    9 },
};

static const fd_part_t fd_parts_think[] = {
    { FD_SUP   , FR_EYE   , 1,  30, 255,  -199,     4,  160,   52,    0,     0,     0,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,   -48,   184,    9,    9,    0,    48,   184,    0 },
    { FD_DISC  , FR_DECO  , 0,   0, 255,   296,  -236,   10,   10,    0,     0,     0,    0 },
    { FD_DISC  , FR_DECO  , 0,   0, 255,   350,  -184,   16,   16,    0,     0,     0,    0 },
    { FD_DISC  , FR_DECO  , 0,   0, 255,   404,  -132,   22,   22,    0,     0,     0,    0 },
};

static const fd_part_t fd_parts_listen[] = {
    { FD_SUP   , FR_EYE   , 1,  21, 255,  -199,   -20,  146,  166,    0,     0,     0,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,   -52,   192,    8,    8,    0,    52,   192,    0 },
};

static const fd_part_t fd_parts_question[] = {
    { FD_SUP   , FR_EYE   , 1,  20, 255,  -199,    -8,  160,  160,    0,     0,     0,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,    14,   200,   56,   26,    0,    12,     0,    0 },
};

static const fd_part_t fd_parts_look[] = {
    { FD_SUP   , FR_EYE   , 1,  20, 255,  -199,     0,  160,  160,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   206,   46,   20,    0,     0,     0,    0 },
};

static const fd_part_t fd_parts_glance[] = {
    { FD_SUP   , FR_EYE   , 0,  20, 255,  -155,     0,  133,  155,    0,     0,     0,    0 },
    { FD_SUP   , FR_EYE   , 0,  20, 255,   259,     0,  155,  155,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   200,   44,   18,    0,     0,     0,    0 },
};

static const fd_part_t fd_parts_pout[] = {
    { FD_SUP   , FR_EYE   , 1,  22, 255,  -199,   -18,  150,  130,    0,     0,     0,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,     0,   212,   60,   28,    0,    16,     0,    0 },
};

static const fd_part_t fd_parts_yawn[] = {
    { FD_SUP   , FR_EYE   , 1,  30, 255,  -199,    -6,  150,   20,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   212,   70,   86,    0,     0,     0,    0 },
};

static const fd_part_t fd_parts_wink[] = {
    { FD_SUP   , FR_EYE   , 0,  20, 255,  -199,     0,  155,  155,    0,     0,     0,    0 },
    { FD_CRES  , FR_EYE   , 0,   0, 255,   199,    -8,  150,   56,    0,    44,     0,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,     0,   192,   78,   30,    0,   -16,     0,    0 },
};

static const fd_part_t fd_parts_cry[] = {
    { FD_SUP   , FR_EYE   , 1,  22, 255,  -199,   -22,  150,  104,    0,     0,     0,    0 },
    { FD_CAP   , FR_DECO  , 1,   0, 255,  -240,    62,   15,   15,    0,  -248,   214,    0 },
    { FD_CAP   , FR_DECO  , 1,   0, 255,  -292,    82,    9,    9,    0,  -298,   172,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,  -106,   220,   10,   10,    0,   -52,   206,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,   -52,   206,   10,   10,    0,    52,   206,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,    52,   206,   10,   10,    0,   106,   220,    0 },
};

static const fd_part_t fd_parts_shock[] = {
    { FD_DISC  , FR_EYE   , 1,   0, 255,  -199,   -14,  112,  112,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   224,   42,   72,    0,     0,     0,    0 },
    { FD_CAP   , FR_DECO  , 0,   0, 255,   300,  -224,   15,   15,    0,   300,  -160,    0 },
};

static const fd_part_t fd_parts_idea[] = {
    { FD_DISC  , FR_EYE   , 1,   0, 255,  -199,   -18,  116,  116,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   228,   40,   64,    0,     0,     0,    0 },
    { FD_DISC  , FR_DECO  , 0,   0, 255,   320,  -300,   90,   90,    0,     0,     0,    0 },
    { FD_CAP   , FR_DECO  , 0,   0, 255,   320,  -222,   30,   30,    0,   320,  -186,    0 },
    { FD_CAP   , FR_DECO  , 0,   0, 255,   320,  -420,   13,   13,    0,   320,  -452,    0 },
    { FD_CAP   , FR_DECO  , 0,   0, 255,   246,  -374,   13,   13,    0,   225,  -395,    0 },
    { FD_CAP   , FR_DECO  , 0,   0, 255,   394,  -374,   13,   13,    0,   415,  -395,    0 },
};

static const fd_part_t fd_parts_star_struck[] = {
    { FD_STAR  , FR_EYE   , 1,   5, 255,  -199,    -8,  176,  176,    0,     0,     0,   42 },
    { FD_CUT   , FR_MOUTH , 0,   0, 255,     0,   206,   90,   70,    0,    90,   -42,    0 },
};

static const fd_part_t fd_parts_dizzy[] = {
    { FD_CAP   , FR_DECO  , 1,   0, 255,  -251,   -52,   15,   15,    0,  -147,    52,    0 },
    { FD_CAP   , FR_DECO  , 1,   0, 255,  -251,    52,   15,   15,    0,  -147,   -52,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,     0,   206,   44,   26,    0,   -14,     0,    0 },
};

static const fd_part_t fd_parts_bored[] = {
    { FD_SUP   , FR_EYE   , 1,  22, 255,  -199,     2,  155,   54,    0,     0,     0,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,   -36,   198,    7,    7,    0,    36,   198,    0 },
};

static const fd_part_t fd_parts_smug[] = {
    { FD_SUP   , FR_EYE   , 1,  24, 255,  -199,     8,  150,   48,    0,     0,     0,    0 },
    { FD_CAP   , FR_MOUTH , 0,   0, 255,   -84,   190,   13,   13,    0,    56,   160,    0 },
};

static const fd_part_t fd_parts_nervous[] = {
    { FD_SUP   , FR_EYE   , 1,  20, 255,  -199,    -6,  140,  140,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   204,   44,   20,    0,     0,     0,    0 },
    { FD_CAP   , FR_DECO  , 0,   0, 255,   298,  -206,   12,   12,    0,   298,  -152,    0 },
};

static const fd_part_t fd_parts_sleepy[] = {
    { FD_SUP   , FR_EYE   , 1,  22, 255,  -199,     6,  155,   78,    0,     0,     0,    0 },
    { FD_SUP   , FR_MOUTH , 0,  20, 255,     0,   202,   34,   30,    0,     0,     0,    0 },
    { FD_RING  , FR_DECO  , 0,   0, 255,   296,  -208,   30,   30,    0,     0,     0,    8 },
};

static const fd_part_t fd_parts_mischief[] = {
    { FD_SUP   , FR_EYE   , 0,  20, 255,  -199,    -6,  155,  155,    0,     0,     0,    0 },
    { FD_SUP   , FR_EYE   , 0,  22, 255,   199,     6,  155,   86,    0,     0,     0,    0 },
    { FD_CRES  , FR_MOUTH , 0,   0, 255,    22,   196,   64,   26,    0,   -16,     0,    0 },
};

static const fd_pose_t fd_poses[] = {
    { "neutral", 0xEF9F, fd_parts_neutral, 1 },
    { "happy", 0xFF77, fd_parts_happy, 2 },
    { "laugh", 0xFF14, fd_parts_laugh, 2 },
    { "shy", 0xFDF9, fd_parts_shy, 4 },
    { "pet", 0xFDF9, fd_parts_shy, 4 },
    { "sad", 0xAE5E, fd_parts_sad, 2 },
    { "sad_anguish", 0x95BD, fd_parts_sad_anguish, 2 },
    { "angry_low", 0xFCB0, fd_parts_angry_low, 2 },
    { "angry_high", 0xFB6B, fd_parts_angry_high, 2 },
    { "surprised", 0xCF1F, fd_parts_surprised, 2 },
    { "loving", 0xFDB7, fd_parts_loving, 4 },
    { "asleep", 0x7C75, fd_parts_asleep, 3 },
    { "think", 0xD6FE, fd_parts_think, 5 },
    { "listen", 0xCF5C, fd_parts_listen, 2 },
    { "question", 0xDF1E, fd_parts_question, 2 },
    { "look", 0xEF9F, fd_parts_look, 2 },
    { "glance", 0xEF9F, fd_parts_glance, 3 },
    { "pout", 0xBE5C, fd_parts_pout, 2 },
    { "yawn", 0xB61B, fd_parts_yawn, 2 },
    { "wink", 0xFF58, fd_parts_wink, 3 },
    { "cry", 0x95FE, fd_parts_cry, 6 },
    { "shock", 0xCEFF, fd_parts_shock, 3 },
    { "idea", 0xFF75, fd_parts_idea, 7 },
    { "star_struck", 0xFF12, fd_parts_star_struck, 2 },
    { "dizzy", 0xD67D, fd_parts_dizzy, 3 },
    { "bored", 0xCE9C, fd_parts_bored, 2 },
    { "smug", 0xFF16, fd_parts_smug, 2 },
    { "nervous", 0xD71A, fd_parts_nervous, 3 },
    { "sleepy", 0xADFA, fd_parts_sleepy, 3 },
    { "mischief", 0xFED6, fd_parts_mischief, 3 },
};

#define FD_POSE_COUNT ((int)(sizeof(fd_poses) / sizeof(fd_poses[0])))

#endif /* _FACE_POSES_V9_H_ */
