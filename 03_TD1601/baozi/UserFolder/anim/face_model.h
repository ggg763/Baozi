/******************************************************************************
 * @file     face_model.h
 * @brief    姿势数据库 + 几何变换（纯计算，不碰屏幕）
 *
 * 三层结构里本文件承担两层：
 *   ① 姿势表：fd_poses[] 由模拟器导出（face_poses_v9.h，自动生成），
 *      "一个表情长什么样"完全由数据决定，加表情不改代码。
 *   ② 几何变换：把【实时参数】叠到姿势的静态几何上 ——
 *      眨眼（压扁眼睛）、视线（平移眼睛）、眼睛大小、张嘴。
 *
 * 为什么变换要单独抽出来、且不碰屏幕：
 *   这样它能在 PC 上用 mingw 编译、和模拟器逐像素对比（见 _probe/c_host）。
 *   上板前就把形状与过渡验证掉，省掉大量烧录往返。
 *
 * 单位约定（与 face_draw.h / 模拟器完全一致，别弄混）：
 *   x, y     相对屏幕中心，1/2 像素
 *   rx, ry   半径，1/2 像素
 *   gaze_dx  1/2 像素（≈ 模拟器的 st.gx）
 *   blink    0..255，255 = 完全闭合
 ******************************************************************************/

#ifndef _FACE_MODEL_H_
#define _FACE_MODEL_H_

#include <stdint.h>
#include "face_draw.h"              /* fd_part_t / fd_pose_t / FR_* */

/* ---------------------------------------------------------------- 实时参数 */
/* 叠加在姿势之上，可随时改、立即生效。只有眼睛吃 gaze/blink/eye_scale，
 * 只有嘴吃 mouth_open —— 用 fd_part_t.role 判断，不靠坐标猜。 */
typedef struct {
    int16_t gaze_dx;        /* 视线横移，1/2 像素，正 = 右 */
    int16_t gaze_dy;        /* 视线纵移，1/2 像素，正 = 下 */
    uint8_t blink;          /* 0 = 睁眼，255 = 完全闭合 */
    int16_t eye_scale;      /* 眼睛尺寸百分比，100 = 原值 */
    uint8_t mouth_open;     /* 嘴额外张开 0..255（0 = 原样）*/
} face_expr_t;

/* ---------------------------------------------------------------- 姿势表 */
int              face_model_count(void);
const fd_pose_t *face_model_get(int idx);           /* 越界返回 0 */
int              face_model_find(const char *name); /* 大小写不敏感，-1 = 没找到 */

/* 姿势 → 部件数组（原样拷贝）。返回部件个数。 */
int face_model_build(const fd_pose_t *ps, fd_part_t *out, int max);

/* 两帧部件插值（过渡用）。按角色分组、组内按 x 排序后顺序配对，
 * 保证"左眼配左眼、右眼配右眼"，不会中途交叉飞过。
 * 数量不等时多出来的部件按半径缩放淡入/淡出。 */
int face_model_mix(const fd_part_t *a, int na, const fd_part_t *b, int nb,
                   uint8_t t, fd_part_t *out, int max);

/* 就地叠加实时参数（眨眼 / 视线 / 眼睛大小 / 张嘴）。e 可传 0。 */
void face_model_apply(fd_part_t *p, int n, const face_expr_t *e);

/* 参数归零（blink=0，eye_scale=100，其余 0）*/
void face_expr_reset(face_expr_t *e);

/* 两个 RGB565 颜色按 t（0..255）插值 —— 过渡时颜色一起渐变 */
uint16_t face_model_lerp_color(uint16_t a, uint16_t b, uint8_t t);

#endif /* _FACE_MODEL_H_ */
