# -*- coding: utf-8 -*-
"""生成识别层的边界样本（C3 回归用），落在 temp/cases/。

每个文件针对一种形态，Python 与 C 必须得出同一结论：
  case_header_at60   表头在第 60 行（验证扫描不硬截断在 50 行）
  case_no_header     没有任何列名（两边都该判「无表头」）
  case_header_only   只有表头没有数据行
  case_multi_block   两块表头，中间隔一行空行
  case_label_row     带跨列合并的分组标题行（labels 计数）
  case_ref_guess     位号列没有列名，只能靠形态猜
  case_qty_junk      数量列里有 "1,000" / "N/A" 这类垃圾
  case_dup_strike    重复料号 + 删除线（dup_i/dup_n 与 strike）
  case_strike_desc   只有说明列带删除线（区分「整行」与「仅三格」两种口径）
  case_wide_cols     料号列在第 28 列（AB），验证列名换算与宽表扫描
  case_pn_half       料号列只有一半行合格（命中率 0.5，默认阈值 0.6 应拒绝）
  case_fallback      表头没有任何标准列名，只能靠「下面几行像料号」倒推
  case_two_alias     两列都叫料号别名，按内容命中率择优（并验证同分取左）
  case_merge_vertical 竖向合并单元格（不应被当成跨列标题行）

用法：python tools/gen_sheet_cases.py
"""
import os
import sys

try:
    from openpyxl import Workbook
    from openpyxl.styles import Font
except ImportError:                      # 本机默认 python 没装，换 miniconda 里那个
    _alt = os.environ.get("BOMCHECK_PY", r"D:\miniconda3\python.exe")
    if os.path.exists(_alt) and os.path.abspath(sys.executable) != os.path.abspath(_alt):
        os.execv(_alt, [_alt] + sys.argv)
    raise SystemExit("需要 openpyxl：pip install openpyxl")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import samples  # noqa: E402  样本位置（tests/data，缺了退回 temp）
OUT = samples.p("cases")

HDR = ["No.", "Part Number", "Reference", "Quantity", "Description"]


def book():
    wb = Workbook()
    ws = wb.active
    ws.title = "main"
    return wb, ws


def put_header(ws, row, names=HDR):
    for c, v in enumerate(names, 1):
        ws.cell(row=row, column=c, value=v)


def put_rows(ws, start, rows):
    for i, r in enumerate(rows):
        for c, v in enumerate(r, 1):
            if v is None:
                continue
            cell = ws.cell(row=start + i, column=c, value=v)
            if str(v).startswith("~") and False:
                cell.font = Font(strike=True)


def save(wb, name):
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, name)
    wb.save(p)
    print("已生成", os.path.relpath(p, ROOT))


def case_header_at60():
    wb, ws = book()
    ws["A1"] = "ElecBOM 明细表"
    for r in range(3, 60):
        ws.cell(row=r, column=1, value=r)          # 噪声行：只有一列
    put_header(ws, 60)
    put_rows(ws, 61, [
        [1, "111-000001-A10", "R1,R2", 2, "电阻"],
        [2, "111-000002-B20", "C1", 1, "电容"],
        [3, "111-000003-C30", "U1,U2", 2, "芯片"],
    ])
    save(wb, "case_header_at60.xlsx")


def case_no_header():
    wb, ws = book()
    for r in range(1, 12):
        ws.cell(row=r, column=1, value=r)
        ws.cell(row=r, column=2, value="%03d-%06d-A%02d" % (r, r * 7, r))
        ws.cell(row=r, column=3, value="R%d" % r)
        ws.cell(row=r, column=4, value=1)
    save(wb, "case_no_header.xlsx")


def case_header_only():
    wb, ws = book()
    put_header(ws, 1)
    save(wb, "case_header_only.xlsx")


def case_multi_block():
    wb, ws = book()
    put_header(ws, 1)
    put_rows(ws, 2, [
        [1, "111-000001-A10", "R1", 1, "电阻"],
        [2, "111-000002-B20", "C1,C2", 2, "电容"],
    ])
    put_header(ws, 5)
    put_rows(ws, 6, [
        [3, "222-000003-C30", "U1", 1, "芯片"],
        [4, "222-000004-D40", "J1,J2", 2, "连接器"],
    ])
    save(wb, "case_multi_block.xlsx")


def case_label_row():
    wb, ws = book()
    put_header(ws, 1)
    put_rows(ws, 2, [[1, "111-000001-A10", "R1", 1, "电阻"]])
    ws.merge_cells("A4:E4")
    ws["A4"] = "电容 Capacitor"
    put_rows(ws, 5, [[2, "111-000002-B20", "C1,C2", 2, "电容"]])
    save(wb, "case_label_row.xlsx")


def case_ref_guess():
    wb, ws = book()
    # 位号列故意不叫 Reference / 位号
    put_header(ws, 1, ["No.", "Part Number", "Location", "Qty", "Desc"])
    put_rows(ws, 2, [
        [1, "111-000001-A10", "R1,R2,R3", 3, "电阻"],
        [2, "111-000002-B20", "C1,C2", 2, "电容"],
        [3, "111-000003-C30", "U1,U2,U3,U4", 4, "芯片"],
        [4, "111-000004-D40", "J1", 1, "连接器"],
    ])
    save(wb, "case_ref_guess.xlsx")


def case_qty_junk():
    wb, ws = book()
    put_header(ws, 1)
    put_rows(ws, 2, [
        [1, "111-000001-A10", "R1", "1,000", "电阻"],
        [2, "111-000002-B20", "C1", "N/A", "电容"],
        [3, "111-000003-C30", "U1", 2.5, "芯片"],
        [4, "111-000004-D40", "J1", " 3 ", "连接器"],
        [5, "111-000005-E50", "J2", None, "连接器"],
    ])
    save(wb, "case_qty_junk.xlsx")


def case_dup_strike():
    wb, ws = book()
    put_header(ws, 1)
    rows = [
        (1, "111-000001-A10", "R1", 1),
        (2, "111-000001-A10", "R2", 1),
        (3, "111-000001-A10", "R3", 1),
        (4, "111-000002-B20", "C1", 2),
        (5, None, "C9", 1),
    ]
    for i, (no, pn, ref, qty) in enumerate(rows):
        r = 2 + i
        ws.cell(row=r, column=1, value=no)
        if pn:
            ws.cell(row=r, column=2, value=pn)
        ws.cell(row=r, column=3, value=ref)
        ws.cell(row=r, column=4, value=qty)
        if i in (1, 3):
            for c in range(1, 5):
                ws.cell(row=r, column=c).font = Font(strike=True)
    save(wb, "case_dup_strike.xlsx")


def case_strike_desc():
    wb, ws = book()
    put_header(ws, 1)
    for i, (pn, ref) in enumerate([("111-000001-A10", "R1"),
                                  ("111-000002-B20", "C1")]):
        r = 2 + i
        ws.cell(row=r, column=1, value=i + 1)
        ws.cell(row=r, column=2, value=pn)
        ws.cell(row=r, column=3, value=ref)
        ws.cell(row=r, column=4, value=1)
        ws.cell(row=r, column=5, value="说明")
        ws.cell(row=r, column=5).font = Font(strike=True)   # 只有非关键列划除
    save(wb, "case_strike_desc.xlsx")


def case_wide_cols():
    wb, ws = book()
    for c in range(1, 31):
        ws.cell(row=1, column=c, value="备注%d" % c if c != 28 else "Part Number")
    ws.cell(row=1, column=1, value="No.")
    ws.cell(row=1, column=2, value="Reference")
    ws.cell(row=1, column=3, value="Quantity")
    for i in range(4):
        r = 2 + i
        ws.cell(row=r, column=1, value=i + 1)
        ws.cell(row=r, column=2, value="R%d" % (i + 1))
        ws.cell(row=r, column=3, value=1)
        ws.cell(row=r, column=28, value="111-00000%d-A1%d" % (i, i))
    save(wb, "case_wide_cols.xlsx")


def case_pn_half():
    wb, ws = book()
    put_header(ws, 1)
    for i in range(6):
        r = 2 + i
        ws.cell(row=r, column=1, value=i + 1)
        # 一半是料号，一半是描述文本：命中率 0.5 < 0.6
        ws.cell(row=r, column=2, value=("111-00000%d-A10" % i) if i % 2 == 0
                else "贴片电阻 0603 1%%")
        ws.cell(row=r, column=3, value="R%d" % (i + 1))
        ws.cell(row=r, column=4, value=1)
    save(wb, "case_pn_half.xlsx")


def case_fallback():
    wb, ws = book()
    ws["A1"] = "ElecBOM 明细"
    # 表头列名全是不规范写法：别名一个都不中
    for c, v in enumerate(["序号", "A 列", "B 列", "C 列"], 1):
        ws.cell(row=2, column=c, value=v)
    for i in range(5):
        r = 3 + i
        ws.cell(row=r, column=1, value=i + 1)
        ws.cell(row=r, column=2, value="111-00000%d-A10" % i)
        ws.cell(row=r, column=3, value="R%d,R%d" % (i + 1, i + 2))
        ws.cell(row=r, column=4, value=2)
    save(wb, "case_fallback.xlsx")


def case_two_alias():
    wb, ws = book()
    for c, v in enumerate(["No.", "Part Number", "PN", "Quantity"], 1):
        ws.cell(row=1, column=c, value=v)
    # 第 2 列 3/4 合格，第 3 列 4/4 合格：两列都过阈值，按命中率选第 3 列
    pns = ["111-000001-A10", "111-000002-B20", "111-000003-C30", "描述文字"]
    alt = ["111-000001-A10", "111-000002-B20", "111-000003-C30", "111-000004-D40"]
    for i in range(4):
        r = 2 + i
        ws.cell(row=r, column=1, value=i + 1)
        ws.cell(row=r, column=2, value=pns[i])
        ws.cell(row=r, column=3, value=alt[i])
        ws.cell(row=r, column=4, value=1)
    save(wb, "case_two_alias.xlsx")


def case_merge_vertical():
    wb, ws = book()
    put_header(ws, 1)
    put_rows(ws, 2, [[1, "111-000001-A10", "R1", 1, "电阻"],
                     [2, "111-000002-B20", "C1", 2, "电容"]])
    ws.merge_cells("A2:A3")       # 竖向合并：不是跨列标题
    save(wb, "case_merge_vertical.xlsx")


def pair_files():
    """成对样本：把四阶段配对 + 2×3 重复一次性压在一对文件上。

    旧 8 行 / 新 9 行，覆盖：
      料号直配后改数量、改位号、改删除线（阶段 A）
      旧无料号 / 新有料号，位号集合相同 → 位号(料号变更)（阶段 C）
      两侧都无料号，位号 2/3 重叠 → 位号(近似)（阶段 C）
      同一料号 旧 2 行 / 新 3 行 → 2 条变更 + 1 条新增，而不是 6 条（R5）
      旧独有 1 行 → 删除；新独有 1 行 → 新增（阶段 D）"""
    old = [
        [1, "111-000001-A01", "R1,R2", 1, "电阻"],
        [2, None, "C1,C2,C3", 1, "电容"],
        [3, "111-000002-A02", "C5,C6", 1, "电容"],
        [4, "111-000003-A03", "R7", 2, "电阻"],
        [5, None, "R8,R9", 1, "芯片"],
        [6, "111-000004-A04", "R10,R11", 1, "芯片"],
        [7, "111-000004-A04", "R12", 1, "芯片"],
        [8, "111-000005-A05", "J1", 1, "连接器"],
    ]
    new = [
        [1, "111-000001-A01", "R1,R2", 2, "电阻"],
        [2, "111-000009-Z09", "C1,C2,C3", 1, "电容"],
        [3, "111-000002-A02", "C5", 1, "电容"],
        [4, "111-000003-A03", "R7", 2, "电阻"],
        [5, None, "R8,R9,R10", 1, "芯片"],
        [6, "111-000004-A04", "R10,R11", 2, "芯片"],
        [7, "111-000004-A04", "R12A", 1, "芯片"],
        [8, "111-000004-A04", "R13", 1, "芯片"],
        [9, None, "J5", 1, "连接器"],
    ]
    for name, rows, strike_row in (("pair_old.xlsx", old, None),
                                   ("pair_new.xlsx", new, 5)):
        wb, ws = book()
        put_header(ws, 1)
        put_rows(ws, 2, rows)
        if strike_row:                       # 第 4 条数据行（r5）整行划除
            for c in range(1, 6):
                ws.cell(row=strike_row, column=c).font = Font(strike=True)
        save(wb, name)


def case_dup_many():
    """10 个料号各出现 2 次：把 STRICT 的取样列表撑过 8 个，验证扩容与排序。"""
    wb, ws = book()
    put_header(ws, 1)
    rows = []
    for i in range(10):
        pn = "111-00005%d-B1%d" % (i, i)
        rows.append([i * 2 + 1, pn, "R%d" % (i * 2 + 1), 1, "电阻"])
        rows.append([i * 2 + 2, pn, "R%d" % (i * 2 + 2), 1, "电阻"])
    put_rows(ws, 2, rows)
    save(wb, "case_dup_many.xlsx")


def main():
    case_header_at60()
    case_no_header()
    case_header_only()
    case_multi_block()
    case_label_row()
    case_ref_guess()
    case_qty_junk()
    case_dup_strike()
    case_strike_desc()
    case_wide_cols()
    case_pn_half()
    case_fallback()
    case_two_alias()
    case_merge_vertical()
    case_dup_many()
    pair_files()


if __name__ == "__main__":
    main()
