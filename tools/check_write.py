# -*- coding: utf-8 -*-
"""结果写出对账（C5）：C 的 `bstest --write` 与 Python `export_xlsx` 比。

两份文件都用 openpyxl 读回来，逐项比：
  - 页码范围：两张表的名字、行列数、冻结窗格、自动筛选范围、列宽
  - 每个非空单元格的文本与填充色（表头底色 / 新增整行 / 删除整行 /
    变更只涂变更项与有差异的成对列 / 重复料号只涂料号格 / 图例四色）
  - 识别诊断页 12 列的全部值

做法是「同一份输入，两边各写一个文件，再都读回来比」—— 而不是拿 C 的输出
去比 C 自己的 bs_result，否则写错列序这类问题两边一起错，对不出来。

用法：python tools/check_write.py [--exe 路径] [--cov]
"""
import glob
import os
import re
import shutil
import subprocess
import sys

# 这里要 openpyxl 生成参考文件、读回两边；WorkBuddy 自带的 3.13 没装它，
# 而 build_c.bat 里的 python 正是那个。缺了就自动切到 conda 的解释器。
try:
    import openpyxl  # noqa: F401
except ImportError:
    _alt = os.path.join("D:" + os.sep, "miniconda3", "python.exe")
    if os.path.exists(_alt) and os.path.abspath(sys.executable) != os.path.abspath(_alt):
        os.execv(_alt, [_alt, os.path.abspath(__file__)] + sys.argv[1:])
    raise

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "pyref"))

import bomcore as B  # noqa: E402
import samples  # noqa: E402  样本位置（tests/data，缺了退回 temp）

# 「两侧同时拒绝」只允许出现在负样本上；真实样本一律算回归
NEGATIVE = {
    "case_no_header.xlsx",   # 负样本：整张表没有表头
    "cover_first.xlsx",      # 第 0 页是封面，本来就没有 BOM 表头
    "dup_sample.xlsx",       # STRICT 模式：重复键必须中止
    "case_dup_many.xlsx",    # 同上
}


def drop(path):
    """尽力删临时目录：某些环境对「一次删几十个文件」有安全拦截
    （SAFE_DELETE_BULK），删不掉就留着，build\\tmp 本来就是临时区。"""
    try:
        shutil.rmtree(path, ignore_errors=True)
    except BaseException:                        # noqa: BLE001
        pass


BSTEST = os.path.join(ROOT, "build", "c", "bin", "bstest.exe")
# 换机器时用环境变量覆盖，不必改源码：set BOMCHECK_GCC=<mingw>\bin\gcc.exe
GCC = os.environ.get("BOMCHECK_GCC", r"D:\AICS\mingw64\bin\gcc.exe")

CORE_SRC = ["bs_file", "bs_inflate", "bs_zip", "bs_xml", "bs_xlsx",
            "bs_pn", "bs_arena", "bs_sheet", "bs_pair", "bs_zipw", "bs_write"]


def base_cfg(scope="all"):
    cfg = {"min_hit_rate": 0.60, "sample_rows": 40,
           "header_min_alias": 1, "header_threshold": 5, "dual_key": 1}
    if scope == "key":
        cfg["strike_scope"] = "KEY"
    return cfg


def norm(v):
    """把 None / 数字 / 字符串统一成字符串，避免 8 与 "8" 打架。"""
    if v is None:
        return ""
    if isinstance(v, float):
        return str(int(v)) if v == int(v) else repr(v)
    return str(v)


def fill_of(c):
    """实心填充返回 'RRGGBB'，无填充返回 ''。"""
    try:
        if c.fill is None or c.fill.patternType != "solid":
            return ""
    except (AttributeError, TypeError):
        return ""
    rgb = c.fill.fgColor.rgb
    if not isinstance(rgb, str):
        return ""
    return rgb[-6:].upper() if rgb != "00000000" else ""


def read_book(path):
    """把工作簿压成 {表名: {...}} 便于逐项比。"""
    from openpyxl import load_workbook
    wb = load_workbook(path)
    book = {}
    for name in wb.sheetnames:
        ws = wb[name]
        cells = {}
        by_fill = {}
        for row in ws.iter_rows():
            for c in row:
                v = norm(c.value)
                f = fill_of(c)
                if v == "" and f == "":
                    continue
                cells[(c.row, c.column)] = (v, f)
                if f:
                    by_fill[f] = by_fill.get(f, 0) + 1
        widths = []
        for col, dim in sorted(ws.column_dimensions.items()):
            if dim.width:
                widths.append((col, round(float(dim.width), 3)))
        book[name] = {
            "cells": cells,
            "by_fill": by_fill,
            "dims": (ws.max_row, ws.max_column),
            "freeze": ws.freeze_panes,
            "filter": ws.auto_filter.ref if ws.auto_filter else None,
            "widths": widths,
        }
    wb.close()
    return book


def py_write(op, oi, np_, ni, scope, out):
    cfg = base_cfg(scope)
    so = B.load_sheet(op, B.list_sheets(op)[oi][0], cfg)
    sn = B.load_sheet(np_, B.list_sheets(np_)[ni][0], cfg)
    pairs, add, dele, _st = B.pair(so.rows, sn.rows, cfg)
    rows = B.build_result(pairs, add, dele)
    diag = []
    for sd, path in ((so, op), (sn, np_)):
        # 第 4 列「数据行」= 建档 + 无键 + 分组标题；第 8 列「建档行」才只是 len(sd.rows)。
        # 旧实现两列都写 len(sd.rows)，C 侧照抄，逐格对账永远绿。
        diag.append([
            path, sd.name, "/".join(str(b[0]) for b in sd.blocks),
            len(sd.rows) + len(sd.unkeyable) + sd.labels,
            sd.cols.get("pn"), sd.cols.get("ref"), sd.cols.get("qty"), len(sd.rows),
            sum(1 for r in sd.rows if r.kind == "R"), sd.labels, len(sd.unkeyable),
            "%.0f%%" % (sd.hit_rate * 100),
        ])
    if os.path.exists(out):
        os.remove(out)
    B.export_xlsx(rows, diag, out)
    return rows


def c_write(op, oi, np_, ni, scope, out):
    if os.path.isfile(out):
        os.remove(out)
    r = subprocess.run([BSTEST, "--write", op, str(oi), np_, str(ni), out, scope],
                       capture_output=True)
    text = r.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    lines = [ln for ln in text.split("\n") if ln]
    if r.returncode != 0 or not lines or not lines[0].startswith("WROTE "):
        return None, lines
    return lines[0][6:], lines


def cmp_book(got, want, limit=6):
    """返回差异描述列表（最多 limit 条，另附总数）。"""
    diffs = []
    if list(got) != list(want):
        diffs.append("表名 %s ≠ %s" % (list(got), list(want)))
        return diffs, 1
    for name in want:
        a, b = got[name], want[name]
        if a["dims"] != b["dims"]:
            diffs.append("%s 尺寸 %s ≠ %s" % (name, a["dims"], b["dims"]))
        if (a["freeze"] or "") != (b["freeze"] or ""):
            diffs.append("%s 冻结 %s ≠ %s" % (name, a["freeze"], b["freeze"]))
        if (a["filter"] or "") != (b["filter"] or ""):
            diffs.append("%s 筛选范围 %s ≠ %s" % (name, a["filter"], b["filter"]))
        if a["widths"] != b["widths"]:
            diffs.append("%s 列宽\n    C : %s\n    Py: %s"
                         % (name, a["widths"], b["widths"]))
        keys = set(a["cells"]) | set(b["cells"])
        for k in sorted(keys):
            ca = a["cells"].get(k, ("", ""))
            cb = b["cells"].get(k, ("", ""))
            if ca != cb:
                diffs.append("%s %s%d  C=%r Py=%r" % (name, col_letter(k[1]), k[0], ca, cb))
                if len(diffs) >= limit:
                    return diffs, len(diffs)
    return diffs, len(diffs)


PALETTE = {"185FA5": "表头", "C6EFCE": "新增", "FFC7CE": "删除",
           "FFEB9C": "变更", "D9D2E9": "重复"}


def color_brief(colors):
    if not colors:
        return "无填色"
    parts = ["%s%d" % (PALETTE.get(k, k), colors[k]) for k in
             ("185FA5", "C6EFCE", "FFC7CE", "FFEB9C", "D9D2E9") if colors.get(k)]
    for k in sorted(colors):
        if k not in PALETTE:
            parts.append("%s%d" % (k, colors[k]))
    return " / ".join(parts)


def col_letter(n):
    s = ""
    while n:
        n, r = divmod(n - 1, 26)
        s = chr(65 + r) + s
    return s


def cases():
    t = samples.base()
    out = [
        ("demo_old.xlsx", 0, "demo_new.xlsx", 0, "all"),
        ("demo_old.xlsx", 0, "demo_new.xlsx", 0, "key"),
        # 0 行结果：autoFilter 退化到 A1:L1，图例仍在第 3 行
        ("demo_old.xlsx", 0, "demo_old.xlsx", 0, "all"),
        # 大量新增/删除：两种整行底色都吃满
        ("demo_old.xlsx", 0, "demo_new.xlsx", 1, "all"),
        # 重复料号：料号格浅紫、其余格不动
        ("dup_sample.xlsx", 0, "dup_sample.xlsx", 0, "all"),
        # 四阶段全开 + 2×3 重复
        ("cases/pair_old.xlsx", 0, "cases/pair_new.xlsx", 0, "all"),
        ("cases/pair_old.xlsx", 0, "cases/pair_new.xlsx", 0, "key"),
        # 有 XML 特殊字符的列名/料号
        ("cases/case_qty_junk.xlsx", 0, "cases/case_dup_strike.xlsx", 0, "all"),
    ]
    for p in sorted(glob.glob(os.path.join(t, "cases", "*.xlsx"))):
        rel = os.path.relpath(p, t)
        out.append((rel, 0, rel, 0, "all"))
    return [(os.path.join(t, a), oi, os.path.join(t, b), ni, sc)
            for a, oi, b, ni, sc in out]


def edge_cases(tmp):
    """两条走不到正常样本里的路：目标被占用时改名、目标目录根本不存在。"""
    op = samples.p("cases", "pair_old.xlsx")
    np_ = samples.p("cases", "pair_new.xlsx")
    bad = 0

    # 目标是个目录：fopen 必失败，应自动退到 locked_2.xlsx
    d = os.path.join(tmp, "locked.xlsx")
    os.makedirs(d, exist_ok=True)
    got, lines = c_write(op, 0, np_, 0, "all", d)
    want = os.path.join(tmp, "locked_2.xlsx")
    if got and os.path.normcase(os.path.abspath(got)) == os.path.normcase(os.path.abspath(want)) \
            and os.path.exists(want):
        print("PASS  %-52s 落到 %s" % ("目标被占用", os.path.basename(got)))
    else:
        bad += 1
        print("FAIL  %-52s got=%s %s" % ("目标被占用", got, lines[:2]))

    # 父目录不存在：重试到头也要报错，不能崩
    got, lines = c_write(op, 0, np_, 0, "all", os.path.join(tmp, "nodir", "x.xlsx"))
    if not got and lines and lines[0].startswith("ERR write"):
        print("PASS  %-52s %s" % ("父目录不存在", lines[0][:44]))
    else:
        bad += 1
        print("FAIL  %-52s got=%s %s" % ("父目录不存在", got, lines[:2]))
    return bad


def run_cov(files, tmp):
    global BSTEST
    outdir = os.path.join(ROOT, "build", "cov_write")
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, "bstest_cov.exe")
    cmd = [GCC, "-std=c99", "-O0", "--coverage", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "src", "core"), '-DBS_VERSION="cov"',
           os.path.join(ROOT, "tests", "test_core.c")]
    cmd += [os.path.join(ROOT, "src", "core", s + ".c") for s in CORE_SRC]
    cmd += ["-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("gcov 编译失败：\n" + (r.stderr or r.stdout)[-2000:])
        return 2
    subprocess.run([exe], cwd=outdir, capture_output=True)
    for op, oi, np_, ni, sc in files:
        subprocess.run([exe, "--write", op, str(oi), np_, str(ni),
                        os.path.join(tmp, "cov_out.xlsx"), sc],
                       cwd=outdir, capture_output=True)
    # 异常路径也要进覆盖率：改名重试、彻底写不进去（要切到 cov 版可执行文件）
    keep = BSTEST
    BSTEST = exe
    try:
        edge_cases(tmp)
    finally:
        BSTEST = keep
    # 异常路径也要进覆盖率：改名重试、彻底写不进去（要切到 cov 版可执行文件）
    keep = BSTEST
    BSTEST = exe
    try:
        edge_cases(tmp)
    finally:
        BSTEST = keep
    print("覆盖  跑了 %d 组写出" % len(files))
    for src in ("bs_write.c", "bs_zipw.c"):
        subprocess.run(["gcov", "-b", "-c",
                        "-o", "bstest_cov-" + src.replace(".c", ".gcno"),
                        os.path.join(ROOT, "src", "core", src)],
                       cwd=outdir, capture_output=True, text=True)
        gc = os.path.join(outdir, src + ".gcov")
        if not os.path.exists(gc):
            continue
        lines = open(gc, encoding="utf-8", errors="replace").read().split("\n")
        total = hit = 0
        branch_ok = branch_total = take_ok = 0
        cur = 0
        missed = []
        for ln in lines:
            m = re.match(r"\s*([#\d-]+):\s*(\d+):", ln)
            if m:
                cur = int(m.group(2))
                if m.group(1) != "-":
                    total += 1
                    if m.group(1) != "#####":
                        hit += 1
                continue
            mb = re.match(r"branch\s+(\d+)\s+(?:taken\s+(\d+)|never executed)", ln.strip())
            if mb:
                branch_total += 1
                if mb.group(2) is not None:
                    branch_ok += 1
                    if int(mb.group(2)) > 0:
                        take_ok += 1
                else:
                    missed.append(cur)
        if total:
            print("覆盖  %-12s 行 %.2f%% of %d / 分支执行 %.2f%% of %d / 至少一次 %.2f%%"
                  % (src, 100.0 * hit / total, total,
                     100.0 * branch_ok / branch_total if branch_total else 0.0, branch_total,
                     100.0 * take_ok / branch_total if branch_total else 0.0))
        if missed:
            print("        未走到分支的行：%s" % ", ".join(str(n) for n in sorted(set(missed))))
    return 0


def bench():
    """2 万行、2000 条变更：量解析 + 配对 + 写出的整链路与文件体积。

    样本现场生成在 build/tmp（不进 temp/cases，否则每次全量回归都要多跑几秒）。
    """
    import time
    from openpyxl import Workbook

    tmp = os.path.join(ROOT, "build", "tmp")
    os.makedirs(tmp, exist_ok=True)
    n = 20000
    pa = os.path.join(tmp, "bench_a.xlsx")
    pb = os.path.join(tmp, "bench_b.xlsx")
    for path, bump in ((pa, 0), (pb, 1)):
        wb = Workbook()
        ws = wb.active
        ws.title = "BOM"
        ws.append(["Part Number", "位号", "数量"])
        for i in range(n):
            qty = 1 + (1 if bump and i % 10 == 0 else 0)
            ws.append(["111-%06d-A01" % (i + 1), "R%d" % (i + 1), qty])
        wb.save(path)
        wb.close()
    out = os.path.join(tmp, "bench_out.xlsx")
    t0 = time.time()
    rc = subprocess.run([BSTEST, "--write", pa, "0", pb, "0", out, "all"],
                        capture_output=True)
    dt = time.time() - t0
    if rc.returncode != 0:
        print("基准失败：" + rc.stdout.decode("utf-8", "replace")[:200])
        return 1
    rows = int([ln for ln in rc.stdout.decode("utf-8", "replace").split("\n")
                if ln.startswith("rows ")][0].split()[1])
    size = os.path.getsize(out)
    print("基准  %d 行 × 2 → %d 条差异，整链路 %.2f s，输出 %.1f MB（stored）"
          % (n, rows, dt, size / 1048576.0))
    return 0


def diag_identity(book):
    """诊断页的自洽性：数据行 == 建档行 + 分组标签 + 无法建档。

    只读 C 自己写出的那一页，不依赖 Python 侧同不同步 —— 两边抄同一个错时
    逐格对账会一致，但这条恒等式会红。
    """
    sheet = book.get("识别诊断")
    if not sheet:
        return []
    cells = sheet["cells"]
    out = []
    for r in sorted({r for (r, c) in cells if c == 4 and r > 1}):
        vals = []
        for c in (4, 8, 10, 11):
            v = cells.get((r, c), ("", ""))[0]
            try:
                vals.append(int(float(v)))
            except (TypeError, ValueError):
                vals = None
                break
        if vals is None:
            continue
        dr, br, lb, uk = vals
        if dr != br + lb + uk:
            out.append("第 %d 行 数据行 %d ≠ 建档 %d + 标签 %d + 无键 %d"
                       % (r, dr, br, lb, uk))
    return out


def main():
    global BSTEST
    argv = sys.argv[1:]
    want_cov = "--cov" in argv
    if "--exe" in argv:
        BSTEST = argv[argv.index("--exe") + 1]
    if not os.path.exists(BSTEST):
        print("找不到 %s，先跑 build_c.bat" % BSTEST)
        return 2

    # 输出放在 build/tmp 而不是系统临时目录：有的沙箱按进程启动位置限制可写
    # 路径，写系统 Temp 会被拒（EACCES），跟程序本身无关。
    if "--bench" in argv:
        return bench()

    tmp = os.path.join(ROOT, "build", "tmp")
    drop(tmp)
    os.makedirs(tmp, exist_ok=True)
    files = cases()
    bad = 0
    try:
        for k, (op, oi, np_, ni, sc) in enumerate(files):
            tag = "%s#%d vs %s#%d%s" % (
                os.path.basename(op), oi, os.path.basename(np_), ni,
                "" if sc == "all" else " [%s]" % sc)
            po = os.path.join(tmp, "py_%02d.xlsx" % k)
            co = os.path.join(tmp, "c_%02d.xlsx" % k)
            try:
                rows = py_write(op, oi, np_, ni, sc, po)
                got_path, lines = c_write(op, oi, np_, ni, sc, co)
                if not got_path:
                    bad += 1
                    print("FAIL  %-52s C 没写出：%s" % (tag, lines[:2]))
                    continue
                if os.path.normcase(os.path.abspath(got_path)) != \
                        os.path.normcase(os.path.abspath(co)):
                    bad += 1
                    print("FAIL  %-52s C 实际写到 %s" % (tag, got_path))
                    continue
                a = read_book(co)
                b = read_book(po)
                for msg in diag_identity(a):
                    bad += 1
                    print("FAIL  %-52s 诊断恒等式：%s" % (tag, msg))
                diffs, n = cmp_book(a, b)
                n_cell = sum(len(v["cells"]) for v in a.values())
                colors = {}
                for v in a.values():
                    for k, cnt in v["by_fill"].items():   # 别用 n，会盖掉差异数
                        colors[k] = colors.get(k, 0) + cnt
                if n:
                    bad += 1
                    print("FAIL  %-52s %d 处不同" % (tag, n))
                    for d in diffs[:6]:
                        print("        " + d.replace("\n", "\n        "))
                else:
                    print("PASS  %-52s %d 行 / %d 格 / %s"
                          % (tag, len(rows), n_cell, color_brief(colors)))
            except B.BomError as e:
                # 只要求「两边都拒」：识别阶段拒绝的样本根本走不到写出，
                # 错误文案面向用户，措辞还会改，不逐字对齐。
                # 但负样本白名单之外一律 FAIL —— 真实样本两侧同时拒绝就是回归。
                got_path, lines = c_write(op, oi, np_, ni, sc, co)
                if not got_path and lines and lines[0].startswith("ERR ") and \
                        (os.path.basename(op) in NEGATIVE or
                         os.path.basename(np_) in NEGATIVE):
                    print("PASS  %-52s 负样本两边都拒（%s）" % (tag, str(e)[:26]))
                else:
                    bad += 1
                    print("FAIL  %-52s Python 拒了（C：%s）" % (tag, lines[:1]))
            except Exception as e:                    # noqa: BLE001
                bad += 1
                print("FAIL  %-52s %s: %s" % (tag, type(e).__name__, e))
            # 不逐组删 py_/c_ 两个文件：下一轮同名直接覆盖，一次删五十个文件会
            # 撞上批量删除的安全拦截（SAFE_DELETE_BULK），整条构建链就断在这
    finally:
        if not want_cov:
            drop(tmp)

    bad += edge_cases(tmp)
    print("\n%d 组写出 + 2 条异常路径，%d 个失败" % (len(files), bad))
    rc = 1 if bad else 0
    if want_cov:
        rc |= run_cov(files, tmp)
        drop(tmp)
    return rc


if __name__ == "__main__":
    sys.exit(main())
