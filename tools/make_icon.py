#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把设计稿 jpg 转成 Windows 多尺寸 .ico。

为什么要裁边 + 抠底：
  1. 设计稿四周是纯白背景，直接缩放会在深色任务栏上留一个白方块；
  2. 圆角卡片外侧的白边会被算进图标，导致 32px 下主体缩得很小。

做法：从四边 floodfill 标出「与画布边界连通的近白像素」→ 置 alpha=0 →
    裁到主体 bbox → 逐尺寸 LANCZOS 缩放（alpha 通道一起缩放即得抗锯齿边缘）。
小尺寸（<=32）额外做一次 UnsharpMask，抵消缩放糊化。

用法：
  python tools/make_icon.py                       # 默认路线
  python tools/make_icon.py --src xx.jpg --out yy.ico
"""
from __future__ import annotations

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

# Windows 资源管理器/任务栏实际会用到的档位；20/40 是 125% / 250% 缩放时的取值
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

# 视为「背景白」的阈值。设计稿的浅紫描边约 224~238，低于它，所以不会被误切
WHITE_MIN = 242


def cut_background(src: Image.Image) -> Image.Image:
    """抠掉与画布边界连通的近白区域，返回 RGBA 且裁到主体 bbox 的图。"""
    im = src.convert("RGB")
    w, h = im.size
    px = im.load()

    # 1) 二值化出「可能是背景」的掩码：三个通道都很亮
    mask = Image.new("L", (w, h), 0)
    mp = mask.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if r >= WHITE_MIN and g >= WHITE_MIN and b >= WHITE_MIN:
                mp[x, y] = 255

    # 2) 只保留与边界连通的部分：从四边若干点 floodfill 成 128
    seeds = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    seeds += [(w // 2, 0), (w // 2, h - 1), (0, h // 2), (w - 1, h // 2)]
    for pt in seeds:
        if mask.getpixel(pt) == 255:
            ImageDraw.floodfill(mask, pt, 128)

    # 3) 外部 -> 透明；内部（卡片里的白纸）保持不透明
    alpha = mask.point(lambda v: 0 if v == 128 else 255)
    rgba = im.convert("RGBA")
    rgba.putalpha(alpha)

    # 4) 裁到主体范围，避免四周留白把小尺寸主体压小
    box = alpha.getbbox()
    if box:
        pad = 1
        box = (max(0, box[0] - pad), max(0, box[1] - pad),
               min(w, box[2] + pad), min(h, box[3] + pad))
        rgba = rgba.crop(box)

    # 5) 补成正方形：ico 每档都是方的，直接 resize 会把 786x771 拉变形
    side = max(rgba.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(rgba, ((side - rgba.size[0]) // 2, (side - rgba.size[1]) // 2))
    return square


# 小到一定程度就别硬缩原图了：16px 下那两张纸只剩两块白斑，改成简化造型
SIMPLE_UPTO = 24

# 与设计稿取色一致：左上蓝 -> 右下紫
GRAD_A = (46, 111, 224)
GRAD_B = (139, 107, 232)


def simplified(size: int) -> Image.Image:
    """小尺寸专用：渐变圆角底 + 白色放大镜，8x 超采样后降采样出抗锯齿。"""
    S = size * 8
    # 渐变先在小图上算，再放大；逐像素在大图上算纯属浪费
    g = Image.new("RGB", (72, 72))
    gp = g.load()
    for y in range(72):
        for x in range(72):
            t = (x + y) / 142.0
            gp[x, y] = tuple(int(GRAD_A[i] + (GRAD_B[i] - GRAD_A[i]) * t)
                             for i in range(3))
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    round_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(round_mask).rounded_rectangle(
        [0, 0, S - 1, S - 1], radius=int(S * 0.19), fill=255)
    img.paste(g.resize((S, S), Image.BICUBIC), (0, 0), round_mask)

    d = ImageDraw.Draw(img)
    cx, cy, r = 0.45 * S, 0.42 * S, 0.215 * S
    stroke = max(1, int(0.095 * S))
    white = (255, 255, 255, 255)
    inner = r - stroke

    # 镜圈用「环形 mask + paste」：ImageDraw 的 alpha=0 是直接覆盖像素，会把渐变挖成透明洞
    ring = Image.new("L", (S, S), 0)
    rd = ImageDraw.Draw(ring)
    rd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    rd.ellipse([cx - inner, cy - inner, cx + inner, cy + inner], fill=0)
    img.paste(Image.new("RGB", (S, S), (255, 255, 255)), (0, 0), ring)

    # 镜片：半透明白，弱化一点避免小尺寸发死
    lens = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(lens).ellipse(
        [cx - inner, cy - inner, cx + inner, cy + inner], fill=(226, 236, 255, 96))
    img = Image.alpha_composite(img, lens)

    # 镜柄：45 度，PIL 线段没有圆头，两端各补一个圆
    d = ImageDraw.Draw(img)
    x0, y0 = cx + r * 0.70, cy + r * 0.70
    x1, y1 = cx + r * 1.72, cy + r * 1.72
    d.line([x0, y0, x1, y1], fill=white, width=stroke)
    for px_, py_ in ((x0, y0), (x1, y1)):
        d.ellipse([px_ - stroke / 2, py_ - stroke / 2,
                   px_ + stroke / 2, py_ + stroke / 2], fill=white)
    return img.resize((size, size), Image.LANCZOS)


def scale_one(master: Image.Image, size: int) -> Image.Image:
    if size <= SIMPLE_UPTO:
        return simplified(size)

    big = max(master.size)
    # 逐级折半缩到目标附近，比一步 LANCZOS 更少振铃
    cur = master
    while cur.size[0] // 2 >= size and cur.size[1] // 2 >= size:
        cur = cur.resize((cur.size[0] // 2, cur.size[1] // 2), Image.LANCZOS)
    out = cur.resize((size, size), Image.LANCZOS)

    if size <= 32:
        # 只锐化颜色通道；alpha 参与锐化会出现毛边
        rgb = out.convert("RGB").filter(
            ImageFilter.UnsharpMask(radius=0.7, percent=110, threshold=0))
        out = rgb.convert("RGBA")
        out.putalpha(master.resize((size, size), Image.LANCZOS).getchannel("A"))
    return out


def build(src_path: str, out_ico: str, preview_dir: str | None) -> None:
    src = Image.open(src_path)
    print(f"[icon] 源图 {src.size[0]}x{src.size[1]}  {src.mode}")
    master = cut_background(src)
    print(f"[icon] 抠底裁边后 {master.size[0]}x{master.size[1]}")

    frames = [scale_one(master, s) for s in SIZES]
    os.makedirs(os.path.dirname(os.path.abspath(out_ico)), exist_ok=True)
    # PIL 写 ico 时以 quality 控制 PNG 块压缩；256 档自动存 PNG 格式，体积小且兼容
    frames[-1].save(out_ico, format="ICO",
                    sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    print(f"[icon] 写出 {out_ico}  共 {len(SIZES)} 档  "
          f"{os.path.getsize(out_ico) / 1024:.1f} KB")

    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)
        # 单档放大图：用来肉眼检查 16/32 是否还认得出
        for s in (16, 24, 32, 48):
            f = frames[SIZES.index(s)]
            f.resize((s * 12, s * 12), Image.NEAREST).save(
                os.path.join(preview_dir, f"zoom_{s}.png"))
        # 深色/浅色底对照表，检查透明边缘有没有白边
        sheet = Image.new("RGBA", (1100, 300), (255, 255, 255, 255))
        d = ImageDraw.Draw(sheet)
        d.rectangle([550, 0, 1099, 299], fill=(32, 32, 32, 255))
        x = 20
        for s in (16, 24, 32, 48, 64):
            f = frames[SIZES.index(s)]
            sheet.alpha_composite(f, (x, 40))
            sheet.alpha_composite(f, (x + 550, 40))
            d.text((x, 120), f"{s}px", fill=(60, 60, 60, 255))
            d.text((x + 550, 120), f"{s}px", fill=(220, 220, 220, 255))
            x += s + 70
        sheet.save(os.path.join(preview_dir, "sheet.png"))
        print(f"[icon] 预览图写到 {preview_dir}")


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(
        here, "temp", "BOM比对程序图标设计.jpg"))
    ap.add_argument("--out", default=os.path.join(here, "src", "res", "app.ico"))
    ap.add_argument("--preview", default=os.path.join(here, "build", "icons"))
    a = ap.parse_args()
    if not os.path.exists(a.src):
        print(f"[icon] 找不到源图：{a.src}")
        return 1
    build(a.src, a.out, a.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
