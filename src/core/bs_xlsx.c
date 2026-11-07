#include "bs_xlsx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bs_xml.h"
#include "bs_zip.h"

/* 诊断计数：解压与 xml 解析各占多少。只写不读地累加，--bench 用完打印。
   clock() 的 1 ms 粒度对单个部件偏粗，但 sheet1 这类几十 MB 的部件足够。 */
double bs_prof_inflate_ms = 0;
double bs_prof_parse_ms = 0;
void bs_prof_reset(void)
{
    bs_prof_inflate_ms = 0;
    bs_prof_parse_ms = 0;
}

static double clk_ms(clock_t a, clock_t b)
{
    return (double)(b - a) * 1000.0 / (double)CLOCKS_PER_SEC;
}

struct bs_workbook {
    bs_zip *z;

    char **names;
    char **parts;
    int n_sheets;

    char **sst;
    int n_sst, cap_sst;

    unsigned char *font_strike;
    int n_fonts, cap_fonts;

    int *xf_font;
    int n_xfs, cap_xfs;
};

/* ---------------- 小工具 ---------------- */
static const char *attr_get(const char *const *a, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(a[i * 2], name) == 0) return a[i * 2 + 1];
    return NULL;
}

static int ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* r:id 在有些生成器里前缀不是 r，按后缀兜底 */
static const char *attr_id(const char *const *a, int n)
{
    const char *v = attr_get(a, n, "r:id");
    int i;
    if (v) return v;
    for (i = 0; i < n; i++)
        if (ends_with(a[i * 2], ":id")) return a[i * 2 + 1];
    return NULL;
}

static int grow(void **p, int *cap, int need, size_t elem)
{
    int c = *cap ? *cap : 16;
    void *np;
    if (need <= *cap) return 1;
    while (c < need) c *= 2;
    np = realloc(*p, (size_t)c * elem);
    if (!np) return 0;
    *p = np;
    *cap = c;
    return 1;
}

static char *dup_str(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* 列号："B12" -> 2；非法返回 0（Python 侧正则不匹配就直接跳过该格）。
   超过 Excel 上限（XFD / 16384）或字母超过 3 个一律当非法：
   旧实现在 7 个 'Z' 时会 int 溢出成负数，负列号再被当下标写就会越界。 */
static int col_from_ref(const char *s)
{
    int n = 0, letters = 0;
    if (!s) return 0;
    while (*s >= 'A' && *s <= 'Z') {
        if (++letters > 3) return 0;
        n = n * 26 + (*s - 'A' + 1);
        if (n > BS_MAX_COL) return 0;
        s++;
    }
    if (n == 0 || *s < '0' || *s > '9') return 0;
    return n;
}

/* 行号：超过 1048576 视为非法（返回 0），避免 int 溢出。 */
static int row_from_ref(const char *s)
{
    long n = 0;
    if (!s) return 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        if (n > BS_MAX_ROW) return 0;
        s++;
    }
    return (int)n;
}

/* <row r="..."> 的行号：超出 Excel 上限返回 -1（整行连同单元格一起忽略），
   非法/缺失时用 fallback（上一行+1）。绝不返回可能溢出的值。 */
static int row_from_attr(const char *s, int fallback)
{
    long n = 0;
    const char *p = s;
    if (!p) return fallback;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return fallback;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n > BS_MAX_ROW) return -1;
        p++;
    }
    return n > 0 ? (int)n : fallback;
}

/* ---------------- workbook.xml ---------------- */
typedef struct {
    char **ids;
    char **names;
    int n, cap_ids, cap_names;
} sheets_ctx;

static void sheets_open(void *ud, const char *name, const char *const *a, int na)
{
    sheets_ctx *c = (sheets_ctx *)ud;
    const char *nm, *id;
    if (strcmp(name, "sheet") != 0) return;
    nm = attr_get(a, na, "name");
    id = attr_id(a, na);
    if (!nm || !id) return;
    if (!grow((void **)&c->ids, &c->cap_ids, c->n + 1, sizeof(char *))) return;
    if (!grow((void **)&c->names, &c->cap_names, c->n + 1, sizeof(char *))) return;
    c->ids[c->n] = dup_str(id, strlen(id));
    c->names[c->n] = dup_str(nm, strlen(nm));
    if (c->ids[c->n] && c->names[c->n]) c->n++;
}

/* ---------------- workbook.xml.rels ---------------- */
typedef struct {
    char **ids;
    char **tgts;
    int n, cap_ids, cap_tgts;
} rels_ctx;

static void rels_open(void *ud, const char *name, const char *const *a, int na)
{
    rels_ctx *c = (rels_ctx *)ud;
    const char *id, *tg;
    if (strcmp(name, "Relationship") != 0) return;
    id = attr_get(a, na, "Id");
    tg = attr_get(a, na, "Target");
    if (!id || !tg) return;
    if (!grow((void **)&c->ids, &c->cap_ids, c->n + 1, sizeof(char *))) return;
    if (!grow((void **)&c->tgts, &c->cap_tgts, c->n + 1, sizeof(char *))) return;
    c->ids[c->n] = dup_str(id, strlen(id));
    c->tgts[c->n] = dup_str(tg, strlen(tg));
    if (c->ids[c->n] && c->tgts[c->n]) c->n++;
}

/* ---------------- sharedStrings.xml ---------------- */
typedef struct {
    char **v;
    int n, cap;
    char *buf;
    size_t len, bcap;
    int in_si, in_t;
} sst_ctx;

static int buf_put(sst_ctx *c, const char *s, size_t n)
{
    if (c->len + n + 1 > c->bcap) {
        size_t cap = c->bcap ? c->bcap : 256;
        char *np;
        while (cap < c->len + n + 1) cap *= 2;
        np = (char *)realloc(c->buf, cap);
        if (!np) return 0;
        c->buf = np;
        c->bcap = cap;
    }
    memcpy(c->buf + c->len, s, n);
    c->len += n;
    return 1;
}

static void sst_open(void *ud, const char *name, const char *const *a, int na)
{
    sst_ctx *c = (sst_ctx *)ud;
    (void)a;
    (void)na;
    if (strcmp(name, "si") == 0) { c->in_si = 1; c->len = 0; }
    else if (strcmp(name, "t") == 0 && c->in_si) c->in_t = 1;
}

static void sst_close(void *ud, const char *name)
{
    sst_ctx *c = (sst_ctx *)ud;
    if (strcmp(name, "t") == 0) {
        c->in_t = 0;
        return;
    }
    if (strcmp(name, "si") != 0 || !c->in_si) return;
    c->in_si = 0;
    if (!grow((void **)&c->v, &c->cap, c->n + 1, sizeof(char *))) return;
    {
        size_t len = c->buf ? c->len : 0;
        char *s = dup_str(c->buf ? c->buf : "", len);
        if (!s) return;
        c->v[c->n++] = s;
    }
}

static void sst_text(void *ud, const char *s, size_t n)
{
    sst_ctx *c = (sst_ctx *)ud;
    if (c->in_t) buf_put(c, s, n);
}

/* ---------------- styles.xml ---------------- */
typedef struct {
    bs_workbook *wb;
    int depth;
    int section; /* 1 = fonts，2 = cellXfs */
    int strike;
} style_ctx;

static void style_open(void *ud, const char *name, const char *const *a, int na)
{
    style_ctx *c = (style_ctx *)ud;
    c->depth++;
    if (c->depth == 2) {
        if (strcmp(name, "fonts") == 0) c->section = 1;
        else if (strcmp(name, "cellXfs") == 0) c->section = 2;
        else c->section = 0;
        return;
    }
    if (c->section == 1 && c->depth == 3 && strcmp(name, "font") == 0) {
        c->strike = 0;
        return;
    }
    if (c->section == 1 && c->depth == 4 && strcmp(name, "strike") == 0) {
        const char *v = attr_get(a, na, "val");
        c->strike = (v == NULL || strcmp(v, "1") == 0 || strcmp(v, "true") == 0);
        return;
    }
    if (c->section == 2 && c->depth == 3 && strcmp(name, "xf") == 0) {
        bs_workbook *wb = c->wb;
        const char *f = attr_get(a, na, "fontId");
        if (grow((void **)&wb->xf_font, &wb->cap_xfs, wb->n_xfs + 1, sizeof(int)))
            wb->xf_font[wb->n_xfs++] = f ? atoi(f) : 0;
    }
}

static void style_close(void *ud, const char *name)
{
    style_ctx *c = (style_ctx *)ud;
    if (c->section == 1 && c->depth == 3 && strcmp(name, "font") == 0) {
        bs_workbook *wb = c->wb;
        if (grow((void **)&wb->font_strike, &wb->cap_fonts, wb->n_fonts + 1, 1))
            wb->font_strike[wb->n_fonts++] = (unsigned char)(c->strike ? 1 : 0);
    }
    c->depth--;
}

/* ---------------- sheet ---------------- */
typedef struct {
    bs_workbook *wb;
    bs_grid *g;

    int in_data;
    int in_merges;
    int skip_row;      /* 当前 <row> 的行号超出 Excel 上限：整行忽略 */

    int cur_row, last_row_no;
    int seq_col;
    int has_cell;
    int cur_col;
    int type;          /* 0 普通 1 sst 2 inlineStr */
    int strike;

    char *vbuf;
    size_t vlen, vcap;
    int in_v, in_t;

    unsigned ord;
    int oom;
} sheet_ctx;

/* 每个串单独带 '\0'。不能只在池尾写一个 —— 后面再追加时会把上一个串的
   终止符覆盖掉，bs_grid_get 返回的指针就会一路读到下一格。 */
static int arena_put(bs_grid *g, const char *s, size_t n, unsigned *off, unsigned *len)
{
    if (g->strs_len + n + 2 > g->strs_cap) {
        size_t cap = g->strs_cap ? g->strs_cap : 4096;
        char *np;
        while (cap < g->strs_len + n + 2) cap *= 2;
        np = (char *)realloc(g->strs, cap);
        if (!np) return 0;
        g->strs = np;
        g->strs_cap = cap;
    }
    *off = (unsigned)g->strs_len;
    *len = (unsigned)n;
    memcpy(g->strs + g->strs_len, s, n);
    g->strs_len += n;
    g->strs[g->strs_len++] = 0;
    return 1;
}

static void sheet_vput(sheet_ctx *c, const char *s, size_t n)
{
    if (c->vlen + n + 1 > c->vcap) {
        size_t cap = c->vcap ? c->vcap : 64;
        char *np;
        while (cap < c->vlen + n + 1) cap *= 2;
        np = (char *)realloc(c->vbuf, cap);
        if (!np) { c->oom = 1; return; }
        c->vbuf = np;
        c->vcap = cap;
    }
    memcpy(c->vbuf + c->vlen, s, n);
    c->vlen += n;
    c->vbuf[c->vlen] = 0;
}

static void cell_commit(sheet_ctx *c)
{
    bs_grid *g = c->g;
    const char *val = c->vbuf ? c->vbuf : "";
    size_t len = c->vlen;

    if (c->type == 1) {
        int idx = c->vbuf ? atoi(c->vbuf) : 0;
        if (idx >= 0 && idx < c->wb->n_sst) {
            val = c->wb->sst[idx];
            len = strlen(val);
        } else {
            val = "";
            len = 0;
        }
    }

    if (!grow((void **)&g->cells, &g->cap_cells, g->n_cells + 1, sizeof(bs_cell))) {
        c->oom = 1;
        return;
    }
    {
        bs_cell *cell = &g->cells[g->n_cells];
        if (!arena_put(g, val, len, &cell->off, &cell->len)) {
            c->oom = 1;
            return;
        }
        cell->row = c->cur_row;
        cell->col = c->cur_col;
        cell->strike = (unsigned char)(c->strike ? 1 : 0);
        cell->ord = c->ord++;
        g->n_cells++;
        if (cell->strike) g->n_strike++;
    }
}

static void sheet_open(void *ud, const char *name, const char *const *a, int na)
{
    sheet_ctx *c = (sheet_ctx *)ud;

    if (strcmp(name, "sheetData") == 0) { c->in_data = 1; return; }

    if (strcmp(name, "mergeCells") == 0) { c->in_merges = 1; return; }
    if (c->in_merges && strcmp(name, "mergeCell") == 0) {
        const char *ref = attr_get(a, na, "ref");
        if (ref) {
            const char *colon = strchr(ref, ':');
            if (colon) {
                int r0 = row_from_ref(ref), c0 = col_from_ref(ref);
                int r1 = row_from_ref(colon + 1), c1 = col_from_ref(colon + 1);
                if (r0 && c0 && r1 && c1 &&
                    grow((void **)&c->g->merges, &c->g->cap_merges,
                         c->g->n_merges + 1, sizeof(bs_merge))) {
                    bs_merge *m = &c->g->merges[c->g->n_merges++];
                    m->r0 = r0; m->c0 = c0; m->r1 = r1; m->c1 = c1;
                }
            }
        }
        return;
    }

    if (!c->in_data) return;

    if (strcmp(name, "row") == 0) {
        const char *r = attr_get(a, na, "r");
        int rn = row_from_attr(r, c->last_row_no + 1);
        if (rn < 0) {           /* 超出 Excel 上限：这一行整行忽略，不留假数据 */
            c->skip_row = 1;
            return;
        }
        c->skip_row = 0;
        if (rn <= 0) rn = c->last_row_no + 1;
        c->cur_row = rn;
        c->seq_col = 0;
        if (rn != c->last_row_no) {
            c->g->n_rows++;
            c->last_row_no = rn;
        }
        if (rn > c->g->max_row) c->g->max_row = rn;
        return;
    }

    if (c->skip_row) return;    /* 越界行里的 <c>/<v>/<t> 一律不建档 */

    if (strcmp(name, "c") == 0) {
        const char *r = attr_get(a, na, "r");
        const char *t = attr_get(a, na, "t");
        const char *s = attr_get(a, na, "s");
        int col = r ? col_from_ref(r) : 0;

        if (r && col == 0) return; /* 非法 ref：整格跳过（同 Python 的正则不匹配） */
        if (!r) col = c->seq_col + 1;
        c->seq_col = col;
        c->has_cell = 1;
        c->cur_col = col;
        c->vlen = 0;
        if (c->vbuf) c->vbuf[0] = 0;
        c->type = 0;
        if (t) {
            if (strcmp(t, "s") == 0) c->type = 1;
            else if (strcmp(t, "inlineStr") == 0) c->type = 2;
        }
        c->strike = 0;
        if (s) {
            int si = atoi(s);
            if (si >= 0 && si < c->wb->n_xfs) {
                int fi = c->wb->xf_font[si];
                if (fi >= 0 && fi < c->wb->n_fonts) c->strike = c->wb->font_strike[fi];
            }
        }
        if (col > c->g->max_col) c->g->max_col = col;
        return;
    }

    if (!c->has_cell) return;
    if (strcmp(name, "v") == 0) c->in_v = 1;
    else if (strcmp(name, "t") == 0) c->in_t = 1;
}

static void sheet_close(void *ud, const char *name)
{
    sheet_ctx *c = (sheet_ctx *)ud;

    if (strcmp(name, "v") == 0) { c->in_v = 0; return; }
    if (strcmp(name, "t") == 0) { c->in_t = 0; return; }
    if (strcmp(name, "c") == 0) {
        if (c->has_cell) cell_commit(c);
        c->has_cell = 0;
        return;
    }
    if (strcmp(name, "sheetData") == 0) { c->in_data = 0; return; }
    if (strcmp(name, "mergeCells") == 0) { c->in_merges = 0; return; }
}

static void sheet_text(void *ud, const char *s, size_t n)
{
    sheet_ctx *c = (sheet_ctx *)ud;
    if (c->has_cell && (c->in_v || c->in_t)) sheet_vput(c, s, n);
}

/* ---------------- 排序与去重 ---------------- */
static int cell_cmp(const void *pa, const void *pb)
{
    const bs_cell *a = (const bs_cell *)pa;
    const bs_cell *b = (const bs_cell *)pb;
    if (a->row != b->row) return a->row < b->row ? -1 : 1;
    if (a->col != b->col) return a->col < b->col ? -1 : 1;
    if (a->ord != b->ord) return a->ord < b->ord ? -1 : 1;
    return 0;
}

/* 同一格写了两次：保留文档顺序靠后的那条（Python dict 后写覆盖前写） */
static void dedupe(bs_grid *g)
{
    int i, w = 0;
    for (i = 0; i < g->n_cells; i++) {
        if (w > 0 && g->cells[w - 1].row == g->cells[i].row &&
            g->cells[w - 1].col == g->cells[i].col)
            g->cells[w - 1] = g->cells[i];
        else
            g->cells[w++] = g->cells[i];
    }
    g->n_cells = w;
}

/* ---------------- 对外接口 ---------------- */
static int load_part(bs_workbook *wb, const char *part, const bsx_cb *cb,
                     int required, char *err, size_t cap)
{
    unsigned char *buf = NULL;
    size_t len = 0;
    clock_t c0, c1, c2;
    int rc;

    c0 = clock();
    if (bs_zip_read(wb->z, part, &buf, &len, err, cap) != 0) {
        if (!required) {
            if (err && cap) err[0] = 0;
            return 0;
        }
        return -1;
    }
    c1 = clock();
    rc = bsx_parse(buf, len, cb, err, cap);
    c2 = clock();
    bs_prof_inflate_ms += clk_ms(c0, c1);
    bs_prof_parse_ms += clk_ms(c1, c2);
    free(buf);
    return rc;
}

static char *resolve_part(const char *target)
{
    size_t n;
    char *out;
    if (!target) return NULL;
    while (target[0] == '/') target++;
    if (strncmp(target, "./", 2) == 0) target += 2;
    if (strncmp(target, "xl/", 3) == 0) return dup_str(target, strlen(target));
    n = strlen(target);
    out = (char *)malloc(n + 4);
    if (!out) return NULL;
    memcpy(out, "xl/", 3);
    memcpy(out + 3, target, n + 1);
    return out;
}

bs_workbook *bs_xlsx_open(const char *path_utf8, bs_err *e)
{
    bs_workbook *wb;
    char err[256] = "";
    sheets_ctx sh;
    rels_ctx rl;
    sst_ctx st;
    style_ctx sy;
    bsx_cb cb;
    int i;

    memset(&sh, 0, sizeof sh);
    memset(&rl, 0, sizeof rl);
    memset(&st, 0, sizeof st);

    wb = (bs_workbook *)calloc(1, sizeof *wb);
    if (!wb) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足");
        return NULL;
    }
    if (bs_zip_open(path_utf8, &wb->z, err, sizeof err) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "%s", err);
        bs_xlsx_close(wb);
        return NULL;
    }

    cb.ud = &sh;
    cb.on_open = sheets_open;
    cb.on_close = NULL;
    cb.on_text = NULL;
    if (load_part(wb, "xl/workbook.xml", &cb, 1, err, sizeof err) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "%s", err);
        goto fail;
    }

    cb.ud = &rl;
    cb.on_open = rels_open;
    cb.on_close = NULL;
    cb.on_text = NULL;
    if (load_part(wb, "xl/_rels/workbook.xml.rels", &cb, 1, err, sizeof err) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "%s", err);
        goto fail;
    }

    wb->n_sheets = sh.n;
    wb->names = (char **)calloc((size_t)(sh.n > 0 ? sh.n : 1), sizeof(char *));
    wb->parts = (char **)calloc((size_t)(sh.n > 0 ? sh.n : 1), sizeof(char *));
    if (!wb->names || !wb->parts) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足（工作表列表）");
        goto fail;
    }
    for (i = 0; i < sh.n; i++) {
        int k, hit = -1;
        wb->names[i] = sh.names[i];
        sh.names[i] = NULL;
        for (k = 0; k < rl.n; k++) {
            if (strcmp(rl.ids[k], sh.ids[i]) == 0) { hit = k; break; }
        }
        wb->parts[i] = resolve_part(hit >= 0 ? rl.tgts[hit] : NULL);
        if (!wb->parts[i]) {
            if (e) snprintf(e->msg, sizeof e->msg, "工作表「%s」缺少部件路径", wb->names[i]);
            goto fail;
        }
    }

    cb.ud = &st;
    cb.on_open = sst_open;
    cb.on_close = sst_close;
    cb.on_text = sst_text;
    load_part(wb, "xl/sharedStrings.xml", &cb, 0, err, sizeof err);
    wb->sst = st.v;
    wb->n_sst = st.n;
    wb->cap_sst = st.cap;
    st.v = NULL;
    free(st.buf);

    memset(&sy, 0, sizeof sy);
    sy.wb = wb;
    cb.ud = &sy;
    cb.on_open = style_open;
    cb.on_close = style_close;
    cb.on_text = NULL;
    load_part(wb, "xl/styles.xml", &cb, 0, err, sizeof err);

    for (i = 0; i < sh.n; i++) free(sh.ids[i]);
    free(sh.ids);
    free(sh.names);
    for (i = 0; i < rl.n; i++) { free(rl.ids[i]); free(rl.tgts[i]); }
    free(rl.ids);
    free(rl.tgts);
    return wb;

fail:
    for (i = 0; i < sh.n; i++) { free(sh.ids[i]); free(sh.names[i]); }
    free(sh.ids);
    free(sh.names);
    for (i = 0; i < rl.n; i++) { free(rl.ids[i]); free(rl.tgts[i]); }
    free(rl.ids);
    free(rl.tgts);
    for (i = 0; i < st.n; i++) free(st.v[i]);
    free(st.v);
    free(st.buf);
    bs_xlsx_close(wb);
    return NULL;
}

void bs_xlsx_close(bs_workbook *wb)
{
    int i;
    if (!wb) return;
    for (i = 0; i < wb->n_sheets; i++) { free(wb->names[i]); free(wb->parts[i]); }
    free(wb->names);
    free(wb->parts);
    for (i = 0; i < wb->n_sst; i++) free(wb->sst[i]);
    free(wb->sst);
    free(wb->font_strike);
    free(wb->xf_font);
    bs_zip_close(wb->z);
    free(wb);
}

int bs_xlsx_sheet_count(const bs_workbook *wb) { return wb ? wb->n_sheets : 0; }

const char *bs_xlsx_sheet_name(const bs_workbook *wb, int i)
{
    if (!wb || i < 0 || i >= wb->n_sheets) return "";
    return wb->names[i];
}

const char *bs_xlsx_sheet_part(const bs_workbook *wb, int i)
{
    if (!wb || i < 0 || i >= wb->n_sheets) return "";
    return wb->parts[i];
}

int bs_xlsx_find_sheet(const bs_workbook *wb, const char *name)
{
    int i;
    if (!wb || !name) return -1;
    for (i = 0; i < wb->n_sheets; i++)
        if (strcmp(wb->names[i], name) == 0) return i;
    return -1;
}

int bs_xlsx_sst_count(const bs_workbook *wb) { return wb ? wb->n_sst : 0; }
int bs_xlsx_font_count(const bs_workbook *wb) { return wb ? wb->n_fonts : 0; }
int bs_xlsx_xf_count(const bs_workbook *wb) { return wb ? wb->n_xfs : 0; }

int bs_xlsx_font_strike(const bs_workbook *wb, int i)
{
    if (!wb || i < 0 || i >= wb->n_fonts) return -1;
    return wb->font_strike[i];
}

void bs_grid_free(bs_grid *g)
{
    if (!g) return;
    free(g->strs);
    free(g->cells);
    free(g->merges);
    free(g);
}

/* cells 已按 (row, col, ord) 排好序时的精确查找 */
static const bs_cell *cell_find(const bs_grid *g, int row, int col)
{
    int lo = 0, hi = g->n_cells - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const bs_cell *c = &g->cells[mid];
        if (c->row == row && c->col == col) return c;
        if (c->row < row || (c->row == row && c->col < col)) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* 合并单元格：把 span 头的值（含删除线）铺到合并区内所有空位。
   Excel 文件里只有左上角存值，但 BOM 常把同一颗料的料号格竖向合并，
   不铺的话下面几行读出来是空料号，会被当成「无法建档」或按位号硬配。
   口径与 pyref/bomcore.py 的 _Grid 完全一致（同一批样本逐格对账）。 */
/* 合并展开的护栏：一个 ref="A1:XFD1048576" 能把一张表放大到百亿格。
   超过这两个上限的合并区直接跳过（与 bomcore._Grid 同规则，否则对账会分叉）。 */
#define MERGE_MAX_CELLS 4096    /* 单个合并区最多铺这么多格 */
#define MERGE_MAX_TOTAL 200000  /* 整张表最多新增这么多格 */

static int expand_merges(bs_grid *g)
{
    bs_cell *add = NULL;
    int n_add = 0, cap_add = 0, i, r, c;
    unsigned next_ord = 0;

    if (!g || g->n_merges <= 0 || g->n_cells <= 0) return 0;
    for (i = 0; i < g->n_cells; i++)
        if (g->cells[i].ord >= next_ord) next_ord = g->cells[i].ord + 1;

    for (i = 0; i < g->n_merges; i++) {
        const bs_merge *m = &g->merges[i];
        const bs_cell *head;
        long area;

        if (n_add >= MERGE_MAX_TOTAL) break;
        area = (long)(m->r1 - m->r0 + 1) * (long)(m->c1 - m->c0 + 1);
        if (area <= 1 || area > MERGE_MAX_CELLS) continue;
        head = cell_find(g, m->r0, m->c0);
        if (!head || head->len == 0) continue;      /* 空格头不铺（同 oracle） */
        for (r = m->r0; r <= m->r1; r++) {
            for (c = m->c0; c <= m->c1; c++) {
                bs_cell nc;
                if (n_add >= MERGE_MAX_TOTAL) break;
                if (r == m->r0 && c == m->c0) continue;
                if (cell_find(g, r, c)) continue;    /* 已有格子不覆盖 */
                if (n_add == cap_add) {
                    int cap = cap_add ? cap_add * 2 : 32;
                    bs_cell *p = (bs_cell *)realloc(add, (size_t)cap * sizeof(*p));
                    if (!p) { free(add); return -1; }
                    add = p;
                    cap_add = cap;
                }
                nc.row = r;
                nc.col = c;
                nc.off = head->off;
                nc.len = head->len;
                nc.strike = head->strike;
                nc.ord = next_ord++;
                add[n_add++] = nc;
            }
        }
    }
    if (n_add) {
        if (!grow((void **)&g->cells, &g->cap_cells, g->n_cells + n_add, sizeof(bs_cell))) {
            free(add);
            return -1;
        }
        memcpy(g->cells + g->n_cells, add, (size_t)n_add * sizeof(bs_cell));
        for (i = 0; i < n_add; i++) {
            const bs_cell *a = &g->cells[g->n_cells + i];
            if (a->row > g->max_row) g->max_row = a->row;
            if (a->col > g->max_col) g->max_col = a->col;
            if (a->strike) g->n_strike++;
        }
        g->n_cells += n_add;
        qsort(g->cells, (size_t)g->n_cells, sizeof(bs_cell), cell_cmp);
        dedupe(g);
    }
    free(add);
    return 0;
}

int bs_xlsx_load_sheet(bs_workbook *wb, int idx, bs_grid **out, bs_err *e)
{
    bs_grid *g;
    sheet_ctx sc;
    bsx_cb cb;
    char err[256] = "";

    if (out) *out = NULL;
    if (!wb || idx < 0 || idx >= wb->n_sheets) {
        if (e) snprintf(e->msg, sizeof e->msg, "工作表序号越界");
        return -1;
    }
    g = (bs_grid *)calloc(1, sizeof *g);
    if (!g) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足");
        return -1;
    }
    memset(&sc, 0, sizeof sc);
    sc.wb = wb;
    sc.g = g;

    cb.ud = &sc;
    cb.on_open = sheet_open;
    cb.on_close = sheet_close;
    cb.on_text = sheet_text;

    if (load_part(wb, wb->parts[idx], &cb, 1, err, sizeof err) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "%s", err);
        free(sc.vbuf);
        bs_grid_free(g);
        return -1;
    }
    free(sc.vbuf);

    if (sc.oom) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足（工作表「%s」）", wb->names[idx]);
        bs_grid_free(g);
        return -1;
    }

    if (g->n_cells > 0) qsort(g->cells, (size_t)g->n_cells, sizeof(bs_cell), cell_cmp);
    dedupe(g);
    if (expand_merges(g) != 0) {
        if (e) snprintf(e->msg, sizeof e->msg, "内存不足（展开合并单元格）");
        bs_grid_free(g);
        return -1;
    }
    if (out) *out = g;
    return 0;
}

const char *bs_grid_get(const bs_grid *g, int row, int col)
{
    int lo, hi;
    if (!g || row < 1 || col < 1 || row > g->max_row || col > g->max_col) return NULL;
    lo = 0;
    hi = g->n_cells - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const bs_cell *c = &g->cells[mid];
        if (c->row == row && c->col == col) return g->strs + c->off;
        if (c->row < row || (c->row == row && c->col < col)) lo = mid + 1;
        else hi = mid - 1;
    }
    return "";
}

int bs_grid_strike(const bs_grid *g, int row, int col)
{
    int lo, hi;
    if (!g || row < 1 || col < 1) return 0;
    lo = 0;
    hi = g->n_cells - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const bs_cell *c = &g->cells[mid];
        if (c->row == row && c->col == col) return c->strike;
        if (c->row < row || (c->row == row && c->col < col)) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}
