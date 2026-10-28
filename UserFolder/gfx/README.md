# gfx 图形库使用说明（TD1601 + 480×480 QSPI 屏）

给做图形/动画设计的同学：这个库里已经封装好了屏幕驱动、DMA 传输、局部刷新等底层细节，
你只需要调用几个绘图函数就能出画面。

---

## 1. 快速上手

```c
#include "gfx.h"

void my_screen(void)
{
    gfx_init();                                          /* 初始化屏幕（一次） */

    gfx_clear(GFX_SKYBLUE);                              /* 刷背景 */
    gfx_fill_rect(40, 40, 140, 90, GFX_ORANGE);           /* 矩形 */
    gfx_fill_circle(330, 110, 70, GFX_YELLOW, GFX_SKYBLUE);/* 圆 */
    gfx_fill_triangle(240, 230, 150, 350, 330, 350, GFX_GREEN, GFX_SKYBLUE);
    gfx_line(40, 430, 440, 430, 8, GFX_DARKGRAY, GFX_SKYBLUE);
}
```

参考完整示例：`UserFolder/gfx/gfx_demo.c`（含静态图元 + 局部刷新的小脸动画）。

---

## 2. API 速查

| 函数 | 说明 |
|---|---|
| `gfx_init()` | 初始化屏幕（含三色自检），调用一次 |
| `gfx_clear(color)` | 全屏填充（整屏刷新，约 20ms） |
| `gfx_fill_rect(x, y, w, h, color)` | 纯色矩形（不需要背景参数） |
| `gfx_fill_circle(cx, cy, r, color, bg)` | 实心圆，`bg` = 该区域底色 |
| `gfx_fill_triangle(x0,y0,x1,y1,x2,y2, color, bg)` | 实心三角形 |
| `gfx_line(x0,y0,x1,y1, thick, color, bg)` | 线段，`thick` = 线宽 |
| `gfx_blit(x, y, w, h, pix)` | 贴位图（`pix` 为 w×h 个 RGB565 像素，行优先） |
| `gfx_blit_key(x, y, w, h, pix, key, bg)` | 贴位图 + 透明色键（等于 `key` 的像素不画，用 `bg` 顶替） |

颜色：
```c
GFX_RGB565(r, g, b)     // 任意颜色，各分量 0~255
GFX_SKYBLUE / GFX_CREAM / GFX_ORANGE / GFX_PINK / GFX_BROWN / GFX_DARKBLUE ...
GFX_BLACK / WHITE / RED / GREEN / BLUE / YELLOW / CYAN / MAGENTA / GRAY / DARKGRAY
```

屏幕尺寸：`GFX_W`、`GFX_H`（都是 480）。

---

## 3. 三条必须知道的约束

1. **局部绘制要传 `bg`（该区域底色）**
   屏幕不支持读回像素，所以往一块区域写入时必须提供窗口内**其余像素**的颜色。
   例如在浅蓝底上画圆传 `bg = GFX_SKYBLUE`，圆外接矩形的四角会被填成浅蓝——
   和底色一致，看不出差别。若要在图片上叠加图形，请把整张图重新 `gfx_blit` 或用
   `gfx_blit`/`gfx_blit_key` 自己合成。

2. **动画请只画变化的区域**
   全屏刷新一次约 20ms（≈50fps 上限），而局部区域（比如 96×96）一次只要一两百微秒。
   推荐做法：给角色划一个矩形"活动区"，每帧在该区域内整块重画（含背景）后 `gfx_blit`，
   这样既快又不会有残影（见 `gfx_demo.c`）。

3. **不要自己碰底层**
   不要直接调用官方 `LCD_DmaSend` / `LCD_Set_Window` 做局部刷新——它们有单次传输
   4095 像素上限、数据长度必须与窗口严格相等、事务间需要间隔等坑，库里都已处理。
   需要新图元（圆角矩形、椭圆、阴影…）请提需求，或参考 `gfx.c` 里 `fill_poly` 的写法。

---

## 4. 性能参考（实测）

| 操作 | 数据量 | 耗时 |
|---|---|---|
| 全屏 `gfx_clear` | 460 KB | ~20 ms |
| 局部 96×96 `gfx_blit` | 18 KB | ~0.3 ms |
| 局部 40×40 小图 | 3 KB | ~0.1 ms |
| 圆 `r=70` | 约 200×200 区域 | ~0.5 ms |

颜色格式是 RGB565（每像素 2 字节），屏幕刷新率远高于动画需要，局部刷新的瓶颈基本只有
你的绘制/合成代码本身。

---

## 5. 用图片/精灵做形象（推荐）

程序化几何（圆/三角/线）能做出简洁图标，但**细腻的角色形象建议用图片**：

### 步骤
1. 在 PC 上用任意绘图软件做图（建议尺寸对齐到你需要的大小，如 96×96、160×160）；
2. 导出 PNG（背景用**纯色**当透明键，或用你固定的底色）；
3. 用下面的脚本把 PNG 转成 C 数组（RGB565）：

```python
# png2c.py  —— 需要 Pillow:  pip install pillow
# 用法: python png2c.py face.png face_sprite > face_sprite.h
import sys
from PIL import Image

src, name = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGB")
w, h = im.size
px = im.load()
print(f"/* {src}  {w}x{h}  RGB565 */")
print(f"#define {name.upper()}_W {w}")
print(f"#define {name.upper()}_H {h}")
print(f"static const uint16_t {name}[{w*h}] = {{")
vals = []
for y in range(h):
    for x in range(w):
        r, g, b = px[x, y]
        vals.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    print("    " + ",".join(f"0x{v:04X}" for v in vals[-w:]) + ",")
print("};")
```

4. 代码里直接贴图：
```c
#include "face_sprite.h"
gfx_blit(x, y, FACE_SPRITE_W, FACE_SPRITE_H, face_sprite);
/* 或用透明色键（PNG 背景是纯色 0xF81F 时）： */
gfx_blit_key(x, y, FACE_SPRITE_W, FACE_SPRITE_H, face_sprite, 0xF81F, GFX_SKYBLUE);
```

### 注意事项
- 一张 160×160 的图占 160×160×2 = 50 KB（存放在程序 flash 里，很充裕）；
- 多帧动画 = 多张图（例如 8 帧眨眼），按顺序轮播即可；
- 若图片很大（接近全屏），请整帧重画（`gfx_clear` 或整屏 `gfx_blit`）而不是局部叠加。

---

## 6. 让动画"灵动"的几点经验

- **缓动**：不要匀速，用 ease-in/out（如 `x = A*sin(t)`、抛物线的速度曲线）；
- **节奏**：动作有快慢对比——眨眼只要 0.15~0.2s，微笑保持 1~2s；
- **微动作**：持续的小幅呼吸/浮动（±4~8px）比静止更"活"；
- **焦点**：眼睛的高光、腮红的颜色渐变会让形象更有神；
- **限制颜色**：统一调色板（3~5 个色），避免高饱和纯色堆叠，画面更高级。
