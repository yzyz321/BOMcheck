# -*- coding: utf-8 -*-
"""生成 10 万行级别的压测 BOM（新旧一对），用于 C9 性能验收。

数据刻意做成「真实 BOM 的形状」而不是随机噪声：
  · 料号是 xxx-xxxxxx-xxx，几乎全表唯一 → 阶段 A 一击命中，阶段 C 只剩零星残行
  · 每行 2~4 个位号，跨行有重叠（同一条网络上的并联电容）
  · 新表相对旧表：约 3% 改数量、1% 删除、1% 新增、少量位号增减
这样测出来的时间才是「用户真会遇到的时间」，而不是「10 万行全不匹配」的病态值。

用法：
  python tools/gen_big.py                    # 默认 10 万行 -> build/bench/
  python tools/gen_big.py --rows 20000
  python tools/gen_big.py --mode nopn        # 全表无料号，专打阶段 C 的 O(n²)
"""
from __future__ import annotations

import argparse
import os
import random

from openpyxl import Workbook

HEADER = ["序号", "位号", "料号", "数量", "描述"]
DESC = ["贴片电阻", "贴片电容", "排针", "晶振", "MOS 管", "电感", "保险丝", "LED"]


def make_pn(i: int) -> str:
    # 固定格式 xxx-xxxxxx-xxx；用几个互质的模保证同批次内不重复
    return "%03d-%06d-%03d" % (100 + i % 900, i % 999983, i % 89)


def make_ref(i: int, rnd: random.Random) -> str:
    n = 2 + (i % 3)          # 2~4 个位号
    base = (i * 7) % 2000
    kind = "CRLQ"[(i // 3) % 4]
    return ",".join("%s%d" % (kind, base + k) for k in range(n))


def write_book(path: str, rows: int, mode: str, seed: int) -> None:
    rnd = random.Random(seed)
    wb = Workbook(write_only=True)
    ws = wb.create_sheet("BOM")
    ws.append(HEADER)
    for i in range(rows):
        pn = "" if mode == "nopn" else make_pn(i)
        ws.append([i + 1, make_ref(i, rnd), pn, i % 10 + 1, DESC[i % len(DESC)]])
    wb.save(path)


def build_new(old_rows: int, mode: str, seed: int) -> list:
    """按旧表派生出新表：改数量 / 删除 / 新增 / 位号增减，比例贴近真实评审单。"""
    rnd = random.Random(seed + 1)
    out = []
    for i in range(old_rows):
        if i % 97 == 0:                     # 约 1% 删除
            continue
        pn = "" if mode == "nopn" else make_pn(i)
        ref = make_ref(i, rnd)
        qty = i % 10 + 1
        if i % 37 == 0:                     # 约 3% 改数量
            qty += 1
        if i % 211 == 0 and mode != "nopn":  # 少量位号增减
            ref = ref + ",C%d" % (5000 + i % 300)
        out.append([len(out) + 1, ref, pn, qty, DESC[i % len(DESC)]])
        if i % 101 == 0:                    # 约 1% 新增
            j = old_rows + i
            out.append([len(out) + 1, make_ref(j, rnd),
                        "" if mode == "nopn" else make_pn(j), 1, "新增件"])
    return out


def write_new(path: str, rows: list) -> None:
    wb = Workbook(write_only=True)
    ws = wb.create_sheet("BOM")
    ws.append(HEADER)
    for r in rows:
        ws.append(r)
    wb.save(path)


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=100000)
    ap.add_argument("--mode", choices=["normal", "nopn"], default="normal")
    ap.add_argument("--seed", type=int, default=20261105)
    ap.add_argument("--out", default=os.path.join(here, "build", "bench"))
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    suffix = "" if a.mode == "normal" else "_nopn"
    old = os.path.join(a.out, "big_old%s.xlsx" % suffix)
    new = os.path.join(a.out, "big_new%s.xlsx" % suffix)

    print("[gen] 旧表 %d 行 -> %s" % (a.rows, old))
    write_book(old, a.rows, a.mode, a.seed)
    rows = build_new(a.rows, a.mode, a.seed)
    print("[gen] 新表 %d 行 -> %s" % (len(rows), new))
    write_new(new, rows)

    for p in (old, new):
        print("[gen] %8.1f MB  %s" % (os.path.getsize(p) / 1048576.0, p))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
