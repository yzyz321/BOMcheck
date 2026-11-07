# -*- coding: utf-8 -*-
"""界面接线对账（C7）：结果表 12 列 / 查重 8 列 / 行底色 / 冻结列 / 三份产物。

做法与其它 check_*.py 一致：让 exe 用探针把自己「画在表里的东西」吐出来，
再与 Python 的 bomcore 逐格比。界面里真正会出错的地方是数据映射
（列序、空串位置、颜色判定），这些都能在文本层对齐；像素层交给截图。

探针：
  --drop-test A B --dump-table      表格内容 + 行/格底色 + 冻结列宽 + 横向偏移
  --drop-test A B --table-scroll    滚到最右再读回冻结列是否还在
  --ui-run A B <目录>               「开始比对 / 开始查重」按钮那条路（含日志）

用法：python tools/check_ui.py [--exe 路径] [--keep]
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "pyref"))

import bomcore as B  # noqa: E402

from findexe import find_exe  # noqa: E402
import samples  # noqa: E402  样本位置（tests/data，缺了退回 temp）

EXE = find_exe()
TMP = os.path.join(ROOT, "build", "tmp_ui")

ROW_HEX = {"新增": "C6EFCE", "删除": "FFC7CE", "变更": "FFEB9C"}
DUP_HEX = "D9D2E9"


def cfg():
    # 与界面启动默认一致：双键关（2026-11-06 起）、逐列比较关
    return {"min_hit_rate": 0.60, "sample_rows": 40,
            "header_min_alias": 1, "header_threshold": 5, "dual_key": 0}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def run(args, timeout=120):
    r = subprocess.run([EXE] + args, cwd=ROOT, capture_output=True, timeout=timeout)
    return r.stdout.decode("utf-8", "replace").replace("\r\n", "\n").split("\n")


def parse_table(lines):
    """返回 (head, rows)：head 是 table 行的键值，rows 是 [(bg, pnbg, [格…])]。"""
    head = None
    rows = []
    for ln in lines:
        if ln.startswith("table "):
            head = dict(kv.split("=", 1) for kv in ln.split()[1:] if "=" in kv)
        elif ln.startswith("row "):
            body = ln.split("\t")
            m = re.match(r"row (\d+) bg=([0-9A-F]{6}) pnbg=([0-9A-F]{6})", body[0])
            if not m:
                raise RuntimeError("表格行格式不认识：" + body[0])
            cells = body[1:]
            rows.append((int(m.group(1)), m.group(2), m.group(3), cells))
    return head, rows


# ── 期望值：全部来自 bomcore ────────────────────────────────────────


def want_diff(op, np_, oi=0, ni=0):
    so = B.load_sheet(op, B.list_sheets(op)[oi][0], cfg())
    sn = B.load_sheet(np_, B.list_sheets(np_)[ni][0], cfg())
    pairs, add, dele, _st = B.pair(so.rows, sn.rows, cfg())
    rows = B.build_result(pairs, add, dele)
    out = []
    for r in rows:
        cells = ["" if v is None else str(v) for v in r]
        bg = ROW_HEX.get(cells[2], "000000")
        # 重复料号只涂料号格：第 11 列（下标 10）非空即涂
        pn = DUP_HEX if cells[10] else "000000"
        out.append((bg, pn, cells))
    return out


def want_dups(path, idx=0):
    sd = B.load_sheet(path, B.list_sheets(path)[idx][0], cfg())
    rows = B.find_dups(sd)["rows"]
    out = []
    for r in rows:
        cells = [r[0], str(r[1]), r[2], r[3], r[4], r[5], r[6], r[7]]
        out.append((DUP_HEX, "000000", cells))
    return out


# ── 用例 ───────────────────────────────────────────────────────────

T = samples.base()


def cases():
    c = os.path.join(T, "cases")
    return [
        (os.path.join(c, "pair_old.xlsx"), os.path.join(c, "pair_new.xlsx")),
        (os.path.join(T, "demo_old.xlsx"), os.path.join(T, "demo_new.xlsx")),
        (os.path.join(T, "demo_new.xlsx"), os.path.join(T, "demo_old.xlsx")),
        (os.path.join(T, "dup_sample.xlsx"), os.path.join(c, "case_strike_desc.xlsx")),
        (os.path.join(c, "case_multi_block.xlsx"), os.path.join(c, "case_ref_guess.xlsx")),
    ]


def dup_cases():
    return [os.path.join(T, "dup_sample.xlsx"),
            os.path.join(T, "cases", "case_dup_many.xlsx")]


def check_table(tag, op, np_):
    lines = run(["--drop-test", op, np_, "--dump-table", "--table-scroll"])
    head, rows = parse_table(lines)
    if not head:
        return 1, "没有 table 行：%s" % " | ".join(lines[:3])
    if head.get("cols") != "12" or head.get("dup") != "0":
        return 1, "列集不对：cols=%s dup=%s" % (head.get("cols"), head.get("dup"))

    want = want_diff(op, np_)
    if len(rows) != len(want):
        return 1, "行数 %d，期望 %d" % (len(rows), len(want))
    for (i, bg, pnbg, cells), (wbg, wpn, wcells) in zip(rows, want):
        if bg != wbg:
            return 1, "第 %d 行底色 %s，期望 %s（%s）" % (i, bg, wbg, wcells[2])
        if pnbg != wpn:
            return 1, "第 %d 行料号格底色 %s，期望 %s" % (i, pnbg, wpn)
        if cells != wcells:
            for k, (a, b) in enumerate(zip(cells, wcells)):
                if a != b:
                    return 1, "第 %d 行第 %d 列 %r，期望 %r" % (i, k, a, b)

    # 冻结列：宽度落在 96–240 逻辑像素，滚到最右不变，表头始终是第 1 列
    frozen = int(head.get("frozen", "0"))
    if not (96 <= frozen <= 240):
        return 1, "冻结列宽 %d 不在 96–240 之间" % frozen
    sc = [ln for ln in lines if ln.startswith("scroll ")]
    if len(sc) != 2:
        return 1, "滚动探针输出不全：%s" % sc
    m0 = re.search(r"start xoff=(\d+) frozen=(\d+) header0=(.*)$", sc[0])
    m1 = re.search(r"end   xoff=(\d+) frozen=(\d+)", sc[1])
    if not m0 or not m1:
        return 1, "滚动探针格式不认识"
    if m0.group(2) != m1.group(2):
        return 1, "冻结列宽随滚动变了 %s → %s" % (m0.group(2), m1.group(2))
    if int(m1.group(1)) <= 0:
        return 1, "滚到最右横向偏移仍为 0（总宽没超过可视宽？）"
    if m0.group(3) != "Part Number":
        return 1, "首列表头是 %r" % m0.group(3)
    return 0, "%d 行 × 12 列 · 冻结 %d · 最右偏移 %s" % (len(rows), frozen, m1.group(1))


def _book_with_cover(path, main_src, extra_src):
    """造一本「封面 + main + 附页」的工作簿，用来验证切表是否重算：
       两本的 main 用同一个源（默认应当 0 差异），附页给不同的源（切过去才有差异）。"""
    import openpyxl
    out = openpyxl.Workbook()
    ws = out.active
    ws.title = "封面"
    ws["A1"] = "某某产品 BOM"
    ws["A2"] = "版本 V1.0 · 修订记录"
    for name, src in (("main", main_src), ("附页", extra_src)):
        b = openpyxl.load_workbook(src)
        t = out.create_sheet(name)
        for row in b[b.sheetnames[0]].iter_rows(values_only=True):
            t.append(list(row))
    out.save(path)


def check_switch():
    """切工作表必须重算（2026-11-05 用户报「标注文件没有颜色、多页面没比」的根因）：
       换表以后差异表 / 诊断页 / 标注三处必须同时换成新表的。留着上一张表的配对
       时，差异表还是旧表的行，诊断与标注却已经是新表 —— 表现就是「差异表有 8 行、
       标注文件一个色块都没有，而且错得悄无声息」。"""
    import openpyxl

    d = os.path.join(TMP, "switch_%d" % os.getpid())
    os.makedirs(d, exist_ok=True)
    op = os.path.join(T, "demo_old.xlsx")
    alt = os.path.join(T, "demo_new.xlsx")
    a = os.path.join(d, "multi_a.xlsx")
    b = os.path.join(d, "multi_b.xlsx")
    _book_with_cover(a, op, op)      # main 相同 → 默认 0 差异
    _book_with_cover(b, op, alt)     # 附页不同 → 切过去才有差异

    # 默认自动挑表：main 有 29 行，过得了 20 行门槛，不会去翻附页
    _h, rows0 = parse_table(run(["--drop-test", a, b, "--dump-table"]))
    if len(rows0) != 0:
        return 1, "默认（main vs main）应当 0 行差异，实际 %d 行" % len(rows0)

    # 两侧都切到附页：差异表必须立刻换成附页的
    head, rows = parse_table(run(["--drop-test", a, b, "--pick-sheet", "old=2,new=2",
                                  "--dump-table"]))
    want = want_diff(a, b, 2, 2)
    if not want:
        return 1, "样本本身没有差异，用例失效"
    if len(rows) != len(want):
        return 1, "切到附页后差异 %d 行，期望 %d（切表没重算？）" % (len(rows), len(want))
    for (i, _bg, _pnbg, cells), (_wbg, _wpn, wcells) in zip(rows, want):
        if cells != wcells:
            return 1, "切表后第 %d 行与 Python 不一致" % i

    # 切表后写出：差异结果 / 诊断 / 标注 必须同源
    out = os.path.join(d, "out")
    os.makedirs(out, exist_ok=True)
    run(["--drop-test", a, b, "--pick-sheet", "old=2,new=2",
         "--out-dir", out, "--click", "act.run"])
    res = os.path.join(out, "multi_a_vs_multi_b_差异结果.xlsx")
    if not os.path.exists(res):
        return 1, "切表后没有写出差异结果"
    wb = openpyxl.load_workbook(res)
    body = [r for r in wb.worksheets[0].iter_rows(min_row=2, values_only=True)
            if r[2] in ("新增", "删除", "变更") and r[0] != "颜色说明"]
    if len(body) != len(want):
        return 1, "写出的差异 %d 行，期望 %d" % (len(body), len(want))
    diag = [r for r in wb.worksheets[-1].iter_rows(min_row=2, values_only=True) if r[1]]
    if [r[1] for r in diag] != ["附页", "附页"]:
        return 1, "诊断页工作表列是 %s，期望两张都是「附页」" % [r[1] for r in diag]

    # 标注：颜色必须真的落在「附页」上，而 main 页一格都不该涂
    old_mark = os.path.join(out, "multi_a_差异标注_旧.xlsx")
    if not os.path.exists(old_mark):
        return 1, "没有写出旧表标注"
    mb = openpyxl.load_workbook(old_mark)
    if "附页" not in mb.sheetnames:
        return 1, "标注里找不到附页"
    cnt = {}

    def marks(ws):
        """只数我们那四色，源表自带的底色（比如表头橙 FFFFC000）不算。"""
        hit = 0
        for row in ws.iter_rows():
            for c in row:
                if c.fill is None or not c.fill.patternType:
                    continue
                rgb = c.fill.fgColor.rgb
                if not isinstance(rgb, str):
                    continue
                tail = rgb[-6:].upper()
                if tail in (ROW_HEX["新增"], ROW_HEX["删除"], ROW_HEX["变更"], DUP_HEX):
                    hit += 1
                    cnt[tail] = cnt.get(tail, 0) + 1
        return hit

    for ws in mb.worksheets:
        n = marks(ws)
        if ws.title == "附页" and n == 0:
            return 1, "附页一格都没涂（标注与配对不同源）"
        if ws.title == "main" and n != 0:
            return 1, "没参与比对的那一页被涂了 %d 格" % n
    if cnt.get(ROW_HEX["删除"], 0) < 1:
        return 1, "附页没有删除行的红色：%s" % cnt
    return 0, "切表后差异 %d 行 · 诊断/标注同源 · 附页涂 %d 格" % (len(want), cnt[ROW_HEX["删除"]])


def check_all_sheets():
    """逐表比对（下拉第一项「全部工作表」）：同名表逐张过，差异结果一表一份，
       标注合并成一份 —— **每张比过的表都得留着颜色**。

       这条是 2026-11-05 第二次报的 bug 的回归：第一版每轮都从原始文件重新复制
       一份标注，于是前面涂好的页全退回原始状态，只剩最后一张表有颜色。
       所以两本的 main 与附页都造出差异，专门盯「先涂的页会不会被后面的冲掉」。"""
    import openpyxl

    d = os.path.join(TMP, "all_%d" % os.getpid())
    out = os.path.join(d, "out")
    os.makedirs(out, exist_ok=True)
    op = os.path.join(T, "demo_old.xlsx")
    alt = os.path.join(T, "demo_new.xlsx")
    a = os.path.join(d, "multi_a.xlsx")
    b = os.path.join(d, "multi_b.xlsx")
    _book_with_cover(a, op, op)      # 三张同名表：封面 / main / 附页
    _book_with_cover(b, alt, alt)    # main 与附页**都有**差异

    # 不给 --all-sheets：两侧都是多表工作簿，应当**自动**进逐表模式。
    # 2026-11-05 那两次「多页没比」的报障，大半原因是人以为已经全比、
    # 实际还停在单表模式（产物名不带表名就是铁证），所以这条专测默认行为。
    lines = run(["--drop-test", a, b, "--out-dir", out, "--click", "act.run"])
    text = "\n".join(lines)
    if "已自动切到逐表比对" not in text:
        return 1, "两侧都是多表工作簿，没有自动进逐表模式：%s" % [
            ln for ln in lines if "工作表" in ln or "比对" in ln][:3]
    if "逐表比对：同名表 3 张 · 写出 2 张 · 跳过 1 张" not in text:
        return 1, "日志汇总不对：%s" % [ln for ln in lines if "逐表比对" in ln][:2]
    if "跳过「封面」" not in text:
        return 1, "没有跳过无数据的封面页"

    got = sorted(f for f in os.listdir(out) if f.endswith(".xlsx"))
    want = sorted([
        "multi_a_vs_multi_b_main_差异结果.xlsx",
        "multi_a_vs_multi_b_附页_差异结果.xlsx",
        "multi_a_差异标注_旧.xlsx", "multi_b_差异标注_新.xlsx",
    ])
    if got != want:
        return 1, "产物 %s" % got

    # 差异结果仍是一表一份，诊断页只写自己那张表、行数各算各的
    for name, sheet, oi in (("multi_a_vs_multi_b_main_差异结果.xlsx", "main", 1),
                            ("multi_a_vs_multi_b_附页_差异结果.xlsx", "附页", 2)):
        want_rows = len(want_diff(a, b, oi, oi))
        if want_rows == 0:
            return 1, "样本 `%s` 没差异，用例失效" % sheet
        wb = openpyxl.load_workbook(os.path.join(out, name))
        body = [r for r in wb.worksheets[0].iter_rows(min_row=2, values_only=True)
                if r[2] in ("新增", "删除", "变更") and r[0] != "颜色说明"]
        diag = [r for r in wb.worksheets[-1].iter_rows(min_row=2, values_only=True) if r[1]]
        if [r[1] for r in diag] != [sheet, sheet]:
            return 1, "%s 诊断页写的是 %s" % (name, [r[1] for r in diag])
        if len(body) != want_rows:
            return 1, "%s 差异 %d 行，期望 %d" % (name, len(body), want_rows)

    # 标注：合并成一份，两个比过的页都得有色，封面一格都不能涂。
    # 两页内容完全一样 → 涂色格数必须**相等**；累积一旦失效，先比的那页会退回 0。
    def sem_hits(ws):
        c = {}
        for row in ws.iter_rows():
            for cell in row:
                if cell.fill is None or not cell.fill.patternType:
                    continue
                rgb = cell.fill.fgColor.rgb
                if not isinstance(rgb, str):
                    continue
                t = rgb[-6:].upper()
                if t in (ROW_HEX["新增"], ROW_HEX["删除"], ROW_HEX["变更"], DUP_HEX):
                    c[t] = c.get(t, 0) + 1
        return c

    for name, title in (("multi_a_差异标注_旧.xlsx", "旧"), ("multi_b_差异标注_新.xlsx", "新")):
        wb = openpyxl.load_workbook(os.path.join(out, name))
        hits = {ws.title: sem_hits(ws) for ws in wb.worksheets}
        if sum(hits.get("封面", {}).values()):
            return 1, "%s 标注里封面被涂了 %s" % (title, hits["封面"])
        # 旧表侧一定涂得出颜色（删除/变更都在旧表）；新表侧要这对样本里真有
        # 「新增」才有得涂，没有就不强求 —— 但两页必须一致
        if title == "旧":
            for ws_title in ("main", "附页"):
                if not hits.get(ws_title):
                    return 1, "%s 标注里 %s 页没颜色（先比的表被后比的冲掉了）" % (title, ws_title)
        if hits["main"] != hits["附页"]:
            return 1, "%s 标注两页色块不一致：main %s vs 附页 %s" % (
                title, hits["main"], hits["附页"])
    return 0, "同名表 3 张 · 写出 2 张 · 跳过封面 · 标注合并且两页都有色"


def check_options():
    """③ 选项区：双键（位号兜底）默认关、能开；近似阈值能调；逐列比较能开。

    盯四件事：
      1. 启动默认：双键关 → 位号/近似配对为 0，无料号行被跳过（不硬凑增删）
      2. 打开双键 → 位号配对出现；阈值 0.90 时近似为 0，降到 0.60 才出现
      3. 点一次控件 → 日志有「比对选项：…」且随后那行「配对 …」是重算结果
      4. 双键关着时阈值框置灰：点它不响应
    """
    a = os.path.join(T, "demo_old.xlsx")
    b = os.path.join(T, "demo_new.xlsx")

    def stats(extra):
        lines = run(["--drop-test", a, b, "--probe-dpi"] + extra)
        for ln in lines:
            if ln.startswith("pair ok="):
                d = dict(re.findall(r"(\w+)=(-?\d+)", ln))
                return {k: int(v) for k, v in d.items()}
        return None

    base = stats([])                                             # 启动默认：双键关
    on = stats(["--dual-key", "1", "--min-overlap", "0.60"])      # 双键开 + 0.60
    hi = stats(["--dual-key", "1", "--min-overlap", "0.95"])      # 双键开 + 几乎全同
    for tag, st in (("默认", base), ("双键开", on), ("阈值 0.95", hi)):
        if not st:
            return 1, "%s 没吐出 pair 行" % tag
    if base["ref"] or base["fuzzy"]:
        return 1, "默认应当双键关闭，却有位号配对：%s" % base
    if not on["ref"] or not on["fuzzy"]:
        return 1, "打开双键后这对样本应当有位号/近似配对：%s" % on
    if on["pairs"] <= base["pairs"]:
        return 1, "打开双键配对没变多：%d → %d" % (base["pairs"], on["pairs"])
    if hi["fuzzy"]:
        return 1, "阈值 0.95 仍有近似配对：%s" % hi
    if hi["ref"] != on["ref"]:
        return 1, "调阈值不该动位号精确配对：%d vs %d" % (hi["ref"], on["ref"])

    # 真点复选框：默认关 → 点一下开启，日志带阈值 0.90（双键开时的默认值）
    lines = run(["--drop-test", a, b, "--click", "opt.dual"])
    text = "\n".join(lines)
    if "双键开启" not in text or "0.90" not in text:
        return 1, "点复选框没按 0.90 开启：%s" % [ln for ln in lines if "选项" in ln][:2]
    pairs = [ln for ln in lines if ln.startswith("配对 ")]
    if not pairs or "位号 0" in pairs[-1]:
        return 1, "点复选框后没重算：%s" % pairs[-1:]

    # 双键刚开时数值就是 0.90，点一下阈值框 → 下一档 0.95
    lines = run(["--drop-test", a, b, "--click", "opt.dual,opt.thr"])
    log = "\n".join(lines)
    if "近似阈值 0.95" not in log:
        return 1, "点阈值没换到下一档：%s" % [ln for ln in log.split("\n") if "阈值" in ln][:3]

    # 双键关着时阈值框该是禁用的：点它不再改值（hit_options 返回 0）
    lines = run(["--drop-test", a, b, "--click", "opt.thr"])
    if "比对选项：近似阈值" in "\n".join(lines):
        return 1, "双键关着还能改阈值，应该置灰不响应"

    # 逐列比较：点一下要记日志（这对样本没有「其他列」差异，行数不该变）
    lines = run(["--drop-test", a, b, "--click", "opt.cell"])
    log = "\n".join(lines)
    if "整行逐列比较开启" not in log:
        return 1, "点逐列比较没记日志：%s" % [ln for ln in lines if "选项" in ln][:2]
    if stats(["--row-diff-all", "1"])["res"] != base["res"]:
        return 1, "这对样本没有别的列差异，行数不该变"
    return 0, "双键默认关（位号 %d→开 %d）· 阈值 0.90→0.95 · 逐列比较可开" % (
        base["ref"], on["ref"])


def check_cell_diff():
    """整行逐列比较：改动描述/厂商两格 → 两行变「变更」，
    变更项写「其他列」，标注里恰好涂这两格。"""
    import openpyxl

    d = os.path.join(TMP, "cell_%d" % os.getpid())
    os.makedirs(d, exist_ok=True)
    src = os.path.join(T, "demo_old.xlsx")
    mod = os.path.join(d, "desc_chg.xlsx")
    wb = openpyxl.load_workbook(src)
    ws = wb.worksheets[0]
    ws.cell(row=7, column=7, value=str(ws.cell(row=7, column=7).value) + "-改")
    ws.cell(row=8, column=6, value="OTHER-VENDOR")
    wb.save(mod)
    wb.close()

    def res_rows(extra):
        out = os.path.join(d, "o" + ("_cell" if extra else "_plain"))
        os.makedirs(out, exist_ok=True)
        run(["--ui-run", src, mod, out, "--pick-sheet", "old=0,new=0"] + extra)
        f = os.path.join(out, "demo_old_vs_desc_chg_差异结果.xlsx")
        if not os.path.exists(f):
            return None, None
        wb = openpyxl.load_workbook(f)
        rows = [r for r in wb.worksheets[0].iter_rows(min_row=2, values_only=True)
                if r[2] in ("新增", "删除", "变更")]
        wb.close()
        return out, rows

    _o1, plain = res_rows([])
    out, cell = res_rows(["--row-diff-all", "1"])
    if plain is None or cell is None:
        return 1, "没写出差异结果"
    if len(cell) != len(plain) + 2:
        return 1, "逐列比较差异 %d 行，期望 %d（描述/厂商两处改动没被算成变更）" % (
            len(cell), len(plain) + 2)
    chg = [r for r in cell if r[2] == "变更"]
    if len(chg) != 2 or any("其他列" not in (r[3] or "") for r in chg):
        return 1, "变更项没写「其他列」：%s" % [(r[0], r[3]) for r in chg]

    # 标注：黄格只落在改动的那两格（G7 描述 / F8 厂商）
    names = {"旧": "demo_old_差异标注_旧.xlsx", "新": "desc_chg_差异标注_新.xlsx"}
    hits = {}
    for side, name in names.items():
        f = os.path.join(out, name)
        if not os.path.exists(f):
            return 1, "没写出%s表标注：%s" % (side, name)
        wb = openpyxl.load_workbook(f)
        got = []
        for row in wb.worksheets[0].iter_rows():
            for c in row:
                if c.fill is not None and c.fill.patternType:
                    rgb = c.fill.fgColor.rgb
                    if isinstance(rgb, str) and rgb[-6:].upper() == ROW_HEX["变更"]:
                        got.append(c.coordinate)
        hits[side] = got
        wb.close()
    for side in ("旧", "新"):
        if "G7" not in hits[side] or "F8" not in hits[side]:
            return 1, "%s 标注没涂到改动的格：%s" % (side, hits[side][:8])
    return 0, "2 行变更为「其他列」，黄格 G7/F8，其余列不受牵连"


def check_help():
    """帮助按钮：手册在 exe 同目录就打开它，不在就退回简要说明。
    为了不受 build/c/bin 里有没有残留文件影响，这里把 exe 放进一个空目录里跑：
    先验「没有手册 → exists=0」，再放一份进去验「exists=1」。

    两个坑都踩过，别再改回去：
    1) 别在 build/c/bin 里造假手册 —— 中途崩了就留着，下一轮构建直接报
       「bin 下不该有手册」，查半天。
    2) 别 rmtree —— 本轮累计删到 50 个文件时安全策略会抛 SystemExit(1)，
       ignore_errors 只兜 OSError，兜不住，整条构建链静默退出（看着像卡死）。
       所以用 mkdtemp 开新目录、硬链接代替复制，收尾逐文件删 + 吞所有异常。"""
    os.makedirs(TMP, exist_ok=True)
    d = tempfile.mkdtemp(dir=TMP, prefix="help_")
    exe = os.path.join(d, os.path.basename(EXE))
    try:
        os.link(EXE, exe)          # 硬链接：不复制 700 KB，也不多占盘
    except OSError:
        shutil.copy2(EXE, exe)

    def probe():
        r = subprocess.run([exe, "--help-probe"], cwd=d, capture_output=True, timeout=60)
        line = next((ln for ln in r.stdout.decode("utf-8", "replace").split("\n")
                     if ln.startswith("help ")), "")
        return line, dict(re.findall(r"(\w+)=([^\s]+)", line))

    line, kv = probe()
    if not line:
        return 1, "没有 help 行"
    path = kv.get("path", "")
    if not path.endswith("用户使用手册.html"):
        return 1, "落点不是手册：%s" % path
    if os.path.normcase(os.path.dirname(path)) != os.path.normcase(d):
        return 1, "落点不在 exe 目录：%s" % path
    if kv.get("exists") != "0":
        return 1, "空目录里不该认到手册：%s" % line

    with open(os.path.join(d, "用户使用手册.html"), "w", encoding="utf-8") as f:
        f.write("<html><body>stub</body></html>")
    line, kv = probe()
    if kv.get("exists") != "1":
        return 1, "手册就位后没认到：%s" % line
    for f in (os.path.join(d, "用户使用手册.html"), exe):
        try:
            os.remove(f)
        except BaseException:                    # noqa: BLE001  删除拦截，留着无妨
            pass
    try:
        os.rmdir(d)
    except BaseException:                        # noqa: BLE001
        pass
    return 0, "落点 = exe 目录/用户使用手册.html · 缺失时 exists=0，就位后 exists=1"


def check_help_pos():
    """帮助按钮必须紧跟在版本徽章后面（2026-11-06 用户要求：原来是浮在右上角）。
    位置从布局表里读，不看像素：`--dump-layout` 的 x/y/w/h 就是实际绘制的坐标。"""
    rect = {}
    for ln in run(["--dump-layout"]):
        f = ln.rstrip("\r\n").split("\t")
        if len(f) == 5:
            rect[f[0]] = tuple(int(v) for v in f[1:])
    for k in ("hdr.ver", "hdr.help"):
        if k not in rect:
            return 1, "布局表里没有 %s" % k
    vx, vy, vw, vh = rect["hdr.ver"]
    hx, hy, hw, hh = rect["hdr.help"]
    gap = hx - (vx + vw)
    if gap != 10:
        return 1, "按钮离版本徽章 %d px（期望 10）：ver @%d w%d，help @%d" % (gap, vx, vw, hx)
    if abs((hy + hh // 2) - (vy + vh // 2)) > 1:
        return 1, "按钮没和版本徽章对齐中线：ver y%d h%d，help y%d h%d" % (vy, vh, hy, hh)
    return 0, "按钮 @%d,%d %d×%d，紧跟版本徽章（间距 %d）" % (hx, hy, hw, hh, gap)


def check_help_brief():
    """手册不在时点帮助按钮：必须弹出简要说明，里面要有「同文件查重」那一段。
    用户报过「点了没反应」，所以这里盯的是「对话框到底有没有出现」，不只是路径对不对。"""
    import ctypes
    import ctypes.wintypes as wt
    import tempfile
    import time

    u = ctypes.WinDLL("user32", use_last_error=True)
    os.makedirs(TMP, exist_ok=True)
    d = tempfile.mkdtemp(dir=TMP, prefix="brief_")
    exe = os.path.join(d, os.path.basename(EXE))
    try:
        os.link(EXE, exe)
    except OSError:
        shutil.copy2(EXE, exe)

    def text_of(hwnd):
        n = u.GetWindowTextLengthW(hwnd)
        b = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, b, n + 1)
        return b.value

    p = subprocess.Popen([exe, "--click", "hdr.help"], cwd=d)
    dlg = 0
    try:
        proto = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        for _ in range(40):
            time.sleep(0.25)
            hits = []

            def each(h, _lp):
                pid = wt.DWORD()
                u.GetWindowThreadProcessId(h, ctypes.byref(pid))
                if pid.value == p.pid and text_of(h).startswith("帮助："):
                    hits.append(h)
                return True
            u.EnumWindows(proto(each), 0)
            if hits:
                dlg = hits[0]
                break
        if not dlg:
            return 1, "点「使用手册」没弹出任何说明（手册缺失时就该有话可说）"
        body = []
        CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        u.EnumChildWindows(dlg, CB(lambda h, _lp: body.append(text_of(h)) or True), 0)
        blob = "\n".join(body)
        for key in ("料号查重", "查重表 8 列", "查重标注.xlsx"):
            if key not in blob:
                return 1, "简要说明里缺「%s」" % key
        u.PostMessageW(dlg, 0x0010, 0, 0)          # WM_CLOSE
        return 0, "弹出「帮助：…」并含同文件查重（%d 字）" % len(blob)
    finally:
        time.sleep(0.2)
        p.terminate()
        time.sleep(0.2)
        for f in os.listdir(d):
            try:
                os.remove(os.path.join(d, f))
            except BaseException:                # noqa: BLE001
                pass
        try:
            os.rmdir(d)
        except BaseException:                    # noqa: BLE001
            pass


def check_dup_table(path):
    lines = run(["--drop-test", path, path, "--dump-table", "--table-scroll"])
    head, rows = parse_table(lines)
    if not head:
        return 1, "没有 table 行"
    if head.get("cols") != "8" or head.get("dup") != "1":
        return 1, "查重列集不对：cols=%s dup=%s" % (head.get("cols"), head.get("dup"))
    want = want_dups(path, 0)
    if len(rows) != len(want):
        return 1, "行数 %d，期望 %d" % (len(rows), len(want))
    for (i, bg, pnbg, cells), (wbg, _wpn, wcells) in zip(rows, want):
        if bg != wbg:
            return 1, "第 %d 行底色 %s，期望 %s" % (i, bg, wbg)
        if cells != wcells:
            for k, (a, b) in enumerate(zip(cells, wcells)):
                if a != b:
                    return 1, "第 %d 行第 %d 列 %r，期望 %r" % (i, k, a, b)
    return 0, "%d 行 × 8 列（%d 个料号）" % (len(rows), len({r[3][0] for r in rows}))


def check_run(tag, op, np_, same):
    # 每个 tag 一个新目录（带 pid）：不整目录删旧产物 —— 一次删几十个文件会被
    # 安全策略拦下（SAFE_DELETE_BULK），整条构建链就断在这一步；新目录同时
    # 保证没有上次的残留干扰「产物必须是这三份」的判定
    outdir = os.path.join(TMP, "%s_%d" % (tag, os.getpid()))
    os.makedirs(outdir, exist_ok=True)

    before = (sha(op), sha(np_))
    # 演示样本两个文件都是两张表，不钉页会自动进「逐表比对」、产物名带表名；
    # 这一项测的是单表三份产物，所以显式选第 0 张（逐表由 all_sheets 专测）
    lines = run(["--ui-run", op, np_, outdir, "--pick-sheet", "old=0,new=0"])
    text = "\n".join(lines)
    if "ui-run ok=1" not in text:
        return 1, "按钮路径没成功：" + " | ".join(ln for ln in lines if ln)[:200]

    files = sorted(f for f in os.listdir(outdir) if f.endswith(".xlsx"))
    stem_o = os.path.splitext(os.path.basename(op))[0]
    stem_n = os.path.splitext(os.path.basename(np_))[0]

    if same:
        want = ["%s_查重标注.xlsx" % stem_o]
    else:
        want = ["%s_vs_%s_差异结果.xlsx" % (stem_o, stem_n),
                "%s_差异标注_旧.xlsx" % stem_o,
                "%s_差异标注_新.xlsx" % stem_n]
    if files != sorted(want):
        return 1, "产物 %s，期望 %s" % (files, want)

    if (sha(op), sha(np_)) != before:
        return 1, "源文件被改了"

    need = ["配对 " if not same else "查重 ", "写差异结果" if not same else "查重标注 "]
    for key in need:
        if key not in text:
            return 1, "日志里没有 %r" % key
    if not same:
        for key in ("标注旧表", "标注新表", "写完了"):
            if key not in text:
                return 1, "日志里没有 %r" % key
    return 0, "3 份" if not same else "1 份"


def main():
    global EXE
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--exe" and i + 1 < len(argv):
            EXE = os.path.abspath(argv[i + 1])

    if not os.path.exists(EXE):
        print("找不到 exe：%s" % EXE)
        return 2
    os.makedirs(TMP, exist_ok=True)

    bad = 0
    n = 0
    for i, (op, np_) in enumerate(cases(), 1):
        tag = "case%02d" % i
        rc, msg = check_table(tag, op, np_)
        n += 1
        if rc:
            bad += 1
            print("FAIL  %-22s 表格 %s" % (tag, msg))
        else:
            print("PASS  %-22s 表格 %s" % (tag, msg))

        rc, msg = check_run(tag, op, np_, False)
        n += 1
        if rc:
            bad += 1
            print("FAIL  %-22s 写出 %s" % (tag, msg))
        else:
            print("PASS  %-22s 写出 %s" % (tag, msg))

    for i, p in enumerate(dup_cases(), 1):
        tag = "dup%02d" % i
        rc, msg = check_dup_table(p)
        n += 1
        if rc:
            bad += 1
            print("FAIL  %-22s 查重表 %s" % (tag, msg))
        else:
            print("PASS  %-22s 查重表 %s" % (tag, msg))

        rc, msg = check_run(tag, p, p, True)
        n += 1
        if rc:
            bad += 1
            print("FAIL  %-22s 查重写出 %s" % (tag, msg))
        else:
            print("PASS  %-22s 查重写出 %s" % (tag, msg))

    # 换工作表：差异表 / 诊断 / 标注 必须一起换（2026-11-05 那个「标注没颜色」的根因）
    rc, msg = check_switch()
    n += 1
    if rc:
        bad += 1
        print("FAIL  %-22s 切表 %s" % ("switch", msg))
    else:
        print("PASS  %-22s 切表 %s" % ("switch", msg))

    # 逐表比对：同名表各出一组，空表跳过
    rc, msg = check_all_sheets()
    n += 1
    if rc:
        bad += 1
        print("FAIL  %-22s 逐表 %s" % ("all_sheets", msg))
    else:
        print("PASS  %-22s 逐表 %s" % ("all_sheets", msg))

    # 真点按钮：页签来回切两次再点「开始比对」，走的是命中 → 分支那条路
    op, np_ = cases()[0]
    outdir = os.path.join(TMP, "click_%d" % os.getpid())
    os.makedirs(outdir, exist_ok=True)
    lines = run(["--drop-test", op, np_, "--out-dir", outdir,
                 "--pick-sheet", "old=0,new=0",
                 "--click", "res.tab2,res.tab1,act.run"])
    n += 1
    got = sorted(f for f in os.listdir(outdir) if f.endswith(".xlsx"))
    if len(got) != 3:
        bad += 1
        print("FAIL  %-22s 点按钮后产物 %s" % ("click", got))
    elif "click act.run" not in "\n".join(lines):
        bad += 1
        print("FAIL  %-22s 没有走到按钮分支" % "click")
    else:
        print("PASS  %-22s 页签来回切 + 点「开始比对」出 3 份" % "click")

    # ③ 选项区：双键可关、近似阈值可调，改完立刻重算
    rc, msg = check_options()
    n += 1
    if rc:
        bad += 1
        print("FAIL  %-22s 选项 %s" % ("options", msg))
    else:
        print("PASS  %-22s 选项 %s" % ("options", msg))

    for tag, fn in (("cell_diff", check_cell_diff), ("help", check_help),
                    ("help_pos", check_help_pos), ("help_brief", check_help_brief)):
        rc, msg = fn()
        n += 1
        if rc:
            bad += 1
            print("FAIL  %-22s %s" % (tag, msg))
        else:
            print("PASS  %-22s %s" % (tag, msg))

    # 无重复料号：一个文件都不生成，日志要明说
    plain = os.path.join(T, "cases", "case_multi_block.xlsx")
    sd = B.load_sheet(plain, B.list_sheets(plain)[0][0], cfg())
    n += 1
    if B.find_dups(sd)["rows"]:
        # 样本前提被改坏时不能让这一项悄悄消失（文档写的是 23 项）
        bad += 1
        print("FAIL  %-22s 样本 case_multi_block 含重复料号，本项前提不成立" % "dup_plain")
    else:
        outdir = os.path.join(TMP, "dup_plain_%d" % os.getpid())
        os.makedirs(outdir, exist_ok=True)
        lines = run(["--ui-run", plain, plain, outdir])
        got = [f for f in os.listdir(outdir) if f.endswith(".xlsx")]
        if got:
            bad += 1
            print("FAIL  %-22s 无重复却生成了 %s" % ("dup_plain", got))
        elif "未发现重复料号" not in "\n".join(lines):
            bad += 1
            print("FAIL  %-22s 日志没交代「未发现重复料号」" % "dup_plain")
        else:
            print("PASS  %-22s 无重复料号不生成文件" % "dup_plain")

    print("\n%d 项检查，%d 个失败" % (n, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
