# -*- coding: utf-8 -*-
"""生成脱敏演示样本 tests/data/demo_old.xlsx / demo_new.xlsx。

为什么要有这个脚本
------------------
回归链原本拿两份真实项目 BOM（客户资料，含料号 / 位号 / 厂商）当主样本。
仓库要公开，这些文件不能跟着走，但「有表头块 + 多工作表 + 有/无料号混排 +
删除线 + 位号近似」这些真实表格的脏结构必须留着 —— 否则 check_pair /
check_write / check_mark / check_ui 就只剩构造的小样本，覆盖不到真实形态。

于是把结构留下、内容全部换成虚构值，并且**写成脚本**而不是手工改表：
任何人跑一次 `python tools/gen_demo.py` 都能复现出逐字节相同的两份文件
（openpyxl 的写出顺序与时间戳在下面统一压平），审查时看得见每一格是哪来的。

虚构口径
--------
  料号   900-1001A0-001   —— 14 位、第 4 / 第 11 位是 '-'，与真实料号同形态，
                             所以识别层的「命中率 100%」照旧成立，但内容全是编的
  位号   R1 / C22 / U3 / TP1 —— 通用器件位号，不对应任何具体图纸
  厂商   VendorA / VendorB / VendorC
  描述   贴片电阻 10KΩ ±1% 这类通用描述
  单位   示例电子（虚构）

用法：python tools/gen_demo.py [--check]
      --check 只重新生成到临时目录并与现有文件逐字节比，不覆盖。
"""
import datetime
import os
import re
import shutil
import sys
import tempfile
import time

import openpyxl
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "data")

TITLE = "示例电子（虚构）"
HEAD = ["序号", "Part Number", "Description", "Reference", "Quantity",
        "Manufacturer", "Part Name", "Note"]

THIN = Side(style="thin", color="FF999999")
BOX = Border(left=THIN, right=THIN, top=THIN, bottom=THIN)
HEAD_FILL = PatternFill("solid", fgColor="FFDCE6F1")


def pn(n, suffix="A0", tail="001"):
    """料号：900-1NNNAs-001，14 位、形态合规但内容虚构。"""
    return "900-%04d%s-%s" % (n, suffix, tail)


# ── 主板：一行一个「逻辑料」，旧新两侧各自取值 ─────────────────────
# (pn_old, pn_new, desc, ref_old, ref_new, qty_old, qty_new, strike_new)
#   pn  给整数 = 该料号；给 None = 这一侧没有这行；给 "" = 这一侧有行但料号空
MAIN = [
    # 20 行两边完全一致（真实 BOM 的常态：绝大多数料号不动）
    (1001, 1001, "贴片电阻 10KΩ ±1% 1/16W", "R1,R2", "R1,R2", 2, 2, 0),
    (1002, 1002, "贴片电阻 100Ω ±1% 1/16W", "R3", "R3", 1, 1, 0),
    (1003, 1003, "贴片电容 100nF 16V X7R", "C1,C2,C3", "C1,C2,C3", 3, 3, 0),
    (1004, 1004, "贴片电容 10uF 10V X5R", "C4", "C4", 1, 1, 0),
    (1005, 1005, "贴片电容 22pF 50V NPO", "C5,C6", "C5,C6", 2, 3, 0),      # 数量 2→3
    (1006, 1006, "肖特基二极管 40V 1A", "D1", "D1", 1, 1, 0),
    (1007, 1007, "发光二极管 绿色 0603", "D2,D3", "D2", 2, 2, 0),          # 位号少一个
    (1008, 1008, "NPN 三极管 40V 200mA", "Q1", "Q1", 1, 1, 0),
    (1009, 1009, "MOSFET N 沟道 30V 5A", "Q2", "Q2", 1, 1, 1),             # 新增删除线
    (1010, 1010, "运算放大器 双路 SO8", "U1", "U1", 1, 1, 0),
    (1011, 1011, "LDO 稳压器 3.3V 500mA", "U2", "U2", 1, 1, 0),
    (1012, 1012, "EEPROM 32Kb I2C", "U3", "U3", 1, 1, 0),
    (1013, 1013, "晶振 12MHz ±20ppm", "Y1", "Y1", 1, 1, 0),
    (1014, 1014, "排阻 4×10KΩ ±5%", "RN1,RN2", "RN1,RN2", 2, 2, 0),
    (1015, 1015, "贴片电感 4.7uH 2A", "L1", "L1", 1, 1, 0),
    (1016, 1016, "连接器 排针 2×5P", "J1", "J1", 1, 1, 0),
    (1017, 1017, "连接器 FPC 16P 0.5mm", "J2", "J2", 1, 1, 0),
    (1018, 1018, "按键 轻触 6×6", "SW1,SW2", "SW1,SW2", 2, 2, 0),
    (1019, 1019, "保险丝 1A 32V", "F1", "F1", 1, 1, 0),
    (1020, 1020, "屏蔽罩 20×20", "SH1", "SH1", 1, 1, 0),
    # 料号被清空、位号不动 → 双键开着才配得上，键类型「位号(料号变更)」
    (1031, "", "贴片电阻 4.7KΩ ±1%", "R41,R42", "R41,R42", 2, 2, 0),
    # 两侧都没有料号、位号各 8 个只差一个（相似度 0.875）→ 位号近似
    ("", "", "贴片电容 1uF 25V X7R", "R201,R202,R203,R204,R205,R206,R207,R208",
     "R201,R202,R203,R204,R205,R206,R207,R209", 8, 8, 0),
    # 无料号（不贴件 / 待定）：双键关着整行跳过
    ("", "", "待定位号（不贴件）", "TP1,TP2", "TP1,TP2", 2, 2, 0),
    ("", "", "待定位号（不贴件）", "TP3", "TP3", 1, 1, 0),
    ("", "", "旧料划除（无料号）", "TP4", "TP4", 1, 1, 0),
    ("", "", "旧料划除（无料号）", "TP5", "TP5", 1, 1, 0),
    # 只在旧表：删除 3 条
    (1041, None, "旧版专用器件 已停产", "U9", None, 1, None, 0),
    (1042, None, "旧版专用器件 已停产", "U10", None, 1, None, 0),
    (1043, None, "旧版专用连接器", "J9", None, 1, None, 0),
    # 只在新表：新增 2 条
    (None, 1051, "新增器件 USB 保护 IC", None, "U20", None, 1, 0),
    (None, 1052, "新增器件 TVS 二极管", None, "D20", None, 1, 0),
]

# ── 电源板：另一张表，结构与主板不同，用来验证跨表 / 逐表比对 ────────
PSU = [
    (2001, 2001, "电解电容 470uF 25V", "C101", "C101", 2, 2, 0),
    (2002, 2002, "电解电容 220uF 35V", "C102", "C102", 1, 1, 0),
    (2003, 2003, "整流桥 600V 2A", "D101", "D101", 1, 1, 0),
    (2004, 2004, "开关变压器 60W", "T101", "T101", 1, 1, 0),
    (2005, 2005, "光耦 5kV", "U101", "U101", 1, 1, 0),
    (2006, 2006, "TL431 基准源", "U102", "U102", 1, 2, 0),
    (2007, 2007, "保险丝 3.15A 250V", "F101", "F101", 1, 1, 0),
    (2008, 2008, "压敏电阻 470V", "RV101", "RV101", 1, 1, 0),
    (2009, 2009, "散热片 30×20", "HS101", "HS101", 1, 1, 0),
    (2010, 2010, "端子 2P 间距 5.08", "J101", "J101", 1, 1, 0),
    (2011, None, "旧版专用 输出滤波电感", "L101", None, 1, None, 0),
    (None, 2012, "新增 共模电感", None, "L102", None, 1, 0),
]


def fill_sheet(ws, spec, side):
    """side = 'old' 写旧侧取值，'new' 写新侧取值。"""
    ws["A1"] = TITLE
    ws["A1"].font = Font(bold=True, size=14)
    ws["A2"] = "产品：DEMO-1000 演示板 ｜ 版本：%s ｜ 日期：%s" % (
        "V1.0" if side == "old" else "V1.1",
        "2026-01-05" if side == "old" else "2026-03-18")
    ws["A3"] = "说明：演示样本，内容全部虚构；由 tools/gen_demo.py 生成，请勿用于生产。"
    ws["A3"].font = Font(size=9, italic=True, color="FF808080")

    for c, name in enumerate(HEAD, 1):
        cell = ws.cell(row=6, column=c, value=name)
        cell.font = Font(bold=True)
        cell.fill = HEAD_FILL
        cell.border = BOX
        cell.alignment = Alignment(horizontal="center", vertical="center")

    widths = [6, 18, 30, 32, 9, 14, 22, 12]
    for c, w in enumerate(widths, 1):
        ws.column_dimensions[get_column_letter(c)].width = w

    r = 7
    seq = 0
    for n_old, n_new, desc, ref_old, ref_new, q_old, q_new, strike in spec:
        no = n_old if side == "old" else n_new
        ref = ref_old if side == "old" else ref_new
        qty = q_old if side == "old" else q_new
        if no is None:
            continue                      # 这一侧没有这行（删除 / 新增的另一半）
        seq += 1
        pn_txt = pn(no) if isinstance(no, int) else None
        # 厂商 / Part Name 由描述派生：两侧同值，料号重编的行才不会在
        # 「整行逐列比较」里多记一笔「其他列」
        mfr = "Vendor%s" % "ABC"[sum(map(ord, desc)) % 3] if pn_txt else ""
        vals = [seq, pn_txt, desc, ref, qty, mfr, desc[:12], None]
        for c, v in enumerate(vals, 1):
            cell = ws.cell(row=r, column=c, value=v)
            cell.border = BOX
            if strike and side == "new":      # 删除线是「新版才划掉」的那一侧
                cell.font = Font(strike=True)
        r += 1

    ws.freeze_panes = "A7"


def build(path):
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "主板BOM"
    fill_sheet(ws, MAIN, "old" if path.endswith("old.xlsx") else "new")

    ws2 = wb.create_sheet("电源板BOM")
    fill_sheet(ws2, PSU, "old" if path.endswith("old.xlsx") else "new")

    wb.properties.creator = "gen_demo.py"
    wb.properties.lastModifiedBy = "gen_demo.py"
    wb.properties.title = "BOM 对比工具演示样本"
    # 固定时间戳：两次生成必须逐字节一致，否则 --check 与 git diff 都没法定论
    wb.properties.created = datetime.datetime(2026, 1, 5, 9, 0, 0)
    wb.properties.modified = datetime.datetime(2026, 1, 5, 9, 0, 0)
    wb.save(path)
    wb.close()
    _fix_zip_time(path)


def _fix_zip_time(path):
    """zip 条目的 DOS 时间戳取的是「此刻」，同一份内容两次生成会差一两个字节。
    这里按原顺序重写一遍、时间戳压成固定值，产物才真的可复现（--check 能逐字节比）。"""
    import zipfile

    with zipfile.ZipFile(path) as zin:
        items = [(i.filename, zin.read(i.filename), i.compress_type) for i in zin.infolist()]
    # openpyxl 在 save 时会把 properties.modified 改写成「此刻」，这里再压回固定值
    items = [(n, re.sub(rb"(<dcterms:modified[^>]*>)[^<]*(</dcterms:modified>)",
                        rb"\g<1>2026-01-05T09:00:00Z\g<2>", d) if n == "docProps/core.xml" else d,
              c) for n, d, c in items]
    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w") as zout:
        for name, data, ctype in items:
            zi = zipfile.ZipInfo(name, date_time=(2026, 1, 5, 9, 0, 0))
            zi.compress_type = ctype
            zi.external_attr = 0o600 << 16
            zout.writestr(zi, data)
    for i in range(20):                      # 杀毒软件扫描新文件时会短暂占用，重试即可
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == 19:
                raise
            time.sleep(0.05)


def main():
    check = "--check" in sys.argv
    out = OUT
    if check:
        out = tempfile.mkdtemp(prefix="demochk_")
    os.makedirs(out, exist_ok=True)
    made = []
    for name in ("demo_old.xlsx", "demo_new.xlsx"):
        p = os.path.join(out, name)
        build(p)
        made.append(p)
        shown = (os.path.basename(p) if check
                 else os.path.relpath(p, ROOT))
        print("写入 %s  %d 字节" % (shown, os.path.getsize(p)))

    if check:
        ok = True
        for p in made:
            ref = os.path.join(OUT, os.path.basename(p))
            a = open(ref, "rb").read() if os.path.exists(ref) else b""
            b = open(p, "rb").read()
            same = a == b
            ok = ok and same
            print("%s %s" % ("一致  " if same else "不同  ", os.path.basename(p)))
        shutil.rmtree(out, ignore_errors=True)
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
