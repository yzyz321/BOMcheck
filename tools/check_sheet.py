# -*- coding: utf-8 -*-
"""识别层对账（C3）：C 的 `bstest --sheet` 与 Python `bomcore.load_sheet` 逐行比对。

比对内容：
  - 表头块（行号、数据区间、料号/位号/数量列）
  - 汇总列名、料号列命中率、标题行数、无法建档行数
  - 每一行的 (块, 列范围, 键类型, 料号, 原位号, 位号签名, 键, 数量, 删除线, 重复序号)

用法：python tools/check_sheet.py [--exe 路径] [--cov] [文件...]
不给文件就扫 temp/*.xlsx（跳过 big_* 压测样本）+ temp/cases/*.xlsx。
--cov 另外编一份带 gcov 的副本，跑完全部样本后报 bs_sheet.c 的行/分支覆盖。
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "pyref"))

import bomcore as B  # noqa: E402
import samples  # noqa: E402  样本位置（tests/data，缺了退回 temp）

BSTEST = os.path.join(ROOT, "build", "c", "bin", "bstest.exe")
# 换机器时用环境变量覆盖，不必改源码：set BOMCHECK_GCC=<mingw>\bin\gcc.exe
GCC = os.environ.get("BOMCHECK_GCC", r"D:\AICS\mingw64\bin\gcc.exe")

# gcov 副本要链上 bstest 用到的 core 源（bs_sheet 依赖 pn/xlsx/zip/xml/file/inflate；
# bs_version.c 要生成的 bs_version_gen.h，单测用不到，不编）
CORE_SRC = ["bs_file", "bs_inflate", "bs_zip", "bs_xml", "bs_xlsx", "bs_pn", "bs_sheet"]

# 与 oracle（pyref/bomcore.py）默认 cfg 同值
CFG = {"min_hit_rate": 0.60, "sample_rows": 40,
       "header_min_alias": 1, "header_threshold": 5, "dual_key": 1}

# 允许「C 与 Python 同时拒绝」的负样本。只有它们能被两边一起判成无表头；
# 其余样本如果两边同时拒，说明识别层回归了（两侧同改也能骗过逐格对账）。
NEGATIVE = {
    "case_no_header.xlsx",   # 负样本：整张表没有表头
    "cover_first.xlsx",      # 第 0 页是封面，本来就没有 BOM 表头
    "dup_sample.xlsx",       # STRICT 模式：重复键必须中止
    "case_dup_many.xlsx",    # 同上
}


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


def c_dump(path, idx, scope="all"):
    cmd = [BSTEST, "--sheet", path, str(idx)]
    if scope == "key":
        cmd.append("key")
    r = subprocess.run(cmd, capture_output=True)
    text = r.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    return [ln for ln in text.split("\n") if ln != ""]


def cn(c):
    return B._col_name(c) if c else "-"


def py_dump(path, idx, scope="all"):
    sheets = B.list_sheets(path)
    name = sheets[idx][0]
    cfg = dict(CFG)
    if scope == "key":
        cfg["strike_scope"] = "KEY"
    sd = B.load_sheet(path, name, cfg)

    out = ["blocks %d" % len(sd.blocks)]
    for i, (hr, start, end, cp, cr, cq) in enumerate(sd.blocks):
        out.append("block %d hr=%d start=%d end=%d pn=%s ref=%s qty=%s"
                   % (i, hr, start, end, cn(cp), cn(cr), cn(cq)))
    # 这里的空串保持空（列名没定位到时 C 侧也是空串）；只有 block 行用 "-"
    out.append("cols pn=%s ref=%s qty=%s"
               % (sd.cols["pn"], sd.cols["ref"], sd.cols["qty"]))
    out.append("hit %.6f" % sd.hit_rate)
    out.append("labels %d" % sd.labels)
    out.append("unkeyable %d" % len(sd.unkeyable))
    out.append("rows %d" % len(sd.rows))
    for r in sd.rows:
        qty = "%.6f" % r.qty if r.qty is not None else "-"
        out.append("row %d %d %d %d %s %s %s %s %s %s %d %d %d"
                   % (r.r, r.block, r.c1, r.c2, r.kind or "0",
                      esc(r.pn), esc(r.ref), esc(r.refsig), esc(r.key), qty,
                      r.strike, r.dup_i, r.dup_n))
    return out


def compare(path, idx, scope="all"):
    got = c_dump(path, idx, scope)
    try:
        want = py_dump(path, idx, scope)
    except B.BomError as e:
        # 「两边都拒」只对负样本成立；真实 BOM 被两边同时拒 = 识别层回归，
        # 必须红（否则一次两侧同步的改坏可以让整链全绿）。
        if got and got[0].startswith("ERR "):
            if os.path.basename(path) in NEGATIVE:
                return True, "负样本：两边都判为无表头（%s）" % str(e)[:40], got
            return False, "真实样本被两边同时拒绝（%s）" % str(e)[:60], got
        return False, "Python 判无表头，C 却给出了结果：%s" % (got[0][:80] if got else "空"), got

    if got == want:
        return True, "%d 块 / %d 行" % (sum(1 for ln in got if ln.startswith("block ")),
                                       sum(1 for ln in got if ln.startswith("row "))), got

    if not got:
        return False, "C 无输出", got
    if got[0].startswith("ERR "):
        return False, "C 报错：" + got[0][:100], got

    for i in range(max(len(got), len(want))):
        a = got[i] if i < len(got) else "<缺行>"
        b = want[i] if i < len(want) else "<缺行>"
        if a != b:
            return False, "第 %d 行不同\n    C : %s\n    Py: %s" % (
                i + 1, a[:300], b[:300]), got
    return False, "行数不同 C=%d Py=%d" % (len(got), len(want)), got


def run_cov(files):
    """gcov 看 bs_sheet.c 的覆盖。只编 core + 单测，不碰 Win32。"""
    outdir = os.path.join(ROOT, "build", "cov_sheet")
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, "bstest_cov.exe")
    cmd = [GCC, "-std=c99", "-O0", "--coverage", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "src", "core"),
           '-DBS_VERSION="cov"',
           os.path.join(ROOT, "tests", "test_core.c")]
    cmd += [os.path.join(ROOT, "src", "core", s + ".c") for s in CORE_SRC]
    cmd += ["-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("gcov 编译失败：\n" + (r.stderr or r.stdout)[-2000:])
        return 2

    subprocess.run([exe], cwd=outdir, capture_output=True)          # 单测
    ran = 0
    for p in files:
        try:
            n = len(B.list_sheets(p))
        except Exception:                                          # noqa: BLE001
            continue
        for idx in range(n):
            subprocess.run([exe, "--sheet", p, str(idx)], cwd=outdir, capture_output=True)
            subprocess.run([exe, "--sheet", p, str(idx), "key"], cwd=outdir, capture_output=True)
            ran += 1
    print("覆盖  跑了 %d 张表（含两种删除线口径）" % ran)

    g = subprocess.run(["gcov", "-b", "-c",
                        "-o", "bstest_cov-bs_sheet.gcno",
                        os.path.join(ROOT, "src", "core", "bs_sheet.c")],
                       cwd=outdir, capture_output=True, text=True)
    text = (g.stdout or "") + (g.stderr or "")
    line_pct = branch_pct = take_pct = None
    for ln in text.splitlines():
        if ln.startswith("Lines executed:"):
            line_pct = ln.split(":", 1)[1].strip()
        elif ln.startswith("Branches executed:"):
            branch_pct = ln.split(":", 1)[1].strip()
        elif ln.startswith("Taken at least once:"):
            take_pct = ln.split(":", 1)[1].strip()
    print("覆盖  行 %s / 分支执行 %s / 分支至少走一次 %s"
          % (line_pct or "?", branch_pct or "?", take_pct or "?"))

    gcov_file = os.path.join(outdir, "bs_sheet.c.gcov")
    missed = []
    if os.path.exists(gcov_file):
        cur = 0
        with open(gcov_file, encoding="utf-8", errors="replace") as f:
            for ln in f:
                m = re.match(r"\s*(\d+):\s*(\d+):", ln)
                if m:
                    cur = int(m.group(2))
                    continue
                if "branch" in ln and ("never executed" in ln or "taken 0%" in ln):
                    missed.append(cur)
    if missed:
        print("未走到分支的行：%s" % ", ".join(str(n) for n in sorted(set(missed))))
    return 0


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_cov = "--cov" in sys.argv[1:]
    if "--exe" in sys.argv[1:]:
        k = sys.argv.index("--exe")
        global BSTEST
        BSTEST = sys.argv[k + 1]
        argv = [a for a in argv if a != BSTEST]

    if not os.path.exists(BSTEST):
        print("找不到 %s，先跑 build_c.bat" % BSTEST)
        return 2

    if argv:
        files = argv
    else:
        # temp 下是真实样本，temp/cases 下是 gen_sheet_cases.py 生成的边界样本
        files = [p for p in samples.xlsx()
                 if not os.path.basename(p).startswith("big_")]
        files += samples.xlsx("cases")
    if not files:
        print("没有样本文件")
        return 2

    bad = 0
    cases = 0
    for p in files:
        try:
            sheets = B.list_sheets(p)
        except Exception as e:                      # noqa: BLE001
            print("FAIL  %-40s 打不开：%s" % (os.path.basename(p), e))
            bad += 1
            continue
        for idx, (name, _part) in enumerate(sheets):
            cases += 1
            ok, msg, got = compare(p, idx)
            tag = "%s#%d" % (os.path.basename(p), idx)
            if ok:
                print("PASS  %-46s %-8s %s" % (tag, name[:8], msg))
            else:
                bad += 1
                print("FAIL  %-46s %-8s %s" % (tag, name[:8], msg))

            # 删除线口径：「仅三格」与「整行」在没有删除线的表上结果相同，
            # 只有真划了行才需要单独验一遍 —— 一样只在有差异时多打一行
            ok2, msg2, got2 = compare(p, idx, "key")
            if not ok2:
                bad += 1
                print("FAIL  %-46s %-8s [仅三格] %s" % (tag, name[:8], msg2))
            elif ok and got2 != got:
                print("PASS  %-46s %-8s [仅三格] %s（删除线口径生效）"
                      % (tag, name[:8], msg2))
    print("\n%d 张表，%d 个失败" % (cases, bad))
    rc = 1 if bad else 0
    if want_cov:
        rc |= run_cov(files)
    return rc


if __name__ == "__main__":
    sys.exit(main())
