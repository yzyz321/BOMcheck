# -*- coding: utf-8 -*-
"""布局对账：C 版实际矩形 vs Python 的 layout_spec，并查重叠 / 越界。

C 版的坐标表由 tools/gen_layout_c.py 生成，最容易出的错是「改了 Python、
忘了重新生成」——那种走样看代码看不出来。这里直接让 exe 打印 --dump-layout，
再和 Python 的 resolve() 对账；另外在基准 / 最小 / 大窗三种尺寸下各查一遍
重叠与越界，等价于文档里「100/150/200% 三档无重叠」的结构性验证
（DPI 只等比缩放，逻辑尺寸一致时排布关系一致）。

用法：python tools/check_layout.py [exe 路径]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "pyref"))

import layout_spec as LS  # noqa: E402
from findexe import find_exe  # noqa: E402

# 不传参时用「版本号最大的产物」，不写死版本号
EXE = sys.argv[1] if len(sys.argv) > 1 else find_exe()

# 不参与重叠检查的：容器、分割线，以及表格与日志（同一位置切页签，刻意重叠）
SKIP_TYPE = {"frame", "line"}
SKIP_ID = {"res.table", "res.log"}

# 基准 / 最小 / 大窗。C 侧用 ppm 斜率近似整除去尾，非基准尺寸允许 2px 偏差
SIZES = [(LS.BASE_W, LS.BASE_H, 0), (LS.MIN_W, LS.MIN_H, 2), (1400, 900, 2)]


def load_dump(w, h):
    out = subprocess.run([EXE, "--client-size", "%dx%d" % (w, h), "--dump-layout"],
                         capture_output=True)
    rows = {}
    for ln in out.stdout.decode("utf-8", "replace").splitlines():
        parts = ln.rstrip("\r").split("\t")
        if len(parts) != 5:
            continue
        rows[parts[0]] = tuple(int(v) for v in parts[1:])
    return rows


def overlap(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    dx = min(ax + aw, bx + bw) - max(ax, bx)
    dy = min(ay + ah, by + bh) - max(ay, by)
    return dx * dy if dx > 0 and dy > 0 else 0


def check_size(w, h, tol):
    got = load_dump(w, h)
    if not got:
        return None, ["--dump-layout 无输出（%dx%d）" % (w, h)], [], []

    py = {c["id"]: (c["x"], c["y"], c["w"], c["h"]) for c in LS.resolve(w, h)}
    diff, over, out_of = [], [], []

    for cid, rect in py.items():
        if cid not in got:
            diff.append("缺 %s" % cid)
        elif max(abs(a - b) for a, b in zip(got[cid], rect)) > tol:
            diff.append("%s C=%s Py=%s" % (cid, got[cid], rect))
    for cid in got:
        if cid not in py:
            diff.append("多 %s" % cid)

    for cid, (x, y, rw, rh) in got.items():
        if x < 0 or y < 0 or x + rw > w or y + rh > h:
            out_of.append("%s (%d,%d,%d,%d) 超出 %dx%d" % (cid, x, y, rw, rh, w, h))

    items = [(c["id"], got[c["id"]]) for c in LS.resolve(w, h)
             if c["type"] not in SKIP_TYPE and c["id"] not in SKIP_ID and c["id"] in got]
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            area = overlap(items[i][1], items[j][1])
            if area > 2:
                over.append("%s × %s（%d px²）" % (items[i][0], items[j][0], area))
    return got, diff, over, out_of


def main():
    if not os.path.exists(EXE):
        print("找不到 exe：%s" % EXE)
        return 2

    bad = 0
    for w, h, tol in SIZES:
        got, diff, over, out_of = check_size(w, h, tol)
        if got is None:
            print("× %dx%d：%s" % (w, h, diff[0]))
            bad += 1
            continue
        tag = "基准" if (w, h) == (LS.BASE_W, LS.BASE_H) else "校验"
        print("%s %dx%d：控件 %d · 不一致 %d · 重叠 %d · 越界 %d"
              % (tag, w, h, len(got), len(diff), len(over), len(out_of)))
        for ln in (diff + over + out_of)[:8]:
            print("    × " + ln)
        bad += len(diff) + len(over) + len(out_of)

    print("结果：%s（exe=%s）" % ("PASS" if bad == 0 else "FAIL 共 %d 处" % bad,
                               os.path.basename(EXE)))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
