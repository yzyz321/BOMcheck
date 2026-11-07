# -*- coding: utf-8 -*-
"""C1 对账：C 侧 norm / pn_key / ref_tokens / ref_sig / make_key 与 pyref/bomcore.py 逐值比对。

做法：把样本里出现过的每个不同单元格值都当作料号喂一遍、再当作位号喂一遍，
加上一批手写的边角用例，一次性交给 `bstest --dump`，和 Python 的结果逐字段比。
任何一处不同都算失败并打印首个差异。

用法：
    python tools/check_pn.py                 # 跑样本 + 边角用例
    python tools/check_pn.py --cov           # 顺带用 gcov 看 bs_pn.c 的分支覆盖
    python tools/check_pn.py file1.xlsx ...  # 指定样本
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

# 与 oracle（pyref/bomcore.py）默认 cfg 同值（load_sheet 只用到前四项）
CFG = {"min_hit_rate": 0.60, "sample_rows": 40,
       "header_min_alias": 1, "header_threshold": 5, "dual_key": 1}

# 手写边角用例：全是两边的口径容易分岔的地方
EXTRA = [
    "", " ", "\t\n", "  \u3000 ",
    "111-000005-C50", "111-000005-c50", "  111-000005-C50  ",
    "111 - 000005 - C50", "111–000005–C50", "111—000005—C50",
    "111－000005－C50", "111−000005−C50", "111‑000005‑C50",
    "111-000005-C5", "111-00005-C50", "1111-000005-C50",
    "ABC-123456-DEF", "abc-123456-def", "A1B-2C3D4E-5F6",
    "111-000005-C50.0", "123456.0", "0.0", ".0", "０.０",
    "１２３-000005-C50", "１１１-000005-C50", "ａｂｃ-123456-def",
    "Part Number", "Ｐａｒｔ　Ｎｕｍｂｅｒ", "料号", "物料编码 ",
    "位号", "Reference", "Qty", "数量",
    "R1", "R1,R2", "R1,R2,R10", "r1 r2", "R1、R2；R3", "R1/R2\\R3|R4;R5",
    "R1,R1,R2", "R1 X R2", "R123456", "R1234567", "R12345678", "ABCD1",
    "C1,C2\nC3", "U1,U2,U10,U3", "L1, L2 ,, L3", " R1 , R2 ",
    "R1-R2", "R1~R2", "TP1,TP2", "J1,J2,J3,J10,J11",
    "一颗料", "带\n换行", "带\t制表", "带\\反斜杠", "带|竖线",
    "1", "0", "3.14", "-", "--", "N/A", "TBD", "无",
    "\u00a0NBSP\u00a0", "\u200b零宽",
]


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


def sample_values(paths, cap_per_file=4000):
    """样本里出现过的不同值（按出现顺序去重，去掉空串）。

    取的是已识别出料号/位号/数量列的那些格子 —— 这才真是以后要喂给 pn_key /
    ref_sig 的输入。读不出来的样本打印出来，不静默跳过（踩过一次：cfg 传 None
    导致所有样本都异常，脚本却"PASS 70 个用例"，其实只跑了手写用例）。"""
    seen, bad = {}, []
    for p in paths:
        n = 0
        try:
            sheets = B.list_sheets(p)
            for name, _part in sheets:
                try:
                    sd = B.load_sheet(p, name, CFG)
                except Exception as e:      # noqa: BLE001
                    bad.append((p, e))
                    continue
                for r in sd.rows:
                    for v in (r.pn, r.ref, r.qty):
                        if isinstance(v, str) and v and v not in seen:
                            seen[v] = 1
                            n += 1
                if n >= cap_per_file:
                    break
        except Exception as e:              # noqa: BLE001
            bad.append((p, e))
    for p, e in bad:
        print("跳过 %s：%s" % (os.path.basename(p), e))
    return list(seen)


def py_expect(pn_raw, ref_raw):
    nm = B.norm(pn_raw)
    k, w, s = B.pn_key(pn_raw)
    ok, bad = B.ref_tokens(ref_raw)
    sig = B.ref_sig(ref_raw)
    _kind, key = B.make_key(pn_raw, ref_raw)
    return [esc(nm), esc(k), "1" if w else "0", "1" if s else "0",
            esc(sig), str(len(ok)), str(bad), esc(key)]


def build_corpus(values):
    lines, cases = [], []
    for v in values:
        for pn_raw, ref_raw in ((v, ""), ("", v)):
            lines.append("%s\t%s" % (esc(pn_raw), esc(ref_raw)))
            cases.append((pn_raw, ref_raw))
    return lines, cases


def run_dump(lines, tag="pn"):
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    path = os.path.join(ROOT, "build", "%s_corpus.txt" % tag)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    out = subprocess.run([BSTEST, "--dump", path], capture_output=True)
    text = out.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    return path, [ln for ln in text.split("\n") if ln != ""]


def compare(cases, got):
    bad = []
    for i, (pn_raw, ref_raw) in enumerate(cases):
        want = py_expect(pn_raw, ref_raw)
        row = got[i].split("\t") if i < len(got) else []
        if row != want:
            bad.append((i, pn_raw, ref_raw, row, want))
    return bad


def show(bad, limit=8):
    names = ["norm", "料号", "wide", "strict", "位号签名", "ok", "bad", "key"]
    for i, pn_raw, ref_raw, row, want in bad[:limit]:
        print("FAIL  #%d  pn=[%s] ref=[%s]" % (i, pn_raw, ref_raw))
        for k, nm in enumerate(names):
            a = row[k] if k < len(row) else "<缺>"
            b = want[k]
            if a != b:
                print("        %-8s C=[%s]  Py=[%s]" % (nm, a, b))


def run_cov(corpus_path):
    """gcov 看分支覆盖。只编 bs_pn.c + 单测，不碰 Win32。"""
    outdir = os.path.join(ROOT, "build", "cov")
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, "bstest_cov.exe")
    cmd = [GCC, "-std=c99", "-O0", "--coverage", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "src", "core"),
           os.path.join(ROOT, "tests", "test_core.c"),
           os.path.join(ROOT, "src", "core", "bs_pn.c"),
           "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("gcov 编译失败：\n" + (r.stderr or r.stdout))
        return 2
    subprocess.run([exe], cwd=outdir, capture_output=True)          # 单测
    subprocess.run([exe, "--dump", corpus_path], cwd=outdir, capture_output=True)  # 全量
    g = subprocess.run(["gcov", "-b", "-c",
                        "-o", "bstest_cov-bs_pn.gcno",
                        os.path.join(ROOT, "src", "core", "bs_pn.c")],
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

    # 没走到的分支列出来，方便补用例
    gcov_file = os.path.join(outdir, "bs_pn.c.gcov")
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
    ok = take_pct and take_pct.startswith("100")
    return 0 if ok else 1


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_cov = "--cov" in sys.argv[1:]

    if not os.path.exists(BSTEST):
        print("找不到 %s，先跑 build_c.bat" % BSTEST)
        return 2

    files = argv or [p for p in samples.xlsx()
                     if not os.path.basename(p).startswith("big_")]
    values = sample_values(files) if files else []
    # 边角用例放前面，出差异时先看到最可能是口径问题的那些
    seen = set(EXTRA)
    values = [v for v in values if v not in seen]
    values = EXTRA + values

    lines, cases = build_corpus(values)
    corpus, got = run_dump(lines)
    bad = compare(cases, got)

    # 顺带给 C3 一点信息：样本里有多少值真的长得像料号 / 位号
    n_pn_w = sum(1 for v in values if B.pn_key(v)[1])
    n_pn_s = sum(1 for v in values if B.pn_key(v)[2])
    print("%d 个不同值 / %d 个用例（每个值当料号、当位号各一次）"
          % (len(values), len(cases)))
    print("其中像料号：宽松 %d，严格 %d；位号可解析 %d"
          % (n_pn_w, n_pn_s, sum(1 for v in values if B.ref_sig(v))))

    if bad:
        show(bad)
        print("\n%d 个用例不同（共 %d）" % (len(bad), len(cases)))
    else:
        print("PASS  全部 %d 个用例与 Python 一致" % len(cases))

    rc = 1 if bad else 0
    if want_cov:
        rc |= run_cov(corpus)
    return rc


if __name__ == "__main__":
    sys.exit(main())
