#include "bs_pair.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bs_arena.h"
#include "bs_pn.h"

/* 位号签名是「排序去重后以 | 连接」的串，所以集合运算全可以按有序表走。 */

static int tok_get(const char **p, const char **t, size_t *n)
{
    const char *s = *p;
    const char *e;

    if (!s || !*s) return 0;
    e = strchr(s, '|');
    if (!e) e = s + strlen(s);
    *t = s;
    *n = (size_t)(e - s);
    *p = (*e == '|') ? e + 1 : e;
    return 1;
}

static int tok_cmp(const char *a, size_t la, const char *b, size_t lb)
{
    size_t m = la < lb ? la : lb;
    int c = m ? memcmp(a, b, m) : 0;

    if (c) return c;
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

static int sig_has(const char *sig, const char *t, size_t n)
{
    const char *p = sig, *q;
    size_t m;

    while (tok_get(&p, &q, &m))
        if (m == n && memcmp(q, t, n) == 0) return 1;
    return 0;
}

/* Python: len(sa & sb) / len(sa | sb)，任一侧为空时 0 */
static double jaccard(const char *a, const char *b)
{
    const char *pa = a, *pb = b, *ta, *tb;
    size_t la, lb;
    int na = 0, nb = 0, inter = 0;
    int ha, hb;

    if (!a || !*a || !b || !*b) return 0.0;
    ha = tok_get(&pa, &ta, &la);
    hb = tok_get(&pb, &tb, &lb);
    while (ha || hb) {
        if (ha && hb) {
            int c = tok_cmp(ta, la, tb, lb);
            if (c < 0) {
                na++;
                ha = tok_get(&pa, &ta, &la);
            } else if (c > 0) {
                nb++;
                hb = tok_get(&pb, &tb, &lb);
            } else {
                na++; nb++; inter++;
                ha = tok_get(&pa, &ta, &la);
                hb = tok_get(&pb, &tb, &lb);
            }
        } else if (ha) {
            na++;
            ha = tok_get(&pa, &ta, &la);
        } else {
            nb++;
            hb = tok_get(&pb, &tb, &lb);
        }
    }
    if (na + nb - inter <= 0) return 0.0;
    return (double)inter / (double)(na + nb - inter);
}

/* ---------------- 阶段 A / B 的按键分组 ----------------
   原来是「两表各排一次序 + 双指针」，10 万行时光排序就吃掉一百多毫秒
   （比较器里全是 strcmp）。改成哈希分桶：插入 O(1)，桶内按插入顺序
   （= 行序）串起来，配对仍然「同键组内按行序一对一」，结果与排序版逐对一致。 */

typedef struct {
    int oi, ni;
    int orow, nrow;          /* 行号随记录一起带走：比较器不必再回头查 g_o/g_n */
    unsigned char kt;
} mref;

static const bs_row *g_o, *g_n;       /* 仅供 qsort 比较器取行号（单线程，够用） */

typedef struct {
    int *head;         /* 桶 -> 首节点下标，-1 表示空桶 */
    int *tail;         /* 桶 -> 尾节点，尾插才保得住行序 */
    int *next;         /* 节点 -> 同桶下一个 */
    const char **key;
    int *idx;
    int nb, n, cap;
} htab;

static unsigned hash_key(const char *s)
{
    unsigned h = 2166136261u;              /* FNV-1a，够散且不用表 */
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void htab_free(htab *t)
{
    free(t->head);
    free(t->tail);
    free(t->next);
    free(t->key);
    free(t->idx);
    memset(t, 0, sizeof(*t));
}

/* 收集 kind 类型里还没配对的行；返回 0 成功，-1 内存不足 */
static int htab_build(htab *t, const bs_row *r, int n, char kind,
                      const unsigned char *used)
{
    int i, cnt = 0, nb = 16;

    memset(t, 0, sizeof(*t));
    for (i = 0; i < n; i++)
        if (!used[i] && r[i].kind == kind && r[i].key && *r[i].key) cnt++;
    while (nb < cnt * 2 && nb < (1 << 22)) nb <<= 1;

    t->head = (int *)malloc((size_t)nb * sizeof(int));
    t->tail = (int *)malloc((size_t)nb * sizeof(int));
    t->next = (int *)malloc((size_t)(cnt ? cnt : 1) * sizeof(int));
    t->key = (const char **)malloc((size_t)(cnt ? cnt : 1) * sizeof(char *));
    t->idx = (int *)malloc((size_t)(cnt ? cnt : 1) * sizeof(int));
    if (!t->head || !t->tail || !t->next || !t->key || !t->idx) {
        htab_free(t);
        return -1;
    }
    for (i = 0; i < nb; i++) {
        t->head[i] = -1;
        t->tail[i] = -1;
    }
    t->nb = nb;
    t->n = 0;
    t->cap = cnt;

    for (i = 0; i < n; i++) {
        unsigned h;
        int k;
        if (used[i] || r[i].kind != kind || !r[i].key || !*r[i].key) continue;
        h = hash_key(r[i].key) & (unsigned)(nb - 1);
        k = t->n++;
        t->key[k] = r[i].key;
        t->idx[k] = i;
        t->next[k] = -1;
        if (t->tail[h] < 0) t->head[h] = k;
        else t->next[t->tail[h]] = k;
        t->tail[h] = k;
    }
    return 0;
}

/* 同 key 的链表头；没有返回 -1 */
static int htab_find(const htab *t, const char *key)
{
    unsigned h = hash_key(key) & (unsigned)(t->nb - 1);
    int i;

    if (!t->nb) return -1;
    for (i = t->head[h]; i >= 0; i = t->next[i])
        if (strcmp(t->key[i], key) == 0) return i;
    return -1;
}

/* 把 kind 类型的行按键相等一对一配掉，返回新配对数 */
static int stage_by_key(const bs_row *o, int no, const bs_row *n, int nn, char kind,
                        unsigned char *uo, unsigned char *un,
                        mref *ms, int nms, int max_ms, unsigned char kt)
{
    htab to, tn;
    int b, i, rc;

    if (htab_build(&to, o, no, kind, uo) != 0) return -1;
    if (htab_build(&tn, n, nn, kind, un) != 0) {
        htab_free(&to);
        return -1;
    }

    for (b = 0; b < to.nb; b++) {
        for (i = to.head[b]; i >= 0; i = to.next[i]) {
            const int oi = to.idx[i];
            int j;
            if (uo[oi]) continue;
            j = htab_find(&tn, to.key[i]);
            /* 同一组里前面几行可能已经配走了，往后挪到第一个未用的 */
            while (j >= 0 && un[tn.idx[j]]) j = tn.next[j];
            if (j < 0) continue;
            if (nms >= max_ms) { b = to.nb; break; }
            ms[nms].oi = oi;
            ms[nms].ni = tn.idx[j];
            ms[nms].orow = o[oi].row;
            ms[nms].nrow = n[tn.idx[j]].row;
            ms[nms].kt = kt;
            uo[oi] = 1;
            un[tn.idx[j]] = 1;
            nms++;
        }
    }

    rc = nms;
    htab_free(&to);
    htab_free(&tn);
    return rc;
}

static int cmp_mref(const void *pa, const void *pb)
{
    const mref *a = (const mref *)pa;
    const mref *b = (const mref *)pb;

    if (a->orow != b->orow) return a->orow < b->orow ? -1 : 1;
    return a->nrow < b->nrow ? -1 : (a->nrow > b->nrow ? 1 : 0);
}


/* ---------------- 阶段 C ---------------- */

typedef struct {
    double j;
    int oi, ni;
    int orow, nrow;
} cand;

static int cmp_cand(const void *pa, const void *pb)
{
    const cand *a = (const cand *)pa;
    const cand *b = (const cand *)pb;

    if (a->j != b->j) return a->j > b->j ? -1 : 1;
    if (a->orow != b->orow) return a->orow < b->orow ? -1 : 1;
    return a->nrow < b->nrow ? -1 : (a->nrow > b->nrow ? 1 : 0);
}

/* ---------------- 输出排序 ---------------- */

typedef struct {
    bs_pair p;
    int seq;
} oref;

static const char *pair_pn(const bs_pair *p)
{
    const char *s = "";

    if (p->ni >= 0) s = g_n[p->ni].pn;
    if (!s || !*s) s = p->oi >= 0 ? g_o[p->oi].pn : "";
    if (!s || !*s) s = "-";
    return s;
}

static int cmp_oref(const void *pa, const void *pb)
{
    const oref *a = (const oref *)pa;
    const oref *b = (const oref *)pb;
    int c;

    if (a->p.act != b->p.act) return a->p.act < b->p.act ? -1 : 1;
    c = strcmp(pair_pn(&a->p), pair_pn(&b->p));
    if (c) return c;
    return a->seq < b->seq ? -1 : (a->seq > b->seq ? 1 : 0);
}

/* ---------------- 严格模式：重复键即中止 ---------------- */

static int strict_guard(const bs_sheet *o, const bs_sheet *n, bs_err *e)
{
    int no = bs_sheet_row_count(o), nn = bs_sheet_row_count(n);
    const char **keys;
    int i, nk = 0, cap = 8;
    int bad = 0;

    keys = (const char **)malloc((size_t)cap * sizeof(char *));
    if (!keys) return -1;
    for (i = 0; i < no + nn; i++) {
        const bs_row *r = i < no ? bs_sheet_row(o, i) : bs_sheet_row(n, i - no);
        int k, dup = 0;
        if (!r || r->dup_n <= 1) continue;
        for (k = 0; k < nk; k++)
            if (strcmp(keys[k], r->key) == 0) { dup = 1; break; }
        if (dup) continue;
        if (nk == cap) {
            const char **t;
            cap *= 2;
            t = (const char **)realloc((void *)keys, (size_t)cap * sizeof(char *));
            if (!t) { free((void *)keys); return -1; }
            keys = t;
        }
        keys[nk++] = r->key;
        bad = 1;
    }
    if (bad) {
        /* Python: sorted(set(...))[:3]，用 、 连接 */
        int a, b;
        char buf[128];
        size_t used = 0;
        for (a = 1; a < nk; a++) {
            const char *v = keys[a];
            for (b = a - 1; b >= 0 && strcmp(keys[b], v) > 0; b--) keys[b + 1] = keys[b];
            keys[b + 1] = v;
        }
        buf[0] = 0;
        for (a = 0; a < nk && a < 3; a++) {
            /* 键本身可能很长（位号键），错误文案里只截前 36 字节 */
            int w = snprintf(buf + used, sizeof(buf) - used, "%s%.36s",
                             a ? "、" : "", keys[a]);
            if (w < 0) break;
            used += (size_t)w;
            if (used >= sizeof(buf) - 8) break;
        }
        if (e) snprintf(e->msg, sizeof(e->msg),
                        "严格模式下检出重复键（%s…），请先清理源表", buf);
    }
    free((void *)keys);
    return bad ? 1 : 0;
}

/* ---------------- 主入口 ---------------- */

/* 「整行逐列比较」：把两侧这一行的每一列都取出来、归一化后比一遍。
   归一化走 bs_norm_str（与表头识别同一条规则：大写、压掉空白），所以
   「R1, R2」与「R1,R2」不算差异，而「10uF」与「22uF」一定算。
   列范围取两侧的并集：新表多出来的列（例如新增「备注」）也要参与比较。
   返回 1 = 至少有一格不一样。 */
static int row_cells_differ(const bs_sheet *so, const bs_row *a,
                            const bs_sheet *sn, const bs_row *b)
{
    int c1 = a->c1, c2 = a->c2, c;
    char x[512], y[512];

    if (b->c1 && (c1 <= 0 || b->c1 < c1)) c1 = b->c1;
    if (b->c2 > c2) c2 = b->c2;
    if (c1 < 1) c1 = 1;
    if (c2 > 512) c2 = 512;            /* 防御：异常文件里 max_col 可能很大 */
    for (c = c1; c <= c2; c++) {
        bs_norm_str(bs_sheet_text_at(so, a->row, c), x, sizeof(x));
        bs_norm_str(bs_sheet_text_at(sn, b->row, c), y, sizeof(y));
        if (strcmp(x, y) != 0) return 1;
    }
    return 0;
}

/* 近似阶段的护栏：两侧剩余行数是完整笛卡尔积，10 万 × 10 万 = 100 亿次
   jaccard，UI 线程上等于永远跑不完。超限就跳过近似、交给阶段 D（增/删），
   候选表也设上限 —— 与 pyref/bomcore.py 同值，否则大样本对账会分叉。 */
#define FUZZY_MAX_PAIRS 50000000LL   /* nro * nrn 的上限 */
#define FUZZY_MAX_CAND  200000       /* 候选表条数上限 */

int bs_pair_rows(const bs_sheet *o, const bs_sheet *n, const bs_cfg *c,
                 bs_pair **out, size_t *np, bs_pairstat *st, bs_err *e)
{
    bs_cfg def;
    bs_pairstat s;
    int no, nn, i, k, nms = 0, max_ms, na = 0, nd = 0, nout = 0;
    unsigned char *uo = NULL, *un = NULL;
    mref *ms = NULL;
    oref *ord = NULL;
    bs_pair *res = NULL;
    int rc = -1;

    if (out) *out = NULL;
    if (np) *np = 0;
    if (st) memset(st, 0, sizeof(*st));
    if (!o || !n || !out || !np) {
        if (e) snprintf(e->msg, sizeof(e->msg), "配对入参为空");
        return -1;
    }
    if (!c) {
        bs_cfg_default(&def);
        c = &def;
    }
    memset(&s, 0, sizeof(s));
    if (c->dup_policy == 2) s.cartesian_fallback = 1;   /* 未实现全组合，按顺序配对 */

    if (c->dup_policy == 1) {
        int g = strict_guard(o, n, e);
        if (g < 0) {
            if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
            return -1;
        }
        if (g > 0) return -1;
    }

    no = bs_sheet_row_count(o);
    nn = bs_sheet_row_count(n);
    uo = (unsigned char *)calloc((size_t)(no ? no : 1), 1);
    un = (unsigned char *)calloc((size_t)(nn ? nn : 1), 1);
    max_ms = no < nn ? no : nn;
    ms = (mref *)calloc((size_t)(max_ms ? max_ms : 1), sizeof(mref));
    ord = (oref *)calloc((size_t)(no + nn ? no + nn : 1), sizeof(oref));
    if (!uo || !un || !ms || !ord) goto done;

    g_o = NULL;
    g_n = NULL;

    /* 阶段 A：料号键 */
    {
        /* stage_by_key 需要行数组指针，先取出来 */
        const bs_row *ro = no ? bs_sheet_row(o, 0) : NULL;
        const bs_row *rn = nn ? bs_sheet_row(n, 0) : NULL;
        k = stage_by_key(ro, no, rn, nn, 'P', uo, un, ms, nms, max_ms, BS_KT_PN);
        if (k < 0) {
            if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
            goto done;
        }
        s.n_pn = k;
        nms = k;
    }
    /* 阶段 B：位号键精确（只用剩余行） */
    if (c->dual_key) {
        const bs_row *ro = no ? bs_sheet_row(o, 0) : NULL;
        const bs_row *rn = nn ? bs_sheet_row(n, 0) : NULL;
        k = stage_by_key(ro, no, rn, nn, 'R', uo, un, ms, nms, max_ms, BS_KT_REF);
        if (k < 0) {
            if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
            goto done;
        }
        s.n_ref = k - nms;
        nms = k;
    }

    /* bs_sheet 的行是连续数组，直接取基址比逐次 bs_sheet_row 便宜 */
    g_o = no ? bs_sheet_row(o, 0) : NULL;
    g_n = nn ? bs_sheet_row(n, 0) : NULL;

    /* 双键关掉时，无料号的行既没有键也没有兜底：按 V2 的口径「跳过」，
       不能丢给阶段 D 去当新增/删除 —— 否则拿同一份文件比自己，这些行会
       成对地冒出来，凭空多出一堆「差异」（2026-11-06 回归里踩到过）。 */
    if (!c->dual_key) {
        int i2;
        for (i2 = 0; i2 < no; i2++)
            if (g_o[i2].kind == 'R') { uo[i2] = 1; s.n_skipped++; }
        for (i2 = 0; i2 < nn; i2++)
            if (g_n[i2].kind == 'R') { un[i2] = 1; s.n_skipped++; }
    }

    /* 阶段 C：位号近似，至少一侧无料号 */
    if (c->dual_key && c->ref_fuzzy) {
        int *ro = (int *)malloc((size_t)(no ? no : 1) * sizeof(int));
        int *rn = (int *)malloc((size_t)(nn ? nn : 1) * sizeof(int));
        int nro = 0, nrn = 0, nc = 0, ccap = 16, q;
        cand *cs = NULL;

        if (!ro || !rn) {
            free(ro);
            free(rn);
            if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
            goto done;
        }
        for (i = 0; i < no; i++) if (!uo[i]) ro[nro++] = i;
        for (i = 0; i < nn; i++) if (!un[i]) rn[nrn++] = i;
        /* 规模超限就不逐对比：剩下的按新增/删除处理（有意的降级） */
        if ((long long)nro * (long long)nrn > FUZZY_MAX_PAIRS) {
            nro = 0;
            nrn = 0;
        }
        cs = (cand *)malloc((size_t)ccap * sizeof(cand));
        for (i = 0; i < nro && cs && nc < FUZZY_MAX_CAND; i++) {
            const bs_row *a = &g_o[ro[i]];
            if (!a->ref_sig || !*a->ref_sig) continue;
            for (k = 0; k < nrn && nc < FUZZY_MAX_CAND; k++) {
                const bs_row *b = &g_n[rn[k]];
                double j;
                if (!b->ref_sig || !*b->ref_sig) continue;
                if (a->kind == 'P' && b->kind == 'P') continue;
                j = jaccard(a->ref_sig, b->ref_sig);
                if (j < c->ref_min_overlap) continue;
                if (nc == ccap) {
                    cand *t;
                    ccap *= 2;
                    t = (cand *)realloc(cs, (size_t)ccap * sizeof(cand));
                    if (!t) break;
                    cs = t;
                }
                cs[nc].j = j;
                cs[nc].oi = ro[i];
                cs[nc].ni = rn[k];
                cs[nc].orow = a->row;
                cs[nc].nrow = b->row;
                nc++;
            }
        }
        if (cs) qsort(cs, (size_t)nc, sizeof(cand), cmp_cand);
        for (q = 0; q < nc && nms < max_ms; q++) {
            if (uo[cs[q].oi] || un[cs[q].ni]) continue;
            ms[nms].oi = cs[q].oi;
            ms[nms].ni = cs[q].ni;
            ms[nms].orow = cs[q].orow;
            ms[nms].nrow = cs[q].nrow;
            ms[nms].kt = g_o[cs[q].oi].kind != g_n[cs[q].ni].kind
                             ? BS_KT_REF_PNCHG : BS_KT_REF_FUZZY;
            uo[cs[q].oi] = 1;
            un[cs[q].ni] = 1;
            nms++;
        }
        for (q = s.n_pn + s.n_ref; q < nms; q++)
            if (ms[q].kt == BS_KT_REF_PNCHG) s.n_pnchg++;
            else s.n_fuzzy++;
        free(cs);
        free(ro);
        free(rn);
    }

    /* 组装：变更 → 新增 → 删除，顺序照 build_result 的入参顺序；最后统一排序 */
    qsort(ms, (size_t)nms, sizeof(mref), cmp_mref);
    for (i = 0; i < nms; i++) {
        const bs_row *a = &g_o[ms[i].oi];
        const bs_row *b = &g_n[ms[i].ni];
        unsigned chg = 0;
        oref *rec = &ord[nout++];

        if (strcmp(a->ref_sig, b->ref_sig) != 0) chg |= BS_CHG_REF;
        if (a->has_qty != b->has_qty || (a->has_qty && a->qty != b->qty)) chg |= BS_CHG_QTY;
        if (strcmp(a->pn, b->pn) != 0) chg |= BS_CHG_PN;
        if ((a->strike != 0) != (b->strike != 0)) chg |= BS_CHG_STRIKE;
        if (c->row_diff_all && row_cells_differ(o, a, n, b)) chg |= BS_CHG_CELL;

        memset(&rec->p, 0, sizeof(rec->p));
        rec->p.oi = ms[i].oi;
        rec->p.ni = ms[i].ni;
        rec->p.act = BS_ACT_CHG;
        rec->p.kt = ms[i].kt;
        rec->p.chg = chg;
        rec->p.strike_dir = !(chg & BS_CHG_STRIKE) ? 0 : (b->strike ? 1 : 2);
        rec->seq = nout - 1;
        if (chg) s.n_changed++;
    }
    for (i = 0; i < nn; i++) {
        oref *rec;
        if (un[i]) continue;
        rec = &ord[nout++];
        memset(&rec->p, 0, sizeof(rec->p));
        rec->p.oi = -1;
        rec->p.ni = i;
        rec->p.act = BS_ACT_ADD;
        rec->p.kt = g_n[i].kind == 'P' ? BS_KT_PN : BS_KT_REF;
        rec->seq = nout - 1;
        na++;
    }
    for (i = 0; i < no; i++) {
        oref *rec;
        if (uo[i]) continue;
        rec = &ord[nout++];
        memset(&rec->p, 0, sizeof(rec->p));
        rec->p.oi = i;
        rec->p.ni = -1;
        rec->p.act = BS_ACT_DEL;
        rec->p.kt = g_o[i].kind == 'P' ? BS_KT_PN : BS_KT_REF;
        rec->seq = nout - 1;
        nd++;
    }

    qsort(ord, (size_t)nout, sizeof(oref), cmp_oref);
    res = (bs_pair *)malloc((size_t)(nout ? nout : 1) * sizeof(bs_pair));
    if (!res) {
        if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
        goto done;
    }
    for (i = 0; i < nout; i++) res[i] = ord[i].p;

    s.n_paired = s.n_pn + s.n_ref + s.n_fuzzy + s.n_pnchg;
    s.n_added = na;
    s.n_deleted = nd;
    *out = res;
    *np = (size_t)nout;
    if (st) *st = s;
    res = NULL;
    rc = 0;

done:
    free(uo);
    free(un);
    free(ms);
    free(ord);
    free(res);
    return rc;
}

void bs_pair_free(bs_pair *p)
{
    free(p);
}

/* ---------------- 12 列结果 ---------------- */

struct bs_result {
    bs_arena a;
    bs_resrow *rows;
    size_t n, cap;
};

static const char *KT_TEXT[] = { "料号", "位号", "位号(近似)", "位号(料号变更)" };
static const char *ACT_TEXT[] = { "变更", "新增", "删除" };
static const char *RESULT_HEADER[BS_RES_COLS] = {
    "Part Number", "键类型", "差异类型", "变更项",
    "旧_多出位号", "新_多出位号", "旧_Quantity", "新_Quantity",
    "旧_删除线", "新_删除线", "重复序号", "配对来源"
};

/* Python: str(int(v)) 或 str(v) */
void bs_qty_text(const bs_row *r, char *buf, size_t cap)
{
    double v;
    int prec;

    buf[0] = 0;
    if (!r->has_qty) return;
    v = r->qty;
    if (v == 0.0) {
        snprintf(buf, cap, "0");
        return;
    }
    if (fabs(v - trunc(v)) < 1e-9) {
        snprintf(buf, cap, "%.0f", trunc(v));
        return;
    }
    for (prec = 15; prec <= 17; prec++) {
        snprintf(buf, cap, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) return;
    }
}

/* from 里存在、other 里不存在的 token，用 "," 连接；顺序即签名顺序（已排序） */
static const char *sig_diff(bs_arena *ar, const char *from, const char *other)
{
    const char *p = from, *t;
    size_t n, len = 0, used = 0;
    char *buf;

    if (!from || !*from) return "";
    while (tok_get(&p, &t, &n))
        if (!sig_has(other, t, n)) len += n + 1;
    if (len == 0) return "";
    buf = (char *)bs_arena_raw(ar, len);
    if (!buf) return "";
    p = from;
    while (tok_get(&p, &t, &n)) {
        if (sig_has(other, t, n)) continue;
        if (used) buf[used++] = ',';
        memcpy(buf + used, t, n);
        used += n;
    }
    buf[used] = 0;
    return buf;
}

/* 用 "," 连接签名的全部 token（新增 / 删除行的位号列） */
static const char *sig_join(bs_arena *ar, const char *sig)
{
    const char *p = sig, *t;
    size_t n, len, used = 0;
    char *buf;

    if (!sig || !*sig) return "";
    len = strlen(sig);
    buf = (char *)bs_arena_raw(ar, len + 1);
    if (!buf) return "";
    while (tok_get(&p, &t, &n)) {
        if (used) buf[used++] = ',';
        memcpy(buf + used, t, n);
        used += n;
    }
    buf[used] = 0;
    return buf;
}

int bs_result_build(const bs_sheet *o, const bs_sheet *n,
                    const bs_pair *p, size_t np, bs_result **out, bs_err *e)
{
    bs_result *r;
    size_t i, cnt = 0, k;

    if (out) *out = NULL;
    if (!o || !n || !out) {
        if (e) snprintf(e->msg, sizeof(e->msg), "结果入参为空");
        return -1;
    }
    for (i = 0; i < np; i++) {
        if (p[i].act == BS_ACT_CHG && p[i].chg == 0) continue;
        cnt++;
    }
    r = (bs_result *)calloc(1, sizeof(*r));
    if (!r) {
        if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
        return -1;
    }
    bs_arena_init(&r->a);
    r->cap = cnt ? cnt : 1;
    r->rows = (bs_resrow *)calloc(r->cap, sizeof(bs_resrow));
    if (!r->rows) {
        bs_arena_free(&r->a);
        free(r);
        if (e) snprintf(e->msg, sizeof(e->msg), "内存不足");
        return -1;
    }

    g_o = bs_sheet_row_count(o) ? bs_sheet_row(o, 0) : NULL;
    g_n = bs_sheet_row_count(n) ? bs_sheet_row(n, 0) : NULL;

    for (i = 0; i < np; i++) {
        const bs_pair *pr = &p[i];
        const bs_row *a = pr->oi >= 0 ? &g_o[pr->oi] : NULL;
        const bs_row *b = pr->ni >= 0 ? &g_n[pr->ni] : NULL;
        bs_resrow *row;
        char buf[256];

        if (pr->act == BS_ACT_CHG && pr->chg == 0) continue;
        row = &r->rows[r->n++];

        if (pr->act == BS_ACT_CHG) {
            row->col[BS_COL_PN] = bs_arena_str(&r->a,
                (b->pn && *b->pn) ? b->pn : ((a->pn && *a->pn) ? a->pn : "-"));
            row->col[BS_COL_KT] = KT_TEXT[pr->kt < 4 ? pr->kt : 0];
            row->col[BS_COL_ACT] = ACT_TEXT[BS_ACT_CHG];
            {
                char why[128];
                why[0] = 0;
                if (pr->chg & BS_CHG_REF) strcpy(why, "位号");
                if (pr->chg & BS_CHG_QTY) {
                    if (why[0]) strcat(why, "+");
                    strcat(why, "数量");
                }
                if (pr->chg & BS_CHG_PN) {
                    if (why[0]) strcat(why, "+");
                    strcat(why, "料号");
                }
                if (pr->chg & BS_CHG_STRIKE) {
                    if (why[0]) strcat(why, "+");
                    strcat(why, b->strike ? "删除线(划除)" : "删除线(恢复)");
                }
                if (pr->chg & BS_CHG_CELL) {
                    if (why[0]) strcat(why, "+");
                    strcat(why, "其他列");
                }
                row->col[BS_COL_CHG] = bs_arena_str(&r->a, why);
            }
            row->col[BS_COL_RO] = sig_diff(&r->a, a->ref_sig, b->ref_sig);
            row->col[BS_COL_RN] = sig_diff(&r->a, b->ref_sig, a->ref_sig);
            bs_qty_text(a, buf, sizeof(buf));
            row->col[BS_COL_QO] = bs_arena_str(&r->a, buf);
            bs_qty_text(b, buf, sizeof(buf));
            row->col[BS_COL_QN] = bs_arena_str(&r->a, buf);
            row->col[BS_COL_SO] = a->strike ? "有" : "无";
            row->col[BS_COL_SN] = b->strike ? "有" : "无";
            if (a->dup_n > 1 || b->dup_n > 1) {
                snprintf(buf, sizeof(buf), "%d/%d", b->dup_i,
                         a->dup_n > b->dup_n ? a->dup_n : b->dup_n);
                row->col[BS_COL_DUP] = bs_arena_str(&r->a, buf);
                snprintf(buf, sizeof(buf), "旧#%d ↔ 新#%d（r%d / r%d）",
                         a->dup_i, b->dup_i, a->row, b->row);
                row->col[BS_COL_SRC] = bs_arena_str(&r->a, buf);
            } else {
                row->col[BS_COL_DUP] = "";
                row->col[BS_COL_SRC] = "";
            }
        } else if (pr->act == BS_ACT_ADD) {
            row->col[BS_COL_PN] = bs_arena_str(&r->a, (b->pn && *b->pn) ? b->pn : "-");
            row->col[BS_COL_KT] = b->kind == 'P' ? KT_TEXT[BS_KT_PN] : KT_TEXT[BS_KT_REF];
            row->col[BS_COL_ACT] = ACT_TEXT[BS_ACT_ADD];
            row->col[BS_COL_CHG] = "";
            row->col[BS_COL_RO] = "";
            row->col[BS_COL_RN] = sig_join(&r->a, b->ref_sig);
            row->col[BS_COL_QO] = "";
            bs_qty_text(b, buf, sizeof(buf));
            row->col[BS_COL_QN] = bs_arena_str(&r->a, buf);
            row->col[BS_COL_SO] = "";
            row->col[BS_COL_SN] = b->strike ? "有" : "无";
            row->col[BS_COL_DUP] = "";
            row->col[BS_COL_SRC] = "";
        } else {
            row->col[BS_COL_PN] = bs_arena_str(&r->a, (a->pn && *a->pn) ? a->pn : "-");
            row->col[BS_COL_KT] = a->kind == 'P' ? KT_TEXT[BS_KT_PN] : KT_TEXT[BS_KT_REF];
            row->col[BS_COL_ACT] = ACT_TEXT[BS_ACT_DEL];
            row->col[BS_COL_CHG] = "";
            row->col[BS_COL_RO] = sig_join(&r->a, a->ref_sig);
            row->col[BS_COL_RN] = "";
            bs_qty_text(a, buf, sizeof(buf));
            row->col[BS_COL_QO] = bs_arena_str(&r->a, buf);
            row->col[BS_COL_QN] = "";
            row->col[BS_COL_SO] = a->strike ? "有" : "无";
            row->col[BS_COL_SN] = "";
            row->col[BS_COL_DUP] = "";
            row->col[BS_COL_SRC] = "";
        }
        for (k = 0; k < BS_RES_COLS; k++)
            if (!row->col[k]) row->col[k] = "";
    }
    *out = r;
    return 0;
}

size_t bs_result_count(const bs_result *r)
{
    return r ? r->n : 0;
}

const bs_resrow *bs_result_row(const bs_result *r, size_t i)
{
    return (r && i < r->n) ? &r->rows[i] : NULL;
}

const char *bs_result_header(int i)
{
    return (i >= 0 && i < BS_RES_COLS) ? RESULT_HEADER[i] : "";
}

void bs_result_close(bs_result *r)
{
    if (!r) return;
    bs_arena_free(&r->a);
    free(r->rows);
    free(r);
}
