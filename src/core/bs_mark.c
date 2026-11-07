#include "bs_mark.h"

#include "bs_file.h"
#include "bs_pair.h"
#include "bs_pn.h"
#include "bs_sheet.h"
#include "bs_xlsx.h"
#include "bs_zip.h"
#include "bs_zipw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 差异四色（附录 A 唯一来源；与 bs_write.c 的 styles_xml 同值） ────── */
static const char *const HEX[4] = {"C6EFCE", "FFC7CE", "FFEB9C", "D9D2E9"};

const char *bs_mark_color_hex(int color)
{
    if (color < 0 || color > 3) return "";
    return HEX[color];
}

int bs_mark_color_count(void) { return 4; }

/* ── 字符串缓冲 ───────────────────────────────────────────────────────── */
typedef struct {
    char *p;
    size_t n, cap;
    int bad;
} sb;

static void sb_grow(sb *b, size_t need)
{
    size_t cap;
    char *p;

    if (b->bad || b->n + need + 1 <= b->cap) return;
    cap = b->cap ? b->cap : 16384;
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
    if (!n) return;
    sb_grow(b, n);
    if (b->bad) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_puts(sb *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_int(sb *b, int v)
{
    char t[24];
    int n = snprintf(t, sizeof t, "%d", v);
    if (n > 0) sb_put(b, t, (size_t)n);
}

static void sb_free(sb *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* ── 列号 ↔ 列字母 ───────────────────────────────────────────────────── */
static void col_letters(int col, char *out)
{
    char t[8];
    int n = 0, i;

    if (col < 1) col = 1;
    while (col > 0 && n < 7) {
        int r = (col - 1) % 26;
        t[n++] = (char)('A' + r);
        col = (col - 1) / 26;
    }
    for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

static int col_index(const char *s, const char *end)
{
    int v = 0, n = 0;
    while (s < end) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') break;
        v = v * 26 + (c - 'A' + 1);
        s++;
        n++;
        if (n > 6) break;
    }
    return n ? v : 0;
}

/* ── XML 小工具 ──────────────────────────────────────────────────────── */
typedef struct {
    const char *np;   /* 属性名起点 */
    size_t nl;
    const char *vp;   /* 属性值起点 */
    size_t vl;
} attr_t;

/* 从标签名之后开始逐个取属性；遇到 '/' 或 '>' 返回 0 */
static int attr_next(const char **pp, const char *end, attr_t *a)
{
    const char *p = *pp;

    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || *p == '/' || *p == '>') { *pp = p; return 0; }

    a->np = p;
    while (p < end && *p != '=' && *p != ' ' && *p != '>' && *p != '/') p++;
    a->nl = (size_t)(p - a->np);
    while (p < end && *p != '=' && *p != '>' && *p != '/') p++;
    if (p < end && *p == '=') {
        char q;
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        q = (p < end) ? *p : 0;
        if (q == '"' || q == '\'') {
            p++;
            a->vp = p;
            while (p < end && *p != q) p++;
            a->vl = (size_t)(p - a->vp);
            if (p < end) p++;
        } else {                       /* 无引号：容错到空白为止 */
            a->vp = p;
            while (p < end && *p != ' ' && *p != '>' && *p != '/') p++;
            a->vl = (size_t)(p - a->vp);
        }
    } else {
        a->vp = p;
        a->vl = 0;
    }
    *pp = p;
    return 1;
}

/* 跳过引号内的 '>'，返回标签结束后的位置 */
static const char *tag_end(const char *p, const char *end)
{
    char q = 0;

    while (p < end) {
        char c = *p;
        if (q) {
            if (c == q) q = 0;
        } else if (c == '"' || c == '\'') {
            q = c;
        } else if (c == '>') {
            return p + 1;
        }
        p++;
    }
    return end;
}

static int tag_name_is(const char *p, const char *end, const char *name)
{
    size_t n = strlen(name);
    char c;

    if (p >= end || *p != '<') return 0;
    if ((size_t)(end - p) < n + 2) return 0;
    if (strncmp(p + 1, name, n) != 0) return 0;
    c = p[1 + n];
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>';
}

static int is_close_tag(const char *p, const char *end, const char *name)
{
    size_t n = strlen(name);

    if (p >= end || *p != '<' || (size_t)(end - p) < n + 3) return 0;
    if (p[1] != '/') return 0;
    if (strncmp(p + 2, name, n) != 0) return 0;
    return p[2 + n] == '>';
}

static int tag_attr_int(const char *tag, const char *end, const char *name, int *out)
{
    const char *p = tag + 1;
    attr_t a;

    while (p < end && *p != ' ' && *p != '>' && *p != '/') p++;
    while (attr_next(&p, end, &a)) {
        if (a.nl == strlen(name) && strncmp(a.np, name, a.nl) == 0) {
            char t[16];
            size_t n = a.vl < sizeof t - 1 ? a.vl : sizeof t - 1;
            memcpy(t, a.vp, n);
            t[n] = 0;
            *out = atoi(t);
            return 1;
        }
    }
    return 0;
}

/* 复制一个标签，把 count 属性改成新值（没有就补在末尾） */
static void emit_tag_count(sb *b, const char *tag, const char *end, int count)
{
    const char *p = tag + 1;
    attr_t a;
    int done = 0;

    while (p < end && *p != ' ' && *p != '>' && *p != '/') p++;
    sb_put(b, tag, (size_t)(p - tag));          /* "<cellXfs" */
    while (attr_next(&p, end, &a)) {
        if (a.nl == 5 && strncmp(a.np, "count", 5) == 0) {
            sb_puts(b, " count=\"");
            sb_int(b, count);
            sb_puts(b, "\"");
            done = 1;
        } else {
            sb_put(b, " ", 1);
            sb_put(b, a.np, a.nl);
            sb_puts(b, "=\"");
            sb_put(b, a.vp, a.vl);
            sb_puts(b, "\"");
        }
    }
    if (!done) {
        sb_puts(b, " count=\"");
        sb_int(b, count);
        sb_puts(b, "\"");
    }
    sb_puts(b, ">");
}

/* ── styles.xml：读现有 cellXfs，按需克隆 ────────────────────────────── */
typedef struct {
    int old_xf, color, new_xf;
} xfmap;

typedef struct {
    char **xf;                    /* 现有 xf 的原文（含 "<xf .../>"） */
    size_t *xfl;
    int n_xf;
    int n_extra;
    sb extra;                     /* 新加的 xf 片段 */
    xfmap *map;
    int n_map, cap_map;
    int fill_base;
} sty;

/* 取第 old_xf 个 xf 的副本，只把 fillId 换成 fill_base+color；
   同一 (xf, 色) 只克隆一次 */
static int style_for(sty *st, int old_xf, int color)
{
    int i;

    if (old_xf < 0 || old_xf >= st->n_xf) old_xf = 0;
    if (st->n_xf == 0) return -1;

    for (i = 0; i < st->n_map; i++)
        if (st->map[i].old_xf == old_xf && st->map[i].color == color)
            return st->map[i].new_xf;

    {
        const char *tag = st->xf[old_xf];
        const char *end = tag + st->xfl[old_xf];
        const char *p = tag + 1;
        attr_t a;
        int fill = st->fill_base + color;

        if (st->n_map == st->cap_map) {
            int cap = st->cap_map ? st->cap_map * 2 : 16;
            xfmap *q = (xfmap *)realloc(st->map, (size_t)cap * sizeof(*q));
            if (!q) return -1;
            st->map = q;
            st->cap_map = cap;
        }
        while (p < end && *p != ' ' && *p != '>' && *p != '/') p++;
        sb_puts(&st->extra, "<xf");
        while (attr_next(&p, end, &a)) {
            if ((a.nl == 6 && strncmp(a.np, "fillId", 6) == 0) ||
                (a.nl == 9 && strncmp(a.np, "applyFill", 9) == 0))
                continue;
            sb_puts(&st->extra, " ");
            sb_put(&st->extra, a.np, a.nl);
            sb_puts(&st->extra, "=\"");
            sb_put(&st->extra, a.vp, a.vl);
            sb_puts(&st->extra, "\"");
        }
        sb_puts(&st->extra, " fillId=\"");
        sb_int(&st->extra, fill);
        sb_puts(&st->extra, "\" applyFill=\"1\"");
        if (p < end && *p == '/') {
            sb_puts(&st->extra, "/>");
        } else {
            sb_puts(&st->extra, ">");
            /* p+1..end 里已经含子元素和 </xf>，别再补一次闭合标签 */
            if (p < end) sb_put(&st->extra, p + 1, (size_t)(end - (p + 1)));
        }

        st->map[st->n_map].old_xf = old_xf;
        st->map[st->n_map].color = color;
        st->map[st->n_map].new_xf = st->n_xf + st->n_extra;
        st->n_extra++;
        return st->map[st->n_map++].new_xf;
    }
}

static void sty_free(sty *st)
{
    int i;
    for (i = 0; i < st->n_xf; i++) free(st->xf[i]);
    free(st->xf);
    free(st->xfl);
    sb_free(&st->extra);
    free(st->map);
    memset(st, 0, sizeof *st);
}

/* ── 标注格表 ────────────────────────────────────────────────────────── */
typedef struct {
    int row, col, color, seq;
} mark1;

typedef struct {
    int row;
    int *col;
    int *color;
    int n, cap;
} markrow;

typedef struct {
    mark1 *v;
    int n, cap;
} marks;

static int mark_push(marks *m, int row, int col, int color)
{
    if (row <= 0 || col <= 0) return 0;
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 64;
        mark1 *p = (mark1 *)realloc(m->v, (size_t)cap * sizeof(*p));
        if (!p) return -1;
        m->v = p;
        m->cap = cap;
    }
    m->v[m->n].row = row;
    m->v[m->n].col = col;
    m->v[m->n].color = color;
    m->v[m->n].seq = m->n;
    m->n++;
    return 0;
}

static int cmp_mark(const void *a, const void *b)
{
    const mark1 *x = (const mark1 *)a, *y = (const mark1 *)b;

    if (x->row != y->row) return x->row - y->row;
    if (x->col != y->col) return x->col - y->col;
    return x->seq - y->seq;                       /* 后写的赢，与 Python 追加顺序一致 */
}

/* 排序 + 同格去重（保留最后写入的那条），再拆成按行的数组 */
static int build_rows(marks *m, markrow **rows_out, int *nrows_out)
{
    int i, k, n;
    markrow *rows;

    if (m->n == 0) { *rows_out = NULL; *nrows_out = 0; return 0; }
    qsort(m->v, (size_t)m->n, sizeof(mark1), cmp_mark);

    for (i = 0, k = 0; i < m->n; i++) {
        if (k > 0 && m->v[k - 1].row == m->v[i].row && m->v[k - 1].col == m->v[i].col)
            m->v[k - 1] = m->v[i];                /* 同格覆盖 */
        else
            m->v[k++] = m->v[i];
    }
    m->n = k;

    n = 0;
    for (i = 0; i < m->n; i++)
        if (i == 0 || m->v[i].row != m->v[i - 1].row) n++;

    rows = (markrow *)calloc((size_t)n, sizeof(markrow));
    if (!rows) return -1;
    {
        int r = -1;
        for (i = 0; i < m->n; i++) {
            if (i == 0 || m->v[i].row != m->v[i - 1].row) {
                r++;
                rows[r].row = m->v[i].row;
            }
            if (rows[r].n == rows[r].cap) {
                int cap = rows[r].cap ? rows[r].cap * 2 : 4;
                int *c = (int *)realloc(rows[r].col, (size_t)cap * sizeof(int));
                int *o = (int *)realloc(rows[r].color, (size_t)cap * sizeof(int));
                if (!c || !o) { free(c ? c : rows[r].col); free(o ? o : rows[r].color);
                                free(rows); return -1; }
                rows[r].col = c;
                rows[r].color = o;
                rows[r].cap = cap;
            }
            rows[r].col[rows[r].n] = m->v[i].col;
            rows[r].color[rows[r].n] = m->v[i].color;
            rows[r].n++;
        }
    }
    *rows_out = rows;
    *nrows_out = n;
    return 0;
}

static void free_rows(markrow *rows, int n)
{
    int i;
    for (i = 0; i < n; i++) { free(rows[i].col); free(rows[i].color); }
    free(rows);
}

/* ── 批注（C6b）────────────────────────────────────────────────────────
   Excel 的「传统批注」是两个部件配套的，缺一不可：
     xl/comments1.xml              文本（authors + commentList）
     xl/drawings/vmlDrawing1.vml   形状（ClientData 的 Row/Column 定位）
   sheet 里一条 <legacyDrawing r:id="…"/> 指向 VML，文本部件由 sheet 的
   关系文件另外挂上 —— 这就是文档说的「三处关系」。少任何一处，Excel
   要么只画黄框不出字，要么直接报修复。

   只给重复料号的**料号格**写批注：四色已经说清哪类差异，批注只补
   「第几个」。与 bomcore.mark_plan / dup_marks 的文案逐字一致。 */
typedef struct {
    int row, col;
    char text[96];
} note1;

typedef struct {
    note1 *v;
    int n, cap;
} notes;

static int note_push(notes *nt, int row, int col, const char *text)
{
    if (nt->n == nt->cap) {
        int cap = nt->cap ? nt->cap * 2 : 32;
        note1 *p = (note1 *)realloc(nt->v, (size_t)cap * sizeof(*p));
        if (!p) return -1;
        nt->v = p;
        nt->cap = cap;
    }
    nt->v[nt->n].row = row;
    nt->v[nt->n].col = col;
    snprintf(nt->v[nt->n].text, sizeof nt->v[nt->n].text, "%s", text);
    nt->n++;
    return 0;
}

/* 从已排序的标注表里挑出「重复料号」的料号格，配上 n/N 文案。
   rows 与 sheet 的行号都升序，用双指针扫一遍，不建索引。
   mode 0 = 比对（重复料号 n/N），1 = 查重（料号重复 n/N）。 */
static int collect_notes(notes *nt, const markrow *rows, int nrows,
                         const bs_sheet *s, int mode)
{
    int i, j = 0, nrow = bs_sheet_row_count(s);

    for (i = 0; i < nrows; i++) {
        const bs_row *r;
        const bs_block *bl;
        int k;

        while (j < nrow && bs_sheet_row(s, j)->row < rows[i].row) j++;
        if (j >= nrow || bs_sheet_row(s, j)->row != rows[i].row) continue;
        r = bs_sheet_row(s, j);
        if (r->dup_n <= 1 || r->block <= 0) continue;
        bl = bs_sheet_block(s, r->block - 1);
        if (!bl || bl->col_pn <= 0) continue;

        for (k = 0; k < rows[i].n; k++) {
            char t[96];
            if (rows[i].color[k] != BS_MARK_DUP) continue;
            if (rows[i].col[k] != bl->col_pn) continue;
            snprintf(t, sizeof t, mode ? "料号重复 %d/%d（本页共 %d 处）"
                                       : "重复料号 %d/%d（本页共 %d 处）",
                     r->dup_i, r->dup_n, r->dup_n);
            if (note_push(nt, rows[i].row, bl->col_pn, t) != 0) return -1;
        }
    }
    return 0;
}

static void notes_free(notes *nt)
{
    free(nt->v);
    nt->v = NULL;
    nt->n = nt->cap = 0;
}

/* ── sheetN.xml 重写 ─────────────────────────────────────────────────── */

/* 单个 <c> 的结束位置（含 "</c>" 或 "/>"） */
static const char *cell_end(const char *p, const char *end)
{
    const char *e = tag_end(p, end);

    if (e > p + 1 && e[-2] == '/') return e;
    while (e < end) {
        const char *q = (const char *)memchr(e, '<', (size_t)(end - e));
        if (!q) return end;
        if (q + 4 <= end && q[1] == '/' && q[2] == 'c' && q[3] == '>') return q + 4;
        e = q + 1;
    }
    return end;
}

static const char *cell_col_span(const char *cell, const char *end, int *col)
{
    const char *p = cell + 2;      /* 跳过 "<c" */
    attr_t a;

    *col = 0;
    while (attr_next(&p, end, &a)) {
        if (a.nl == 1 && a.np[0] == 'r') {
            *col = col_index(a.vp, a.vp + a.vl);
            return a.vp;
        }
    }
    return NULL;
}

static int cell_style(const char *cell, const char *end)
{
    const char *p = cell + 2;
    attr_t a;

    while (attr_next(&p, end, &a))
        if (a.nl == 1 && a.np[0] == 's') {
            char t[12];
            size_t n = a.vl < sizeof t - 1 ? a.vl : sizeof t - 1;
            memcpy(t, a.vp, n);
            t[n] = 0;
            return atoi(t);
        }
    return 0;
}

/* 输出一个改过 s 的 <c>，其余属性与内容原样保留 */
static void emit_cell(sb *b, const char *cell, const char *end, int style)
{
    const char *p = cell + 2;
    attr_t a;

    sb_puts(b, "<c");
    while (attr_next(&p, end, &a)) {
        if (a.nl == 1 && a.np[0] == 's') continue;
        sb_puts(b, " ");
        sb_put(b, a.np, a.nl);
        sb_puts(b, "=\"");
        sb_put(b, a.vp, a.vl);
        sb_puts(b, "\"");
    }
    sb_puts(b, " s=\"");
    sb_int(b, style);
    sb_puts(b, "\"");
    if (p < end && *p == '/') sb_puts(b, "/>");
    else if (p < end) sb_put(b, p, (size_t)(end - p));
    else sb_puts(b, "/>");
}

static void emit_synth_cell(sb *b, int row, int col, int style)
{
    char L[8];
    col_letters(col, L);
    sb_puts(b, "<c r=\"");
    sb_puts(b, L);
    sb_int(b, row);
    sb_puts(b, "\" s=\"");
    sb_int(b, style);
    sb_puts(b, "\"/>");
}

static void emit_synth_row(sb *b, const markrow *mr, sty *st)
{
    int i;

    sb_puts(b, "<row r=\"");
    sb_int(b, mr->row);
    sb_puts(b, "\">");
    for (i = 0; i < mr->n; i++) {
        int s = style_for(st, 0, mr->color[i]);
        if (s >= 0) emit_synth_cell(b, mr->row, mr->col[i], s);
    }
    sb_puts(b, "</row>");
}

/* ── 主重写 ──────────────────────────────────────────────────────────── */
/* 输出 <worksheet …> 开标签，缺 xmlns:r 时补上 —— <legacyDrawing r:id>
   要用这个前缀，虽然 Excel 生成的文件都有，但不能假定每个源文件都有。 */
static void emit_worksheet_open(sb *b, const char *p, const char *te)
{
    const char *q;
    int has_r = 0;

    for (q = p; q + 8 <= te; q++)
        if (memcmp(q, "xmlns:r=", 8) == 0) { has_r = 1; break; }
    if (has_r) { sb_put(b, p, (size_t)(te - p)); return; }

    {
        const char *ins = (te > p + 1 && te[-2] == '/') ? te - 2 : te - 1;
        sb_put(b, p, (size_t)(ins - p));
        sb_puts(b, " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"");
        sb_put(b, ins, (size_t)(te - ins));
    }
}

static int rewrite_sheet(const char *xml, size_t len, markrow *rows, int nrows,
                         sty *st, sb *out, int *n_skipped, const char *extra)
{
    const char *p = xml, *end = xml + len;
    int ri = 0, prev_row = 0, active = 0, cur_i = 0, prev_col = 0;
    int extra_done = 0;
    markrow *cur = NULL;

    *n_skipped = 0;

    while (p < end) {
        if (*p != '<') {
            const char *q = (const char *)memchr(p, '<', (size_t)(end - p));
            if (!q) q = end;
            sb_put(out, p, (size_t)(q - p));
            p = q;
            continue;
        }

        if (tag_name_is(p, end, "row")) {
            const char *te = tag_end(p, end);
            int selfclose = (te > p + 1 && te[-2] == '/');
            int r = prev_row + 1;

            tag_attr_int(p, te, "r", &r);
            prev_row = r;
            prev_col = 0;

            /* 比本行更早、XML 里却没有 <row> 的行：先补出来 */
            while (ri < nrows && rows[ri].row < r) {
                emit_synth_row(out, &rows[ri], st);
                ri++;
            }

            if (selfclose) {
                sb_put(out, p, (size_t)(te - p - 2));
                sb_puts(out, ">");
                if (ri < nrows && rows[ri].row == r) {
                    int i;
                    for (i = 0; i < rows[ri].n; i++) {
                        int s = style_for(st, 0, rows[ri].color[i]);
                        if (s >= 0) emit_synth_cell(out, r, rows[ri].col[i], s);
                    }
                    ri++;
                }
                sb_puts(out, "</row>");
            } else {
                sb_put(out, p, (size_t)(te - p));
                cur = (ri < nrows && rows[ri].row == r) ? &rows[ri] : NULL;
                cur_i = 0;
                active = cur != NULL;
            }
            p = te;
            continue;
        }

        if (active && tag_name_is(p, end, "c")) {
            const char *ce = cell_end(p, end);
            int col = 0;

            cell_col_span(p, ce, &col);
            if (col <= 0) col = prev_col + 1;
            prev_col = col;

            while (cur_i < cur->n && cur->col[cur_i] < col) {
                int s = style_for(st, 0, cur->color[cur_i]);
                if (s >= 0) emit_synth_cell(out, cur->row, cur->col[cur_i], s);
                cur_i++;
            }
            if (cur_i < cur->n && cur->col[cur_i] == col) {
                int s = style_for(st, cell_style(p, ce), cur->color[cur_i]);
                if (s >= 0) emit_cell(out, p, ce, s);
                else sb_put(out, p, (size_t)(ce - p));
                cur_i++;
            } else {
                sb_put(out, p, (size_t)(ce - p));
            }
            p = ce;
            continue;
        }

        if (active && is_close_tag(p, end, "row")) {
            while (cur_i < cur->n) {
                int s = style_for(st, 0, cur->color[cur_i]);
                if (s >= 0) emit_synth_cell(out, cur->row, cur->col[cur_i], s);
                cur_i++;
            }
            active = 0;
            ri++;
            sb_puts(out, "</row>");
            p = tag_end(p, end);
            continue;
        }

        if (tag_name_is(p, end, "sheetData")) {
            const char *te = tag_end(p, end);
            int selfclose = (te > p + 1 && te[-2] == '/');

            if (selfclose) {
                sb_puts(out, "<sheetData>");
                while (ri < nrows) { emit_synth_row(out, &rows[ri], st); ri++; }
                sb_puts(out, "</sheetData>");
            } else {
                sb_put(out, p, (size_t)(te - p));
            }
            p = te;
            continue;
        }

        if (is_close_tag(p, end, "sheetData")) {
            while (ri < nrows) { emit_synth_row(out, &rows[ri], st); ri++; }
            sb_put(out, p, (size_t)(tag_end(p, end) - p));
            p = tag_end(p, end);
            continue;
        }

        /* <legacyDrawing> 在 CT_Worksheet 里的位置靠后：合并单元格、条件
           格式、打印设置、drawing 之后，extLst 之前。插在 </worksheet> 前
           天然满足；源表带 <extLst> 时改插在它前面，否则 Excel 报修复。 */
        if (extra && !extra_done && tag_name_is(p, end, "extLst")) {
            sb_puts(out, extra);
            extra_done = 1;
        }
        if (extra && !extra_done && is_close_tag(p, end, "worksheet")) {
            sb_puts(out, extra);
            extra_done = 1;
        }
        if (tag_name_is(p, end, "worksheet")) {
            emit_worksheet_open(out, p, tag_end(p, end));
            p = tag_end(p, end);
            continue;
        }

        /* 其他标签 / 注释：整段原样拷贝 */
        {
            const char *te = tag_end(p, end);
            if (te <= p) te = p + 1;
            sb_put(out, p, (size_t)(te - p));
            p = te;
        }
    }

    *n_skipped = nrows - ri;
    return out->bad ? -1 : 0;
}

/* ── styles.xml 组装 ─────────────────────────────────────────────────── */
typedef struct {
    const char *fills_open;       /* "<fills ...>" 起点 */
    const char *fills_open_end;
    const char *fills_close;      /* "</fills>" 起点 */
    const char *xfs_open;
    const char *xfs_open_end;
    const char *xfs_close;
    int n_fills;
} spills;

static int scan_styles(const char *s, const char *end, spills *sp)
{
    const char *p = s;

    memset(sp, 0, sizeof *sp);
    while (p < end) {
        if (*p != '<') { p++; continue; }
        if (tag_name_is(p, end, "fills")) {
            sp->fills_open = p;
            sp->fills_open_end = tag_end(p, end);
            sp->fills_close = NULL;
            {
                const char *q = sp->fills_open_end;
                while (q < end) {
                    if (is_close_tag(q, end, "fills")) { sp->fills_close = q; break; }
                    if (*q == '<') q = tag_end(q, end);
                    else q++;
                }
            }
        } else if (tag_name_is(p, end, "cellXfs")) {
            sp->xfs_open = p;
            sp->xfs_open_end = tag_end(p, end);
            sp->xfs_close = NULL;
            {
                const char *q = sp->xfs_open_end;
                while (q < end) {
                    if (is_close_tag(q, end, "cellXfs")) { sp->xfs_close = q; break; }
                    if (*q == '<') q = tag_end(q, end);
                    else q++;
                }
            }
        }
        p = tag_end(p, end);
        if (p <= s) p = s + 1;
    }
    return (sp->fills_open && sp->fills_close && sp->xfs_open && sp->xfs_close) ? 0 : -1;
}

/* 数 <fills> 里已有几个 <fill>（不信 count 属性，Excel 有时写不准） */
static int count_fills(const char *p, const char *end)
{
    int n = 0;

    while (p < end) {
        if (tag_name_is(p, end, "fill")) n++;
        p = tag_end(p, end);
        if (p >= end) break;
    }
    return n;
}

static int collect_xfs(sty *st, const char *p, const char *end)
{
    int n = 0, cap = 0;
    char **arr = NULL;
    size_t *len = NULL;

    while (p < end) {
        if (tag_name_is(p, end, "xf")) {
            const char *te = tag_end(p, end);

            if (!(te > p + 1 && te[-2] == '/')) {       /* 带 <alignment/> 的那种 */
                const char *q = te;
                while (q + 4 <= end) {
                    if (q[0] == '<' && q[1] == '/' && q[2] == 'x' && q[3] == 'f') {
                        const char *gt = (const char *)memchr(q, '>', (size_t)(end - q));
                        if (gt) { te = gt + 1; break; }
                    }
                    q++;
                }
            }
            if (n == cap) {
                int c = cap ? cap * 2 : 16;
                char **a2 = (char **)realloc(arr, (size_t)c * sizeof(char *));
                size_t *l2 = (size_t *)realloc(len, (size_t)c * sizeof(size_t));
                if (!a2 || !l2) { free(a2 ? a2 : arr); free(l2 ? l2 : len); return -1; }
                arr = a2;
                len = l2;
                cap = c;
            }
            arr[n] = (char *)malloc((size_t)(te - p) + 1);
            if (!arr[n]) return -1;
            memcpy(arr[n], p, (size_t)(te - p));
            arr[n][te - p] = 0;
            len[n] = (size_t)(te - p);
            n++;
            p = te;
        } else {
            p = tag_end(p, end);
            if (p >= end) break;
        }
    }
    st->xf = arr;
    st->xfl = len;
    st->n_xf = n;
    return 0;
}

static void emit_fill(sb *b, int color)
{
    sb_puts(b, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF");
    sb_puts(b, HEX[color]);
    sb_puts(b, "\"/><bgColor indexed=\"64\"/></patternFill></fill>");
}

/* ── 组装结果文件 ────────────────────────────────────────────────────── */
/* 一个要落进 zip 的部件：名字 + 内容（内容由 write_copy 复制，不接管） */
typedef struct {
    const char *name;
    const sb *data;
} part;

static int write_copy(const char *dst, bs_zip *z, const part *rep, int nrep,
                      const part *add, int nadd,
                      char *out_path, size_t out_cap, bs_err *e)
{
    bs_zipw *w = bs_zipw_new();
    char cand[1024];
    int i, k, rc = -1, attempt;

    if (!w) { snprintf(e->msg, sizeof e->msg, "内存不足"); return -1; }

    for (i = 0; i < bs_zip_count(z); i++) {
        const char *name = bs_zip_entry_name(z, i);
        const unsigned char *raw = NULL;
        size_t csize = 0, usize = 0;
        unsigned method = 0, crc = 0;
        char *copy;
        const sb *repl = NULL;

        if (bs_zip_entry_raw(z, i, &raw, &csize, &method, &crc, &usize,
                             e->msg, sizeof e->msg) != 0)
            goto done;

        for (k = 0; k < nrep; k++)
            if (rep[k].name && strcmp(name, rep[k].name) == 0) { repl = rep[k].data; break; }

        if (repl) {
            char *buf = (char *)malloc(repl->n + 1);
            if (!buf) { snprintf(e->msg, sizeof e->msg, "内存不足"); goto done; }
            memcpy(buf, repl->p ? repl->p : "", repl->n);
            buf[repl->n] = 0;
            if (bs_zipw_add(w, name, buf, repl->n) != 0) {
                snprintf(e->msg, sizeof e->msg, "内存不足（%s）", name);
                goto done;
            }
        } else {
            copy = (char *)malloc(csize ? csize : 1);
            if (!copy) { snprintf(e->msg, sizeof e->msg, "内存不足"); goto done; }
            memcpy(copy, raw, csize);
            if (bs_zipw_add_raw(w, name, copy, csize, usize, method, crc) != 0) {
                snprintf(e->msg, sizeof e->msg, "内存不足（%s）", name);
                goto done;
            }
        }
    }

    /* 新增部件（comments / vml / rels）：源文件里没有，直接追加 */
    for (k = 0; k < nadd; k++) {
        const sb *d = add[k].data;
        char *buf;
        if (!add[k].name || !d) continue;
        buf = (char *)malloc(d->n + 1);
        if (!buf) { snprintf(e->msg, sizeof e->msg, "内存不足"); goto done; }
        memcpy(buf, d->p ? d->p : "", d->n);
        buf[d->n] = 0;
        if (bs_zipw_add(w, add[k].name, buf, d->n) != 0) {
            snprintf(e->msg, sizeof e->msg, "内存不足（%s）", add[k].name);
            goto done;
        }
    }

    for (attempt = 0; attempt < 20; attempt++) {
        if (attempt == 0) {
            snprintf(cand, sizeof cand, "%s", dst);
        } else {
            const char *dot = strrchr(dst, '.');
            const char *s1 = strrchr(dst, '/');
            const char *s2 = strrchr(dst, '\\');
            const char *sep = s1 > s2 ? s1 : s2;
            size_t stem;

            if (dot && (!sep || dot > sep) && dot != dst) stem = (size_t)(dot - dst);
            else { dot = NULL; stem = strlen(dst); }
            if (dot) snprintf(cand, sizeof cand, "%.*s_%d%s", (int)stem, dst, attempt + 1, dot);
            else snprintf(cand, sizeof cand, "%s_%d", dst, attempt + 1);
        }
        if (bs_zipw_save(w, cand, e->msg, sizeof e->msg) == 0) {
            if (out_path && out_cap) snprintf(out_path, out_cap, "%s", cand);
            rc = 0;
            break;
        }
    }

done:
    bs_zipw_free(w);
    return rc;
}

/* ── 批注部件 ────────────────────────────────────────────────────────── */
static int chr_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int str_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        int x = chr_lower((unsigned char)*a), y = chr_lower((unsigned char)*b);
        if (x != y) return x - y;
        a++; b++;
    }
    return chr_lower((unsigned char)*a) - chr_lower((unsigned char)*b);
}

/* zip 里实际存的名字（大小写可能与约定不同）；没有返回 NULL */
static const char *zip_name_ci(bs_zip *z, const char *name)
{
    int i;

    for (i = 0; i < bs_zip_count(z); i++)
        if (strcmp(bs_zip_entry_name(z, i), name) == 0) return bs_zip_entry_name(z, i);
    for (i = 0; i < bs_zip_count(z); i++)
        if (str_icmp(bs_zip_entry_name(z, i), name) == 0) return bs_zip_entry_name(z, i);
    return NULL;
}

/* 从 prefix 起找第一个未被占用的编号（comments1.xml / vmlDrawing1.vml） */
static int free_part_no(bs_zip *z, const char *fmt, int from)
{
    char t[128];
    int i;

    for (i = from; i < from + 999; i++) {
        snprintf(t, sizeof t, fmt, i);
        if (!zip_name_ci(z, t)) return i;
    }
    return -1;
}

static void sb_xml_text(sb *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '&') sb_puts(b, "&amp;");
        else if (*s == '<') sb_puts(b, "&lt;");
        else if (*s == '>') sb_puts(b, "&gt;");
        else sb_put(b, s, 1);
    }
}

/* xl/commentsN.xml：批注文本。作者与 Python 侧同为 BOMCheck */
static void build_comments(const notes *nt, sb *out)
{
    int i;

    sb_puts(out, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n");
    sb_puts(out, "<comments xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                "<authors><author>BOMCheck</author></authors><commentList>");
    for (i = 0; i < nt->n; i++) {
        char col[8];

        col_letters(nt->v[i].col, col);
        sb_puts(out, "<comment ref=\"");
        sb_puts(out, col);
        sb_int(out, nt->v[i].row);
        sb_puts(out, "\" authorId=\"0\"><text><r><t>");
        sb_xml_text(out, nt->v[i].text);
        sb_puts(out, "</t></r></text></comment>");
    }
    sb_puts(out, "</commentList></comments>");
}

/* xl/drawings/vmlDrawingN.vml：批注的形状。没有 <x:Anchor> 也合法 ——
   Excel 按 ClientData 的 Row/Column 自动摆放，与 openpyxl 写的一致。 */
static void build_vml(const notes *nt, sb *out)
{
    int i;

    sb_puts(out, "<xml xmlns:v=\"urn:schemas-microsoft-com:vml\""
                " xmlns:o=\"urn:schemas-microsoft-com:office:office\""
                " xmlns:x=\"urn:schemas-microsoft-com:office:excel\">");
    sb_puts(out, "<o:shapelayout v:ext=\"edit\"><o:idmap v:ext=\"edit\" data=\"1\"/></o:shapelayout>");
    sb_puts(out, "<v:shapetype id=\"_x0000_t202\" coordsize=\"21600,21600\" o:spt=\"202\""
                " path=\"m,l,21600r21600,l21600,xe\"><v:stroke joinstyle=\"miter\"/>"
                "<v:path gradientshapeok=\"t\" o:connecttype=\"rect\"/></v:shapetype>");
    for (i = 0; i < nt->n; i++) {
        sb_puts(out, "<v:shape id=\"_x0000_s");
        sb_int(out, 1025 + i);
        sb_puts(out, "\" type=\"#_x0000_t202\" style=\"position:absolute;margin-left:59.25pt;"
                    "margin-top:1.5pt;width:144px;height:79px;z-index:1;visibility:hidden\""
                    " fillcolor=\"#ffffe1\" o:insetmode=\"auto\">");
        sb_puts(out, "<v:fill color2=\"#ffffe1\"/><v:shadow color=\"black\" obscured=\"t\"/>"
                    "<v:path o:connecttype=\"none\"/>");
        sb_puts(out, "<v:textbox style=\"mso-direction-alt:auto\"><div style=\"text-align:left\"></div></v:textbox>");
        sb_puts(out, "<x:ClientData ObjectType=\"Note\"><x:MoveWithCells/><x:SizeWithCells/>"
                    "<x:AutoFill>False</x:AutoFill><x:Row>");
        sb_int(out, nt->v[i].row - 1);
        sb_puts(out, "</x:Row><x:Column>");
        sb_int(out, nt->v[i].col - 1);
        sb_puts(out, "</x:Column></x:ClientData></v:shape>");
    }
    sb_puts(out, "</xml>");
}

/* p 起是否忽略大小写等于 lit（不越过 end） */
static int ci_match(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit), i;

    if ((size_t)(end - p) < n) return 0;
    for (i = 0; i < n; i++)
        if (chr_lower((unsigned char)p[i]) != chr_lower((unsigned char)lit[i])) return 0;
    return 1;
}

static const char *ci_find(const char *start, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    const char *p;

    if (!n) return start;
    for (p = start; (size_t)(end - p) >= n; p++)
        if (ci_match(p, end, lit)) return p;
    return NULL;
}

/* 在 [Content_Types].xml 里补 vml 默认扩展与 comments 的 override。
   已经有的不重复加（原文件可能自带批注）。 */
static int build_content_types(const char *src, size_t len, const char *comment_part,
                               sb *out, bs_err *e)
{
    const char *end = src + len;
    const char *types = ci_find(src, end, "<Types");
    const char *close = ci_find(src, end, "</Types>");
    const char *te;

    if (!types || !close || close < types) {
        snprintf(e->msg, sizeof e->msg, "[Content_Types].xml 结构不认识（缺 Types）");
        return -1;
    }
    te = (const char *)memchr(types, '>', (size_t)(end - types));
    if (!te || te > close) {
        snprintf(e->msg, sizeof e->msg, "[Content_Types].xml 开标签不完整");
        return -1;
    }

    sb_put(out, src, (size_t)(te + 1 - src));
    if (!ci_find(src, close, "extension=\"vml\""))
        sb_puts(out, "<Default Extension=\"vml\" ContentType=\"application/vnd.openxmlformats-officedocument.vmlDrawing\"/>");
    sb_put(out, te + 1, (size_t)(close - (te + 1)));
    sb_puts(out, "<Override PartName=\"/");
    sb_puts(out, comment_part);
    sb_puts(out, "\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml\"/>");
    sb_put(out, close, (size_t)(end - close));
    return 0;
}

/* sheetN.xml.rels：vmlDrawing + comments 两条关系。原文件已有 rels 时追加，
   并把 rId 取到现有最大值之后，避免撞号。 */
static int build_rels(const char *src, size_t len, int has_src,
                      const char *vml_target, const char *comment_target,
                      int rid_vml, int rid_cmt, sb *out)
{
    static const char VML_TYPE[] =
        "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/vmlDrawing\" Target=\"";
    static const char CMT_TYPE[] =
        "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments\" Target=\"";
    const char *end = src + len, *close = NULL;
    int have_vml = 0, have_cmt = 0;

    if (has_src) {
        close = ci_find(src, end, "</Relationships>");
        have_vml = ci_find(src, end, "relationships/vmldrawing") != NULL;
        have_cmt = ci_find(src, end, "relationships/comments\"") != NULL;
    }

    if (close) sb_put(out, src, (size_t)(close - src));
    else {
        sb_puts(out, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n");
        sb_puts(out, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    }
    if (!have_vml) {
        sb_puts(out, "<Relationship Id=\"rId"); sb_int(out, rid_vml);
        sb_puts(out, VML_TYPE); sb_puts(out, vml_target); sb_puts(out, "\"/>");
    }
    if (!have_cmt) {
        sb_puts(out, "<Relationship Id=\"rId"); sb_int(out, rid_cmt);
        sb_puts(out, CMT_TYPE); sb_puts(out, comment_target); sb_puts(out, "\"/>");
    }
    if (close) sb_put(out, close, (size_t)(end - close));
    else sb_puts(out, "</Relationships>");
    return 0;
}

/* rels 里已用的最大 rId 号：新建关系的编号从它往后排 */
static int rels_max_rid(const char *src, size_t len)
{
    const char *end = src + len, *p;
    int mx = 0;

    for (p = src; (size_t)(end - p) >= 4; p++) {
        if (!ci_match(p, end, "Id=\"rId")) continue;
        p += 7;
        {
            int v = 0, any = 0;
            while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
            if (any && v > mx) mx = v;
        }
    }
    return mx;
}

/* ── 公共流程 ────────────────────────────────────────────────────────── */
static int mark_core(const bs_sheet *s, const char *src, marks *m, int note_mode,
                     const char *dst, char *out_path, size_t out_cap, bs_err *e)
{
    bs_zip *z = NULL;
    bs_workbook *wb = NULL;
    unsigned char *styles = NULL, *sheet = NULL;
    unsigned char *ct = NULL, *rels = NULL;
    size_t styles_len = 0, sheet_len = 0, ct_len = 0, rels_len = 0;
    char styles_name[128] = "", sheet_name[256] = "";
    char rels_name[320] = "", cmt_name[96] = "", vml_name[96] = "";
    const char *ct_real = NULL;
    char err[256];
    char extra[160] = "";
    sb styles_new = {0}, sheet_new = {0}, ct_new = {0}, rels_new = {0};
    sb cmt_new = {0}, vml_new = {0};
    part rep[4], add[4];
    int nrep = 0, nadd = 0;
    notes nt = {0};
    int has_rels = 0, note_skip = 0, rid_vml = 1;
    sty st;
    spills sp;
    markrow *rows = NULL;
    int nrows = 0, n_skip = 0, rc = -1;
    const char *sname = bs_sheet_name(s);
    bs_err dummy;
    int idx;

    memset(&st, 0, sizeof st);
    err[0] = 0;
    if (!e) e = &dummy;
    e->msg[0] = 0;

    if (!s || !src || !dst) {
        if (e) snprintf(e->msg, sizeof e->msg, "内部错误：空指针");
        return -1;
    }
    if (!sname || !*sname) {
        if (e) snprintf(e->msg, sizeof e->msg,
                        "不知道源文件里是哪张表，无法标注（from_grid 之后要补表名）");
        return -1;
    }

    wb = bs_xlsx_open(src, e);
    if (!wb) return -1;
    idx = bs_xlsx_find_sheet(wb, sname);
    if (idx < 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "源文件里找不到工作表「%s」", sname);
        bs_xlsx_close(wb);
        return -1;
    }
    snprintf(sheet_name, sizeof sheet_name, "%s", bs_xlsx_sheet_part(wb, idx));
    bs_xlsx_close(wb);

    if (bs_zip_open(src, &z, err, sizeof err) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "%s", err);
        return -1;
    }

    /* styles.xml 路径不一定是 xl/styles.xml，按 [Content_Types] 里的惯例走，
       找不到再退回默认名 */
    snprintf(styles_name, sizeof styles_name, "xl/styles.xml");
    if (bs_zip_read(z, styles_name, &styles, &styles_len, err, sizeof err) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "缺 styles.xml：%.180s", err);
        goto done;
    }
    if (bs_zip_read(z, sheet_name, &sheet, &sheet_len, err, sizeof err) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "缺工作表部件：%.160s", err);
        goto done;
    }

    if (scan_styles((const char *)styles, (const char *)styles + styles_len, &sp) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "styles.xml 结构不认识（缺 fills / cellXfs）");
        goto done;
    }
    st.fill_base = count_fills(sp.fills_open_end, sp.fills_close);
    if (collect_xfs(&st, sp.xfs_open_end, sp.xfs_close) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足");
        goto done;
    }

    if (build_rows(m, &rows, &nrows) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足（标注表）");
        goto done;
    }

    /* ── 批注：只给重复料号的料号格 ────────────────────────────────── */
    if (collect_notes(&nt, rows, nrows, s, note_mode) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足（批注表）");
        goto done;
    }
    if (nt.n > 0) {
        /* 源表自带批注/控件：<legacyDrawing> 在 schema 里只能有一个，
           再挂一条就是坏文件。宁可少批注，也不能把原批注挤掉。 */
        if (ci_find((const char *)sheet, (const char *)sheet + sheet_len, "<legacyDrawing")) {
            note_skip = 1;
            nt.n = 0;
        }
    }
    if (nt.n > 0) {
        const char *slash = strrchr(sheet_name, '/');
        int no_cmt = free_part_no(z, "xl/comments%d.xml", 1);
        int no_vml = free_part_no(z, "xl/drawings/vmlDrawing%d.vml", 1);

        ct_real = zip_name_ci(z, "[Content_Types].xml");
        if (!ct_real || no_cmt < 0 || no_vml < 0) {
            if (e) snprintf(e->msg, sizeof e->msg,
                            no_cmt < 0 || no_vml < 0 ? "批注部件编号用尽"
                                                     : "缺 [Content_Types].xml，无法挂批注");
            goto done;
        }
        snprintf(cmt_name, sizeof cmt_name, "xl/comments%d.xml", no_cmt);
        snprintf(vml_name, sizeof vml_name, "xl/drawings/vmlDrawing%d.vml", no_vml);

        if (bs_zip_read(z, ct_real, &ct, &ct_len, err, sizeof err) != 0) {
            if (e) snprintf(e->msg, sizeof e->msg, "读 [Content_Types].xml 失败：%.160s", err);
            goto done;
        }
        if (slash)
            snprintf(rels_name, sizeof rels_name, "%.*s/_rels/%s.rels",
                     (int)(slash - sheet_name), sheet_name, slash + 1);
        else
            snprintf(rels_name, sizeof rels_name, "_rels/%s.rels", sheet_name);
        has_rels = bs_zip_read(z, rels_name, &rels, &rels_len, err, sizeof err) == 0;
        err[0] = 0;

        /* 原 rels 已挂过 vmlDrawing 却没有 legacyDrawing：关系是死的，
           再挂一条会和它抢同一个部件，直接放弃批注 */
        if (has_rels &&
            ci_find((const char *)rels, (const char *)rels + rels_len, "relationships/vmlDrawing")) {
            note_skip = 1;
            nt.n = 0;
        }
    }
    if (nt.n > 0) {
        char vml_target[128], cmt_target[128];

        build_comments(&nt, &cmt_new);
        build_vml(&nt, &vml_new);
        if (build_content_types((const char *)ct, ct_len, cmt_name, &ct_new, e) != 0)
            goto done;

        snprintf(vml_target, sizeof vml_target, "/%s", vml_name);
        snprintf(cmt_target, sizeof cmt_target, "/%s", cmt_name);
        rid_vml = rels_max_rid(has_rels ? (const char *)rels : "", has_rels ? rels_len : 0) + 1;
        build_rels(has_rels ? (const char *)rels : "", has_rels ? rels_len : 0, has_rels,
                   vml_target, cmt_target, rid_vml, rid_vml + 1, &rels_new);

        snprintf(extra, sizeof extra, "<legacyDrawing r:id=\"rId%d\"/>", rid_vml);
        if (ct_new.bad || rels_new.bad || cmt_new.bad || vml_new.bad) {
            if (e) snprintf(e->msg, sizeof e->msg, "内存不足（批注部件）");
            goto done;
        }
    }

    if (rewrite_sheet((const char *)sheet, sheet_len, rows, nrows, &st,
                      &sheet_new, &n_skip, extra[0] ? extra : NULL) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足（重写工作表）");
        goto done;
    }

    /* 组装 styles：fills 末尾追加 4 个，cellXfs 追加克隆出来的 xf */
    {
        const char *s0 = (const char *)styles;
        int nf = st.fill_base;

        sb_put(&styles_new, s0, (size_t)(sp.fills_open - s0));
        emit_tag_count(&styles_new, sp.fills_open, sp.fills_open_end, nf + 4);
        sb_put(&styles_new, sp.fills_open_end,
               (size_t)(sp.fills_close - sp.fills_open_end));
        emit_fill(&styles_new, BS_MARK_ADD);
        emit_fill(&styles_new, BS_MARK_DEL);
        emit_fill(&styles_new, BS_MARK_CHG);
        emit_fill(&styles_new, BS_MARK_DUP);
        sb_put(&styles_new, sp.fills_close, (size_t)(sp.xfs_open - sp.fills_close));

        emit_tag_count(&styles_new, sp.xfs_open, sp.xfs_open_end, st.n_xf + st.n_extra);
        sb_put(&styles_new, sp.xfs_open_end, (size_t)(sp.xfs_close - sp.xfs_open_end));
        if (st.extra.p) sb_put(&styles_new, st.extra.p, st.extra.n);
        sb_put(&styles_new, sp.xfs_close, (size_t)((s0 + styles_len) - sp.xfs_close));
        if (styles_new.bad || sheet_new.bad) {
            if (e) snprintf(e->msg, sizeof e->msg, "内存不足（组装）");
            goto done;
        }
    }

    rep[nrep].name = styles_name; rep[nrep++].data = &styles_new;
    rep[nrep].name = sheet_name;  rep[nrep++].data = &sheet_new;
    if (nt.n > 0) {
        rep[nrep].name = ct_real; rep[nrep++].data = &ct_new;
        if (has_rels) { rep[nrep].name = rels_name; rep[nrep++].data = &rels_new; }
        else { add[nadd].name = rels_name; add[nadd++].data = &rels_new; }
        add[nadd].name = cmt_name; add[nadd++].data = &cmt_new;
        add[nadd].name = vml_name; add[nadd++].data = &vml_new;
    }

    rc = write_copy(dst, z, rep, nrep, add, nadd, out_path, out_cap, e);
    if (rc == 0 && e) {
        if (n_skip > 0)
            snprintf(e->msg, sizeof e->msg, "有 %d 行不在工作表里，已跳过", n_skip);
        else if (note_skip)
            snprintf(e->msg, sizeof e->msg, "源表已有批注，本次未追加重复料号批注");
    }

done:
    free(styles);
    free(sheet);
    free(ct);
    free(rels);
    sb_free(&styles_new);
    sb_free(&sheet_new);
    sb_free(&ct_new);
    sb_free(&rels_new);
    sb_free(&cmt_new);
    sb_free(&vml_new);
    notes_free(&nt);
    sty_free(&st);
    free_rows(rows, nrows);
    bs_zip_close(z);
    if (rc != 0 && e && !e->msg[0]) snprintf(e->msg, sizeof e->msg, "标注失败");
    return rc;
}

/* ── 对外两个入口 ────────────────────────────────────────────────────── */
int bs_mark_workbook(const bs_sheet *s, const bs_sheet *other,
                     const char *src_utf8, const bs_pair *p,
                     size_t np, int side, const char *dst_utf8,
                     char *out_path, size_t out_cap, bs_err *e)
{
    marks m;
    size_t i;
    int rc;

    memset(&m, 0, sizeof m);
    if (!s || !p) {
        if (e) snprintf(e->msg, sizeof e->msg, "内部错误：空指针");
        return -1;
    }

    for (i = 0; i < np; i++) {
        const bs_pair *pr = &p[i];
        /* 两个索引分属两张表：拿错表去查会把 oi 当成 ni，越界得到 NULL，
           于是「另一侧缺行」的兜底分支被误触发，整行错标 */
        const bs_row *a = pr->oi < 0 ? NULL
            : bs_sheet_row(side == BS_SIDE_OLD ? s : other, pr->oi);
        const bs_row *b = pr->ni < 0 ? NULL
            : bs_sheet_row(side == BS_SIDE_OLD ? other : s, pr->ni);
        const bs_row *r = side == BS_SIDE_OLD ? a : b;

        if (!r) continue;                     /* 这一侧没有这行 */

        if (pr->act == BS_ACT_CHG) {
            const bs_block *bl = bs_sheet_block(s, r->block - 1);
            int pn_col, ref_col, qty_col;

            if (!bl) continue;
            pn_col = bl->col_pn;
            ref_col = bl->col_ref;
            qty_col = bl->col_qty;

            /* 变更项按位掩码来，与 12 列「变更项」同源 */
            if (pr->chg & BS_CHG_PN) mark_push(&m, r->row, pn_col, BS_MARK_CHG);
            if (pr->chg & BS_CHG_QTY) mark_push(&m, r->row, qty_col, BS_MARK_CHG);
            if (pr->chg & BS_CHG_REF) mark_push(&m, r->row, ref_col, BS_MARK_CHG);
            /* 删除线没有独立列，标在键所在的列（料号优先，无料号用位号） */
            if (pr->chg & BS_CHG_STRIKE)
                mark_push(&m, r->row, pn_col ? pn_col : ref_col, BS_MARK_CHG);
            /* 整行逐列：只涂真正不一样的那些格（归一化规则与 bs_pair 的
               row_cells_differ 一致，否则会出现「说有差异却一格没涂」） */
            if ((pr->chg & BS_CHG_CELL) && a && b) {
                const bs_sheet *so = side == BS_SIDE_OLD ? s : other;
                const bs_sheet *sn = side == BS_SIDE_OLD ? other : s;
                int c1 = a->c1 ? a->c1 : 1, c2 = a->c2 ? a->c2 : 1, c;
                char x[512], y[512];
                if (b->c1 && b->c1 < c1) c1 = b->c1;
                if (b->c2 && b->c2 > c2) c2 = b->c2;
                if (c2 > 512) c2 = 512;
                for (c = c1; c <= c2; c++) {
                    bs_norm_str(bs_sheet_text_at(so, a->row, c), x, sizeof(x));
                    bs_norm_str(bs_sheet_text_at(sn, b->row, c), y, sizeof(y));
                    if (strcmp(x, y) != 0) mark_push(&m, r->row, c, BS_MARK_CHG);
                }
            }
        } else if (pr->act == BS_ACT_DEL && side == BS_SIDE_OLD) {
            int c1 = r->c1 ? r->c1 : (r->c2 ? r->c2 : 1);
            int c2 = r->c2 ? r->c2 : c1;
            int c;
            for (c = c1; c <= c2; c++) mark_push(&m, r->row, c, BS_MARK_DEL);
        } else if (pr->act == BS_ACT_ADD && side == BS_SIDE_NEW) {
            int c1 = r->c1 ? r->c1 : (r->c2 ? r->c2 : 1);
            int c2 = r->c2 ? r->c2 : c1;
            int c;
            for (c = c1; c <= c2; c++) mark_push(&m, r->row, c, BS_MARK_ADD);
        }
    }

    /* 重复料号最后涂，与 Python mark_plan 的追加顺序一致（同格后者赢） */
    {
        int n = bs_sheet_row_count(s), i2;
        for (i2 = 0; i2 < n; i2++) {
            const bs_row *r = bs_sheet_row(s, i2);
            const bs_block *bl;
            if (r->dup_n <= 1) continue;
            bl = bs_sheet_block(s, r->block - 1);
            if (bl) mark_push(&m, r->row, bl->col_pn, BS_MARK_DUP);
        }
    }

    rc = mark_core(s, src_utf8, &m, 0, dst_utf8, out_path, out_cap, e);
    free(m.v);
    return rc;
}

int bs_mark_dups(const bs_sheet *s, const char *src_utf8, const char *dst_utf8,
                 char *out_path, size_t out_cap, bs_err *e)
{
    marks m;
    int i, n, rc;

    memset(&m, 0, sizeof m);
    if (!s) {
        if (e) snprintf(e->msg, sizeof e->msg, "内部错误：空指针");
        return -1;
    }
    n = bs_sheet_row_count(s);
    for (i = 0; i < n; i++) {
        const bs_row *r = bs_sheet_row(s, i);
        int c1, c2, c;
        if (r->dup_n <= 1) continue;
        /* 查重场景没有红绿，整行涂紫看得更清楚（与 bomcore.dup_marks 一致） */
        c1 = r->c1 ? r->c1 : (r->c2 ? r->c2 : 1);
        c2 = r->c2 ? r->c2 : c1;
        for (c = c1; c <= c2; c++) mark_push(&m, r->row, c, BS_MARK_DUP);
    }
    rc = mark_core(s, src_utf8, &m, 1, dst_utf8, out_path, out_cap, e);
    free(m.v);
    return rc;
}
