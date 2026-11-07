# -*- coding: utf-8 -*-
"""模糊输入：喂畸形 / 极端 xlsx，程序必须「明确报错或正常跑完」，绝不能崩。

判定标准只有一条：进程退出码必须是 0/1/2（我们自己的约定），
不能是 0xC0000005 这类访问违例。崩溃在 Windows 上表现为 -1073741819。

用例分三批：
  A 手工拼的畸形 zip —— 结构层面不合法（缺部件、截断、垃圾字节）
  B openpyxl 造的极端表 —— 结构合法但内容极端（空表、超长单元格、位号上千）
  C 引用/行号越界 —— 修复 2026-11-07 P0/P1 的回归：列号溢出必须不崩、
    行号溢出必须不挂（曾经 7 个 'Z' 会堆越界，r="2147483647" 会死循环）

用法：python tools/check_fuzz.py [--exe <exe>] [--clean]

默认把产物写进 build/fuzz/out 并就地覆盖（同名文件直接重写），
--clean 才先整目录清空；不默认清空是因为一次删几十个文件在某些
带安全策略的环境里会被拦下，让整条构建链断在这一步。
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "tools"))
from findexe import find_exe  # noqa: E402

WORK = os.path.join(HERE, "build", "fuzz")

CT = """<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
</Types>"""

RELS = """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>"""

WB = """<?xml version="1.0" encoding="UTF-8"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
 xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<sheets><sheet name="BOM" sheetId="1" r:id="rId1"/></sheets></workbook>"""

WB_RELS = """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
</Relationships>"""


def sheet(rows_xml: str) -> str:
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
            '<sheetData>' + rows_xml + '</sheetData></worksheet>')


def cell(ref: str, text: str) -> str:
    esc = (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))
    return '<c r="%s" t="inlineStr"><is><t>%s</t></is></c>' % (ref, esc)


def row(n: int, cells: str) -> str:
    return '<row r="%d">%s</row>' % (n, cells)


def raw_book(path: str, sheet_xml: str | None) -> None:
    """拼一个「结构合法」的包；sheet_xml 为 None 时干脆缺 sheet 部件"""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CT)
        z.writestr("_rels/.rels", RELS)
        z.writestr("xl/workbook.xml", WB)
        z.writestr("xl/_rels/workbook.xml.rels", WB_RELS)
        if sheet_xml is not None:
            z.writestr("xl/worksheets/sheet1.xml", sheet_xml)


def build_cases() -> list[tuple[str, str]]:
    os.makedirs(WORK, exist_ok=True)
    out: list[tuple[str, str]] = []

    def p(name: str) -> str:
        return os.path.join(WORK, name)

    header = row(1, cell("A1", "序号") + cell("B1", "位号") + cell("C1", "料号") + cell("D1", "数量"))

    # A 批：结构畸形
    with zipfile.ZipFile(p("empty_zip.xlsx"), "w"):
        pass
    out.append(("空 zip", p("empty_zip.xlsx")))

    with zipfile.ZipFile(p("junk.xlsx"), "w") as z:
        z.writestr("hello.txt", "not a workbook at all")
    out.append(("只有无关文件", p("junk.xlsx")))

    with open(p("zero.xlsx"), "wb"):
        pass
    out.append(("0 字节", p("zero.xlsx")))

    with open(p("plain.xlsx"), "wb") as f:
        f.write(b"PK\x03\x04" + b"\x00" * 64)          # 只有本地头，没有中央目录
    out.append(("假 PK 头", p("plain.xlsx")))

    raw_book(p("no_sheet.xlsx"), None)
    out.append(("缺 sheet 部件", p("no_sheet.xlsx")))

    raw_book(p("bad_xml.xlsx"), '<worksheet><sheetData><row r="1">')
    out.append(("未闭合 xml", p("bad_xml.xlsx")))

    # 真截断：先写完整的包再砍掉尾巴
    full = p("trunc_src.xlsx")
    raw_book(full, sheet(header + row(2, cell("C2", "100-000001-001"))))
    data = open(full, "rb").read()
    with open(p("truncated.xlsx"), "wb") as f:
        f.write(data[: len(data) // 2])
    out.append(("截断的包", p("truncated.xlsx")))

    # B 批：结构合法、内容极端
    raw_book(p("header_only.xlsx"), sheet(header))
    out.append(("只有表头", p("header_only.xlsx")))

    raw_book(p("empty_rows.xlsx"), sheet(header + "".join(row(i, "") for i in range(2, 60))))
    out.append(("58 个空行", p("empty_rows.xlsx")))

    big = "A" * 200000
    raw_book(p("huge_cell.xlsx"), sheet(header + row(2, cell("C2", big) + cell("D2", "1"))))
    out.append(("20 万字符单元格", p("huge_cell.xlsx")))

    refs = ",".join("R%d" % i for i in range(1, 1201))
    raw_book(p("many_refs.xlsx"),
             sheet(header + row(2, cell("B2", refs) + cell("D2", "1"))))
    out.append(("单行 1200 个位号", p("many_refs.xlsx")))

    rows = [header]
    for i in range(2, 400):
        rows.append(row(i, cell("A%d" % i, str(i)) + cell("B%d" % i, "C%d" % i) +
                        cell("D%d" % i, "1")))
    raw_book(p("no_pn.xlsx"), sheet("".join(rows)))
    out.append(("全表无料号", p("no_pn.xlsx")))

    # 非法行号 / 越界列
    raw_book(p("bad_ref.xlsx"),
             sheet(header + row(2, '<c r="ZZZZ9999" t="inlineStr"><is><t>x</t></is></c>')))
    out.append(("非法单元格引用", p("bad_ref.xlsx")))

    # C 批：列号/行号越界（2026-11-07 P0/P1 回归）
    # 7 个 Z = 8,353,082,582，旧实现 int 回绕成 -236,852,010 → 位图负下标越界写
    raw_book(p("bad_colref_7z.xlsx"),
             sheet(header + row(2, '<c r="ZZZZZZZ1" t="inlineStr"><is><t>x</t></is></c>')))
    out.append(("列号溢出(7字母)", p("bad_colref_7z.xlsx")))

    # INT_MAX 行号：旧实现 r++ 溢出，-O2 下循环条件恒真 → 永不退出
    raw_book(p("bad_row_intmax.xlsx"),
             sheet(header + '<row r="2147483647">' + cell("A2147483647", "zz") + "</row>"))
    out.append(("行号 INT_MAX", p("bad_row_intmax.xlsx")))

    # 略超 Excel 上限：应整行忽略，而不是把行号钳进表格变成假数据
    raw_book(p("bad_row_over.xlsx"),
             sheet(header + '<row r="1048577">' + cell("A1048577", "zz") + "</row>"))
    out.append(("行号 1048577", p("bad_row_over.xlsx")))

    # 十亿级行号：旧实现每张表要多扫十亿个空行（约 3 秒）
    raw_book(p("bad_row_1e9.xlsx"),
             sheet(header + '<row r="1000000000">' + cell("A1000000000", "zz") + "</row>"))
    out.append(("行号 1e9", p("bad_row_1e9.xlsx")))

    # 超大合并区（A1:XFD1048576 ≈ 170 亿格）：展开时必须走护栏直接跳过
    raw_book(p("huge_merge.xlsx"),
             sheet(header + row(2, cell("C2", "100-000001-001"))).replace(
                 "</sheetData>",
                 '</sheetData><mergeCells count="1">'
                 '<mergeCell ref="A1:XFD1048576"/></mergeCells>'))
    out.append(("超大合并区", p("huge_merge.xlsx")))
    return out


def run(exe: str, args: list[str]) -> tuple[int, str]:
    """退出码 0/1/2 是约定；-99 专指超时（程序挂死），与崩溃一样算失败。"""
    try:
        p = subprocess.run([exe] + args, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return -99, "TIMEOUT"
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


def main() -> int:
    exe = None
    if "--exe" in sys.argv:
        i = sys.argv.index("--exe")
        if i + 1 < len(sys.argv):
            exe = sys.argv[i + 1]
    if not exe:
        exe = find_exe()
    if not exe:
        print("找不到产物，先 build")
        return 1
    cases = build_cases()
    outdir = os.path.join(WORK, "out")
    if "--clean" in sys.argv:
        shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir, exist_ok=True)

    bad = 0
    for name, path in cases:
        # --xlsx-dump 只走读取链；--bench 再走识别 + 配对 + 写出
        rc1, o1 = run(exe, ["--xlsx-dump", path])
        rc2, o2 = run(exe, ["--bench", path, path, outdir])
        for rc in (rc1, rc2):
            if rc == -99:
                bad += 1
                print("HANG  %-18s 120 秒未退出" % name)
                break
            if rc not in (0, 1, 2):
                bad += 1
                print("CRASH %-18s rc=%d" % (name, rc))
                break
        else:
            note = "ok"
            if "ERROR" in o1 or "error" in o2:
                note = "已拒绝"
            print("PASS  %-18s dump=%d bench=%d  %s" % (name, rc1, rc2, note))

    print("\n%d 个模糊用例，%d 个崩溃" % (len(cases), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
