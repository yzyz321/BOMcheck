# -*- coding: utf-8 -*-
"""把 pyref/layout_spec.py 的控件表导出成 C 头文件（src/ui/layout_gen.h）。

为什么要有这一步：文档、Python 原型、C 版三处的控件坐标必须一致。
手抄一份 C 的坐标表迟早会走样，所以让 Python 的 resolve() 当唯一数据源，
C 版只做「读表 + 按窗口尺寸拉伸」。

拉伸怎么表达：layout_spec 里大量坐标是 w/h 的线性函数（如 x = w - 240），
但卡片宽度含整除（cw = (w-2m-gap)/2），不是严格线性。做法是在两个尺寸上
各算一次，用「每 1000 逻辑像素的增长量」（ppm）作为斜率，C 侧按比例插值。
对整数整除带来的 ±1 像素阶梯，这个近似足够用，且不会累积误差。
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "pyref"))

import layout_spec as LS  # noqa: E402

DELTA = 400          # 采样增量（逻辑像素）
OUT = os.path.join(ROOT, "src", "ui", "layout_gen.h")

SECS = ["header", "files", "options", "actions", "results", "status"]


def ppm(v1, v2):
    """每像素增长量 ×1000。"""
    return int(round((v2 - v1) * 1000.0 / DELTA))


def main():
    base = LS.resolve(LS.BASE_W, LS.BASE_H)
    big = LS.resolve(LS.BASE_W + DELTA, LS.BASE_H + DELTA)

    ids = [c["id"] for c in base]
    assert len(ids) == len(set(ids)), "layout_spec 里有重复 id"
    assert ids == [c["id"] for c in big], "两个尺寸下的控件集合不一致"
    unknown = {c["sec"] for c in base} - set(SECS)
    assert not unknown, "未知分区：%s" % unknown

    L = []
    L.append("// 由 tools/gen_layout_c.py 从 pyref/layout_spec.py 生成，请勿手改。")
    L.append("// 数据源：layout_spec.resolve(%d, %d) 与 resolve(%d, %d) 两次采样。"
             % (LS.BASE_W, LS.BASE_H, LS.BASE_W + DELTA, LS.BASE_H + DELTA))
    L.append("#ifndef BS_LAYOUT_GEN_H")
    L.append("#define BS_LAYOUT_GEN_H")
    L.append("")
    L.append("#include \"layout.h\"")
    L.append("")
    L.append("namespace bs {")
    L.append("")
    L.append("constexpr int kBaseLogicalW = %d;" % LS.BASE_W)
    L.append("constexpr int kBaseLogicalH = %d;" % LS.BASE_H)
    L.append("constexpr int kMinLogicalW  = %d;" % LS.MIN_W)
    L.append("constexpr int kMinLogicalH  = %d;" % LS.MIN_H)
    L.append("")
    # 颜色：界面与导出必须同色，故也从这里出
    L.append("// layout_spec.COLORS 的同源副本（ARGB 无关，统一 0xRRGGBB）")
    for key in ("bg", "card", "border", "text", "muted", "hint", "accent",
                "accent_dk", "accent_bg", "amber", "ok", "err", "grid",
                "row_add", "row_del", "row_chg", "row_dup"):
        L.append("constexpr unsigned kCol%s = 0x%s;" % (key.title().replace("_", ""),
                                                        LS.COLORS[key].lstrip("#").upper()))
    L.append("")
    L.append("static const LayoutItem kLayoutItems[] = {")
    for a, b in zip(base, big):
        gx = ppm(a["x"], b["x"])
        gy = ppm(a["y"], b["y"])
        gw = ppm(a["w"], b["w"])
        gh = ppm(a["h"], b["h"])
        L.append('    {"%s", %d, %d, %d, %d, %d, %d, %d, %d, %d, "%s"},'
                 % (a["id"], SECS.index(a["sec"]), a["x"], a["y"], a["w"], a["h"],
                    gx, gy, gw, gh, a["type"]))
    L.append("};")
    L.append("")
    L.append("constexpr int kLayoutCount = %d;" % len(base))
    L.append("")
    L.append("}  // namespace bs")
    L.append("")
    L.append("#endif  // BS_LAYOUT_GEN_H")

    text = "\n".join(L) + "\n"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("生成 %s：%d 个控件，%d 字节" % (os.path.relpath(OUT, ROOT), len(base), len(text)))


if __name__ == "__main__":
    main()
