#include "bs_write.h"

#include "bs_zipw.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 样式索引（与 styles_xml 里的 cellXfs 顺序一一对应） ────────── */
enum {
    XF_DEF = 0,    /* 默认 */
    XF_HEAD,       /* 表头：深蓝底 + 白粗体 + 居中 */
    XF_BODY,       /* 数据行：只有下边框 */
    XF_ADD,        /* 新增：整行浅绿 */
    XF_DEL,        /* 删除：整行浅红 */
    XF_CHG,        /* 变更格：浅黄 */
    XF_DUP,        /* 重复料号：只涂料号格，浅紫 */
    XF_LEG_T,      /* 图例标题：粗体 */
    XF_LEG_ADD,    /* 图例块（无边框） */
    XF_LEG_DEL,
    XF_LEG_CHG,
    XF_LEG_DUP,
    XF_NOTE        /* 图例右侧说明：9 号灰 */
};

#define XML_HEAD \
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"

static const char NS_MAIN[] =
    "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
static const char NS_R[] =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

/* 列宽与 bomcore.export_xlsx 完全一致（单位：字符数） */
static const int W_DIFF[BS_RES_COLS] = {18, 14, 9, 24, 22, 22, 11, 11, 10, 10, 10, 22};
static const int W_DIAG[12] = {26, 18, 8, 8, 9, 9, 9, 9, 10, 10, 10, 12};

/* ── 字符串缓冲 ───────────────────────────────────────────────── */
typedef struct {
    char *p;
    size_t n, cap;
    int bad;          /* 分配失败后就地罢工，调用方最后统一判 */
} sb;

static void sb_grow(sb *b, size_t need)
{
    size_t cap;
    char *p;

    if (b->bad || b->n + need + 1 <= b->cap) return;
    cap = b->cap ? b->cap : 8192;
    while (cap < b->n + need + 1) {
        if (cap > (size_t)-1 / 2) { b->bad = 1; return; }
        cap *= 2;
    }
    p = (char *)realloc(b->p, cap);
    if (!p) { b->bad = 1; return; }
    b->p = p;
    b->cap = cap;
}

static void sb_put(sb *b, const char *s, size_t n)
{
    sb_grow(b, n);
    if (b->bad) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_puts(sb *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

static void sb_addf(sb *b, const char *fmt, ...)
{
    char t[512];
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (n < 0) { b->bad = 1; return; }
    if (n >= (int)sizeof(t)) n = (int)sizeof(t) - 1;
    sb_put(b, t, (size_t)n);
}

/* XML 文本转义。控制字符在 XML 1.0 里非法（哪怕是 &#x1;），直接丢；
   \t \n \r 换成空格 —— BOM 里的换行留在单元格里只会把行撑高。 */
static void sb_esc(sb *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    for (; *p; p++) {
        unsigned c = *p;
        if (c < 0x20) {
            if (c == '\t' || c == '\n' || c == '\r') sb_put(b, " ", 1);
            continue;
        }
        switch (c) {
        case '&':  sb_puts(b, "&amp;");  break;
        case '<':  sb_puts(b, "&lt;");   break;
        case '>':  sb_puts(b, "&gt;");   break;
        case '"':  sb_puts(b, "&quot;"); break;
        default:   sb_put(b, (const char *)p, 1);
        }
    }
}

/* 1 基列号 → "A".."L"（本模块最多 12 列，仍按通用写法） */
static void col_name(int c, char *out)
{
    if (c <= 26) {
        out[0] = (char)('A' + c - 1);
        out[1] = 0;
    } else {
        out[0] = (char)('A' + (c - 1) / 26 - 1);
        out[1] = (char)('A' + (c - 1) % 26);
        out[2] = 0;
    }
}

static void cell_open(sb *b, int col, int row, int xf)
{
    char cn[4];
    col_name(col, cn);
    sb_addf(b, "<c r=\"%s%d\" s=\"%d\"", cn, row, xf);
}

/* 空串写成「有样式没有值」的单元格：openpyxl 读回 None，与 Python 写空串的表现一致 */
static void cell_text(sb *b, int col, int row, const char *v, int xf)
{
    cell_open(b, col, row, xf);
    if (v && *v) {
        sb_puts(b, " t=\"inlineStr\"><is><t xml:space=\"preserve\">");
        sb_esc(b, v);
        sb_puts(b, "</t></is></c>");
    } else {
        sb_puts(b, "/>");
    }
}

static void cell_int(sb *b, int col, int row, long v, int xf)
{
    cell_open(b, col, row, xf);
    sb_addf(b, "><v>%ld</v></c>", v);
}

/* ── 静态部件 ─────────────────────────────────────────────────── */

/* fills: 0 none / 1 gray125 / 2 表头 185FA5 / 3 新增 C6EFCE /
          4 删除 FFC7CE / 5 变更 FFEB9C / 6 重复 D9D2E9 */
static const char STYLES_XML[] =
XML_HEAD
"<styleSheet xmlns=\"" "http://schemas.openxmlformats.org/spreadsheetml/2006/main" "\">"
"<fonts count=\"4\">"
  "<font><sz val=\"10\"/><name val=\"微软雅黑\"/><family val=\"2\"/></font>"
  "<font><b/><sz val=\"10\"/><color rgb=\"FFFFFFFF\"/><name val=\"微软雅黑\"/><family val=\"2\"/></font>"
  "<font><b/><sz val=\"10\"/><name val=\"微软雅黑\"/><family val=\"2\"/></font>"
  "<font><sz val=\"9\"/><color rgb=\"FF6B7280\"/><name val=\"微软雅黑\"/><family val=\"2\"/></font>"
"</fonts>"
"<fills count=\"7\">"
  "<fill><patternFill patternType=\"none\"/></fill>"
  "<fill><patternFill patternType=\"gray125\"/></fill>"
  "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF185FA5\"/><bgColor indexed=\"64\"/></patternFill></fill>"
  "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFC6EFCE\"/><bgColor indexed=\"64\"/></patternFill></fill>"
  "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFFFC7CE\"/><bgColor indexed=\"64\"/></patternFill></fill>"
  "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFFFEB9C\"/><bgColor indexed=\"64\"/></patternFill></fill>"
  "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFD9D2E9\"/><bgColor indexed=\"64\"/></patternFill></fill>"
"</fills>"
"<borders count=\"2\">"
  "<border><left/><right/><top/><bottom/><diagonal/></border>"
  "<border><left/><right/><top/>"
    "<bottom style=\"thin\"><color rgb=\"FFD9DCE1\"/></bottom><diagonal/></border>"
"</borders>"
"<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
"<cellXfs count=\"13\">"
  /* 0 DEF   */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
  /* 1 HEAD  */ "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"0\" xfId=\"0\""
                 " applyFont=\"1\" applyFill=\"1\" applyAlignment=\"1\">"
                 "<alignment horizontal=\"center\" vertical=\"center\"/></xf>"
  /* 2 BODY  */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\""
                 " applyBorder=\"1\"/>"
  /* 3 ADD   */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"3\" borderId=\"1\" xfId=\"0\""
                 " applyFill=\"1\" applyBorder=\"1\"/>"
  /* 4 DEL   */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"4\" borderId=\"1\" xfId=\"0\""
                 " applyFill=\"1\" applyBorder=\"1\"/>"
  /* 5 CHG   */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"5\" borderId=\"1\" xfId=\"0\""
                 " applyFill=\"1\" applyBorder=\"1\"/>"
  /* 6 DUP   */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"6\" borderId=\"1\" xfId=\"0\""
                 " applyFill=\"1\" applyBorder=\"1\"/>"
  /* 7 LEG_T */ "<xf numFmtId=\"0\" fontId=\"2\" fillId=\"0\" borderId=\"0\" xfId=\"0\""
                 " applyFont=\"1\"/>"
  /* 8 LEG_ADD */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"3\" borderId=\"0\" xfId=\"0\""
                 " applyFill=\"1\" applyAlignment=\"1\">"
                 "<alignment horizontal=\"center\" vertical=\"center\"/></xf>"
  /* 9 LEG_DEL */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"4\" borderId=\"0\" xfId=\"0\""
                 " applyFill=\"1\" applyAlignment=\"1\">"
                 "<alignment horizontal=\"center\" vertical=\"center\"/></xf>"
  /* 10 LEG_CHG */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"5\" borderId=\"0\" xfId=\"0\""
                 " applyFill=\"1\" applyAlignment=\"1\">"
                 "<alignment horizontal=\"center\" vertical=\"center\"/></xf>"
  /* 11 LEG_DUP */ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"6\" borderId=\"0\" xfId=\"0\""
                 " applyFill=\"1\" applyAlignment=\"1\">"
                 "<alignment horizontal=\"center\" vertical=\"center\"/></xf>"
  /* 12 NOTE  */ "<xf numFmtId=\"0\" fontId=\"3\" fillId=\"0\" borderId=\"0\" xfId=\"0\""
                 " applyFont=\"1\"/>"
"</cellXfs>"
"<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
"</styleSheet>";

static const char CONTENT_TYPES[] =
XML_HEAD
"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
"<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
"<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
"<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
"<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
"<Override PartName=\"/xl/worksheets/sheet2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
"<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
"</Types>";

static const char ROOT_RELS[] =
XML_HEAD
"<Relationships xmlns=\"" "http://schemas.openxmlformats.org/package/2006/relationships" "\">"
"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
"</Relationships>";

static const char WORKBOOK_RELS[] =
XML_HEAD
"<Relationships xmlns=\"" "http://schemas.openxmlformats.org/package/2006/relationships" "\">"
"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
"<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet2.xml\"/>"
"<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
"</Relationships>";

/* ── 差异结果页 ───────────────────────────────────────────────── */
static void sheet_diff_xml(sb *b, const bs_result *r)
{
    size_t n = bs_result_count(r), i;
    int c, lr;

    sb_puts(b, XML_HEAD);
    sb_addf(b, "<worksheet xmlns=\"%s\">", NS_MAIN);
    sb_puts(b, "<sheetViews><sheetView tabSelected=\"1\" workbookViewId=\"0\">"
               "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
               "<selection pane=\"bottomLeft\" activeCell=\"A2\" sqref=\"A2\"/>"
               "</sheetView></sheetViews>");
    sb_puts(b, "<sheetFormatPr defaultRowHeight=\"15\"/>");
    sb_puts(b, "<cols>");
    for (c = 1; c <= BS_RES_COLS; c++)
        sb_addf(b, "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>",
                c, c, W_DIFF[c - 1]);
    sb_puts(b, "</cols>");

    sb_puts(b, "<sheetData>");
    sb_puts(b, "<row r=\"1\" ht=\"22\" customHeight=\"1\">");
    for (c = 0; c < BS_RES_COLS; c++) cell_text(b, c + 1, 1, bs_result_header(c), XF_HEAD);
    sb_puts(b, "</row>");

    for (i = 0; i < n; i++) {
        const bs_resrow *row = bs_result_row(r, i);
        const char *kind = row->col[BS_COL_ACT];
        int xf[BS_RES_COLS];
        int rr = (int)i + 2;

        for (c = 0; c < BS_RES_COLS; c++) xf[c] = XF_BODY;
        if (kind && strcmp(kind, "新增") == 0) {
            for (c = 0; c < BS_RES_COLS; c++) xf[c] = XF_ADD;
        } else if (kind && strcmp(kind, "删除") == 0) {
            for (c = 0; c < BS_RES_COLS; c++) xf[c] = XF_DEL;
        } else if (kind && strcmp(kind, "变更") == 0) {
            /* 变更项必涂；旧/新成对的三组（位号 5-6、数量 7-8、删除线 9-10）
               只有真不同才涂 —— 与 export_xlsx 的 hot 集合一致 */
            xf[BS_COL_CHG] = XF_CHG;
            for (c = BS_COL_RO; c <= BS_COL_SN; c += 2)
                if (strcmp(row->col[c], row->col[c + 1]) != 0)
                    xf[c] = xf[c + 1] = XF_CHG;
        }
        if (row->col[BS_COL_DUP][0]) xf[BS_COL_PN] = XF_DUP;

        sb_addf(b, "<row r=\"%d\">", rr);
        for (c = 0; c < BS_RES_COLS; c++) cell_text(b, c + 1, rr, row->col[c], xf[c]);
        sb_puts(b, "</row>");
    }

    /* 颜色说明栏：数据区下空一行，四格底色即图例（在筛选范围之外） */
    lr = (int)n + 3;
    sb_addf(b, "<row r=\"%d\">", lr);
    cell_text(b, 1, lr, "颜色说明", XF_LEG_T);
    cell_text(b, 2, lr, "新增", XF_LEG_ADD);
    cell_text(b, 3, lr, "删除", XF_LEG_DEL);
    cell_text(b, 4, lr, "变更", XF_LEG_CHG);
    cell_text(b, 5, lr, "重复料号", XF_LEG_DUP);
    cell_text(b, 6, lr, "新增/删除为整行，变更涂变更格，重复料号仅涂料号格", XF_NOTE);
    sb_puts(b, "</row>");
    sb_puts(b, "</sheetData>");

    sb_addf(b, "<autoFilter ref=\"A1:L%lu\"/>", (unsigned long)(n + 1));
    sb_puts(b, "</worksheet>");
}

/* ── 识别诊断页 ───────────────────────────────────────────────── */
static void sheet_diag_xml(sb *b, const bs_diag *d, int n)
{
    static const char *H[12] = {
        "文件", "工作表", "表头行", "数据行", "料号列", "位号列",
        "数量列", "建档行", "位号键行", "分组标签", "无法建档", "料号命中率"
    };
    int c, i;

    sb_puts(b, XML_HEAD);
    sb_addf(b, "<worksheet xmlns=\"%s\">", NS_MAIN);
    sb_puts(b, "<sheetViews><sheetView workbookViewId=\"0\">"
               "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
               "<selection pane=\"bottomLeft\" activeCell=\"A2\" sqref=\"A2\"/>"
               "</sheetView></sheetViews>");
    sb_puts(b, "<sheetFormatPr defaultRowHeight=\"15\"/>");
    sb_puts(b, "<cols>");
    for (c = 1; c <= 12; c++)
        sb_addf(b, "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>",
                c, c, W_DIAG[c - 1]);
    sb_puts(b, "</cols>");
    sb_puts(b, "<sheetData>");

    sb_puts(b, "<row r=\"1\" ht=\"22\" customHeight=\"1\">");
    for (c = 0; c < 12; c++) cell_text(b, c + 1, 1, H[c], XF_HEAD);
    sb_puts(b, "</row>");

    for (i = 0; i < n; i++) {
        const bs_diag *x = &d[i];
        int rr = i + 2;
        char pct[16];

        snprintf(pct, sizeof(pct), "%.0f%%", x->hit_rate * 100.0);
        sb_addf(b, "<row r=\"%d\">", rr);
        cell_text(b, 1, rr, x->file, XF_BODY);
        cell_text(b, 2, rr, x->sheet, XF_BODY);
        cell_text(b, 3, rr, x->header, XF_BODY);
        cell_int (b, 4, rr, x->data_rows, XF_BODY);
        cell_text(b, 5, rr, x->col_pn, XF_BODY);
        cell_text(b, 6, rr, x->col_ref, XF_BODY);
        cell_text(b, 7, rr, x->col_qty, XF_BODY);
        cell_int (b, 8, rr, x->built_rows, XF_BODY);
        cell_int (b, 9, rr, x->ref_rows, XF_BODY);
        cell_int (b, 10, rr, x->labels, XF_BODY);
        cell_int (b, 11, rr, x->unkeyable, XF_BODY);
        cell_text(b, 12, rr, pct, XF_BODY);
        sb_puts(b, "</row>");
    }
    sb_puts(b, "</sheetData>");
    sb_puts(b, "</worksheet>");
}

/* ── 诊断数据 ─────────────────────────────────────────────────── */
void bs_diag_fill(bs_diag *d, const bs_sheet *s, const char *file)
{
    int i, nb, used = 0;

    if (!d) return;
    memset(d, 0, sizeof(*d));
    if (!s) return;
    snprintf(d->file, sizeof(d->file), "%s", file ? file : "");
    snprintf(d->sheet, sizeof(d->sheet), "%s", bs_sheet_name(s));
    d->header[0] = 0;
    nb = bs_sheet_block_count(s);
    for (i = 0; i < nb; i++) {
        int w = snprintf(d->header + used, sizeof(d->header) - (size_t)used,
                         "%s%d", i ? "/" : "", bs_sheet_block(s, i)->hr);
        if (w < 0 || (size_t)w >= sizeof(d->header) - (size_t)used) break;
        used += w;
    }
    d->data_rows = bs_sheet_data_rows(s);   /* 建档 + 无键 + 分组标题 */
    d->built_rows = bs_sheet_row_count(s);  /* 真正进了 bs_row 的行 */
    snprintf(d->col_pn, sizeof(d->col_pn), "%s", bs_sheet_col_name(s, 'P'));
    snprintf(d->col_ref, sizeof(d->col_ref), "%s", bs_sheet_col_name(s, 'R'));
    snprintf(d->col_qty, sizeof(d->col_qty), "%s", bs_sheet_col_name(s, 'Q'));
    d->ref_rows = bs_sheet_ref_rows(s);
    d->labels = bs_sheet_labels(s);
    d->unkeyable = bs_sheet_unkeyable(s);
    d->hit_rate = bs_sheet_hit_rate(s);
}

/* ── 落盘 ─────────────────────────────────────────────────────── */
static int add_lit(bs_zipw *z, const char *name, const char *lit)
{
    size_t n = strlen(lit);
    char *p = (char *)malloc(n ? n : 1);

    if (!p) return -1;
    memcpy(p, lit, n);
    return bs_zipw_add(z, name, p, n);
}

/* "a/b.xlsx" → "a/b_2.xlsx"（k 从 2 起） */
static void alt_path(char *dst, size_t cap, const char *src, int k)
{
    const char *dot = strrchr(src, '.');
    const char *s1 = strrchr(src, '/');
    const char *s2 = strrchr(src, '\\');
    const char *sep = NULL;
    size_t stem;

    if (s1) sep = s1;
    if (s2 && (!sep || s2 > sep)) sep = s2;
    if (dot && (!sep || dot > sep) && dot != src) stem = (size_t)(dot - src);
    else { dot = NULL; stem = strlen(src); }
    if (dot) snprintf(dst, cap, "%.*s_%d%s", (int)stem, src, k, dot);
    else snprintf(dst, cap, "%s_%d", src, k);
}

int bs_write_result(const char *path_utf8, const bs_result *r,
                    const bs_diag *diag, int n_diag,
                    char *out_path, size_t out_cap, bs_err *e)
{
    sb s1, s2;
    bs_zipw *z = NULL;
    char cur[1024];
    int i, rc = -1;

    memset(&s1, 0, sizeof(s1));
    memset(&s2, 0, sizeof(s2));
    if (e) e->msg[0] = 0;
    if (!path_utf8 || !*path_utf8 || !r) {
        if (e) snprintf(e->msg, sizeof(e->msg), "内部错误：没有要写的内容");
        return -1;
    }

    sheet_diff_xml(&s1, r);
    sheet_diag_xml(&s2, diag, n_diag < 0 ? 0 : n_diag);
    if (s1.bad || s2.bad) {
        free(s1.p);
        free(s2.p);
        if (e) snprintf(e->msg, sizeof(e->msg), "内存不足（结果表太大）");
        return -1;
    }

    z = bs_zipw_new();
    if (!z) goto oom;

    /* workbook.xml 里的表名带中文，UTF-8 直接写；关系用 r:id 对应 rels */
    {
        sb wb;
        memset(&wb, 0, sizeof(wb));
        sb_puts(&wb, XML_HEAD);
        sb_addf(&wb, "<workbook xmlns=\"%s\" xmlns:r=\"%s\">", NS_MAIN, NS_R);
        sb_puts(&wb, "<sheets>"
                     "<sheet name=\"差异结果\" sheetId=\"1\" r:id=\"rId1\"/>"
                     "<sheet name=\"识别诊断\" sheetId=\"2\" r:id=\"rId2\"/>"
                     "</sheets></workbook>");
        if (wb.bad) { free(wb.p); goto oom; }
        if (bs_zipw_add(z, "xl/workbook.xml", wb.p, wb.n) != 0) goto oom;
    }

    if (add_lit(z, "[Content_Types].xml", CONTENT_TYPES) ||
        add_lit(z, "_rels/.rels", ROOT_RELS) ||
        add_lit(z, "xl/_rels/workbook.xml.rels", WORKBOOK_RELS) ||
        add_lit(z, "xl/styles.xml", STYLES_XML))
        goto oom;

    /* 两个 sheet 的 buffer 直接交给 zip，不复制。
       bs_zipw_add 的契约是「接管 data，失败也 free」：调用前先把 s1/s2 置空，
       否则失败路径上的 oom 会对同一块内存 free 第二次。 */
    {
        char *p1 = s1.p, *p2 = s2.p;
        s1.p = s2.p = NULL;
        if (bs_zipw_add(z, "xl/worksheets/sheet1.xml", p1, s1.n) != 0) {
            free(p2);           /* 还没交给 zip，得自己收 */
            goto oom;
        }
        if (bs_zipw_add(z, "xl/worksheets/sheet2.xml", p2, s2.n) != 0) goto oom;
    }

    snprintf(cur, sizeof(cur), "%s", path_utf8);
    for (i = 0; i < 20; i++) {
        if (bs_zipw_save(z, cur, e ? e->msg : NULL, e ? sizeof(e->msg) : 0) == 0) {
            rc = 0;
            break;
        }
        if (i < 19) alt_path(cur, sizeof(cur), path_utf8, i + 2);
    }
    if (rc == 0 && out_path && out_cap) snprintf(out_path, out_cap, "%s", cur);
    bs_zipw_free(z);
    return rc;

oom:
    free(s1.p);
    free(s2.p);
    if (z) bs_zipw_free(z);
    if (e) snprintf(e->msg, sizeof(e->msg), "内存不足（结果表太大）");
    return -1;
}
