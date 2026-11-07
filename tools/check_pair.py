# -*- coding: utf-8 -*-
"""配对与差异对账（C4）：C 的 `bstest --pair` 与 Python `pair()` / `build_result()` 比。

比对内容：
  - 四阶段统计（配对总数、按料号/位号/近似/料号变更、新增、删除、变更行、cartesian 标志）
  - 12 列结果逐行逐格（含空串位置、位号差集顺序、数量文本、重复序号、配对来源）

用法：python tools/check_pair.py [--exe 路径] [--cov]
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

CORE_SRC = ["bs_file", "bs_inflate", "bs_zip", "bs_xml", "bs_xlsx",
            "bs_pn", "bs_arena", "bs_sheet", "bs_pair"]

DUP_TEXT = {0: "SEQ", 1: "STRICT", 2: "CARTESIAN"}

# 「两边都拒」只允许出现在负样本上；真实样本被两边同时拒绝 = 识别/配对回归
NEGATIVE = {
    "case_no_header.xlsx",   # 负样本：整张表没有表头
    "cover_first.xlsx",      # 第 0 页是封面，本来就没有 BOM 表头
    "dup_sample.xlsx",       # STRICT 模式：重复键必须中止
    "case_dup_many.xlsx",    # 同上
}


def base_cfg(scope="all", dup=0):
    cfg = {"min_hit_rate": 0.60, "sample_rows": 40,
           "header_min_alias": 1, "header_threshold": 5, "dual_key": 1}
    if scope == "key":
        cfg["strike_scope"] = "KEY"
    if dup:
        cfg["dup_policy"] = DUP_TEXT[dup]
    return cfg


def esc(v):
    if v is None:
        return ""
    out = []
    for ch in str(v):
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


def c_dump(op, oi, np_, ni, scope="all", dup=0):
    cmd = [BSTEST, "--pair", op, str(oi), np_, str(ni), scope, str(dup)]
    r = subprocess.run(cmd, capture_output=True)
    text = r.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    return [ln for ln in text.split("\n") if ln != ""]


def py_dump(op, oi, np_, ni, scope="all", dup=0):
    cfg = base_cfg(scope, dup)
    so = B.load_sheet(op, B.list_sheets(op)[oi][0], cfg)
    sn = B.load_sheet(np_, B.list_sheets(np_)[ni][0], cfg)
    pairs, add, dele, st = B.pair(so.rows, sn.rows, cfg)
    rows = B.build_result(pairs, add, dele)
    out = ["stat pairs=%d pn=%d ref=%d fuzzy=%d pnchg=%d add=%d del=%d chg=%d cart=%d"
           % (len(pairs), st["pn"], st["ref"], st["fuzzy"], st["pnchg"],
              len(add), len(dele),
              sum(1 for r in rows if r[2] == "变更"),
              st.get("cartesian_fallback", 0))]
    out.append("cols %d" % len(rows))
    for r in rows:
        out.append("row\t" + "\t".join(esc(v) for v in r))
    return out


def compare(op, oi, np_, ni, scope="all", dup=0):
    got = c_dump(op, oi, np_, ni, scope, dup)
    try:
        want = py_dump(op, oi, np_, ni, scope, dup)
    except B.BomError as e:
        # 只要求「两边都拒」，错误文案不逐字对齐（它面向用户，措辞会改）；
        # 但负样本白名单之外一律算 FAIL —— 否则两侧同步的回归会静默变绿。
        if got and got[0].startswith("ERR "):
            if os.path.basename(op) in NEGATIVE or os.path.basename(np_) in NEGATIVE:
                note = "" if got[0] == "ERR " + esc(str(e)) else "（文案不同）"
                return True, "负样本两边都拒（%s）%s" % (str(e)[:30], note), got
            return False, "真实样本被两边同时拒绝（%s）" % str(e)[:50], got
        return False, "Python 拒了，C 却是：%s" % (got[0][:80] if got else "空"), got

    if got == want:
        return True, "%s" % (got[0].split(" ", 1)[1] if got else ""), got
    if not got:
        return False, "C 无输出", got
    if got[0].startswith("ERR "):
        return False, "C 报错：" + got[0][:100], got
    for i in range(max(len(got), len(want))):
        a = got[i] if i < len(got) else "<缺行>"
        b = want[i] if i < len(want) else "<缺行>"
        if a != b:
            return False, "第 %d 行不同\n    C : %s\n    Py: %s" % (
                i + 1, a[:400], b[:400]), got
    return False, "行数不同 C=%d Py=%d" % (len(got), len(want)), got


def cases():
    """(旧文件, 旧页, 新文件, 新页, 删除线口径, 重复策略)"""
    t = samples.base()
    c = os.path.join(t, "cases")
    out = [
        ("demo_old.xlsx", 0, "demo_new.xlsx", 0, "all", 0),
        ("demo_old.xlsx", 0, "demo_new.xlsx", 0, "key", 0),
        # 同一文件：全等配对，0 条结果（阶段 A 吃满，且没有任何差异）
        ("demo_old.xlsx", 0, "demo_old.xlsx", 0, "all", 0),
        # 跨表：主板 vs 电源板，大量新增/删除，走阶段 D
        ("demo_old.xlsx", 0, "demo_new.xlsx", 1, "all", 0),
        # 反方向：新增/删除互换，仍要逐格对上
        ("demo_new.xlsx", 0, "demo_old.xlsx", 0, "all", 0),
        # 重复键：2/3/4 处，验 dup 映射与 2×3 不炸成 6 行
        ("dup_sample.xlsx", 0, "dup_sample.xlsx", 0, "all", 0),
        ("dup_sample.xlsx", 0, "dup_sample.xlsx", 0, "all", 1),   # STRICT 应两边都拒
        ("dup_sample.xlsx", 0, "dup_sample.xlsx", 0, "all", 2),   # CARTESIAN 打标但不换行为
        # 位号近似：一侧有料号、一侧没有
        ("cases/case_dup_strike.xlsx", 0, "cases/case_strike_desc.xlsx", 0, "all", 0),
        ("cases/case_ref_guess.xlsx", 0, "cases/case_dup_strike.xlsx", 0, "all", 0),
        ("cases/case_qty_junk.xlsx", 0, "cases/case_multi_block.xlsx", 0, "all", 0),
        # 四阶段 + 2×3 重复都压在这一对上（gen_sheet_cases.pair_files）
        ("cases/pair_old.xlsx", 0, "cases/pair_new.xlsx", 0, "all", 0),
        ("cases/pair_old.xlsx", 0, "cases/pair_new.xlsx", 0, "key", 0),
        # 传 NULL 配置（内部取默认值）这条路
        ("cases/pair_old.xlsx", 0, "cases/pair_new.xlsx", 0, "def", 0),
        # 10 个重复料号：STRICT 的取样表要扩容，文案取排序后前 3 个
        ("cases/case_dup_many.xlsx", 0, "cases/case_dup_many.xlsx", 0, "all", 1),
        ("cases/case_dup_many.xlsx", 0, "cases/case_dup_many.xlsx", 0, "all", 0),
    ]
    # 每个边界样本再自比一次（0 差异），覆盖不同列定位下的空结果
    for p in sorted(glob.glob(os.path.join(c, "*.xlsx"))):
        out.append((os.path.relpath(p, t), 0, os.path.relpath(p, t), 0, "all", 0))
    return [(os.path.join(t, a), oi, os.path.join(t, b), ni, sc, dp)
            for a, oi, b, ni, sc, dp in out]


def run_cov(files):
    outdir = os.path.join(ROOT, "build", "cov_pair")
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
    ran = 0
    for op, oi, np_, ni, sc, dp in files:
        subprocess.run([exe, "--pair", op, str(oi), np_, str(ni), sc, str(dp)],
                       cwd=outdir, capture_output=True)
        ran += 1
    print("覆盖  跑了 %d 组配对" % ran)
    for src in ("bs_pair.c", "bs_arena.c"):
        g = subprocess.run(["gcov", "-b", "-c",
                            "-o", "bstest_cov-" + src.replace(".c", ".gcno"),
                            os.path.join(ROOT, "src", "core", src)],
                           cwd=outdir, capture_output=True, text=True)
        text = (g.stdout or "") + (g.stderr or "")
        vals = {}
        for ln in text.splitlines():
            for k, tag in (("line", "Lines executed:"),
                           ("branch", "Branches executed:"),
                           ("take", "Taken at least once:")):
                if ln.startswith(tag):
                    vals[k] = ln.split(":", 1)[1].strip()
        print("覆盖  %-12s 行 %s / 分支执行 %s / 至少一次 %s"
              % (src, vals.get("line", "?"), vals.get("branch", "?"), vals.get("take", "?")))
        gc = os.path.join(outdir, src + ".gcov")
        missed = []
        if os.path.exists(gc):
            cur = 0
            with open(gc, encoding="utf-8", errors="replace") as f:
                for ln in f:
                    m = re.match(r"\s*(\d+):\s*(\d+):", ln)
                    if m:
                        cur = int(m.group(2))
                        continue
                    if "branch" in ln and ("never executed" in ln or "taken 0%" in ln):
                        missed.append(cur)
        if missed:
            print("        未走到分支的行：%s" % ", ".join(str(n) for n in sorted(set(missed))))
    return 0


def main():
    global BSTEST
    argv = sys.argv[1:]
    want_cov = "--cov" in argv
    if "--exe" in argv:
        BSTEST = argv[argv.index("--exe") + 1]
    if not os.path.exists(BSTEST):
        print("找不到 %s，先跑 build_c.bat" % BSTEST)
        return 2

    files = cases()
    bad = 0
    for op, oi, np_, ni, sc, dp in files:
        wo = os.path.basename(os.path.dirname(op)) == "cases"
        wn = os.path.basename(os.path.dirname(np_)) == "cases"
        tag = "%s#%d vs %s#%d" % (os.path.basename(op) if not wo else "cases/" + os.path.basename(op),
                                  oi,
                                  os.path.basename(np_) if not wn else "cases/" + os.path.basename(np_),
                                  ni)
        if sc != "all":
            tag += " [%s]" % sc
        if dp:
            tag += " [%s]" % DUP_TEXT[dp]
        try:
            ok, msg, _got = compare(op, oi, np_, ni, sc, dp)
        except Exception as e:                       # noqa: BLE001
            ok, msg = False, "%s: %s" % (type(e).__name__, e)
        if ok:
            print("PASS  %-58s %s" % (tag, msg))
        else:
            bad += 1
            print("FAIL  %-58s %s" % (tag, msg))

    print("\n%d 组配对，%d 个失败" % (len(files), bad))
    rc = 1 if bad else 0
    if want_cov:
        rc |= run_cov(files)
    return rc


if __name__ == "__main__":
    sys.exit(main())
