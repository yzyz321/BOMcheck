# -*- coding: utf-8 -*-
"""BOM 对比引擎 —— Python 参考实现（回归对账的 oracle）。

生产版是 src/core 下的 C 实现（libbscore）；这份 Python 版与它接口、口径
逐项一致，tools/check_pair / check_sheet / check_mark / check_write 拿两者
对同一批样本跑出的结果逐格比对，谁改了口径跑一遍就现形。改这里的语义
必须同时改 C 侧，反之亦然。

接口（与 src/core 的导出对齐）：
    list_sheets(path)       -> [(页名, zip 内路径)]
    load_sheet(path, name, cfg) -> SheetData
    pair(old, new, cfg)     -> PairResult
    build_rows(...)         -> 12 列结果
    export_xlsx(rows, diag, path)

解析走 zipfile + ElementTree，与 C 侧同一路径；openpyxl 只用于导出。
"""
import os
import re
import zipfile
import xml.etree.ElementTree as ET

NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
NS_R = "{http://schemas.openxmlformats.org/officeDocument/2006/relationships}"

# ---------------- 规范与别名 ----------------
PAT_WIDE = re.compile(r"^[A-Za-z0-9]{3}-[A-Za-z0-9]{6}-[A-Za-z0-9]{3}$")
PAT_STRICT = re.compile(r"^\d{3}-\d{6}-[A-Za-z0-9]{3}$")
REF_TOKEN = re.compile(r"^[A-Z]{1,3}\d{1,5}[A-Z0-9]{0,2}$")
REF_SPLIT = re.compile(r"[,;、，；\s/\\|]+")
DASHES = "\u2010\u2011\u2012\u2013\u2014\u2015\u2212\uff0d"

ALIAS_PN = ["partnumber", "partno", "pn", "料号", "物料编码", "物料号", "物料编号",
            "物料代码", "零件号", "产品料号", "子件代码", "子件料号", "元件编码"]
ALIAS_REF = ["reference", "ref", "位号", "元件位号", "位置号", "designator",
             "referencedesignator", "子件位号"]
ALIAS_QTY = ["quantity", "qty", "数量", "用量", "单机用量", "子件需求数量",
             "需求数量", "单机数量", "子件数量"]

RESULT_COLS = ["Part Number", "键类型", "差异类型", "变更项",
               "旧_多出位号", "新_多出位号", "旧_Quantity", "新_Quantity",
               "旧_删除线", "新_删除线", "重复序号", "配对来源"]

# 查重模式的列（同一个文件时不比差异，按 Part Number 数重复行）
DUP_COLS = ["Part Number", "重复次数", "查重结果", "位号", "数量",
            "删除线", "行号", "工作表"]


class BomError(Exception):
    """带用户可读中文说明的异常。"""


class OutDirError(BomError):
    """默认输出目录不可写（只读盘、无权限等），调用方应改让用户指定目录。"""

    def __init__(self, msg, path):
        BomError.__init__(self, msg)
        self.path = path


# 四色标注：与 layout_spec.COLORS 的 row_* 同值（界面 / 导出 / 标注三处同色）
MARK_COLORS = {"add": "C6EFCE", "del": "FFC7CE", "chg": "FFEB9C", "dup": "D9D2E9"}


# ---------------- 归一化 ----------------
def norm(s):
    """全角转半角 + 去空白 + 小写，用于列名比对。"""
    if s is None:
        return ""
    out = []
    for ch in str(s):
        o = ord(ch)
        if o == 0x3000:
            out.append(" ")
        elif 0xFF01 <= o <= 0xFF5E:
            out.append(chr(o - 0xFEE0))
        else:
            out.append(ch)
    return re.sub(r"\s+", "", "".join(out)).lower()


def pn_key(v):
    """料号归一化，返回 (key, 宽松命中, 严格命中)。"""
    if v is None:
        return "", False, False
    s = str(v).strip()
    for d in DASHES:
        s = s.replace(d, "-")
    s = re.sub(r"\s+", "", s).upper()
    if s.endswith(".0") and s[:-2].isdigit():   # pandas 数值型遗留形态
        s = s[:-2]
    return s, bool(PAT_WIDE.match(s)), bool(PAT_STRICT.match(s))


def ref_tokens(v):
    """拆分位号并做形态校验，返回 (合法 token, 被拒数量)。"""
    if v is None:
        return [], 0
    parts = [p for p in REF_SPLIT.split(str(v).upper()) if p]
    ok = [p for p in parts if REF_TOKEN.match(p)]
    return ok, len(parts) - len(ok)


def ref_sig(v):
    """位号集合签名：大写 → 去重 → 排序 → '|' 连接（顺序无关）。"""
    ok, _bad = ref_tokens(v)
    return "|".join(sorted(set(ok)))


def make_key(pn, ref):
    k, _w, _s = pn_key(pn)
    if k:
        return "P", "P:" + k
    s = ref_sig(ref)
    if s:
        return "R", "R:" + s
    return "", ""


# ---------------- xlsx 底层 ----------------
def _col_idx(letters):
    n = 0
    for ch in letters:
        n = n * 26 + (ord(ch) - 64)
    return n


def _col_name(i):
    s = ""
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s


def _load_sst(z):
    try:
        root = ET.fromstring(z.read("xl/sharedStrings.xml"))
    except KeyError:
        return []
    return ["".join(t.text or "" for t in si.iter(NS + "t"))
            for si in root.findall(NS + "si")]


def _load_styles(z):
    """返回 (fonts_strike[], cellXfs_fontId[])。对应 C 侧 xl_styles.c。"""
    fonts, xfs = [], []
    try:
        root = ET.fromstring(z.read("xl/styles.xml"))
    except KeyError:
        return fonts, xfs
    fe = root.find(NS + "fonts")
    if fe is not None:
        for f in fe.findall(NS + "font"):
            st = f.find(NS + "strike")
            on = False
            if st is not None:
                v = st.get("val")
                on = (v is None or v in ("1", "true"))
            fonts.append(on)
    cx = root.find(NS + "cellXfs")
    if cx is not None:
        for xf in cx.findall(NS + "xf"):
            xfs.append(int(xf.get("fontId", "0")))
    return fonts, xfs


def list_sheets(path):
    """只读 workbook.xml，返回 [(页名, zip 内路径)]。"""
    try:
        with zipfile.ZipFile(path) as z:
            wb = ET.fromstring(z.read("xl/workbook.xml"))
            rels = ET.fromstring(z.read("xl/_rels/workbook.xml.rels"))
            rmap = {r.get("Id"): r.get("Target") for r in rels}
            out = []
            for sh in wb.find(NS + "sheets"):
                tgt = rmap.get(sh.get(NS_R + "id"), "")
                tgt = tgt.lstrip("/") if tgt.startswith("/") else "xl/" + tgt.lstrip("./")
                out.append((sh.get("name"), tgt))
            return out
    except zipfile.BadZipFile:
        raise BomError("不是有效的 .xlsx 文件（可能被占用或已损坏）")
    except KeyError as e:
        raise BomError("缺少 xlsx 必需部件：%s" % e)


class _Grid:
    """一张工作表的单元格网格：{行: {列: (值, 删除线)}}。"""

    def __init__(self, z, part, sst, fonts, xfs):
        root = ET.fromstring(z.read(part))
        self.cells, self.maxr, self.maxc = {}, 0, 0
        data = root.find(NS + "sheetData")
        if data is None:
            raise BomError("工作表为空（没有 sheetData）")
        for r in data.findall(NS + "row"):
            rn = int(r.get("r"))
            d = {}
            for c in r.findall(NS + "c"):
                m = re.match(r"([A-Z]+)(\d+)", c.get("r", ""))
                if not m:
                    continue
                ci = _col_idx(m.group(1))
                t = c.get("t")
                v = c.find(NS + "v")
                if t == "s" and v is not None:
                    val = sst[int(v.text)]
                elif t == "inlineStr":
                    val = "".join(x.text or "" for x in c.iter(NS + "t"))
                elif v is not None:
                    val = v.text
                else:
                    val = None
                s = c.get("s")
                st = False
                if s is not None:
                    try:
                        fi = xfs[int(s)]
                        st = fonts[fi] if fi < len(fonts) else False
                    except (IndexError, ValueError):
                        st = False
                d[ci] = (val, st)
                self.maxc = max(self.maxc, ci)
            self.cells[rn] = d
            self.maxr = max(self.maxr, rn)
        self.merges = []
        mc = root.find(NS + "mergeCells")
        if mc is not None:
            for m in mc.findall(NS + "mergeCell"):
                a, b = (m.get("ref") or "").split(":")
                ma, mb = re.match(r"([A-Z]+)(\d+)", a), re.match(r"([A-Z]+)(\d+)", b)
                if ma and mb:
                    self.merges.append((int(ma.group(2)), _col_idx(ma.group(1)),
                                        int(mb.group(2)), _col_idx(mb.group(1))))
        # 合并单元格：把 span 头的值铺到整个合并区（与 C 侧 bs_xlsx.c 的
        # expand_merges 同口径）。Excel 只在左上角存值，不铺的话竖向合并的
        # 料号格后面几行会读成空，整行被当成「无料号」。
        # 护栏与 C 侧同值：单区 >4096 格直接跳过，整表最多新增 20 万格。
        added = 0
        for (r0, c0, r1, c1) in self.merges:
            if added >= 200000:
                break
            if (r1 - r0 + 1) * (c1 - c0 + 1) <= 1 or \
                    (r1 - r0 + 1) * (c1 - c0 + 1) > 4096:
                continue
            head = self.cells.get(r0, {}).get(c0)
            if not head or head[0] in (None, ""):
                continue
            for r in range(r0, r1 + 1):
                d = self.cells.setdefault(r, {})
                for c in range(c0, c1 + 1):
                    if added >= 200000:
                        break
                    if r == r0 and c == c0:
                        continue
                    if c not in d:
                        d[c] = head
                        added += 1
                if r > self.maxr:
                    self.maxr = r
            if c1 > self.maxc:
                self.maxc = c1
        # 整行合并的分组标签行（跨度 >= 3 列）
        self.label_rows = {r0 for (r0, c0, r1, c1) in self.merges
                           if r1 == r0 and c1 - c0 >= 3}

    def row(self, r):
        return self.cells.get(r, {})

    def has_content(self, r):
        return any(v[0] not in (None, "") for v in self.row(r).values())


# ---------------- 识别 ----------------
def _row_score(grid, r, min_alias=1):
    """表头候选打分；n_alias==0 一律不作候选（否则数据行也拿 7 分）。"""
    row = grid.row(r)
    if not row:
        return -99, 0
    cells = [v for (v, _s) in row.values() if v not in (None, "")]
    if not cells:
        return -99, 0
    hit = {"pn": 0, "ref": 0, "qty": 0}
    for (v, _s) in row.values():
        k = norm(v)
        if not k:
            continue
        for name, al in (("pn", ALIAS_PN), ("ref", ALIAS_REF), ("qty", ALIAS_QTY)):
            if k in al:
                hit[name] = 1
                break
    n_alias = sum(hit.values())
    score = {0: 0, 1: 2, 2: 4, 3: 6}[n_alias]
    nxt = grid.row(r + 1)
    if any(pn_key(v)[1] for (v, _s) in nxt.values()):
        score += 4
    if len(cells) >= 3:
        score += 1
    nxt_cells = [v for (v, _s) in nxt.values() if v not in (None, "")]
    if nxt_cells and len(nxt_cells) >= 0.7 * len(cells):
        score += 2
    if sum(1 for v in cells if len(str(v)) > 40) / max(1, len(cells)) > 0.4:
        score -= 3
    return score, n_alias


def _fallback_blocks(grid):
    """无列名命中时：靠「下一行连续多行符合料号格式」倒推表头。"""
    out = []
    for r in sorted(grid.cells):
        row = grid.row(r)
        cells = [v for (v, _s) in row.values() if v not in (None, "")]
        if len(cells) < 3 or any(pn_key(v)[1] for v in cells):
            continue
        hits = sum(1 for k in range(1, 6)
                   if any(pn_key(v)[1] for (v, _s) in grid.row(r + k).values()))
        if hits >= 4:
            out.append(r)
    return out


def _header_blocks(grid, min_alias=1, threshold=5):
    cands = []
    for r in sorted(grid.cells):
        sc, na = _row_score(grid, r)
        if na >= min_alias and sc >= threshold:
            cands.append(r)
    return cands or _fallback_blocks(grid)


def _find_pn_column(grid, hr, end, cfg):
    """双通道：列名别名 + 内容格式命中率。"""
    hrow = grid.row(hr)
    cols = sorted({c for r in grid.cells for c in grid.cells[r]})
    sample = int(cfg.get("sample_rows", 40))
    best, info = None, {}
    for c in cols:
        name = norm(hrow.get(c, (None, False))[0])
        by_alias = name in ALIAS_PN
        tot = hit = 0
        for r in range(hr + 1, min(end, hr + sample) + 1):
            v = grid.row(r).get(c, (None, False))[0]
            if v is None or str(v).strip() == "":
                continue
            key, okw, _s = pn_key(v)
            if not key:
                continue
            tot += 1
            hit += 1 if okw else 0
        rate = hit / tot if tot else 0.0
        info[c] = {"name": name, "alias": by_alias, "n": tot, "rate": rate}
        if rate >= float(cfg.get("min_hit_rate", 0.60)):
            rank = (1 if by_alias else 0, rate, -c)
            if best is None or rank > best[0]:
                best = (rank, c)
    return (best[1] if best else None), info


def _locate(grid, hr, aliases):
    for c, (v, _s) in sorted(grid.row(hr).items()):
        if norm(v) in aliases:
            return c
    return None


def _guess_ref_column(grid, hr, end, exclude):
    """无位号列名时，按位号形态命中率推断（≥50% 才认）。"""
    hrow = grid.row(hr)
    best, info = None, {}
    cols = sorted({c for r in grid.cells for c in grid.cells[r]})
    for c in cols:
        if c == exclude:
            continue
        name = norm(hrow.get(c, (None, False))[0])
        if name in ALIAS_QTY or name in ALIAS_PN:
            continue
        tot = ok = 0
        for r in range(hr + 1, min(end, hr + 40) + 1):
            v = grid.row(r).get(c, (None, False))[0]
            if v is None or str(v).strip() == "":
                continue
            tot += 1
            toks, bad = ref_tokens(v)
            if toks and not bad:
                ok += 1
        if tot >= 3 and ok / tot >= 0.5:
            info[c] = ok / tot
            if best is None or ok / tot > info.get(best, 0):
                best = c
    return best, info


# ---------------- 行与页 ----------------
class Row:
    __slots__ = ("r", "pn", "ref", "refsig", "qty", "strike", "kind", "key",
                 "dup_i", "dup_n", "reason", "block", "c1", "c2")

    def __init__(self, r, pn, ref, qty, strike, block=1):
        self.r = r
        self.c1 = self.c2 = 0          # 该行实际有内容的列范围（标注整行用）
        self.pn = pn_key(pn)[0]
        self.ref = str(ref) if ref not in (None, "") else ""
        self.refsig = ref_sig(ref)
        self.kind, self.key = make_key(pn, ref)
        q = str(qty).strip() if qty not in (None, "") else ""
        try:
            self.qty = float(q)
        except ValueError:
            self.qty = None
        self.strike = 1 if strike else 0
        self.dup_i = self.dup_n = 1
        self.block = block
        if self.key:
            self.reason = ""
        else:
            self.reason = "位号无合法 token" if self.ref else "料号与位号均为空"

    @property
    def strike_text(self):
        return "有" if self.strike else "无"

    @property
    def qty_text(self):
        if self.qty is None:
            return ""
        return str(int(self.qty)) if abs(self.qty - int(self.qty)) < 1e-9 else str(self.qty)


class SheetData:
    def __init__(self, name):
        self.name = name
        self.rows = []
        self.unkeyable = []
        self.labels = 0
        self.blocks = []      # [(表头行, 起始, 结束, pn, ref, qty)]
        self.cols = {}
        self.hit_rate = 0.0


def load_sheet(path, sheet_name, cfg, progress=None):
    if progress:
        progress(5, "载入 %s / %s" % (os.path.basename(path), sheet_name))
    with zipfile.ZipFile(path) as z:
        sst = _load_sst(z)
        fonts, xfs = _load_styles(z)
        part = dict(list_sheets(path)).get(sheet_name)
        if not part:
            raise BomError("找不到工作表「%s」" % sheet_name)
        grid = _Grid(z, part, sst, fonts, xfs)

    blocks = _header_blocks(grid, int(cfg.get("header_min_alias", 1)),
                            int(cfg.get("header_threshold", 5)))
    if not blocks:
        raise BomError("未识别到表头行（前 50 行内没有列名与料号格式的双证据）")

    sd = SheetData(sheet_name)
    for bi, hr in enumerate(blocks, 1):
        end = blocks[bi] - 1 if bi < len(blocks) else grid.maxr
        col_pn, info = _find_pn_column(grid, hr, end, cfg)
        col_ref = _locate(grid, hr, ALIAS_REF)
        if col_ref is None:
            # 与 C 版一致：位号列猜测与双键无关（识别 vs 配对是两件事）
            col_ref, _ri = _guess_ref_column(grid, hr, end, col_pn)
        col_qty = _locate(grid, hr, ALIAS_QTY)
        sd.blocks.append((hr, hr + 1, end, col_pn, col_ref, col_qty))
        if col_pn is None:
            continue
        sd.hit_rate = max(sd.hit_rate, info.get(col_pn, {}).get("rate", 0.0))
        for r in range(hr + 1, end + 1):
            if not grid.has_content(r):
                continue
            if r in grid.label_rows:
                sd.labels += 1
                continue
            row = grid.row(r)
            pn = row.get(col_pn, (None, False))[0]
            ref = row.get(col_ref, (None, False))[0] if col_ref else None
            qty = row.get(col_qty, (None, False))[0] if col_qty else None
            if cfg.get("strike_scope") == "KEY":
                strike = row.get(col_pn, (None, False))[1]
                if col_ref:
                    strike = strike or row.get(col_ref, (None, False))[1]
                if col_qty:
                    strike = strike or row.get(col_qty, (None, False))[1]
            else:
                strike = any(v[1] for v in row.values())   # strike_scope = ALL
            rec = Row(r, pn, ref, qty, strike, bi)
            cols = [k for k, v in row.items() if v[0] not in (None, "")]
            if cols:
                rec.c1, rec.c2 = min(cols), max(cols)
            if not rec.key:
                sd.unkeyable.append(rec)
                continue
            sd.rows.append(rec)

    # 重复分组（按实际使用的键）
    groups = {}
    for rec in sd.rows:
        groups.setdefault(rec.key, []).append(rec)
    for _k, g in groups.items():
        for i, rec in enumerate(g, 1):
            rec.dup_i, rec.dup_n = i, len(g)
    sd.groups = {k: v for k, v in groups.items() if len(v) > 1}
    sd.cols = {"pn": _col_name(next((b[3] for b in sd.blocks if b[3]), 0)),
               "ref": _col_name(next((b[4] for b in sd.blocks if b[4]), 0)),
               "qty": _col_name(next((b[5] for b in sd.blocks if b[5]), 0))}
    if progress:
        progress(30, "已解析 %d 行" % len(sd.rows))
    return sd


# ---------------- 配对 ----------------
# 近似阶段的护栏（与 C 侧 bs_pair.c 同值）：超限就跳过近似，交给增删
FUZZY_MAX_PAIRS = 50000000
FUZZY_MAX_CAND = 200000


def _jaccard(a, b):
    sa = set(a.split("|")) if a else set()
    sb = set(b.split("|")) if b else set()
    return len(sa & sb) / len(sa | sb) if sa and sb else 0.0


def pair(old, new, cfg):
    """四阶段配对：A 料号精确 → B 位号精确 → C 位号近似 → D 增删。"""
    dup_groups = [r.key for r in old + new if r.dup_n > 1]
    if cfg.get("dup_policy") == "STRICT" and dup_groups:
        sample = sorted(set(dup_groups))[:3]
        raise BomError("严格模式下检出重复键（%s…），请先清理源表" % "、".join(sample))
    stats = {"pn": 0, "ref": 0, "fuzzy": 0, "pnchg": 0, "cartesian_fallback": 0}
    skipped = 0
    if not cfg.get("dual_key", 1):
        # 与 C 版一致：双键关 → 无料号的行没有键也没有兜底，整行跳过，
        # 不能当新增/删除（否则同一份文件比自己也会冒出一堆差异）
        skipped = sum(1 for r in old if r.kind == "R") + sum(1 for r in new if r.kind == "R")
        old = [r for r in old if r.kind != "R"]
        new = [r for r in new if r.kind != "R"]
    stats["skipped"] = skipped
    if cfg.get("dup_policy") == "CARTESIAN":
        stats["cartesian_fallback"] = 1       # 参考引擎未实现全组合，按顺序配对处理
    used_o, used_n = set(), set()
    pairs = []

    def take(o_list, n_list):
        for i in range(min(len(o_list), len(n_list))):
            pairs.append((o_list[i], n_list[i]))
            used_o.add(id(o_list[i]))
            used_n.add(id(n_list[i]))

    idx = {}
    for rec in new:
        if rec.kind == "P":
            idx.setdefault(rec.key, []).append(rec)
    oidx = {}
    for rec in old:
        if rec.kind == "P":
            oidx.setdefault(rec.key, []).append(rec)
    for key, o_list in oidx.items():
        n_list = idx.get(key, [])
        take(o_list, n_list)
        stats["pn"] += min(len(o_list), len(n_list))

    if cfg.get("dual_key", 1):
        ro = [x for x in old if id(x) not in used_o and x.kind == "R"]
        rn = [x for x in new if id(x) not in used_n and x.kind == "R"]
        i2 = {}
        for rec in rn:
            i2.setdefault(rec.key, []).append(rec)
        o2 = {}
        for rec in ro:
            o2.setdefault(rec.key, []).append(rec)
        for key, o_list in o2.items():
            n_list = i2.get(key, [])
            take(o_list, n_list)
            stats["ref"] += min(len(o_list), len(n_list))

        # 阶段 C：位号近似，且至少一侧无料号
        if cfg.get("ref_fuzzy", 1):
            ro = [x for x in old if id(x) not in used_o]
            rn = [x for x in new if id(x) not in used_n]
            thr = float(cfg.get("ref_min_overlap", 0.6))
            cand = []
            if len(ro) * len(rn) > FUZZY_MAX_PAIRS:
                ro, rn = [], []          # 降级：不逐对比，剩下按增/删
            for a in ro:
                if not a.refsig or len(cand) >= FUZZY_MAX_CAND:
                    break
                for b in rn:
                    if len(cand) >= FUZZY_MAX_CAND:
                        break
                    if not b.refsig or (a.kind == "P" and b.kind == "P"):
                        continue
                    j = _jaccard(a.refsig, b.refsig)
                    if j >= thr:
                        cand.append((j, a.r, b.r, a, b))
            for j, _ra, _rb, a, b in sorted(cand, key=lambda t: (-t[0], t[1], t[2])):
                if id(a) in used_o or id(b) in used_n:
                    continue
                pairs.append((a, b))
                used_o.add(id(a))
                used_n.add(id(b))
                if a.kind != b.kind:
                    stats["pnchg"] += 1
                else:
                    stats["fuzzy"] += 1
    else:
        # 不做双键时，料号为空的行直接视为未配对（V2 行为）
        pass

    add = [x for x in new if id(x) not in used_n]
    dele = [x for x in old if id(x) not in used_o]
    # 配对结果按旧行号排序，便于对照原表
    pairs.sort(key=lambda p: (p[0].r, p[1].r))
    return pairs, add, dele, stats


def _key_type_label(a, b):
    if b.kind == "P" and a.kind == "P":
        return "料号"
    if a.kind == "P" and b.kind == "P":
        return "料号"
    if a.kind != b.kind:
        return "位号(料号变更)"
    if a.refsig != b.refsig:
        return "位号(近似)"
    return "位号"


def _ref_delta(a_sig, b_sig):
    sa = set(a_sig.split("|")) if a_sig else set()
    sb = set(b_sig.split("|")) if b_sig else set()
    return ",".join(sorted(sa - sb)), ",".join(sorted(sb - sa))


def build_result(pairs, add, dele):
    out = []
    for a, b in pairs:
        why = []
        r_o, r_n = _ref_delta(a.refsig, b.refsig)
        if a.refsig != b.refsig:
            why.append("位号")
        if a.qty != b.qty:
            why.append("数量")
        if a.pn != b.pn:
            why.append("料号")
        if a.strike != b.strike:
            why.append("删除线(划除)" if b.strike else "删除线(恢复)")
        if not why:
            continue
        dup = ""
        src = ""
        if a.dup_n > 1 or b.dup_n > 1:
            dup = "%d/%d" % (b.dup_i, max(a.dup_n, b.dup_n))
            src = "旧#%d ↔ 新#%d（r%d / r%d）" % (a.dup_i, b.dup_i, a.r, b.r)
        out.append([b.pn or a.pn or "-", _key_type_label(a, b), "变更",
                    "+".join(why), r_o, r_n, a.qty_text, b.qty_text,
                    a.strike_text, b.strike_text, dup, src])
    for b in add:
        out.append([b.pn or "-", "料号" if b.kind == "P" else "位号", "新增",
                    "", "", ",".join(b.refsig.split("|")) if b.refsig else "",
                    "", b.qty_text, "", b.strike_text, "", ""])
    for a in dele:
        out.append([a.pn or "-", "料号" if a.kind == "P" else "位号", "删除",
                    "", ",".join(a.refsig.split("|")) if a.refsig else "", "",
                    a.qty_text, "", a.strike_text, "", "", ""])
    order = {"变更": 0, "新增": 1, "删除": 2}
    out.sort(key=lambda r: (order.get(r[2], 9), r[0]))
    return out


# ---------------- 标注计划与三份输出 ----------------
def _block_cols(sd, rec):
    """rec 所在表头块的 (料号列, 位号列, 数量列)，列号 1 基。"""
    try:
        _hr, _s, _e, pn, ref, qty = sd.blocks[rec.block - 1]
    except (IndexError, AttributeError, TypeError):
        return None, None, None
    return pn, ref, qty


def mark_plan(pairs, add, dele, so, sn):
    """生成两侧要涂的格：{"old": [(行, 列, 颜色键, 批注)], "new": [...]}。

    口径与文档 2.5 一致：
      新增——新表整行绿；删除——旧表整行红；变更——两侧各自变化的列黄；
      重复料号——仅料号格浅紫 + 批注 n/N。旧表不标新增，新表不标删除。
    """
    plan = {"old": [], "new": []}

    def put(side, r, c, key, note=None):
        if r and c:
            plan[side].append((r, c, key, note))

    def whole(side, rec, key):
        c1 = rec.c1 or rec.c2 or 1
        c2 = rec.c2 or c1
        for c in range(c1, c2 + 1):
            put(side, rec.r, c, key)

    for a, b in pairs:
        po, ro, qo = _block_cols(so, a)
        pn_, rn, qn = _block_cols(sn, b)
        if a.pn != b.pn:
            put("old", a.r, po, "chg")
            put("new", b.r, pn_, "chg")
        if a.qty != b.qty:
            put("old", a.r, qo, "chg")
            put("new", b.r, qn, "chg")
        if a.refsig != b.refsig:
            put("old", a.r, ro, "chg")
            put("new", b.r, rn, "chg")
        if a.strike != b.strike:
            # 删除线本身没有独立列，标在键所在的列（料号优先，无料号用位号）
            put("old", a.r, po or ro, "chg")
            put("new", b.r, pn_ or rn, "chg")
    for rec in dele:
        whole("old", rec, "del")
    for rec in add:
        whole("new", rec, "add")
    for side, sd in (("old", so), ("new", sn)):
        for rec in sd.rows:
            if rec.dup_n > 1:
                put(side, rec.r, _block_cols(sd, rec)[0], "dup",
                    "重复料号 %d/%d（本页共 %d 处）" % (rec.dup_i, rec.dup_n, rec.dup_n))
    return plan


def default_outdir(old_path, override=None):
    """默认写到旧 BOM 同级的 checkresults/；override 优先。"""
    if override:
        return override
    return os.path.join(os.path.dirname(os.path.abspath(old_path)), "checkresults")


def _ensure_dir(d):
    try:
        os.makedirs(d, exist_ok=True)
        probe = os.path.join(d, ".bomcheck_probe_%d" % os.getpid())
        with open(probe, "wb"):
            pass
        os.remove(probe)
        return True
    except OSError:
        return False


def _save_any(wb, path):
    """保存；被 Excel 占用时自动改名 _2/_3…，返回实际路径。"""
    try:
        wb.save(path)
        return path
    except PermissionError:
        stem, ext = os.path.splitext(path)
        for i in range(2, 20):
            cand = "%s_%d%s" % (stem, i, ext)
            try:
                wb.save(cand)
                return cand
            except PermissionError:
                continue
        raise BomError("目标文件正被占用，且无法另存：\n%s" % path)


def mark_workbook(src, sheet, marks, dst):
    """复制源工作簿并在副本上涂色（源文件只读，绝不改写）。"""
    try:
        from openpyxl import load_workbook
        from openpyxl.comments import Comment
        from openpyxl.styles import PatternFill
    except ImportError:
        raise BomError("缺少 openpyxl，无法生成标注文件（pip install openpyxl）")

    keep_vba = src.lower().endswith(".xlsm")
    wb = load_workbook(src, keep_vba=keep_vba, data_only=False)
    if sheet not in wb.sheetnames:
        wb.close()
        raise BomError("标注失败：工作簿里找不到工作表「%s」" % sheet)
    ws = wb[sheet]
    fills = {k: PatternFill("solid", fgColor=v) for k, v in MARK_COLORS.items()}
    for r, c, key, note in marks:
        cell = ws.cell(row=r, column=c)
        cell.fill = fills[key]
        if note:
            cell.comment = Comment(note, "BOMCheck")
    out = _save_any(wb, dst)
    wb.close()
    return out


def write_outputs(rows, diag, plan, old_path, old_sheet, new_path, new_sheet,
                  outdir=None):
    """一次写三份：差异结果 + 旧/新两份标注。

    返回 {"dir":…, "result":…, "old":…, "new":…}；目录不可写抛 OutDirError。
    """
    d = default_outdir(old_path, outdir)
    if not _ensure_dir(d):
        raise OutDirError("输出目录不可写（只读或权限不足）：\n%s" % d, d)
    ob = os.path.splitext(os.path.basename(old_path))[0]
    nb = os.path.splitext(os.path.basename(new_path))[0]
    ext_o = ".xlsm" if old_path.lower().endswith(".xlsm") else ".xlsx"
    ext_n = ".xlsm" if new_path.lower().endswith(".xlsm") else ".xlsx"
    p_res = os.path.join(d, "%s_vs_%s_差异结果.xlsx" % (ob, nb))
    p_old = os.path.join(d, "%s_差异标注_旧%s" % (ob, ext_o))
    p_new = os.path.join(d, "%s_差异标注_新%s" % (nb, ext_n))

    p_res = export_xlsx(rows, diag, p_res)
    p_old = mark_workbook(old_path, old_sheet, plan["old"], p_old)
    p_new = mark_workbook(new_path, new_sheet, plan["new"], p_new)
    return {"dir": d, "result": p_res, "old": p_old, "new": p_new}


# ---------------- 查重（同一个文件） ----------------
def same_file(a, b):
    """两个路径是否指向同一个文件（同一个文件就没法比差异，只能查重）。"""
    if not a or not b:
        return False
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.normcase(os.path.abspath(a)) == \
            os.path.normcase(os.path.abspath(b))


def find_dups(sd):
    """按 Part Number 查重。

    返回 {"rows": 结果行, "groups": [(料号, [行…])], "stats": 统计}。
    只认料号：无料号的位号键行不参与判重（记在 stats["ref_rows"] 里交代清楚）。
    分组按「出现次数降序、料号升序」排，组内按原表行号。
    """
    by_pn = {}
    for rec in sd.rows:
        if rec.pn:
            by_pn.setdefault(rec.pn, []).append(rec)
    groups = [(pn, g) for pn, g in by_pn.items() if len(g) > 1]
    groups.sort(key=lambda kv: (-len(kv[1]), kv[0]))

    rows = []
    for pn, g in groups:
        for rec in g:
            rows.append([pn, len(g), "重复", rec.ref, rec.qty_text,
                         rec.strike_text, "r%d" % rec.r, sd.name])

    pn_rows = sum(1 for rec in sd.rows if rec.pn)
    n_dup = len(rows)
    stats = {"sheet": sd.name, "rows": len(sd.rows), "pn_rows": pn_rows,
             "ref_rows": len(sd.rows) - pn_rows, "unkeyable": len(sd.unkeyable),
             "labels": sd.labels, "groups": len(groups), "dup_rows": n_dup,
             "extra": n_dup - len(groups), "unique": len(by_pn),
             "max": max((len(g) for _p, g in groups), default=0),
             "top": groups[0][0] if groups else ""}
    return {"rows": rows, "groups": groups, "stats": stats}


def dup_marks(sd, groups=None):
    """重复行整行涂浅紫；料号格额外加批注「第 n/N 处」。"""
    if groups is None:
        groups = find_dups(sd)["groups"]
    marks = []
    for _pn, g in groups:
        for i, rec in enumerate(g, 1):
            pncol = _block_cols(sd, rec)[0]
            c1 = rec.c1 or pncol or 1
            c2 = rec.c2 or c1
            for c in range(c1, c2 + 1):
                marks.append((rec.r, c, "dup", None))
            if pncol:
                marks.append((rec.r, pncol, "dup",
                              "料号重复 %d/%d（本页共 %d 处）" % (i, len(g), len(g))))
    return marks


def mark_workbook_multi(src, marks_by_sheet, dst, summary=()):
    """复制源工作簿，按工作表分别涂色，可选追加「查重汇总」页。

    marks_by_sheet: {工作表名: [(行, 列, 颜色键, 批注)]}
    summary: [{"sheet":…, "stats":…, "groups":[…]}]，非空才加汇总页。
    """
    try:
        from openpyxl import load_workbook
        from openpyxl.comments import Comment
        from openpyxl.styles import PatternFill
    except ImportError:
        raise BomError("缺少 openpyxl，无法生成标注文件（pip install openpyxl）")

    keep_vba = src.lower().endswith(".xlsm")
    wb = load_workbook(src, keep_vba=keep_vba, data_only=False)
    fills = {k: PatternFill("solid", fgColor=v) for k, v in MARK_COLORS.items()}
    try:
        for sheet, marks in marks_by_sheet.items():
            if sheet not in wb.sheetnames:
                raise BomError("标注失败：工作簿里找不到工作表「%s」" % sheet)
            ws = wb[sheet]
            for r, c, key, note in marks:
                cell = ws.cell(row=r, column=c)
                cell.fill = fills[key]
                if note:
                    cell.comment = Comment(note, "BOMCheck")
        if summary:
            _write_dup_summary(wb, src, summary)
        return _save_any(wb, dst)
    finally:
        wb.close()


def _write_dup_summary(wb, src, summary):
    """在副本里加一页「查重汇总」：总账 + 每个重复料号的明细。"""
    from openpyxl.styles import Font, PatternFill, Alignment
    from openpyxl.utils import get_column_letter

    name = "查重汇总"
    if name in wb.sheetnames:
        del wb[name]
    ws = wb.create_sheet(name, 0)
    head_fill = PatternFill("solid", fgColor="185FA5")
    head_font = Font(color="FFFFFF", bold=True, size=10)
    title_font = Font(bold=True, size=13)
    dup_fill = PatternFill("solid", fgColor=MARK_COLORS["dup"])

    ws.cell(row=1, column=1, value="查重汇总（按 Part Number）").font = title_font
    ws.cell(row=2, column=1, value="源文件").font = Font(bold=True, size=10)
    ws.cell(row=2, column=2, value=src)
    ws.cell(row=3, column=1, value="口径").font = Font(bold=True, size=10)
    ws.cell(row=3, column=2,
            value="同一料号（Part Number）出现 2 行及以上即为重复；"
                  "无料号行不参与；重复行在各自工作表内整行涂浅紫")

    head = ["工作表", "数据行", "有料号行", "无料号行", "无法建档",
            "重复料号数", "涉及行数", "多出行数", "最多重复"]
    hr = 5
    for i, h in enumerate(head, 1):
        cell = ws.cell(row=hr, column=i, value=h)
        cell.fill, cell.font = head_fill, head_font
        cell.alignment = Alignment(horizontal="center", vertical="center")
    r = hr + 1
    for item in summary:
        s = item["stats"]
        for i, v in enumerate([s["sheet"], s["rows"], s["pn_rows"], s["ref_rows"],
                               s["unkeyable"], s["groups"], s["dup_rows"],
                               s["extra"], s["max"]], 1):
            ws.cell(row=r, column=i, value=v).alignment = Alignment(
                horizontal="center" if i > 1 else "left")
        r += 1

    r += 1
    ws.cell(row=r, column=1, value="重复明细").font = Font(bold=True, size=11)
    r += 1
    head2 = ["工作表", "料号", "出现次数", "行号", "数量合计", "位号"]
    for i, h in enumerate(head2, 1):
        cell = ws.cell(row=r, column=i, value=h)
        cell.fill, cell.font = head_fill, head_font
        cell.alignment = Alignment(horizontal="center", vertical="center")
    r += 1
    for item in summary:
        for pn, g in item["groups"]:
            qty = sum(x.qty for x in g if x.qty is not None)
            vals = [item["stats"]["sheet"], pn, len(g),
                    ", ".join("r%d" % x.r for x in g),
                    (str(int(qty)) if abs(qty - int(qty)) < 1e-9 else str(qty)) if qty else "",
                    " ".join(x.ref for x in g if x.ref)]
            for i, v in enumerate(vals, 1):
                cell = ws.cell(row=r, column=i, value=v)
                if i == 2:
                    cell.fill = dup_fill
            r += 1

    for i, w in enumerate([16, 22, 10, 26, 10, 40], 1):
        ws.column_dimensions[get_column_letter(i)].width = w
    ws.freeze_panes = "A6"


def write_dup_output(src, marks_by_sheet, summary, outdir=None):
    """写一份「<主名>_查重标注.xlsx」：原文件副本 + 重复行涂色 + 查重汇总页。

    返回 {"dir":…, "file":…}；目录不可写抛 OutDirError。源文件不动。
    """
    d = default_outdir(src, outdir)
    if not _ensure_dir(d):
        raise OutDirError("输出目录不可写（只读或权限不足）：\n%s" % d, d)
    base = os.path.splitext(os.path.basename(src))[0]
    ext = ".xlsm" if src.lower().endswith(".xlsm") else ".xlsx"
    dst = os.path.join(d, "%s_查重标注%s" % (base, ext))
    p = mark_workbook_multi(src, marks_by_sheet, dst, summary)
    return {"dir": d, "file": p}


# ---------------- 导出 ----------------
def export_xlsx(rows, diag_rows, path):
    """结果表 + 诊断页；返回实际写入路径（被占用时自动改名）。失败抛 BomError。"""
    try:
        from openpyxl import Workbook
        from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
        from openpyxl.utils import get_column_letter
    except ImportError:
        raise BomError("缺少 openpyxl，无法导出（pip install openpyxl）")

    wb = Workbook()
    ws = wb.active
    ws.title = "差异结果"
    head_fill = PatternFill("solid", fgColor="185FA5")
    head_font = Font(color="FFFFFF", bold=True, size=10)
    thin = Side(style="thin", color="D9DCE1")
    ws.append(RESULT_COLS)
    for c in range(1, len(RESULT_COLS) + 1):
        cell = ws.cell(row=1, column=c)
        cell.fill, cell.font = head_fill, head_font
        cell.alignment = Alignment(horizontal="center", vertical="center")
    fills = {"新增": PatternFill("solid", fgColor="C6EFCE"),
             "删除": PatternFill("solid", fgColor="FFC7CE"),
             "变更": PatternFill("solid", fgColor="FFEB9C")}
    dup_fill = PatternFill("solid", fgColor="D9D2E9")     # 仅料号格
    ncol = len(RESULT_COLS)
    for row in rows:
        ws.append(row)
        tr = ws.max_row
        for c in range(1, ncol + 1):
            ws.cell(row=tr, column=c).border = Border(bottom=thin)
        kind = row[2]
        if kind in ("新增", "删除"):
            for c in range(1, ncol + 1):                  # 新增/删除：整行
                ws.cell(row=tr, column=c).fill = fills[kind]
        elif kind == "变更":
            hot = {4}                                     # 变更项 必涂
            for a, b in ((5, 6), (7, 8), (9, 10)):        # 旧/新成对比较
                if str(row[a - 1] or "") != str(row[b - 1] or ""):
                    hot.update((a, b))
            for c in hot:
                ws.cell(row=tr, column=c).fill = fills["变更"]
        if row[10]:                                       # 重复料号：只涂料号格
            ws.cell(row=tr, column=1).fill = dup_fill
    widths = [18, 14, 9, 24, 22, 22, 11, 11, 10, 10, 10, 22]
    for i, w in enumerate(widths, 1):
        ws.column_dimensions[get_column_letter(i)].width = w
    last_data = ws.max_row
    ws.freeze_panes = "A2"
    ws.auto_filter.ref = "A1:%s%d" % (get_column_letter(ncol), last_data)

    # 颜色说明栏：数据区下方空一行，四格底色即图例（不进筛选范围）
    lr = last_data + 2
    ws.cell(row=lr, column=1, value="颜色说明").font = Font(bold=True, size=10)
    for i, (name, color) in enumerate(
            (("新增", "C6EFCE"), ("删除", "FFC7CE"),
             ("变更", "FFEB9C"), ("重复料号", "D9D2E9")), start=2):
        cell = ws.cell(row=lr, column=i, value=name)
        cell.fill = PatternFill("solid", fgColor=color)
        cell.alignment = Alignment(horizontal="center", vertical="center")
    ws.cell(row=lr, column=6,
            value="新增/删除为整行，变更涂变更格，重复料号仅涂料号格").font = \
        Font(size=9, color="6B7280")

    d = wb.create_sheet("识别诊断")
    d.append(["文件", "工作表", "表头行", "数据行", "料号列", "位号列", "数量列",
              "建档行", "位号键行", "分组标签", "无法建档", "料号命中率"])
    for c in range(1, 13):
        d.cell(row=1, column=c).fill, d.cell(row=1, column=c).font = head_fill, head_font
    for r in diag_rows:
        d.append(r)
    for i, w in enumerate([26, 18, 8, 8, 9, 9, 9, 9, 10, 10, 10, 12], 1):
        d.column_dimensions[get_column_letter(i)].width = w
    d.freeze_panes = "A2"
    return _save_any(wb, path)
