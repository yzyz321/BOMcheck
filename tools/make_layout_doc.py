# -*- coding: utf-8 -*-
"""由 pyref/layout_spec.py 生成《GUI 控件位置对照图》（自包含 HTML，V4）。

坐标不是手抄的：直接读 resolve()，所以改了布局重跑本脚本即可同步，
不会出现「文档上的坐标和程序对不上」。

与另外两条消费方同源：
    pyref/layout_spec.py ──► tools/gen_layout_c.py ──► src/ui/layout_gen.h（摆控件）
                       ──► BOMCheck --export-layout-doc ──► docs/GUI控件位置对照表.md
                       ──► 本脚本 ──► docs/GUI控件位置对照图_v4.html（视觉图）

版本号不在脚本里写死：从 CMakeLists.txt 的 project(... VERSION ...) 取。
"""
import base64
import datetime
import os
import re
import sys
from string import Template

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "pyref"))
import layout_spec as L      # noqa: E402

OUT = os.path.join(ROOT, "docs", "GUI控件位置对照图_v4.html")
TITLEBAR_H = 36
NUM_TYPES = {"frame", "badge", "entry", "combo", "button", "check", "spin",
             "progress", "tab", "table", "legend"}

FS = {"h1": 21, "h2": 14, "h3": 13, "base": 12, "small": 10.5, "mono": 11}
C = L.COLORS


def version():
    """唯一来源：CMakeLists.txt 的 project(BOMCheck VERSION x.y.z)。"""
    p = os.path.join(ROOT, "CMakeLists.txt")
    with open(p, encoding="utf-8") as f:
        m = re.search(r"project\s*\(\s*BOMCheck\s+VERSION\s+([0-9.]+)", f.read())
    return "V" + m.group(1) if m else "V?"


VER = version()
TITLE = "%s %s" % (L.APP_NAME, VER)


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def text_w(s, size):
    """粗略估算文本宽度：中文按 1em，其余按 0.56em。"""
    w = 0.0
    for ch in str(s):
        w += 1.0 if ord(ch) > 0x2E80 else 0.56
    return w * size


def clip(s, size, maxw):
    """按可用宽度截断并加省略号。"""
    if not s or text_w(s, size) <= maxw:
        return s
    out = ""
    for ch in str(s):
        if text_w(out + ch + "…", size) > maxw:
            break
        out += ch
    return out + "…"


def txt(c, x, y, s, size, fill, anchor="start", weight=400, extra=""):
    return ('<text x="%.1f" y="%.1f" font-size="%.1f" fill="%s" text-anchor="%s" '
            'font-weight="%d" dominant-baseline="central"%s>%s</text>'
            % (x, y, size, fill, anchor, weight, extra, esc(s)))


def render(c, n):
    t, x, y, w, h = c["type"], c["x"], c["y"], c["w"], c["h"]
    o = []
    if c["id"] == "sta.bar":
        return ('<rect x="%d" y="%d" width="%d" height="%d" fill="#EDF0F3"/>'
                % (x, y, w, h))
    if t == "frame":
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="10" fill="%s" '
                 'stroke="%s" stroke-width="1"/>' % (x, y, w, h, C["card"], C["border"]))
    elif t == "line":
        o.append('<rect x="%d" y="%d" width="%d" height="1" fill="%s"/>'
                 % (x, y, w, C["border"]))
    elif t == "label":
        size = FS[c["font"]]
        anchor = "end" if c["anchor"] == "e" else ("middle" if c["anchor"] == "c" else "start")
        px = x + w if anchor == "end" else (x + w / 2 if anchor == "middle" else x)
        fill = C["muted"] if c["font"] == "small" else C["text"]
        weight = 700 if c["font"] in ("h1", "h3") else 400
        maxw = w - 4
        o.append(txt(c, px, y + h / 2, clip(c["text"], size, maxw), size, fill, anchor, weight))
    elif t == "badge":
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="4" fill="%s"/>'
                 % (x, y, w, h, C["accent_bg"]))
        o.append(txt(c, x + w / 2, y + h / 2, VER, FS[c["font"]], C["accent"],
                     "middle", 700))
    elif t in ("entry", "combo", "spin"):
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="5" fill="#FFFFFF" '
                 'stroke="%s"/>' % (x, y, w, h, C["border"]))
        val = c["text"]
        if t == "entry" and not c["text"]:
            val = "D:\\…\\demo_old.xlsx"
        size = FS["small"]
        pad = 8
        mw = w - pad * 2 - (14 if t in ("combo", "spin") else 0)
        o.append(txt(c, x + pad, y + h / 2, clip(val, size, mw), size,
                     C["text"] if c["text"] else C["hint"]))
        if t == "combo":
            cx, cy = x + w - 11, y + h / 2 - 1
            o.append('<path d="M%.1f %.1f L%.1f %.1f L%.1f %.1f" fill="none" stroke="%s" '
                     'stroke-width="1.4" stroke-linecap="round"/>'
                     % (cx - 4, cy - 2, cx, cy + 2.5, cx + 4, cy - 2, C["muted"]))
        if t == "spin":
            cx, cy = x + w - 11, y + h / 2
            o.append('<path d="M%.1f %.1f L%.1f %.1f L%.1f %.1f Z" fill="%s"/>'
                     % (cx - 3.5, cy - 3, cx + 3.5, cy - 3, cx, cy - 7, C["muted"]))
            o.append('<path d="M%.1f %.1f L%.1f %.1f L%.1f %.1f Z" fill="%s"/>'
                     % (cx - 3.5, cy + 3, cx + 3.5, cy + 3, cx, cy + 7, C["muted"]))
    elif t == "button":
        accent = c["id"] == "act.run"
        dis = c["state"] == "disabled"
        fill = C["accent"] if accent else ("#F4F5F7" if dis else "#FFFFFF")
        stroke = C["accent"] if accent else C["border"]
        color = "#FFFFFF" if accent else (C["hint"] if dis else C["text"])
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="%s" stroke="%s"/>'
                 % (x, y, w, h, fill, stroke))
        size = FS[c["font"]]
        o.append(txt(c, x + w / 2, y + h / 2, c["text"], size, color, "middle",
                     700 if accent else 400))
    elif t == "check":
        # V4 默认：双键关、整行逐列比较关 —— 只有 state=="checked" 才打勾
        box = 13
        by = y + (h - box) / 2
        o.append('<rect x="%d" y="%.1f" width="%d" height="%d" rx="3" fill="#FFFFFF" '
                 'stroke="%s"/>' % (x + 2, by, box, box, C["muted"]))
        if c["state"] == "checked":
            o.append('<path d="M%.1f %.1f L%.1f %.1f L%.1f %.1f" fill="none" stroke="%s" '
                     'stroke-width="1.8" stroke-linecap="round"/>'
                     % (x + 5, by + 6.6, x + 7.8, by + 9.4, x + 12.2, by + 3.4, C["accent"]))
        o.append(txt(c, x + 21, y + h / 2, clip(c["text"], FS["base"], w - 24),
                     FS["base"], C["text"]))
    elif t == "progress":
        th = 8
        o.append('<rect x="%d" y="%.1f" width="%d" height="%d" rx="4" fill="#E7EBEF"/>'
                 % (x, y + (h - th) / 2, w, th))
        if c["text"] not in ("", "0%"):
            o.append('<rect x="%d" y="%.1f" width="%d" height="%d" rx="4" fill="%s"/>'
                     % (x, y + (h - th) / 2, w * 0.62, th, C["accent"]))
    elif t == "tab":
        active = c["id"] == "res.tab1"
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="%s" stroke="%s"/>'
                 % (x, y, w, h, C["accent_bg"] if active else "#F0F2F5",
                    C["accent"] if active else C["border"]))
        o.append(txt(c, x + w / 2, y + h / 2, c["text"], FS["small"],
                     C["accent"] if active else C["muted"], "middle"))
    elif t == "table":
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="4" fill="#FFFFFF" '
                 'stroke="%s"/>' % (x, y, w, h, C["border"]))
        hh = 22
        o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#F3F5F7"/>'
                 % (x, y, w, hh))
        # 与 bs_pair.c 的 RESULT_HEADER（12 列）一致
        cols = ["Part Number", "键类型", "差异类型", "变更项", "旧_多出位号",
                "新_多出位号", "旧_Quantity", "新_Quantity", "旧_删除线",
                "新_删除线", "重复序号", "配对来源"]
        ws = [120, 70, 62, 128, 96, 96, 82, 82, 78, 78, 70, 76]
        cx = x
        for i, name in enumerate(cols):
            if cx - x > w - 40:
                break
            o.append(txt(None, cx + 6, y + hh / 2, name, 9, C["muted"]))
            cx += ws[i % len(ws)]
        for r in range(3):
            ry = y + hh + r * 22
            if ry + 22 > y + h:
                break
            o.append('<rect x="%d" y="%d" width="%d" height="22" fill="%s"/>'
                     % (x, ry, w, "#FFF8E1"))
            o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
                     % (x, ry + 22, x + w, ry + 22, C["grid"]))
        o.append(txt(None, x + 8, y + hh + 11, "C12345", 9, C["text"]))
        o.append(txt(None, x + 128, y + hh + 11, "料号", 9, C["text"]))
        o.append(txt(None, x + 198, y + hh + 11, "变更", 9, C["text"]))
        o.append(txt(None, x + 260, y + hh + 11, "其他列", 9, C["text"]))
        # 纵向滚动条示意
        o.append('<rect x="%d" y="%d" width="6" height="%d" rx="3" fill="#E3E7EB"/>'
                 % (x + w - 12, y + hh + 6, max(10, h - hh - 12)))
    elif t == "legend":
        # 与 App::paint_results 的图例同构：色块 + 名称 + 范围 + 尾注
        cy = y + h / 2
        box = 10
        cx = x
        for name, key, scope in L.LEGEND:
            o.append('<rect x="%d" y="%.1f" width="%d" height="%d" fill="%s" '
                     'stroke="%s"/>' % (cx, cy - box / 2, box, box, C[key], C["border"]))
            cx += box + 6
            o.append(txt(None, cx, cy, name, 10.5, C["muted"]))
            cx += text_w(name, 10.5) + 6
            o.append(txt(None, cx, cy, scope, 9.5, C["hint"]))
            cx += text_w(scope, 9.5) + 18
        o.append(txt(None, cx, cy, clip(L.LEGEND_TAIL, 9.5, x + w - cx), 9.5, C["hint"]))
    elif t == "log":
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="4" fill="#FFFFFF" '
                 'stroke="%s"/>' % (x, y, w, h, C["border"]))
        for i, s in enumerate(["10:41:07  引擎：bscore 4.1.6（C 核心，零第三方依赖）",
                               "10:41:07  DPI：144（缩放 150%）｜每显示器感知 v2",
                               "10:41:08  已载入 old：demo_old.xlsx（2 个工作表）"]):
            o.append(txt(None, x + 10, y + 16 + i * 18, s, 9.5, C["muted"]))
    return "\n".join(o)


def number_badge(x, y, n, corner="lt"):
    bx = max(x - 6, 6) if corner == "lt" else x
    by = max(y - 6, 6)
    return ('<g><circle cx="%d" cy="%d" r="9" fill="%s" stroke="#FFFFFF" stroke-width="1.5"/>'
            '%s</g>' % (bx, by, C["accent"],
                        txt(None, bx, by + 0.5, str(n), 10.5, "#FFFFFF", "middle", 600)))


def build_svg(controls):
    parts = []
    vb_h = L.BASE_H + TITLEBAR_H
    parts.append('<svg viewBox="0 0 %d %d" width="100%%" role="img" '
                 'xmlns="http://www.w3.org/2000/svg" '
                 'font-family="Microsoft YaHei UI, Segoe UI, system-ui, sans-serif">'
                 % (L.BASE_W, vb_h))
    parts.append('<title>BOM 对比工具 %s 控件位置对照图</title>' % VER)
    # 窗口投影
    parts.append('<rect x="0" y="0" width="%d" height="%d" rx="8" fill="#FFFFFF" '
                 'stroke="#D6DAE0"/>' % (L.BASE_W, vb_h))
    # 标题栏（操作系统）
    parts.append('<path d="M0 8a8 8 0 0 1 8-8h%dv%dH0z" fill="#F7F8FA"/>'
                 % (L.BASE_W - 16, TITLEBAR_H))
    parts.append('<line x1="0" y1="%d" x2="%d" y2="%d" stroke="#E4E7EB"/>'
                 % (TITLEBAR_H, L.BASE_W, TITLEBAR_H))
    parts.append('<circle cx="16" cy="18" r="6" fill="#185FA5" opacity=".85"/>')
    parts.append(txt(None, 30, 18, TITLE, 11.5, C["text"], "start", 600))
    for i, g in enumerate(("–", "□", "×")):
        parts.append(txt(None, L.BASE_W - 76 + i * 26, 18, g, 12, "#5B6068", "middle"))
    parts.append('<g transform="translate(0,%d)">' % TITLEBAR_H)
    parts.append('<rect x="0" y="0" width="%d" height="%d" fill="%s"/>'
                 % (L.BASE_W, L.BASE_H, C["bg"]))
    for c in controls:
        if c["type"] == "log":          # 与结果表同区域，画了会盖住表格
            continue
        parts.append(render(c, 0))
    for c in controls:
        if c["type"] in NUM_TYPES and "_n" in c:
            parts.append(number_badge(c["x"] + (c["w"] if c["type"] == "frame" else 0),
                                      c["y"], c["_n"],
                                      "rt" if c["type"] == "frame" else "lt"))
    parts.append("</g></svg>")
    return "\n".join(parts)


def build_rows(controls):
    rows = []
    for sec, name, desc in L.SECTIONS:
        cs = [c for c in controls if c["sec"] == sec]
        rows.append('<tr class="sec"><td colspan="9"><b>%s</b>'
                    '<span class="muted">　%s　·　%d 个控件</span></td></tr>'
                    % (esc(name), esc(desc), len(cs)))
        for c in cs:
            num = ("<span class='num'>%d</span>" % c["_n"]) if "_n" in c else "—"
            rows.append(
                "<tr><td class='c'>%s</td><td><code>%s</code></td><td>%s</td>"
                "<td class='n'>%d</td><td class='n'>%d</td><td class='n'>%d</td>"
                "<td class='n'>%d</td><td>%s</td><td class='note'>%s</td></tr>"
                % (num, esc(c["id"]), esc(c["type"]), c["x"], c["y"], c["w"], c["h"],
                   esc(c["text"] or "—"), esc(c["note"] or "")))
    return "\n".join(rows)


def build_tsv(controls):
    lines = ["编号\t控件id\t类型\tx\ty\t宽\t高\t文字\t说明"]
    for c in controls:
        lines.append("\t".join(str(v) for v in (
            c.get("_n", ""), c["id"], c["type"], c["x"], c["y"], c["w"], c["h"],
            c["text"], c["note"])))
    return "\n".join(lines)


def main():
    controls = L.resolve(L.BASE_W, L.BASE_H)
    n = 0
    for c in controls:
        if c["type"] in NUM_TYPES and c["type"] != "log":
            n += 1
            c["_n"] = n
    svg = build_svg(controls)
    rows = build_rows(controls)
    tsv = build_tsv(controls)
    shot = os.path.join(ROOT, "docs", "images", "ui_main.png")
    shot_html = ""
    if os.path.exists(shot):
        b64 = base64.b64encode(open(shot, "rb").read()).decode()
        shot_html = ('<h2>1.1　实机运行截图（Per-Monitor V2）</h2>'
                     '<div class="card"><img style="width:100%%;display:block" '
                     'src="data:image/png;base64,%s" alt="实机截图"></div>'
                     '<p class="sub" style="margin-top:8px">上图为同一份代码的真实渲染'
                     '（非合成），可据此核对坐标表：卡片、选项、操作区与结果区的位置'
                     '与上图一致。</p>' % b64)

    tips = [c for c in controls if c["tip"]]
    tip_html = "\n".join(
        "<tr><td><code>%s</code></td><td>%s</td><td class='note'>%s</td></tr>"
        % (esc(c["id"]), esc(c["text"] or "—"), esc(c["tip"])) for c in tips)
    today = datetime.date.today().isoformat()

    html = Template("""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GUI 控件位置对照图 · $ver</title>
<style>
:root{--bg:#F4F6F8;--card:#fff;--line:#E1E5EA;--text:#1F2328;--muted:#6B7280;
--hint:#8A9199;--accent:#185FA5;--accent-bg:#E6F1FB}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);
font:14px/1.65 "Microsoft YaHei UI","Segoe UI",system-ui,sans-serif}
.wrap{max-width:1240px;margin:0 auto;padding:28px 24px 60px}
h1{font-size:24px;margin:0 0 6px}
h2{font-size:17px;margin:34px 0 12px;padding-left:10px;border-left:3px solid var(--accent)}
.sub{color:var(--muted);margin:0 0 18px;font-size:13px}
.warn{margin:14px 0 16px;padding:12px 16px;border:1px solid #E8C88A;border-radius:8px;
    background:#FDF6E7;color:#5A4520;font-size:13px;line-height:1.75}
.warn b{color:#8A5A0B;font-weight:600}
.warn code{font:12px ui-monospace,Consolas,monospace;background:#F3E8D2;padding:1px 5px;border-radius:3px}
.meta{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 20px}
.tag{background:var(--accent-bg);color:var(--accent);border-radius:5px;padding:3px 9px;
font-size:12px}
.tag.g{background:#EAF3EE;color:#0F6E56}
.tag.a{background:#FAEEDA;color:#87520B}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;
padding:16px;overflow:auto}
.card svg{display:block;min-width:900px}
table{border-collapse:collapse;width:100%;background:var(--card);font-size:13px}
th,td{border-bottom:1px solid var(--line);padding:7px 10px;text-align:left;vertical-align:top}
th{background:#F3F5F7;color:var(--muted);font-weight:600;font-size:12px;
position:sticky;top:0}
td.n{text-align:right;font-variant-numeric:tabular-nums;color:#37414D}
td.c{text-align:center}
td.note{color:var(--muted);font-size:12.5px}
tr.sec td{background:#F7F9FB;color:var(--text);padding:9px 10px}
code{font-family:Consolas,monospace;font-size:12px;background:#F3F5F7;
padding:1px 5px;border-radius:4px}
.num{display:inline-flex;width:20px;height:20px;border-radius:50%;background:var(--accent);
color:#fff;font-size:11px;align-items:center;justify-content:center}
.muted{color:var(--muted)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.box{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
.box h3{margin:2px 0 8px;font-size:14px}
.box p,.box li{font-size:13px;color:#37414D}
.box ol,.box ul{margin:6px 0 0 18px;padding:0}
kbd{font-family:Consolas,monospace;background:#F3F5F7;border:1px solid var(--line);
border-radius:4px;padding:0 5px;font-size:12px}
.toolbar{display:flex;gap:8px;align-items:center;margin:0 0 10px;flex-wrap:wrap}
button{border:1px solid var(--line);background:#fff;border-radius:6px;padding:6px 12px;
font:13px inherit;cursor:pointer;color:var(--text)}
button:hover{background:var(--accent-bg);border-color:var(--accent);color:var(--accent)}
textarea{width:100%;height:150px;white-space:pre;overflow:auto;font:12px Consolas,monospace;border:1px solid var(--line);
border-radius:8px;padding:10px;color:#37414D;background:#FBFCFD}
details{margin:10px 0 0}
summary{cursor:pointer;color:var(--accent);font-size:13px}
footer{margin-top:40px;color:var(--hint);font-size:12px;border-top:1px solid var(--line);
padding-top:14px}
@media print{body{background:#fff}.wrap{padding:0}.card,table{break-inside:auto}
button,.toolbar{display:none}}
</style></head><body><div class="wrap">

<h1>BOM 对比工具 $ver · GUI 控件位置对照图</h1>
<div class="warn"><b>V4 起界面由 C++17 + Win32 自绘实现</b>，编译为单一 <code>BOMCheck_$ver.exe</code>（静态链接、零第三方运行库）。<br>
<b>坐标仍然只有一份源头</b>：<code>pyref/layout_spec.py</code> → <code>tools/gen_layout_c.py</code> → <code>src/ui/layout_gen.h</code>（程序摆控件用）；<code>BOMCheck --export-layout-doc</code> 出 Markdown 表，本脚本出这张视觉图 —— 三者同源，永远不会出现「图上的坐标和程序对不上」。<br>
几何在运行时按 <code>MulDiv(逻辑值, dpi, 96)</code> 换算，字体用 <code>CreateFontW(-MulDiv(pt, dpi, 72))</code>，同一基准不二次放大。</div>
<p class="sub">坐标系为逻辑像素（96 DPI 基准），原点在客户区左上角；实际像素 = 逻辑值 × (当前 DPI / 96)。
图下方标注的编号对应第 2 节表格。</p>
<div class="meta">
  <span class="tag">客户区 $bw × $bh</span>
  <span class="tag">控件 $total 个（编号 $num 个）</span>
  <span class="tag g">DPI 感知：Per-Monitor V2</span>
  <span class="tag a">生成于 $date</span>
</div>

<h2>1　整体对照图</h2>
<div class="toolbar">
  <button onclick="document.getElementById('svgbox').requestFullscreen()">全屏查看</button>
  <span class="muted">圆圈编号对应下方表格；灰色标题栏为操作系统标题栏（含版本号）。</span>
</div>
<div class="card" id="svgbox">$svg</div>
$shot

<h2>2　控件清单与坐标</h2>
<div class="toolbar">
  <button onclick="copyTsv(this)">复制为 TSV（可粘贴到 Excel）</button>
  <span class="muted">x/y 为左上角，w/h 为宽高，单位：逻辑像素。</span>
</div>
<table>
<thead><tr><th style="width:56px">编号</th><th style="width:120px">控件 id</th>
<th style="width:86px">类型</th><th style="width:62px">x</th><th style="width:62px">y</th>
<th style="width:62px">宽</th><th style="width:62px">高</th><th style="width:200px">文字</th>
<th>说明</th></tr></thead>
<tbody>
$rows
</tbody></table>

<h2>3　DPI 与缩放：为什么不会发虚</h2>
<div class="grid2">
  <div class="box">
    <h3>发虚的根因</h3>
    <p>进程若未声明 DPI 感知，Windows 会把整个窗口按位图拉伸到目标缩放比例：
    100%→150% 时每个像素被放大 1.5 倍再插值，文字边缘必然模糊。这与字体大小、
    控件尺寸都无关，<b>是渲染管线的选择</b>。</p>
    <p>声明之后，窗口按物理像素绘制，字形由 GDI 以目标 DPI 重新栅格化，
    所以 150%、200% 下依然清晰。</p>
  </div>
  <div class="box">
    <h3>本程序的三条措施</h3>
    <ol>
      <li><b>创建窗口之前</b>调 <code>SetProcessDpiAwarenessContext(-4)</code>
      （Per-Monitor V2），失败则退回 <code>SetProcessDpiAwareness(2)</code> /
      <code>SetProcessDPIAware()</code>。</li>
      <li>几何一律按 <code>逻辑值 × scale</code> 计算，<code>scale = dpi / 96</code>（<code>src/ui/dpi.cpp</code>）；
      字体用<b>点</b>字号 <code>CreateFontW(-MulDiv(pt, dpi, 72))</code>，两者同一基准，不会二次放大。</li>
      <li>运行中每 1.5 秒比对实际 DPI，跨到不同缩放的显示器时按比例调整窗口并整体重排
      （布局表里所有 x/y/w/h 都是逻辑值，缩放只影响 <code>px()</code> 的换算）。</li>
    </ol>
  </div>
  <div class="box">
    <h3>实测环境</h3>
    <p>标题区右侧实时显示 <code>DPI 144 · 缩放 150% · 每显示器感知</code>；若感知声明失败，
    该处会变成告警色，便于现场排查。</p>
    <p>1080p @ 150% 的逻辑高度只有 720，因此初始窗口按屏幕逻辑尺寸收缩，
    纵向布局压在 720 以内，最小高度 620（<code>layout_spec.py</code> 的 <code>MIN_W/MIN_H</code>）。</p>
  </div>
  <div class="box">
    <h3>打包注意</h3>
    <ul>
      <li>manifest 里声明 <code>&lt;dpiAwareness&gt;PerMonitorV2&lt;/dpiAwareness&gt;</code>
      （<code>src/res/app.manifest.in</code>），代码里的运行时声明作为兜底。</li>
      <li>声明 <code>asInvoker</code>：以管理员身份运行时，UIPI 会拦截资源管理器的拖放，
      表现为“拖了没反应”。</li>
      <li>界面全部为 GDI 矢量绘制（圆角矩形 + DrawTextW），位图只用于图标，高 DPI 下不拉伸。</li>
    </ul>
  </div>
</div>

<h2>4　控件上的提示文字（悬停 420 ms 出现）</h2>
<table><thead><tr><th style="width:140px">控件</th><th style="width:200px">显示文字</th>
<th>提示内容</th></tr></thead><tbody>
$tips
</tbody></table>

<h2>5　坐标表（TSV）</h2>
<textarea id="tsv" readonly>$tsv</textarea>

<footer>本文件由 <code>tools/make_layout_doc.py</code> 生成，请勿手工修改坐标；
改布局请改 <code>pyref/layout_spec.py</code> 后重跑本脚本（构建链第 0 步会同步 C 头文件）。</footer>
</div>
<script>
function copyTsv(btn){
  var t=document.getElementById('tsv');
  t.removeAttribute('readonly'); t.focus(); t.select(); document.execCommand('copy');
  t.setAttribute('readonly','readonly');
  var old=btn.textContent; btn.textContent='已复制'; setTimeout(function(){btn.textContent=old;},1200);
}
</script>
</body></html>""").substitute(dict(
        svg=svg, rows=rows, tips=tip_html, tsv=esc(tsv), shot=shot_html,
        ver=VER, bw=L.BASE_W, bh=L.BASE_H,
        total=len(controls), num=n, date=today,
    ))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(html)
    print("written:", OUT)
    print("version: %s | controls: %d (numbered %d) | svg bytes: %d"
          % (VER, len(controls), n, len(svg)))


if __name__ == "__main__":
    main()
