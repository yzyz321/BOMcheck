# -*- coding: utf-8 -*-
"""界面布局的单一数据源。

界面布局（src/ui/layout_gen.h 由 tools/gen_layout_c.py 生成）与对照表
（BOMCheck --export-layout-doc）都调用 resolve()，
因此「文档上的坐标」与「实际控件位置」永远一致，不会走样。

坐标系：逻辑像素（96 DPI 基准），原点在客户区左上角。
实际像素 = 逻辑值 × ui_scale，ui_scale = 当前 DPI / 96。
程序版本号不在这里（唯一来源是 CMakeLists.txt 的 project(... VERSION ...)）。
"""

APP_NAME = "BOM 对比工具"

BASE_W, BASE_H = 1080, 720
MIN_W, MIN_H = 980, 620

COLORS = {
    "bg": "#F4F6F8",
    "card": "#FFFFFF",
    "border": "#E1E5EA",
    "text": "#1F2328",
    "muted": "#6B7280",
    "hint": "#8A9199",
    "accent": "#185FA5",
    "accent_dk": "#124A80",
    "accent_bg": "#E6F1FB",
    "amber": "#BA7517",
    "ok": "#0F6E56",
    "err": "#A32D2D",
    "grid": "#EDEFF2",
    # ── 差异标注四色：唯一来源。界面行色、导出表底色、标注文件底色
    #    全部读这四个键，任何一处改动即三处同步（见文档 2.5 / 附录 A）──
    "row_add": "#C6EFCE",   # 新增 整行
    "row_del": "#FFC7CE",   # 删除 整行
    "row_chg": "#FFEB9C",   # 变更 单元格
    "row_dup": "#D9D2E9",   # 重复料号 仅料号格
    "strike_old": "#9AA0A6",
}

# 颜色说明栏：顺序即界面从左到右的顺序（色块, 名称, 着色范围）
LEGEND = [
    ("新增", "row_add", "整行"),
    ("删除", "row_del", "整行"),
    ("变更", "row_chg", "整行"),
    ("重复料号", "row_dup", "◆ 导出涂料号格"),
]
LEGEND_TAIL = "色值与导出标注文件一致 · 窗口按行着色，导出按格着色"

# 查重模式的图例：只有一个色，说明「重复行」整行浅紫
LEGEND_DUP = [
    ("重复行", "row_dup", "整行浅紫"),
]
LEGEND_TAIL_DUP = "色值与导出的查重标注文件一致 · 只标料号重复的行，无料号行不参与"

# 字号用「点」，程序侧 CreateFontW(-MulDiv(pt, dpi, 72)) 换算，避免二次缩放
FONTS = {
    "h1": (18, "bold"),
    "h2": (13, "bold"),
    "h3": (11, "bold"),
    "base": (10, "normal"),
    "small": (9, "normal"),
    "mono": (9, "normal"),
}
FAMILY = "Microsoft YaHei UI"
FAMILY_FALLBACK = "Segoe UI"
FAMILY_MONO = "Consolas"

DUP_OPTIONS = ["顺序配对（推荐）", "严格中止", "全组合（兼容旧版）"]
DUP_VALUES = ["SEQUENTIAL", "STRICT", "CARTESIAN"]
STRIKE_OPTIONS = ["整行任一格", "仅料号/位号/数量"]
STRIKE_VALUES = ["ALL", "KEY"]

# 分区：id, 名称, 一句话说明
SECTIONS = [
    ("header", "标题区", "版本号、DPI 状态与全局提示"),
    ("files", "文件区", "左右两张卡片，各含路径、浏览、工作表选择与识别摘要"),
    ("options", "选项区", "重复策略、删除线范围（固定推荐）；双键、阈值、逐列比较（可改）"),
    ("actions", "操作区", "执行、导出、停止，以及进度与状态"),
    ("results", "结果区", "差异结果表 / 运行日志两个页签"),
    ("status", "状态栏", "常驻显示引擎版本与本次统计"),
]


def resolve(w=BASE_W, h=BASE_H):
    """按给定客户区尺寸算出全部控件矩形。

    返回 list[dict]，字段：
        id, sec, type, x, y, w, h, text, font, anchor, note, tip, values, state
    """
    C = []

    def add(**kw):
        kw.setdefault("text", "")
        kw.setdefault("font", "base")
        kw.setdefault("anchor", "w")
        kw.setdefault("note", "")
        kw.setdefault("tip", "")
        kw.setdefault("values", None)
        kw.setdefault("state", "normal")
        C.append(kw)

    # ── 标题区 ───────────────────────────────────────────────
    # 标题与版本徽章的显示值由 exe 在运行时填（bs_version / 宽字符转换），
    # 这里的 text 只在布局文档里当占位说明。
    add(id="hdr.title", sec="header", type="label", x=20, y=10, w=180, h=28,
        text=APP_NAME, font="h1",
        note="窗口主标题（非操作系统标题栏）",
        tip="标题文字 = APP_NAME + 空格 + 版本号（CMake 派生）")
    add(id="hdr.ver", sec="header", type="badge", x=206, y=15, w=78, h=22,
        text="V?.?.?", font="h3", anchor="c",
        note="版本徽章；操作系统标题栏同样带版本号；右边紧跟「使用手册」按钮",
        tip="版本号唯一来源：CMakeLists.txt 的 project(BOMCheck VERSION x.y.z)")
    add(id="hdr.sub", sec="header", type="label", x=20, y=38, w=560, h=18,
        text="双键比对 · 删除线入项 · 表头块自动识别 · 未配对的差异单独标注",
        font="small",
        note="一句话功能说明，替代弹窗式帮助")
    add(id="hdr.dpi", sec="header", type="label", x=w - 20 - 320, y=10, w=320, h=18,
        anchor="e", text="DPI 96 · 缩放 100% · 每显示器感知",
        font="small",
        note="实时 DPI 状态：未声明感知时这里会显示告警色",
        tip="进程已按 Per-Monitor DPI 声明感知，跨屏不会位图拉伸")
    add(id="hdr.hint", sec="header", type="label", x=w - 20 - 320, y=32, w=320, h=16,
        anchor="e", text="拖拽 .xlsx 到下方卡片，可直接载入", font="small",
        note="拖拽入口的操作提示")
    add(id="hdr.help", sec="header", type="button", x=294, y=15, w=84, h=22,
        text="使用手册", anchor="c",
        note="帮助按钮：紧跟在版本徽章后面；打开 exe 同目录的用户使用手册.html",
        tip="手册缺失时改为弹出「双键 / 近似阈值 / 整行逐列比较 / 同文件查重」的简要说明")
    add(id="hdr.line", sec="header", type="line", x=0, y=60, w=w, h=1)

    # ── 文件区 ───────────────────────────────────────────────
    gap, m = 16, 16
    cw = int((w - 2 * m - gap) / 2)
    for i, (key, cap, tip) in enumerate((
            ("old", "① 旧 BOM", "作为基准的版本，例如上一版发布 BOM"),
            ("new", "② 新 BOM", "待比较的版本，例如本次变更后的 BOM"))):
        x = m + i * (cw + gap)
        add(id="card." + key, sec="files", type="frame", x=x, y=72, w=cw, h=152,
            text="", note="文件卡片容器（可整体作为拖放目标）")
        add(id="%s.title" % key, sec="files", type="label", x=x + 16, y=84, w=110, h=22,
            text=cap, font="h3", tip=tip,
            note="卡片标题")
        add(id="%s.hint" % key, sec="files", type="label", x=x + 132, y=87, w=200, h=18,
            text="拖入文件，或点「浏览…」", font="small",
            note="拖放提示，与 hdr.hint 呼应")
        add(id="%s.path" % key, sec="files", type="entry", x=x + 16, y=112, w=cw - 120, h=30,
            text="", note="已选文件路径（只读，可选中复制）",
            tip="支持 .xlsx；拖入时会自动填到这里")
        add(id="%s.browse" % key, sec="files", type="button", x=x + cw - 96, y=112, w=80, h=30,
            text="浏览…", anchor="c", note="打开文件对话框")
        add(id="%s.sheetlab" % key, sec="files", type="label", x=x + 16, y=154, w=60, h=20,
            text="工作表", font="small", note="工作表选择器标签")
        add(id="%s.sheet" % key, sec="files", type="combo", x=x + 78, y=150, w=cw - 94, h=28,
            text="", note="载入后列出全部工作表；多页可分别比对")
        add(id="%s.status" % key, sec="files", type="label", x=x + 16, y=186, w=cw - 32, h=20,
            text="未载入", font="small",
            note="识别摘要：C2 起显示表数与行列数，C3 后补表头块 / 料号列 / 位号键行",
            tip="现为：2 表 · 161 行 × 12 列 · 删除线 21 格 · 3 ms（C3 后追加：3 块 · 料号列 H）")

    # ── 选项区 ───────────────────────────────────────────────
    add(id="opt.frame", sec="options", type="frame", x=16, y=236, w=w - 32, h=92)
    add(id="opt.title", sec="options", type="label", x=32, y=246, w=140, h=20,
        text="③ 比对选项", font="h3", note="选项标题")
    add(id="opt.duplab", sec="options", type="label", x=32, y=282, w=64, h=18,
        text="重复料号", font="small", note="重复料号处理策略标签")
    add(id="opt.dup", sec="options", type="combo", x=100, y=276, w=140, h=28,
        text=DUP_OPTIONS[0], values=DUP_OPTIONS,
        note="重复料号策略：顺序配对 / 严格中止 / 全组合",
        tip="顺序配对：old#1↔new#1；严格中止：检出重复即报错；全组合仅用于回归对照")
    add(id="opt.strikelab", sec="options", type="label", x=252, y=282, w=64, h=18,
        text="删除线范围", font="small", note="删除线判定范围标签")
    add(id="opt.strike", sec="options", type="combo", x=320, y=276, w=120, h=28,
        text=STRIKE_OPTIONS[0], values=STRIKE_OPTIONS,
        note="整行任一格 vs 仅三列（兼容旧行为）",
        tip="实测删除线常打在数量/位号/描述列，料号列反而没有，故默认整行")
    add(id="opt.dual", sec="options", type="check", x=456, y=276, w=152, h=28,
        text="双键（位号兜底）", note="默认关闭；勾上后料号为空的行用位号集合建键",
        tip="开启后可识别「料号换了、位号没变」；关闭则只按料号配，这类行算一增一删")
    add(id="opt.thrlab", sec="options", type="label", x=618, y=282, w=64, h=18,
        text="近似阈值", font="small", note="阶段 C 的 Jaccard 阈值")
    add(id="opt.thr", sec="options", type="spin", x=686, y=276, w=62, h=28,
        text="0.90", note="0.30–0.95，仅对位号近似配对生效",
        tip="位号集合重叠度 ≥ 该值且至少一侧无料号时才配对；调高更保守")
    add(id="opt.cell", sec="options", type="check", x=764, y=276, w=176, h=28,
        text="整行逐列比较", note="除料号/位号/数量/删除线外，其余列也逐格比",
        tip="勾上后描述、规格、厂商等任一格不同都算变更，并把不同的格子涂黄")
    add(id="opt.hint", sec="options", type="label", x=32, y=308, w=w - 76, h=16,
        text="双键已关：没料号的行不按位号配对，只算新增/删除 · 勾「整行逐列比较」连描述/规格等列一起比",
        font="small", note="选项区的解释性注释，始终可见")

    # ── 操作区 ───────────────────────────────────────────────
    add(id="act.run", sec="actions", type="button", x=16, y=336, w=140, h=44,
        text="开始比对", font="h3", anchor="c",
        note="主操作（Enter 亦可触发）；两侧同文件时自动变「开始查重」")
    add(id="act.export", sec="actions", type="button", x=168, y=336, w=120, h=44,
        text="另存结果…", anchor="c",
        note="另选目录重写：比对三份 / 查重一份；无结果时禁用",
        tip="比对完会自动写到源文件同级的 checkresults；这里可换目录重写（查重时只重写那一份标注）",
        state="disabled")
    add(id="act.stop", sec="actions", type="button", x=300, y=342, w=88, h=32,
        text="停止", anchor="c",
        note="占位：本版比对/写出在 UI 线程同步执行，没有可取消的检查点；恒为禁用态（不参与命中）",
        state="disabled")
    add(id="act.progress", sec="actions", type="progress", x=410, y=350, w=w - 410 - 190, h=18,
        text="0%", note="进度条：解析 → 配对 → 写表")
    add(id="act.pct", sec="actions", type="label", x=w - 180, y=350, w=60, h=18,
        text="0%", font="small", note="进度百分比")
    add(id="act.state", sec="actions", type="label", x=w - 116, y=350, w=100, h=18,
        anchor="e", text="就绪", font="small", note="当前阶段文字")

    # ── 结果区 ───────────────────────────────────────────────
    nb_y = 388
    nb_h = h - 388 - 28
    add(id="res.frame", sec="results", type="frame", x=16, y=nb_y, w=w - 32, h=nb_h,
        note="结果区卡片（内含页签切换与内容区）")
    add(id="res.tab1", sec="results", type="tab", x=28, y=nb_y + 10, w=92, h=26,
        text="差异结果", font="small", anchor="c", note="页签一：差异表（比对 12 列 / 查重 8 列）",
        tip="差异结果：比对 12 列、查重 8 列，含键类型与变更项")
    add(id="res.tab2", sec="results", type="tab", x=126, y=nb_y + 10, w=88, h=26,
        text="运行日志", font="small", anchor="c", note="页签二：识别诊断与告警",
        tip="运行日志：表头块、列定位、命中率、无法建档行号")
    add(id="res.count", sec="results", type="label", x=w - 240, y=nb_y + 14, w=212, h=20,
        anchor="e", text="0 条差异", font="small",
        note="结果条数（按变更/新增/删除拆分显示）")
    lg_h = 22
    add(id="res.table", sec="results", type="table", x=28, y=nb_y + 40, w=w - 56,
        h=nb_h - 52 - lg_h - 6, text="",
        note="差异表：比对 12 列 / 查重 8 列（含纵横滚动条；行底色与标注文件同色）")
    add(id="res.legend", sec="results", type="legend", x=28, y=nb_y + nb_h - 12 - lg_h,
        w=w - 56, h=lg_h, text="",
        note="颜色说明栏：比对四色 / 查重单色（重复行），常驻在结果表下方",
        tip="色值与导出的标注文件完全同色；查重模式只剩「重复行」；◆ 表示该行料号重复")
    # 日志与表格同尺寸重叠：切页签只抬 Z 序，不重新 place/unplace，
    # 尺寸不一致时会在露出的缝隙里看到另一页签的内容
    add(id="res.log", sec="results", type="log", x=28, y=nb_y + 40, w=w - 56,
        h=nb_h - 52 - lg_h - 6, text="", note="运行日志（只读，等宽字体）")

    # ── 状态栏 ───────────────────────────────────────────────
    add(id="sta.bar", sec="status", type="frame", x=0, y=h - 24, w=w, h=24)
    add(id="sta.text", sec="status", type="label", x=16, y=h - 19, w=w - 32, h=18,
        text="就绪 · 拖入或「浏览…」选择两份 .xlsx · 单份也可先放进来",
        font="small", note="常驻状态：引擎版本、耗时、跳过行数、无法建档行号")
    return C


def section_of(cid):
    for c in resolve():
        if c["id"] == cid:
            return c["sec"]
    return ""


if __name__ == "__main__":
    cs = resolve()
    print("控件数：%d" % len(cs))
    for s in SECTIONS:
        n = [c for c in cs if c["sec"] == s[0]]
        print("  %-8s %2d 个  %s" % (s[1], len(n), s[2]))
