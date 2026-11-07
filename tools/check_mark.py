# -*- coding: utf-8 -*-
"""标注回写对账（C6）：C 的 `bstest --mark` 与 Python `mark_plan` / `mark_workbook` 比。

比三件事：

1. **涂色位置与颜色**：期望值来自 `bomcore.mark_plan`（新/旧两侧各自要涂哪些格），
   再从 C 写出的文件里读回实际底色逐格比；另外再让 Python `mark_workbook` 也写
   一份，三方差一致才算过 —— 只比 C 与自己的计划，一起错就查不出来。
2. **原有样式不能丢**：字体、边框、对齐、数字格式逐格比源文件。这是 C6 最容易
   翻车的地方（`<c s="5">` 直接换成只有 fill 的新 xf 会把字体边框全抹掉）。
3. **源文件没被碰过**：跑前跑后算 sha256。

用法：python tools/check_mark.py [--exe 路径] [--cov]
"""
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

try:
    import openpyxl  # noqa: F401
except ImportError:
    _alt = os.environ.get("BOMCHECK_PY", r"D:\miniconda3\python.exe")
    if os.path.exists(_alt) and os.path.abspath(sys.executable) != os.path.abspath(_alt):
        os.execv(_alt, [_alt, os.path.abspath(__file__)] + sys.argv[1:])
    raise

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "pyref"))

import bomcore as B  # noqa: E402
import samples  # noqa: E402  样本位置（tests/data，缺了退回 temp）

BSTEST = os.path.join(ROOT, "build", "c", "bin", "bstest.exe")
# 换机器时用环境变量覆盖，不必改源码：set BOMCHECK_GCC=<mingw>\bin\gcc.exe
GCC = os.environ.get("BOMCHECK_GCC", r"D:\AICS\mingw64\bin\gcc.exe")

CORE_SRC = ["bs_file", "bs_inflate", "bs_zip", "bs_xml", "bs_xlsx",
            "bs_pn", "bs_arena", "bs_sheet", "bs_pair", "bs_zipw", "bs_write", "bs_mark"]


def base_cfg(scope="all"):
    cfg = {"min_hit_rate": 0.60, "sample_rows": 40,
           "header_min_alias": 1, "header_threshold": 5, "dual_key": 1}
    if scope == "key":
        cfg["strike_scope"] = "KEY"
    return cfg


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fill_of(c):
    try:
        if c.fill is None or c.fill.patternType != "solid":
            return ""
    except (AttributeError, TypeError):
        return ""
    rgb = c.fill.fgColor.rgb
    if not isinstance(rgb, str) or rgb == "00000000":
        return ""
    return rgb[-6:].upper()


def style_of(c):
    """除了底色之外的全部可见样式。"""
    f = c.font
    color = ""
    try:
        if f.color is not None and f.color.type == "rgb":
            color = f.color.rgb
    except (AttributeError, ValueError):
        color = ""
    b = c.border
    return (f.name, f.sz, bool(f.b), bool(f.i), bool(f.u), color,
            b.left.style, b.right.style, b.top.style, b.bottom.style,
            c.number_format, c.alignment.horizontal, c.alignment.vertical,
            bool(c.alignment.wrap_text))


def comment_of(c):
    """(文本, 作者)；没有批注返回 None。"""
    try:
        cm = c.comment
    except AttributeError:
        return None
    if cm is None:
        return None
    return (cm.text or "", cm.author or "")


def norm(v):
    if v is None:
        return ""
    if isinstance(v, float):
        return str(int(v)) if v == int(v) else repr(v)
    return str(v)


def read_sheet(path, name=None):
    from openpyxl import load_workbook
    wb = load_workbook(path)
    ws = wb[name or wb.sheetnames[0]]
    data = {}
    notes = {}
    for row in ws.iter_rows():
        for c in row:
            data[(c.row, c.column)] = (norm(c.value), fill_of(c), style_of(c))
            cm = comment_of(c)
            if cm:
                notes[(c.row, c.column)] = cm
    info = {
        "cells": data,
        "notes": notes,
        "names": list(wb.sheetnames),
        "dims": ws.calculate_dimension(),
        "merges": sorted(str(m) for m in ws.merged_cells.ranges),
        "widths": sorted((k, round(float(v.width), 3))
                         for k, v in ws.column_dimensions.items() if v.width),
        "freeze": ws.freeze_panes,
    }
    wb.close()
    return info


def expected_marks(op, oi, np_, ni, scope):
    """返回 (plan, notes, raw, so, sn)：
    plan 是 {(r,c): 色值}，notes 是 {(r,c): (文本, 作者)}，raw 是带 note 的原计划。"""
    cfg = base_cfg(scope)
    so = B.load_sheet(op, B.list_sheets(op)[oi][0], cfg)
    sn = B.load_sheet(np_, B.list_sheets(np_)[ni][0], cfg)
    pairs, add, dele, _st = B.pair(so.rows, sn.rows, cfg)
    raw = B.mark_plan(pairs, add, dele, so, sn)
    out = {}
    notes = {}
    for side in ("old", "new"):
        d = {}
        nt = {}
        for r, c, key, note in raw[side]:
            d[(r, c)] = B.MARK_COLORS[key]
            if note:
                nt[(r, c)] = (note, "BOMCheck")
        out[side] = d
        notes[side] = nt
    return out, notes, raw, so, sn


def run_c_mark(op, oi, np_, ni, scope, tmp):
    """跑 C 的标注，返回 (两侧实际路径, C 自报的批注条数)。"""
    r = subprocess.run([BSTEST, "--mark", op, str(oi), np_, str(ni), tmp, scope],
                       cwd=ROOT, capture_output=True)
    out = r.stdout.decode("utf-8", "replace").splitlines()
    got = {}
    notes = {}
    for ln in out:
        if ln.startswith("MARK "):
            _tag, side, path = ln.split(" ", 2)
            got[side] = os.path.join(ROOT, path) if not os.path.isabs(path) else path
        elif ln.startswith("NOTES "):
            _tag, side, n = ln.split(" ", 2)
            notes[side] = int(n)
    if len(got) != 2:
        raise RuntimeError("C 没写出两个文件：" + " | ".join(out[:4]))
    return got["old"], got["new"], notes


def run_py_mark(op, oi, np_, ni, scope, tmp):
    plan, notes, raw, so, sn = expected_marks(op, oi, np_, ni, scope)
    out = {}
    for side, src, sd in (("old", op, so), ("new", np_, sn)):
        dst = os.path.join(tmp, "py_%s.xlsx" % side)
        rev = {v: k for k, v in B.MARK_COLORS.items()}
        marks = [(r, c, rev[hv], note) for r, c, hv, note in
                 ((r, c, B.MARK_COLORS[key], note) for r, c, key, note in raw[side])]
        out[side] = B.mark_workbook(src, sd.name, marks, dst)
    return out, plan, notes


def check_side(tag, src, marked, exp, tmp, bad, exp_notes=None):
    """比四件事：涂色集合、原有样式、批注、页面结构。"""
    a = read_sheet(src)
    b = read_sheet(marked, a["names"][0] if a["names"] else None)

    diffs = []
    if a["names"] != b["names"]:
        diffs.append("工作表名不同 %s vs %s" % (a["names"], b["names"]))
    if a["dims"] != b["dims"]:
        diffs.append("范围不同 %s vs %s" % (a["dims"], b["dims"]))
    if a["merges"] != b["merges"]:
        diffs.append("合并区不同 %s vs %s" % (a["merges"][:3], b["merges"][:3]))
    if a["widths"] != b["widths"]:
        diffs.append("列宽不同")

    if exp_notes is not None and b["notes"] != exp_notes:
        want_n = exp_notes
        got_n = b["notes"]
        only = sorted(set(got_n) - set(want_n))[:3]
        miss = sorted(set(want_n) - set(got_n))[:3]
        wrong = [(k, want_n[k], got_n[k]) for k in set(want_n) & set(got_n)
                 if want_n[k] != got_n[k]][:2]
        diffs.append("批注不符：多 %s 少 %s 文本差 %s" % (only, miss, wrong))

    keys = set(a["cells"]) | set(b["cells"])
    n_mark = 0
    for k in keys:
        va, fa, sa = a["cells"].get(k, ("", "", None))
        vb, fb, sb = b["cells"].get(k, ("", "", None))
        if va != vb:
            diffs.append("值变了 %s: %r -> %r" % (k, va, vb))
        want = exp.get(k, fa)
        if fb != want:
            diffs.append("底色 %s: 期望 %s 实得 %s" % (k, want or "无", fb or "无"))
        if k in exp:
            n_mark += 1
        if sa is not None and sb is not None and sa != sb:
            diffs.append("样式被改 %s: %s -> %s" % (k, sa, sb))
        if len(diffs) > 6:
            break

    if diffs:
        bad += 1
        print("FAIL  %-46s %s" % (tag, diffs[0]))
        for d in diffs[1:6]:
            print("      %s" % d)
    else:
        extra = " / %d 条批注" % len(b["notes"]) if exp_notes else ""
        print("PASS  %-46s 涂 %d 格 / %d 格样式保持%s"
              % (tag, n_mark, len(keys), extra))
    return bad


def one_case(op, oi, np_, ni, scope, tmp):
    bad = 0
    before = (sha(op), sha(np_))
    tag = "%s#%d vs %s#%d%s" % (os.path.basename(op), oi,
                                os.path.basename(np_), ni,
                                "" if scope == "all" else " [key]")

    c_old, c_new, c_notes = run_c_mark(op, oi, np_, ni, scope, tmp)
    py, plan, notes = run_py_mark(op, oi, np_, ni, scope, tmp)

    # C 自报的批注条数（它写完又用 bs_zip 读回自己数的）与 openpyxl 读回的要对上
    for side, path in (("old", c_old), ("new", c_new)):
        real_n = len(read_sheet(path)["notes"])
        if c_notes.get(side, -1) != real_n:
            bad += 1
            print("FAIL  %-46s 自报 %s 条批注，实际读回 %d 条"
                  % (tag + " " + side, c_notes.get(side), real_n))

    # Python 自己写出来的底图，用来交叉验证 plan
    for side, src in (("old", op), ("new", np_)):
        got = read_sheet(py[side])
        src_fill = {k: v[1] for k, v in read_sheet(src)["cells"].items() if v[1]}
        py_fill = dict(src_fill)
        py_fill.update({k: v for k, v in got and {kk: vv[1] for kk, vv in got["cells"].items()}.items()})
        py_fill = {k: v for k, v in py_fill.items() if v}
        want = dict(src_fill)
        want.update({k: v for k, v in plan[side].items() if v})
        if py_fill != want:
            only_py = set(py_fill) - set(want)
            only_plan = set(want) - set(py_fill)
            diffc = [k for k in set(py_fill) & set(want) if py_fill[k] != want[k]]
            bad += 1
            print("FAIL  %-46s Python 与 plan 不一致：多 %s 少 %s 色差 %s"
                  % (tag + " " + side, sorted(only_py)[:3], sorted(only_plan)[:3], diffc[:3]))
        if got["notes"] != notes[side]:
            only_py = sorted(set(got["notes"]) - set(notes[side]))[:3]
            only_plan = sorted(set(notes[side]) - set(got["notes"]))[:3]
            bad += 1
            print("FAIL  %-46s Python 批注与 plan 不一致：多 %s 少 %s"
                  % (tag + " " + side, only_py, only_plan))

    bad = check_side(tag + " 旧", op, c_old, plan["old"], tmp, bad, notes["old"])
    bad = check_side(tag + " 新", np_, c_new, plan["new"], tmp, bad, notes["new"])

    after = (sha(op), sha(np_))
    if before != after:
        bad += 1
        print("FAIL  %-46s 源文件被改了！" % tag)
    return bad


def edge_cases(tmp):
    """目标被同名目录占住时，应自动落到 _2 而不是失败（与 Python _save_any 一致）。"""
    op = samples.p("cases", "pair_old.xlsx")
    np_ = samples.p("cases", "pair_new.xlsx")
    sub = os.path.join(tmp, "edge")
    os.makedirs(os.path.join(sub, "mark_旧.xlsx"), exist_ok=True)
    r = subprocess.run([BSTEST, "--mark", op, "0", np_, "0", sub, "all"],
                       cwd=ROOT, capture_output=True)
    out = r.stdout.decode("utf-8", "replace").splitlines()
    old = [ln.split(" ", 2)[2] for ln in out if ln.startswith("MARK old ")]
    if old and os.path.basename(old[0]).startswith("mark_旧_"):
        print("PASS  %-46s 落到 %s" % ("目标被目录占住", os.path.basename(old[0])))
        return 0
    print("FAIL  %-46s %s" % ("目标被目录占住", out[:2]))
    return 1


def dup_comment_case(tmp):
    """查重模式（--markdups）：批注要与 bomcore.dup_marks 逐条一致。"""
    src = samples.p("dup_sample.xlsx")
    sub = os.path.join(tmp, "dups")
    os.makedirs(sub, exist_ok=True)
    out = os.path.join(sub, "c_dups.xlsx")
    r = subprocess.run([BSTEST, "--markdups", src, "0", out], cwd=ROOT, capture_output=True)
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    got_path = [ln.split(" ", 2)[2] for ln in lines if ln.startswith("MARK dup ")]
    if not got_path:
        print("FAIL  %-46s %s" % ("查重批注", lines[:2]))
        return 1
    self_n = [int(ln.split(" ", 2)[2]) for ln in lines if ln.startswith("NOTES dup ")]

    sd = B.load_sheet(src, B.list_sheets(src)[0][0], base_cfg("all"))
    exp = {}
    for rr, cc, _key, note in B.dup_marks(sd):
        if note:
            exp[(rr, cc)] = (note, "BOMCheck")
    got = read_sheet(got_path[0])["notes"]
    if got != exp:
        only = sorted(set(got) - set(exp))[:3]
        miss = sorted(set(exp) - set(got))[:3]
        diff = [(k, exp[k], got[k]) for k in set(exp) & set(got) if exp[k] != got[k]][:2]
        print("FAIL  %-46s 多 %s 少 %s 文本差 %s" % ("查重批注", only, miss, diff))
        return 1
    if self_n and self_n[0] != len(got):
        print("FAIL  %-46s 自报 %d 条，实际 %d 条" % ("查重批注", self_n[0], len(got)))
        return 1
    print("PASS  %-46s %d 条批注" % ("查重批注（dup_marks）", len(exp)))
    return 0


def legacy_case(tmp):
    """源表自带批注：<legacyDrawing> 只能一条，C 应保留原批注、不再追加。"""
    try:
        from openpyxl import load_workbook
        from openpyxl.comments import Comment
    except ImportError:
        return 0

    sub = os.path.join(tmp, "legacy")
    os.makedirs(sub, exist_ok=True)
    src = os.path.join(sub, "src.xlsx")
    wb = load_workbook(samples.p("dup_sample.xlsx"))
    ws = wb.active
    ws["A1"].comment = Comment("原有批注", "原作者")
    wb.save(src)
    wb.close()

    out = os.path.join(sub, "c_legacy.xlsx")
    r = subprocess.run([BSTEST, "--markdups", src, "0", out], cwd=ROOT, capture_output=True)
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    got_path = [ln.split(" ", 2)[2] for ln in lines if ln.startswith("MARK dup ")]
    if not got_path:
        print("FAIL  %-46s %s" % ("源表自带批注", lines[:2]))
        return 1

    got = read_sheet(got_path[0])["notes"]
    want = {(1, 1): ("原有批注", "原作者")}
    if got != want:
        print("FAIL  %-46s 批注变成 %s" % ("源表自带批注", got))
        return 1
    import zipfile
    names = zipfile.ZipFile(got_path[0]).namelist()
    extra = [n for n in names if "comments2" in n or "vmlDrawing2" in n]
    if extra:
        print("FAIL  %-46s 多写了部件 %s" % ("源表自带批注", extra))
        return 1
    print("PASS  %-46s 原批注保留，未追加" % "源表自带批注")
    return 0


def make_rels_sample(sub):
    """造一份「已有 sheet rels（打印机设置）+ 有重复料号」的样本。
    真实 BOM 常带这类关系，追加批注关系时不能覆盖、rId 不能撞号。"""
    import zipfile

    os.makedirs(sub, exist_ok=True)
    src = os.path.join(sub, "src.xlsx")
    zin = zipfile.ZipFile(samples.p("dup_sample.xlsx"))
    with zipfile.ZipFile(src, "w", zipfile.ZIP_DEFLATED) as zout:
        for it in zin.infolist():
            data = zin.read(it.filename)
            if it.filename == "[Content_Types].xml":
                data = data.replace(
                    b"</Types>",
                    b'<Default Extension="bin" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.printerSettings"/></Types>')
            zout.writestr(it.filename, data)
        zout.writestr("xl/printerSettings/printerSettings1.bin", b"\x00")
        zout.writestr(
            "xl/worksheets/_rels/sheet1.xml.rels",
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/printerSettings"'
            ' Target="../printerSettings/printerSettings1.bin"/></Relationships>')
    zin.close()
    return src


def rels_case(tmp):
    """源文件已有 sheet rels：追加关系时不能覆盖，rId 不能撞号。"""
    import zipfile

    sub = os.path.join(tmp, "rels")
    src = make_rels_sample(sub)

    out = os.path.join(sub, "c_rels.xlsx")
    r = subprocess.run([BSTEST, "--markdups", src, "0", out], cwd=ROOT, capture_output=True)
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    got_path = [ln.split(" ", 2)[2] for ln in lines if ln.startswith("MARK dup ")]
    if not got_path:
        print("FAIL  %-46s %s" % ("已有 rels 时追加批注", lines[:2]))
        return 1

    z = zipfile.ZipFile(got_path[0])
    rels = z.read("xl/worksheets/_rels/sheet1.xml.rels").decode("utf-8")
    problems = []
    if "printerSettings" not in rels:
        problems.append("printerSettings 关系被覆盖")
    if rels.count("relationships/vmlDrawing") != 1 or rels.count('relationships/comments"') != 1:
        problems.append("vml/comments 关系不全")
    ids = re.findall(r'Id="(rId\d+)"', rels)
    if len(ids) != len(set(ids)):
        problems.append("rId 撞号 %s" % ids)
    sheet = z.read("xl/worksheets/sheet1.xml").decode("utf-8")
    m = re.search(r'<legacyDrawing r:id="(rId\d+)"', sheet)
    if not m or m.group(1) not in ids:
        problems.append("<legacyDrawing> 指不到关系 %s" % (m.group(1) if m else "无"))
    n_notes = len(read_sheet(got_path[0])["notes"])
    if n_notes != 9:
        problems.append("批注只有 %d 条" % n_notes)

    if problems:
        print("FAIL  %-46s %s" % ("已有 rels 时追加批注", "；".join(problems)))
        return 1
    print("PASS  %-46s %d 条关系、%d 条批注" % ("已有 rels 时追加批注", len(ids), n_notes))
    return 0


def cases():
    out = []
    for op, np_ in (("demo_old.xlsx", "demo_new.xlsx"),
                    ("demo_new.xlsx", "demo_old.xlsx"),
                    ("demo_old.xlsx", "demo_old.xlsx"),
                    ("dup_sample.xlsx", "dup_sample.xlsx")):
        p = samples.p(op)
        q = samples.p(np_)
        if os.path.exists(p) and os.path.exists(q):
            out.append((p, 0, q, 0, "all"))
    for name in ("pair_old.xlsx", "case_dup_strike.xlsx", "case_wide_cols.xlsx"):
        p = samples.p("cases", name)
        if os.path.exists(p):
            out.append((p, 0, samples.p("cases", "pair_new.xlsx"), 0, "all"))
    out.append((samples.p("cases", "pair_old.xlsx"), 0,
                samples.p("cases", "pair_new.xlsx"), 0, "key"))
    return out


def run_cov(files, tmp):
    global BSTEST
    outdir = os.path.join(ROOT, "build", "cov_mark")
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, "bstest_cov.exe")
    cmd = [GCC, "-std=c99", "-O0", "--coverage", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "src", "core"), '-DBS_VERSION="cov"',
           os.path.join(ROOT, "tests", "test_core.c")]
    cmd += [os.path.join(ROOT, "src", "core", s + ".c") for s in CORE_SRC]
    cmd += ["-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("覆盖率编译失败：\n" + (r.stderr or r.stdout)[-2000:])
        return 2

    subprocess.run([exe], cwd=outdir, capture_output=True)
    for k, (op, oi, np_, ni, sc) in enumerate(files):
        sub = os.path.join(tmp, "cov%02d" % k)
        os.makedirs(sub, exist_ok=True)
        subprocess.run([exe, "--mark", op, str(oi), np_, str(ni), sub, sc],
                       cwd=outdir, capture_output=True)
    # 查重入口也要跑，否则 bs_mark_dups 是死代码；再跑一份带 rels 的，
    # 让 build_rels 的追加分支也进覆盖
    sub = os.path.join(tmp, "covdups")
    os.makedirs(sub, exist_ok=True)
    subprocess.run([exe, "--markdups", files[0][0], "0",
                    os.path.join(sub, "dups.xlsx")], cwd=outdir, capture_output=True)
    try:
        rels_src = make_rels_sample(os.path.join(tmp, "covrels"))
        subprocess.run([exe, "--markdups", rels_src, "0",
                        os.path.join(tmp, "covrels", "out.xlsx")],
                       cwd=outdir, capture_output=True)
    except Exception:                                  # noqa: BLE001
        pass

    for src in ("bs_mark.c", "bs_zipw.c", "bs_zip.c"):
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


def main():
    global BSTEST
    argv = sys.argv[1:]
    if "--exe" in argv:
        BSTEST = argv[argv.index("--exe") + 1]
    want_cov = "--cov" in argv

    # 单独的临时目录：check_write.py 结束时会清 build/tmp，两边的 rmtree 撞上
    # 会把它正在写的文件删掉（沙箱的 safe-delete 是异步的）。用 pid 隔离。
    tmp = os.path.join(ROOT, "build", "tmp_mark")
    os.makedirs(tmp, exist_ok=True)

    files = cases()
    bad = 0
    for k, (op, oi, np_, ni, sc) in enumerate(files):
        # 每组一个子目录：不删旧文件（沙箱对 os.remove 有拦截），也就不会撞名
        sub = os.path.join(tmp, "case%02d" % k)
        os.makedirs(sub, exist_ok=True)
        try:
            bad += one_case(op, oi, np_, ni, sc, sub)
        except Exception as e:                      # noqa: BLE001
            bad += 1
            print("FAIL  %-46s %s: %s" % (os.path.basename(op), type(e).__name__, e))

    bad += edge_cases(tmp)
    bad += dup_comment_case(tmp)
    bad += rels_case(tmp)
    bad += legacy_case(tmp)

    if want_cov:
        rc = run_cov(files, tmp)
        print("\n%d 组标注 + 4 条专项，%d 个失败" % (len(files), bad))
        return rc or (1 if bad else 0)

    print("\n%d 组标注 + 4 条专项，%d 个失败" % (len(files), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())