# -*- coding: utf-8 -*-
"""读取链对账：C 版 --xlsx-dump 与 Python oracle（pyref/bomcore.py 的 _Grid）逐格比对。

比对内容：工作表列表、sst/字体/xf 数量、每张表的 max_row/max_col/单元格数/行数/
删除线数/合并区数，以及每一个 (行, 列, 删除线, 值)。任何一处不同都算失败。

用法：python tools/check_xlsx.py [--exe 路径] [文件...]
不给文件就扫 temp/*.xlsx。
"""
import glob
import os
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "pyref"))

import bomcore as B  # noqa: E402

from findexe import find_exe  # noqa: E402
import samples  # noqa: E402  样本位置（tests/data，缺了退回 temp）

EXE = find_exe()


def esc(s):
    if s is None:
        return ""
    out = []
    for ch in str(s):
        if ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        else:
            out.append(ch)
    return "".join(out)


def c_dump(path):
    out = subprocess.run([EXE, "--xlsx-dump", path], capture_output=True)
    text = out.stdout.decode("utf-8", "replace")
    lines = text.replace("\r\n", "\n").split("\n")
    return [ln for ln in lines if ln != ""]


def py_dump(path):
    """用 bomcore 的同一套底层函数生成期望值。"""
    exp = []
    with zipfile.ZipFile(path) as z:
        sheets = B.list_sheets(path)
        sst = B._load_sst(z)
        fonts, xfs = B._load_styles(z)

        exp.append("sheets=%d" % len(sheets))
        for i, (name, part) in enumerate(sheets):
            exp.append("sheet%d=%s|%s" % (i, name, part))
        exp.append("sst=%d fonts=%d xfs=%d" % (len(sst), len(fonts), len(xfs)))
        exp.append("strike_fonts=" + ",".join("1" if f else "0" for f in fonts))

        for i, (name, part) in enumerate(sheets):
            g = B._Grid(z, part, sst, fonts, xfs)
            n_cells = sum(len(v) for v in g.cells.values())
            n_strike = sum(1 for row in g.cells.values() for v in row.values() if v[1])
            exp.append("--- %d ---" % i)
            exp.append("stats rows=%d cols=%d cells=%d nrows=%d strike=%d merges=%d"
                       % (g.maxr, g.maxc, n_cells, len(g.cells), n_strike, len(g.merges)))
            rows = []
            for r, row in g.cells.items():
                for c, (val, st) in row.items():
                    rows.append((r, c, 1 if st else 0, val))
            rows.sort(key=lambda t: (t[0], t[1]))
            for r, c, st, val in rows:
                exp.append("cell %d %d %d %s" % (r, c, st, esc(val)))
    return exp


def compare(path):
    got = c_dump(path)
    want = py_dump(path)
    if got == want:
        n_cell = sum(1 for ln in got if ln.startswith("cell "))
        n_sheet = sum(1 for ln in got if ln.startswith("--- "))
        return True, "%d 表 / %d 格" % (n_sheet, n_cell)

    for i in range(max(len(got), len(want))):
        a = got[i] if i < len(got) else "<缺行>"
        b = want[i] if i < len(want) else "<缺行>"
        if a != b:
            # 值里可能有超长文本，截断显示
            return False, "第 %d 行不同\n    C : %s\n    Py: %s" % (
                i + 1, a[:200], b[:200])
    return False, "行数不同 C=%d Py=%d" % (len(got), len(want))


def main():
    argv = sys.argv[1:]
    exe = None
    if "--exe" in argv:
        k = argv.index("--exe")
        exe = argv[k + 1]
        del argv[k:k + 2]
    if exe:
        global EXE
        EXE = exe

    # 默认跳过 big_*.xlsx：那是压测样本（40 万格），逐个对账要 10 秒，
    # 想跑就显式给路径
    files = argv or [p for p in samples.xlsx()
                     if not os.path.basename(p).startswith("big_")]
    if not files:
        print("没有样本文件")
        return 2

    bad = 0
    for p in files:
        ok, msg = compare(p)
        name = os.path.basename(p)
        if ok:
            print("PASS  %-40s %s" % (name, msg))
        else:
            bad += 1
            print("FAIL  %-40s %s" % (name, msg))
    print("\n%d 个文件，%d 个失败" % (len(files), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
