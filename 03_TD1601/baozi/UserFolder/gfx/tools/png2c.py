#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
png2c.py —— 把 PNG 图片转换成 gfx 库可直接显示的 C 数组（RGB565）

依赖：Pillow     安装： pip install pillow

用法示例
--------
1) 单张图（生成一个数组）：
     python png2c.py face.png --name face > face_sprite.h

2) 多张图 = 动画帧（生成二维数组 + 帧数宏，帧序就是命令行给出的顺序）：
     python png2c.py blink1.png blink2.png blink3.png --name blink > blink_sprite.h

3) 只输出到文件而不是 stdout：
     python png2c.py face.png --name face --out face_sprite.h

参数
----
  --name NAME   数组名前缀（默认取第一个文件的主文件名）
  --out FILE    输出到文件（默认打印到标准输出）
  --key RRGGBB  透明色键（十六进制，如 F81F）。仅用于在注释里标注，
                数组内容仍是原始颜色；代码里用 gfx_blit_key(..., 0xF81F, bg) 显示
  --scale N     缩放倍数（0.5 / 2 等，便于控制资源体积），默认 1

输出内容
--------
  #define NAME_W / NAME_H        图片宽高
  #define NAME_FRAME_COUNT n     帧数（单张时为 1）
  static const uint16_t NAME_frames[n][W*H] = { ... }   （单张时为 NAME[W*H]）

在代码里显示
------------
  #include "face_sprite.h"
  gfx_blit(x, y, FACE_W, FACE_H, face_frames[0]);                    // 不透明
  gfx_blit_key(x, y, FACE_W, FACE_H, face_frames[i], 0xF81F, GFX_SKYBLUE);  // 带色键
"""

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("错误：需要 Pillow。请先运行  pip install pillow", file=sys.stderr)
    sys.exit(1)


def load_rgb565(path, scale):
    im = Image.open(path).convert("RGB")
    if scale != 1:
        w = max(1, int(round(im.width * scale)))
        h = max(1, int(round(im.height * scale)))
        im = im.resize((w, h), Image.LANCZOS)
    w, h = im.size
    px = im.load()
    data = []
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            data.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return w, h, data


def emit(name, w, h, frames, key, out):
    up = name.upper()
    if len(frames) == 1:
        out.write("/* %s  %dx%d  RGB565%s */\n" % (name, w, h, (", key=0x%s" % key) if key else ""))
        out.write("#define %s_W %d\n" % (up, w))
        out.write("#define %s_H %d\n" % (up, h))
        out.write("static const uint16_t %s[%d] = {\n" % (name, w * h))
        data = frames[0]
        for i in range(0, len(data), 16):
            out.write("    " + ",".join("0x%04X" % v for v in data[i:i + 16]) + ",\n")
        out.write("};\n")
    else:
        out.write("/* %s  %dx%d  RGB565  %d frames%s */\n" %
                  (name, w, h, len(frames), (", key=0x%s" % key) if key else ""))
        out.write("#define %s_W %d\n" % (up, w))
        out.write("#define %s_H %d\n" % (up, h))
        out.write("#define %s_FRAME_COUNT %d\n" % (up, len(frames)))
        out.write("static const uint16_t %s_frames[%d][%d] = {\n" % (name, len(frames), w * h))
        for fi, data in enumerate(frames):
            out.write("  { /* frame %d */\n" % fi)
            for i in range(0, len(data), 16):
                out.write("    " + ",".join("0x%04X" % v for v in data[i:i + 16]) + ",\n")
            out.write("  },\n")
        out.write("};\n")


def main():
    ap = argparse.ArgumentParser(description="PNG -> RGB565 C array (for gfx lib)")
    ap.add_argument("images", nargs="+", help="一个或多个 PNG（多个 = 动画帧）")
    ap.add_argument("--name", default=None, help="数组名前缀")
    ap.add_argument("--out", default=None, help="输出文件（默认 stdout）")
    ap.add_argument("--key", default=None, help="透明色键，如 F81F（仅注释标注）")
    ap.add_argument("--scale", type=float, default=1.0, help="缩放倍数（默认 1）")
    args = ap.parse_args()

    name = args.name or os.path.splitext(os.path.basename(args.images[0]))[0]
    w = h = None
    frames = []
    for p in args.images:
        iw, ih, data = load_rgb565(p, args.scale)
        if w is None:
            w, h = iw, ih
        elif (iw, ih) != (w, h):
            print("错误：所有帧尺寸必须一致（%dx%d 与 %dx%d 不一致）" % (w, h, iw, ih), file=sys.stderr)
            sys.exit(2)
        frames.append(data)

    buf = sys.stdout if not args.out else open(args.out, "w", encoding="utf-8")
    emit(name, w, h, frames, args.key, buf)
    if args.out:
        buf.close()

    total = w * h * len(frames)
    print("已生成：%s  %dx%d  帧数=%d  每帧 %d 字节  合计 %.1f KB" %
          (name, w, h, len(frames), w * h * 2, total * 2 / 1024.0), file=sys.stderr)


if __name__ == "__main__":
    main()
