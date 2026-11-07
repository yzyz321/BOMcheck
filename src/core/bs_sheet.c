#include "bs_sheet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bs_arena.h"
#include "bs_pn.h"

/* 识别层分段耗时（毫秒），下标含义见 bs_sheet.h。只给 --bench 用 */
double bs_prof_sheet[4] = {0, 0, 0, 0};

static double clk_ms(clock_t a, clock_t b)
{
    return (double)(b - a) * 1000.0 / (double)CLOCKS_PER_SEC;
}

/* ---------------- 配置默认值 ---------------- */
void bs_cfg_default(bs_cfg *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->seg1 = 3;
    c->seg2 = 6;
    c->seg3 = 3;
    c->min_hit_rate = 0.60;
    c->sample_rows = 40;
    c->alias_exact = 1;
    c->header_scan_rows = 50;
    c->header_min_alias = 1;
    c->header_threshold = 5;
    c->block_mode = 1;
    c->strike_scope = 1;          /* 整行任一格 */
    c->dual_key = 1;
    c->ref_require_token = 1;
    c->ref_fuzzy = 1;
    c->ref_min_overlap = 0.60;
    c->skip_label_rows = 1;
    c->dup_policy = 0;            /* SEQUENTIAL */
    c->row_diff_all = 0;          /* 逐列比较默认关：只在界面上勾选后生效 */
}

/* ---------------- 别名表（与 bomcore.ALIAS_* 同值） ---------------- */
static const char *const ALIAS_PN[] = {
    "partnumber", "partno", "pn", "料号", "物料编码", "物料号", "物料编号",
    "物料代码", "零件号", "产品料号", "子件代码", "子件料号", "元件编码"
};
static const char *const ALIAS_REF[] = {
    "reference", "ref", "位号", "元件位号", "位置号", "designator",
    "referencedesignator", "子件位号"
};
static const char *const ALIAS_QTY[] = {
    "quantity", "qty", "数量", "用量", "单机用量", "子件需求数量",
    "需求数量", "单机数量", "子件数量"
};
#define N_OF(t) ((int)(sizeof(t) / sizeof((t)[0])))

static int in_list(const char *k, const char *const *al, int n)
{
    int i;

    if (!k || !*k) return 0;
    for (i = 0; i < n; i++)
        if (strcmp(k, al[i]) == 0) return 1;
    return 0;
}


/* ---------------- 网格小工具 ---------------- */
static const char *cell_text(const bs_grid *g, const bs_cell *c)
{
    return g->strs + c->off;
}

/* cells 按 (row, col, ord) 有序，二分找 row >= r 的第一个 */
static int row_lower(const bs_grid *g, int r)
{
    int lo = 0, hi = g->n_cells;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (g->cells[mid].row < r) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int cell_empty(const bs_cell *c)
{
    return c->len == 0;
}

/* 按码点计长度（Python 的 len(str)）。非法字节各算一个 */
static int cp_len(const char *s)
{
    int n = 0;

    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* 取第 r 行在 cells 里的区间 [*i0, *i1)。没有该行时返回 0。
   注意：下面的扫描都用「顺序游标」而不是逐行二分 —— 40 万格的表上每行两次
   二分就是 8 百多万次随机访存，比真正的计算还贵（实测占了一半时间）。 */
static int row_span(const bs_grid *g, int r, int *i0, int *i1)
{
    int lo = row_lower(g, r), hi = lo;

    while (hi < g->n_cells && g->cells[hi].row == r) hi++;
    *i0 = lo;
    *i1 = hi;
    return lo < hi;
}

static int row_nonempty_span(const bs_grid *g, int i0, int i1)
{
    int i, n = 0;

    for (i = i0; i < i1; i++)
        if (!cell_empty(&g->cells[i])) n++;
    return n;
}

static int row_has_wide_pn_span(const bs_grid *g, int i0, int i1)
{
    int i;
    char buf[512];

    for (i = i0; i < i1; i++) {
        int w = 0, st = 0;
        const char *v = cell_text(g, &g->cells[i]);
        if (!*v) continue;
        bs_pn_str(v, buf, sizeof(buf), &w, &st);
        if (w) return 1;
    }
    return 0;
}

static int row_nonempty(const bs_grid *g, int r)
{
    int i0, i1;

    if (!row_span(g, r, &i0, &i1)) return 0;
    return row_nonempty_span(g, i0, i1);
}

static int row_has_wide_pn(const bs_grid *g, int r)
{
    int i0, i1;

    if (!row_span(g, r, &i0, &i1)) return 0;
    return row_has_wide_pn_span(g, i0, i1);
}

/* ---------------- 表头打分 ---------------- */
/* min_alias 不满足时直接返回 -99：后面那几项加分（下一行像不像料号）就别算了。
   Python 是算完整分再由调用方筛，结论一样 —— 数据行永远过不了 na >= min_alias
   这一关，所以省下的是纯开销：5 万行的样本靠这一条从 870 ms 降到 200 ms 出头。 */
static int row_score(const bs_grid *g, int a0, int a1, int n0, int n1,
                     int min_alias, int *n_alias_out)
{
    int i = a0, e = a1;
    int n_cells = 0, n_long = 0, hit_pn = 0, hit_ref = 0, hit_qty = 0;
    char nm[512];

    if (n_alias_out) *n_alias_out = 0;
    if (i >= e) return -99;

    for (; i < e; i++) {
        const char *v = cell_text(g, &g->cells[i]);
        if (!*v) continue;
        n_cells++;
        if (cp_len(v) > 40) n_long++;
        bs_norm_str(v, nm, sizeof(nm));
        if (!nm[0]) continue;
        if (!hit_pn && in_list(nm, ALIAS_PN, N_OF(ALIAS_PN))) hit_pn = 1;
        else if (!hit_ref && in_list(nm, ALIAS_REF, N_OF(ALIAS_REF))) hit_ref = 1;
        else if (!hit_qty && in_list(nm, ALIAS_QTY, N_OF(ALIAS_QTY))) hit_qty = 1;
    }
    if (!n_cells) return -99;

    {
        int na = hit_pn + hit_ref + hit_qty;
        int score, nxt;

        if (n_alias_out) *n_alias_out = na;
        if (na < min_alias) return -99;      /* 当不了表头，省掉下面的 pn 判定 */
        score = na == 1 ? 2 : (na == 2 ? 4 : 6);
        if (n1 > n0 && row_has_wide_pn_span(g, n0, n1)) score += 4;
        if (n_cells >= 3) score += 1;
        nxt = row_nonempty_span(g, n0, n1);
        if (nxt && (double)nxt >= 0.7 * (double)n_cells) score += 2;
        if ((double)n_long / (double)(n_cells ? n_cells : 1) > 0.4) score -= 3;
        return score;
    }
}

typedef struct {
    int *v, n, cap;
} ivec;

static void ivec_push(ivec *a, int x)
{
    if (a->n == a->cap) {
        int nc = a->cap ? a->cap * 2 : 16;
        int *p = (int *)realloc(a->v, (size_t)nc * sizeof(int));
        if (!p) return;
        a->v = p;
        a->cap = nc;
    }
    a->v[a->n++] = x;
}

/* 无列名命中时：靠「后面连续多行像料号」倒推表头 */
static void fallback_blocks(const bs_grid *g, ivec *out)
{
    int i, last_row = -1;

    for (i = 0; i < g->n_cells; i++) {
        int r = g->cells[i].row, k, hits = 0;
        if (r == last_row) continue;
        last_row = r;
        if (row_nonempty(g, r) < 3) continue;
        if (row_has_wide_pn(g, r)) continue;
        for (k = 1; k <= 5; k++)
            if (row_has_wide_pn(g, r + k)) hits++;
        if (hits >= 4) ivec_push(out, r);
    }
}

static int *header_blocks(const bs_grid *g, const bs_cfg *c, int *out_n)
{
    ivec cand = {NULL, 0, 0};
    int i = 0;

    while (i < g->n_cells) {
        int r = g->cells[i].row, a0 = i, a1, n0 = 0, n1 = 0, na = 0, sc, j;

        while (i < g->n_cells && g->cells[i].row == r) i++;
        a1 = i;
        /* 前瞻下一行，但不能动 i —— 它还要从下一行开头继续扫 */
        j = i;
        if (j < g->n_cells && g->cells[j].row == r + 1) {
            n0 = j;
            while (j < g->n_cells && g->cells[j].row == r + 1) j++;
            n1 = j;
        }
        sc = row_score(g, a0, a1, n0, n1, c->header_min_alias, &na);
        if (na >= c->header_min_alias && sc >= c->header_threshold)
            ivec_push(&cand, r);
    }
    if (!cand.n) fallback_blocks(g, &cand);
    *out_n = cand.n;
    return cand.v;
}

/* ---------------- 列定位 ---------------- */
/* 网格里最后一个有内容的行号（cells 已按 row 排序）。空表返回 0。
   行扫描的上界不能直接用 max_row：损坏文件可以把 <row r> 写成 10 亿，
   那样每张表都要扫十亿个空行（实测 1e9 行号要 3 秒）。 */
static int last_content_row(const bs_grid *g)
{
    int i;

    for (i = g->n_cells - 1; i >= 0; i--)
        if (!cell_empty(&g->cells[i])) return g->cells[i].row;
    return 0;
}

static char *present_cols(const bs_grid *g)
{
    char *p;
    int i;

    if (g->max_col <= 0) return NULL;
    p = (char *)calloc((size_t)g->max_col + 1, 1);
    if (!p) return NULL;
    for (i = 0; i < g->n_cells; i++) {
        const int col = g->cells[i].col;
        /* 解析层已经限幅，这里再兜一道：任何越界列都不许当位图下标 */
        if (col > 0 && col <= g->max_col) p[col] = 1;
    }
    return p;
}

/* 双通道：列名别名 + 内容格式命中率 */
static int find_pn_column(const bs_grid *g, const bs_cfg *c, int hr, int end,
                          double *rate_out)
{
    char *present = present_cols(g);
    int c1, best = 0, best_alias = 0, sample, last;
    double best_rate = 0.0;

    if (rate_out) *rate_out = 0.0;
    if (!present) return 0;

    sample = c->sample_rows > 0 ? c->sample_rows : 40;
    last = hr + sample;
    if (last > end) last = end;

    for (c1 = 1; c1 <= g->max_col; c1++) {
        int by_alias, tot = 0, hit = 0, r;
        double rate;
        char nm[512], key[512];

        if (!present[c1]) continue;
        bs_norm_str(bs_grid_get(g, hr, c1), nm, sizeof(nm));
        by_alias = in_list(nm, ALIAS_PN, N_OF(ALIAS_PN));

        for (r = hr + 1; r <= last; r++) {
            const char *v = bs_grid_get(g, r, c1);
            int w = 0, st = 0;
            if (!v || !*v) continue;
            bs_pn_str(v, key, sizeof(key), &w, &st);
            if (!key[0]) continue;
            tot++;
            if (w) hit++;
        }
        rate = tot ? (double)hit / (double)tot : 0.0;
        if (rate < c->min_hit_rate) continue;
        /* rank = (by_alias, rate, -c)，c 升序遍历所以并列时保留小的 */
        if (!best || by_alias > best_alias ||
            (by_alias == best_alias && rate > best_rate)) {
            best = c1;
            best_alias = by_alias;
            best_rate = rate;
        }
    }
    free(present);
    if (rate_out) *rate_out = best_rate;
    return best;
}

static int locate(const bs_grid *g, int hr, const char *const *al, int n)
{
    int i = row_lower(g, hr), e = row_lower(g, hr + 1);
    char nm[512];

    for (; i < e; i++) {
        bs_norm_str(cell_text(g, &g->cells[i]), nm, sizeof(nm));
        if (in_list(nm, al, n)) return g->cells[i].col;
    }
    return 0;
}

/* 无位号列名时按形态命中率猜（≥50% 才认） */
static int guess_ref_column(const bs_grid *g, int hr, int end, int exclude)
{
    char *present = present_cols(g);
    int c1, best = 0, last = hr + 40;
    double best_ratio = 0.0;
    char buf[1024];

    if (!present) return 0;
    if (last > end) last = end;

    for (c1 = 1; c1 <= g->max_col; c1++) {
        int tot = 0, ok = 0, r;
        double ratio;
        char nm[512];

        if (!present[c1] || c1 == exclude) continue;
        bs_norm_str(bs_grid_get(g, hr, c1), nm, sizeof(nm));
        if (in_list(nm, ALIAS_QTY, N_OF(ALIAS_QTY))) continue;
        if (in_list(nm, ALIAS_PN, N_OF(ALIAS_PN))) continue;

        for (r = hr + 1; r <= last; r++) {
            const char *v = bs_grid_get(g, r, c1);
            int n_ok = 0, n_bad = 0;
            if (!v || !*v) continue;
            tot++;
            bs_ref_sig_str(v, buf, sizeof(buf), &n_ok, &n_bad);
            if (n_ok > 0 && n_bad == 0) ok++;
        }
        if (tot < 3) continue;
        ratio = (double)ok / (double)tot;
        if (ratio >= 0.5 && (!best || ratio > best_ratio)) {
            best = c1;
            best_ratio = ratio;
        }
    }
    free(present);
    return best;
}

static void col_name_of(int c, char *out, size_t cap)
{
    char tmp[8];
    int i = 0, n = 0;

    out[0] = 0;
    if (c <= 0) return;
    while (c && n < 7) {
        int r = (c - 1) % 26;
        tmp[n++] = (char)('A' + r);
        c = (c - 1) / 26;
    }
    if (n + 1 > (int)cap) n = (int)cap - 1;
    while (n > 0) out[i++] = tmp[--n];
    out[i] = 0;
}

/* ---------------- 行装配 ---------------- */
struct bs_sheet {
    bs_grid *g;
    bs_block *blocks;
    int n_blocks;
    bs_row *rows;
    int n_rows, cap_rows;
    int labels, unkeyable;
    double hit_rate;
    bs_arena a;
    int *label_rows, n_label_rows;
    char name_pn[8], name_ref[8], name_qty[8];
    char name[72];                 /* 工作表名，from_grid 时为空 */
    char *sigbuf;
    size_t sigcap;
};

static int is_label_row(const bs_sheet *s, int r)
{
    int lo = 0, hi = s->n_label_rows;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (s->label_rows[mid] < r) lo = mid + 1;
        else hi = mid;
    }
    return lo < s->n_label_rows && s->label_rows[lo] == r;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* 跨列合并（跨度 ≥ 3 列且只占一行）就是分组标题行，不建档 */
static void collect_label_rows(bs_sheet *s)
{
    int i, n = 0;

    if (!s->g->n_merges) return;
    s->label_rows = (int *)malloc((size_t)s->g->n_merges * sizeof(int));
    if (!s->label_rows) return;
    for (i = 0; i < s->g->n_merges; i++) {
        const bs_merge *m = &s->g->merges[i];
        if (m->r1 == m->r0 && m->c1 - m->c0 >= 3)
            s->label_rows[n++] = m->r0;
    }
    if (!n) {
        free(s->label_rows);
        s->label_rows = NULL;
        return;
    }
    qsort(s->label_rows, (size_t)n, sizeof(int), cmp_int);
    /* 去重 */
    {
        int w = 1;
        for (i = 1; i < n; i++)
            if (s->label_rows[i] != s->label_rows[w - 1])
                s->label_rows[w++] = s->label_rows[i];
        n = w;
    }
    s->n_label_rows = n;
}

static int parse_qty(const char *s, double *out)
{
    char *end = NULL;
    double v;

    if (!s || !*s) return 0;
    /* Python 的 float() 不认 0x 十六进制，strtod 认；挡住 */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return 0;
    v = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

static int grow_buf(char **p, size_t *cap, size_t need)
{
    size_t c = *cap ? *cap : 256;
    char *q;

    while (c < need) c *= 2;
    q = (char *)realloc(*p, c);
    if (!q) return -1;
    *p = q;
    *cap = c;
    return 0;
}

/* 在已知的行区间 [i0, i1) 里取第 col 列。行内格子通常只有 5~10 个，
   线性扫比 bs_grid_get 的全局二分（每行 6 次 × 19 步随机访存）便宜得多。 */
static const bs_cell *row_cell(const bs_grid *g, int i0, int i1, int col)
{
    int i;

    if (col <= 0) return NULL;
    for (i = i0; i < i1; i++)
        if (g->cells[i].col == col) return &g->cells[i];
    return NULL;
}

static void add_row(bs_sheet *s, int r, int bi, const bs_block *b, const bs_cfg *c,
                    int i0, int i1)
{
    const bs_grid *g = s->g;
    const bs_cell *cpn = row_cell(g, i0, i1, b->col_pn);
    const bs_cell *cref = row_cell(g, i0, i1, b->col_ref);
    const bs_cell *cqty = row_cell(g, i0, i1, b->col_qty);
    const char *pnv = cpn ? cell_text(g, cpn) : "";
    const char *refv = cref ? cell_text(g, cref) : "";
    const char *qtyv = cqty ? cell_text(g, cqty) : "";
    char pnbuf[512];
    const char *pn, *sig, *key;
    int w = 0, st = 0, n_ok = 0, n_bad = 0, strike = 0, i, c1 = 0, c2 = 0;
    bs_row row;

    bs_pn_str(pnv ? pnv : "", pnbuf, sizeof(pnbuf), &w, &st);
    pn = bs_arena_put(&s->a, pnbuf, strlen(pnbuf));

    if (grow_buf(&s->sigbuf, &s->sigcap, 256)) return;
    while (bs_ref_sig_str(refv ? refv : "", s->sigbuf, s->sigcap, &n_ok, &n_bad) == -1) {
        if (grow_buf(&s->sigbuf, &s->sigcap, s->sigcap + 256)) return;
    }
    sig = bs_arena_put(&s->a, s->sigbuf, strlen(s->sigbuf));

    /* 与 bs_make_key 同规则（料号优先、位号兜底），这里自己拼是因为两段
       都已在手上，不必再走一遍归一化 */
    if (pn[0]) {
        size_t n = strlen(pn);
        char *k = (char *)bs_arena_raw(&s->a, n + 3);
        if (!k) return;
        memcpy(k, "P:", 2);
        memcpy(k + 2, pn, n + 1);
        key = k;
    } else if (sig[0]) {
        size_t n = strlen(sig);
        char *k = (char *)bs_arena_raw(&s->a, n + 3);
        if (!k) return;
        memcpy(k, "R:", 2);
        memcpy(k + 2, sig, n + 1);
        key = k;
    } else {
        key = "";
    }

    /* 删除线：0 = 只看料号/位号/数量三格，1 = 整行任一格 */
    if (c->strike_scope == 0) {
        strike = (cpn && cpn->strike) || (cref && cref->strike) || (cqty && cqty->strike);
    } else {
        for (i = i0; i < i1; i++)
            if (g->cells[i].strike) { strike = 1; break; }
    }

    for (i = i0; i < i1; i++) {
        if (cell_empty(&g->cells[i])) continue;
        if (!c1) c1 = g->cells[i].col;
        c2 = g->cells[i].col;
    }

    memset(&row, 0, sizeof(row));
    row.row = r;
    row.block = bi + 1;
    row.c1 = c1;
    row.c2 = c2;
    row.pn = pn;
    row.ref = bs_arena_put(&s->a, refv ? refv : "", refv ? strlen(refv) : 0);
    row.ref_sig = sig;
    row.key = key;
    row.kind = key[0] == 'P' ? 'P' : (key[0] == 'R' ? 'R' : 0);
    row.has_qty = parse_qty(qtyv, &row.qty);
    row.strike = (unsigned char)(strike ? 1 : 0);
    row.dup_i = row.dup_n = 1;

    if (!key[0]) {
        s->unkeyable++;
        return;
    }

    if (s->n_rows == s->cap_rows) {
        int nc = s->cap_rows ? s->cap_rows * 2 : 256;
        bs_row *p = (bs_row *)realloc(s->rows, (size_t)nc * sizeof(bs_row));
        if (!p) return;
        s->rows = p;
        s->cap_rows = nc;
    }
    s->rows[s->n_rows++] = row;
}

/* 同键行按出现顺序编号：1/2、2/2 …
   原来是整表 qsort 一次（10 万行 × strcmp 约 120 ms）。改成哈希分桶：
   绝大多数键只出现一次，桶里只有一个元素，根本不用排；只有真重复的键
   才在桶内排一下。病态输入（全表同键）退化成原来的 qsort，不会更差。

   比较器只用元素自身的字段，不碰任何全局：载入两张表是可以并行的，
   谁也不能让另一张表的 rows 指针跑到自己脚下（踩过这个坑）。 */
typedef struct {
    const char *key;
    int row;
    int idx;
} dupref;

static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;              /* FNV-1a */
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static int cmp_dup(const void *pa, const void *pb)
{
    const dupref *a = (const dupref *)pa;
    const dupref *b = (const dupref *)pb;
    const int c = strcmp(a->key, b->key);

    if (c) return c;
    return a->row < b->row ? -1 : (a->row > b->row ? 1 : 0);
}

static void group_dups(bs_sheet *s)
{
    int n = s->n_rows, i, b, nb = 16;
    int *head = NULL, *tail = NULL, *next = NULL;
    dupref *tmp = NULL;

    if (n < 2) return;
    while (nb < n * 2 && nb < (1 << 22)) nb <<= 1;

    head = (int *)malloc((size_t)nb * sizeof(int));
    tail = (int *)malloc((size_t)nb * sizeof(int));
    next = (int *)malloc((size_t)n * sizeof(int));
    tmp = (dupref *)malloc((size_t)n * sizeof(dupref));
    if (!head || !tail || !next || !tmp) {
        free(head);
        free(tail);
        free(next);
        free(tmp);
        return;
    }

    for (b = 0; b < nb; b++) { head[b] = -1; tail[b] = -1; }
    for (i = 0; i < n; i++) {
        const int k = (int)(hash_str(s->rows[i].key) & (unsigned)(nb - 1));
        next[i] = -1;
        if (tail[k] < 0) head[k] = i;
        else next[tail[k]] = i;
        tail[k] = i;
    }

    for (b = 0; b < nb; b++) {
        int cnt = 0;
        if (head[b] < 0) continue;
        for (i = head[b]; i >= 0; i = next[i]) {
            tmp[cnt].key = s->rows[i].key;
            tmp[cnt].row = s->rows[i].row;
            tmp[cnt].idx = i;
            cnt++;
        }
        if (cnt > 1) qsort(tmp, (size_t)cnt, sizeof(dupref), cmp_dup);
        for (i = 0; i < cnt;) {
            int e = i + 1, k;
            while (e < cnt && strcmp(tmp[e].key, tmp[i].key) == 0) e++;
            for (k = i; k < e; k++) {
                s->rows[tmp[k].idx].dup_i = k - i + 1;
                s->rows[tmp[k].idx].dup_n = e - i;
            }
            i = e;
        }
    }

    free(head);
    free(tail);
    free(next);
    free(tmp);
}

int bs_sheet_analyze(bs_workbook *wb, int idx, const bs_cfg *c,
                     bs_sheet **out, bs_err *e)
{
    bs_grid *g = NULL;
    int rc;

    if (out) *out = NULL;
    if (!wb) return -1;
    if (bs_xlsx_load_sheet(wb, idx, &g, e) != 0) return -1;
    rc = bs_sheet_from_grid(g, c, out, e);
    if (rc == 0 && out && *out) {   /* 名字要在关工作簿前抄下来，诊断页要用 */
        const char *nm = bs_xlsx_sheet_name(wb, idx);
        snprintf((*out)->name, sizeof((*out)->name), "%s", nm ? nm : "");
    }
    return rc;
}

int bs_sheet_from_grid(bs_grid *g, const bs_cfg *c, bs_sheet **out, bs_err *e)
{
    bs_cfg def;
    bs_sheet *s;
    int *hs = NULL, n_hs = 0, i;
    clock_t ck;

    if (out) *out = NULL;
    if (!g || !out) {
        bs_grid_free(g);
        return -1;
    }
    if (!c) {
        bs_cfg_default(&def);
        c = &def;
    }

    s = (bs_sheet *)calloc(1, sizeof(*s));
    if (!s) {
        bs_grid_free(g);
        if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
        return -1;
    }
    s->g = g;
    bs_arena_init(&s->a);
    ck = clock();
    collect_label_rows(s);
    bs_prof_sheet[0] += clk_ms(ck, clock());

    ck = clock();
    hs = header_blocks(g, c, &n_hs);
    bs_prof_sheet[1] += clk_ms(ck, clock());
    if (!hs || !n_hs) {
        if (e) snprintf(e->msg, sizeof(e->msg),
                        "未识别到表头行：前 %d 行内没有「列名 + 料号格式」的双证据",
                        c->header_scan_rows);
        free(hs);
        bs_sheet_close(s);
        return -1;
    }

    s->blocks = (bs_block *)calloc((size_t)n_hs, sizeof(bs_block));
    if (!s->blocks) {
        free(hs);
        if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
        bs_sheet_close(s);
        return -1;
    }
    s->n_blocks = n_hs;

    for (i = 0; i < n_hs; i++) {
        bs_block b;
        double rate = 0.0;
        int r;

        b.hr = hs[i];
        b.start = hs[i] + 1;
        b.end = (i + 1 < n_hs) ? hs[i + 1] - 1 : g->max_row;
        b.col_pn = find_pn_column(g, c, b.hr, b.end, &rate);
        b.col_ref = locate(g, b.hr, ALIAS_REF, N_OF(ALIAS_REF));
        /* 位号列猜测与「双键」无关：那是识别问题（哪一列是位号），双键只决定
           配对时要不要用它。以前这里是 && c->dual_key，结果双键一关，新表里
           「Location」这种列名认不出来 → 位号全空 → 料号相同的两行被当成
           「位号改了」，同一份表比自己都能冒差异。 */
        if (!b.col_ref)
            b.col_ref = guess_ref_column(g, b.hr, b.end, b.col_pn);
        b.col_qty = locate(g, b.hr, ALIAS_QTY, N_OF(ALIAS_QTY));
        s->blocks[i] = b;

        if (!b.col_pn) continue;
        if (rate > s->hit_rate) s->hit_rate = rate;

        {
            clock_t ck2 = clock();
            int cur = row_lower(g, b.start);
            /* 扫描上界取「最后一个有内容的行」：b.end 仍按表结构给（诊断页要原值），
               但不必为损坏文件里虚高的行号空转 —— 1e9 的行号曾让每张表多扫 3 秒 */
            int last_row = last_content_row(g);
            int scan_end = last_row < b.end ? last_row : b.end;
            for (r = b.start; r <= scan_end; r++) {
                int i0, i1;
                while (cur < g->n_cells && g->cells[cur].row < r) cur++;
                i0 = cur;
                while (cur < g->n_cells && g->cells[cur].row == r) cur++;
                i1 = cur;
                if (i0 == i1) continue;                       /* 这一行没有任何格子 */
                if (!row_nonempty_span(g, i0, i1)) continue;  /* 有格子但都空 */
                if (is_label_row(s, r)) {
                    s->labels++;
                    continue;
                }
                add_row(s, r, i, &b, c, i0, i1);
            }
            bs_prof_sheet[2] += clk_ms(ck2, clock());
        }
    }

    ck = clock();
    group_dups(s);
    bs_prof_sheet[3] += clk_ms(ck, clock());

    {
        int j;
        for (j = 0; j < s->n_blocks; j++)
            if (s->blocks[j].col_pn) {
                col_name_of(s->blocks[j].col_pn, s->name_pn, sizeof(s->name_pn));
                break;
            }
        for (j = 0; j < s->n_blocks; j++)
            if (s->blocks[j].col_ref) {
                col_name_of(s->blocks[j].col_ref, s->name_ref, sizeof(s->name_ref));
                break;
            }
        for (j = 0; j < s->n_blocks; j++)
            if (s->blocks[j].col_qty) {
                col_name_of(s->blocks[j].col_qty, s->name_qty, sizeof(s->name_qty));
                break;
            }
    }

    free(hs);
    *out = s;
    return 0;
}

int bs_sheet_open(const char *path_utf8, int idx, const bs_cfg *c,
                  bs_sheet **out, bs_err *e)
{
    bs_workbook *wb;
    int rc;

    if (out) *out = NULL;
    wb = bs_xlsx_open(path_utf8, e);
    if (!wb) return -1;
    rc = bs_sheet_analyze(wb, idx, c, out, e);
    bs_xlsx_close(wb);
    return rc;
}

void bs_sheet_close(bs_sheet *s)
{
    if (!s) return;
    bs_grid_free(s->g);
    free(s->blocks);
    free(s->rows);
    free(s->label_rows);
    free(s->sigbuf);
    bs_arena_free(&s->a);
    free(s);
}

const bs_grid *bs_sheet_grid(const bs_sheet *s)
{
    return s ? s->g : NULL;
}

int bs_sheet_block_count(const bs_sheet *s)
{
    return s ? s->n_blocks : 0;
}

const bs_block *bs_sheet_block(const bs_sheet *s, int i)
{
    if (!s || i < 0 || i >= s->n_blocks) return NULL;
    return &s->blocks[i];
}

int bs_sheet_row_count(const bs_sheet *s)
{
    return s ? s->n_rows : 0;
}

/* 数据行 = 建档行 + 无法建档行 + 分组标题行。
   这是识别诊断页第 4 列的口径；旧实现直接拿 row_count，导致「数据行」与
   「建档行」永远相同（少算了无法建档和分组标题那几行）。 */
int bs_sheet_data_rows(const bs_sheet *s)
{
    if (!s) return 0;
    return s->n_rows + s->unkeyable + s->labels;
}

const bs_row *bs_sheet_row(const bs_sheet *s, int i)
{
    if (!s || i < 0 || i >= s->n_rows) return NULL;
    return &s->rows[i];
}

/* 某行某列的原位文本。空 / 越界一律返回 ""。
   cells 按 (row, col, ord) 有序，同格写两次时后者生效，所以取最后一个非空的。
   只给「整行逐列比较」用：一次比较要读两侧各十几格，行号已知，二分 + 行内线性扫足够快。 */
const char *bs_sheet_text_at(const bs_sheet *s, int row, int col)
{
    int i0, i1, i;
    const char *best = "";

    if (!s || !s->g || row <= 0 || col <= 0) return "";
    if (!row_span(s->g, row, &i0, &i1)) return "";
    for (i = i0; i < i1; i++) {
        const bs_cell *c = &s->g->cells[i];
        if (c->col != col || c->len == 0) continue;
        best = cell_text(s->g, c);
    }
    return best;
}

int bs_sheet_labels(const bs_sheet *s)
{
    return s ? s->labels : 0;
}

int bs_sheet_unkeyable(const bs_sheet *s)
{
    return s ? s->unkeyable : 0;
}

double bs_sheet_hit_rate(const bs_sheet *s)
{
    return s ? s->hit_rate : 0.0;
}

const char *bs_sheet_col_name(const bs_sheet *s, char kind)
{
    if (!s) return "";
    if (kind == 'P') return s->name_pn;
    if (kind == 'R') return s->name_ref;
    if (kind == 'Q') return s->name_qty;
    return "";
}

const char *bs_sheet_name(const bs_sheet *s)
{
    return s ? s->name : "";
}

void bs_sheet_set_name(bs_sheet *s, const char *name)
{
    if (!s) return;
    if (!name) name = "";
    snprintf(s->name, sizeof s->name, "%.71s", name);
}

int bs_sheet_ref_rows(const bs_sheet *s)
{
    int i, n = 0;
    if (!s) return 0;
    for (i = 0; i < s->n_rows; i++)
        if (s->rows[i].kind == 'R') n++;
    return n;
}
