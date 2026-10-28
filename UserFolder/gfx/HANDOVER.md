# 屏幕显示模块 · 交接文档

> 面向：后续接手屏幕/图形/动画开发的同学（含美术合作）
> 工程：`projects/baozi`（CDK / TD1601 EVB）
> 屏幕：480×480 QSPI 屏（TR230S 系列，扩展板 CON2 接口）

---

## 1. 五分钟上手

```bash
# 1) 打开工程
projects/baozi/project.cdkws

# 2) 编译烧录（CDK 里 Build + Download），串口 115200
```

代码入口在 `src/main.c`（当前指向 `gfx_sprite_demo()`）。写自己的画面只要三步：

```c
#include "gfx.h"          /* 注意：-IUserFolder/ 已配置，或用 "gfx/gfx.h" */

gfx_init();                                   /* 一次 */
gfx_clear(GFX_SKYBLUE);                       /* 背景 */
gfx_fill_circle(240, 240, 80, GFX_ORANGE, GFX_SKYBLUE);   /* 画东西 */
```

示例代码：
- `UserFolder/gfx/gfx_demo.c` —— 图元 + 局部刷新小脸动画
- `UserFolder/gfx/gfx_sprite_demo.c` —— **图片精灵多帧动画（推荐从这个开始）**

---

## 2. 分层结构（谁负责什么）

```
应用层（你写的画面逻辑）
   ↓  调用
gfx 库（UserFolder/gfx/gfx.c/.h）
   · 局部/全屏窗口设置、DMA 分段发送、裁剪、图元、贴图
   ↓  调用
官方屏幕驱动（src/video.c，来自 LCD 模板，勿改）
   · 引脚/复位/初始化命令、QSPI 模式切换、my_write / LCD_DmaSend
   ↓
硬件（TD1601 OSPI + 480×480 屏）
```

`src/video.c` + `include/video.h` 是**官方示例原样**（已验证点亮），是可靠底座；
所有屏幕绘制请走 `gfx` 库，不要直接操作驱动。

---

## 3. gfx 库 API

| 函数 | 说明 |
|---|---|
| `gfx_init()` | 初始化屏幕（含三色自检），调用一次 |
| `gfx_clear(color)` | 全屏填充（整屏刷新，约 20ms） |
| `gfx_fill_rect(x,y,w,h,color)` | 纯色矩形（不需要背景参数） |
| `gfx_fill_circle(cx,cy,r,color,bg)` | 实心圆，`bg`=该区域底色 |
| `gfx_fill_triangle(x0,y0,x1,y1,x2,y2,color,bg)` | 实心三角形 |
| `gfx_line(x0,y0,x1,y1,thick,color,bg)` | 线段，`thick`=线宽 |
| `gfx_blit(x,y,w,h,pix)` | 贴位图（w×h 个 RGB565 像素，行优先） |
| `gfx_blit_key(x,y,w,h,pix,key,bg)` | 贴位图 + 透明色键 |

颜色：`GFX_RGB565(r,g,b)`，或 `GFX_SKYBLUE / GFX_CREAM / GFX_ORANGE / GFX_PINK / GFX_BROWN / GFX_DARKBLUE / GFX_WHITE / …`
屏幕尺寸：`GFX_W`、`GFX_H`（480）。

---

## 4. 图片精灵工作流（形象设计推荐路线）

### 4.1 美术侧
1. 每帧画一张 PNG，**同一套尺寸、同一套取景**（如 96×96、160×160）；
2. 背景用**统一纯色**做透明键（推荐 `#F81F` 洋红，或与背景一致的色）；
3. 帧序命名清晰（`idle.png`、`blink.png`、`happy.png`…）。

### 4.2 转换（PC 上执行）
```bash
cd projects/baozi/UserFolder/gfx
pip install pillow                     # 只装一次
python tools/png2c.py idle.png blink.png happy.png --name face --out face_sprite.h
```
输出内容：
```c
#define FACE_W 96
#define FACE_H 96
#define FACE_FRAME_COUNT 3
static const uint16_t face_frames[3][96*96] = { ... };
```
（单张图则输出 `face[96*96]`；`--scale 0.5` 可以缩小以省空间。）

### 4.3 代码里显示
```c
#include "face_sprite.h"

static int frame = 0;
for (;;) {
    gfx_blit_key(x, y, FACE_W, FACE_H, face_frames[frame], 0xF81F, GFX_SKYBLUE);
    if (++tick % 60 == 0) frame = (frame + 1) % FACE_FRAME_COUNT;
}
```

### 4.4 资源占用参考
| 尺寸 | 每帧 | 8 帧动画 |
|---|---|---|
| 96×96 | 18 KB | 144 KB |
| 160×160 | 50 KB | 400 KB |
| 240×240 | 113 KB | 900 KB |

程序存放在外部 flash（8 MB，XIP 直接执行/读取），按上表放几十帧没问题。
**注意**：图片数组会占用 flash，别把几十 MB 的素材都塞进去。

---

## 5. 硬约束（踩过的坑，务必遵守）

1. **DMA 单次传输上限 4095 像素**（DW DMA 块大小 12 位）。
   实测：8 行（3840 像素）正常；16 行（7680）DMA 不完成。
   `gfx` 库已按 8 行分段，请勿自己写更长的一次性 DMA。
2. **发往某个窗口的数据长度必须严格等于窗口像素数**。
   少发/多发都会让面板的事务在错误时机结束 → 画面错位、撕裂、"半屏黑"，
   而且会**持续污染**（后续画面也不对）直到下次完整重画。
3. **不要用局部窗口做"两次紧挨着"的刷新**（例如先清旧位置再画新位置）。
   面板偶尔来不及处理第二个窗口命令 → 残留竖条。
   正确做法（库中已示范）：把"旧位置 ∪ 新位置"合并成一个矩形，**一次事务**刷完。
4. **局部绘制必须传 `bg`**：面板不支持读回像素，窗口内其余像素的颜色要由调用者给出。
5. **事务之间需要间隔**：`gfx` 库内每次事务后留 60µs，别绕过。
6. 不要直接调用官方 `LCD_DmaSend` / `LCD_Set_Window` 做局部刷新（上面 1~5 条它都不管）。

---

## 6. 调试手册（症状 → 原因 → 处理）

| 症状 | 可能原因 | 处理 |
|---|---|---|
| 红绿蓝自检后一直黑/一直蓝 | 程序卡住（多在 DMA 等待）或复位循环 | 看串口是否有周期性打印；检查是否超 4095 上限 |
| 画面撕裂、错位、半屏黑、色带 | 数据长度与窗口不匹配 / 局部窗口被漏处理 | 改回"一次事务 + 严格等长 + 60µs 间隔"（用 gfx 库的 API 即可） |
| 局部刷新后残留竖条 | 同一帧做了两次相邻窗口事务 | 合并成一个矩形区域，一次刷完 |
| 图形位置整体偏移 | 窗口参数与实际数据量不符 | 核对 w×h 与发送像素数 |
| 帧率忽然很低 | 每帧全屏重画 / 逐行调用发送 | 改成"只画变化区域"（局部刷新） |
| 屏幕/背光发烫 | 全屏持续刷新 + 背光常亮满功率 | 用局部刷新减少数据量；背光可改 PWM 调光（见 §8） |

**测帧率**：在循环里每 10 帧 `printf("[xx] frame N\r\n")`，
用**带时间戳的串口助手**看两条打印的时间差 ÷ 10 = 单帧耗时。

---

## 7. 性能参考（实测）

| 操作 | 数据量 | 耗时 |
|---|---|---|
| 全屏 `gfx_clear` | 460 KB | ~20 ms（≈50fps 上限，受 50MHz 四线 QSPI 带宽限制） |
| 96×96 `gfx_blit` | 18 KB | ~0.3 ms |
| 48×48 精灵 `gfx_blit_key` | 4.6 KB | ~0.1 ms |
| 圆 `r=70` | ~200×200 区域 | ~0.5 ms |

结论：**动画请只画变化区域**——局部刷新可以轻松跑到几十~几百 fps，
全屏重画才是瓶颈（460KB/帧）。

---

## 8. 发热与功耗建议

发热来源：① 背光（当前 `BL=PA31` 常亮高电平，满功率）；② 面板控制器持续接收数据。
- **首选**：局部刷新（数据量降 1~2 个数量级，控制器负载与发热大幅下降）；
- **其次**：背光改 PWM 调光（PA31 输出 PWM，占空比 40~70%，亮度略降、发热明显下降）；
- 提高 QSPI 时钟（50→100MHz）能提速但**加剧发热**，非必要不做。

---

## 9. 工程文件清单

| 路径 | 说明 |
|---|---|
| `src/video.c` / `include/video.h` | **官方屏幕驱动（基准，勿改）** |
| `src/main.c` | 程序入口（当前指向 `gfx_sprite_demo()`） |
| `UserFolder/gfx/gfx.h` `.c` | **图形库（在用）** |
| `UserFolder/gfx/gfx_demo.c` | 图元 + 局部刷新动画示例 |
| `UserFolder/gfx/gfx_sprite_demo.c` | **图片精灵多帧动画示例（在用）** |
| `UserFolder/gfx/tools/png2c.py` | PNG → C 数组转换工具 |
| `UserFolder/QSPI_test/*.c` | 历史调试程序（局部刷新/DMA/3D 等实验，未参与编译） |
| `package.yaml` | `source_file` 列表决定哪些文件参与编译 |

新增文件后记得把它加进 `package.yaml` 的 `source_file`，否则不会被编译。

---

## 10. 后续可做（按性价比）

1. **图片精灵资源落地**（路线 A，已支持）：把美术 PNG 转成数组替换示例里的 `g_frames`；
2. **渲染能力增强**（路线 B，待开发）：圆角矩形、椭圆、弧线/扇形、渐变、描边阴影、
   **抗锯齿（alpha 混合）**——纯代码画面质感可再上一个台阶；
3. 背光 PWM 调光（降热）；
4. **离线预渲染动画**（路线 C）：PC 上渲染整段动画序列帧（Blender/AE）→ 转数组 → 逐帧播放，
   可做电影级效果，代价是资源体积与制作工作量。

---

## 11. 上手自检清单

- [ ] 能编译烧录，串口 115200 能看到 `[sprite] ...` 打印
- [ ] 改一个颜色/坐标，画面立刻变化
- [ ] 用 `png2c.py` 把自己的 3 帧图转成头文件并跑起来
- [ ] 明白三条硬约束（4095 上限、数据长度=窗口像素数、局部绘制要传 bg）
- [ ] 会用串口时间戳测帧率
