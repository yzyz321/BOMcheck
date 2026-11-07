#include "bs_pn.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

/* 可增长字符串。p 可能是调用方复用的缓冲（见 sb_wrap），所以自身不负责释放。 */
typedef struct {
    char  *p;
    size_t len, cap;
    int    owned;      /* 1 = p 是 malloc 来的，换缓冲时要 free 旧的 */
} sbuf;

/* 短串走栈上缓冲，避免每次调用都 malloc —— norm/pn 在一张 5 万行的表上
   各要被调用几十万次，光分配就占了大头（实测 550 ms 里约 400 ms）。 */
static void sb_stack(sbuf *b, char *buf, size_t cap)
{
    b->p = buf;
    b->cap = cap;
    b->len = 0;
    b->owned = 0;
    buf[0] = 0;
}

static void sb_wrap(sbuf *b, char **p, size_t *cap)
{
    b->p = *p;
    b->cap = *cap;
    b->len = 0;
    b->owned = 1;          /* 这块归调用方（bs_keys）所有，扩容时旧的要释放 */
    if (b->p) b->p[0] = 0;
}

static void sb_unwrap(sbuf *b, char **p, size_t *cap)
{
    *p = b->p;
    *cap = b->cap;
}

static void sb_reset(sbuf *b)
{
    b->len = 0;
    if (b->p) b->p[0] = 0;
}

/* 保证还能再写 need 个字节（不含结尾 '\0'）。失败返回 -1，调用方忽略即可。 */
static int sb_reserve(sbuf *b, size_t need)
{
    size_t c;
    char *q;

    if (b->cap >= need + 1) return 0;
    c = b->cap ? b->cap : 64;
    while (c < need + 1) c *= 2;
    q = (char *)malloc(c);
    if (!q) return -1;
    if (b->len) memcpy(q, b->p, b->len);
    q[b->len] = 0;
    if (b->owned) free(b->p);
    else b->owned = 1;
    b->p = q;
    b->cap = c;
    return 0;
}

static void sb_write(sbuf *b, const char *s, size_t n)
{
    if (sb_reserve(b, b->len + n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_byte(sbuf *b, unsigned char c)
{
    if (sb_reserve(b, b->len + 1)) return;
    b->p[b->len++] = (char)c;
    b->p[b->len] = 0;
}

static void sb_cp(sbuf *b, unsigned cp)
{
    unsigned char t[4];
    int n;

    if (cp < 0x80) {
        t[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        t[0] = (unsigned char)(0xC0 | (cp >> 6));
        t[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (unsigned char)(0xE0 | (cp >> 12));
        t[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (unsigned char)(0xF0 | (cp >> 18));
        t[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    sb_write(b, (const char *)t, (size_t)n);
}

/* 非法 UTF-8 字节用 0x10000 + byte 标记，输出时还原成单字节原样写回。 */
#define RAW_MARK 0x10000u

static int sb_unit(sbuf *b, unsigned cp)
{
    if (cp >= RAW_MARK) {
        sb_byte(b, (unsigned char)(cp - RAW_MARK));
        return 0;
    }
    sb_cp(b, cp);
    return 0;
}

/* 解一个 UTF-8 码点，*pp 至少前进 1 字节；非法时返回 -1。 */
static int u8_next(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;
    unsigned cp;
    int n, i;

    if (p >= end) return -1;
    if (*p < 0x80) {
        *pp = p + 1;
        return (int)*p;
    }
    if ((*p & 0xE0) == 0xC0) {
        n = 1;
        cp = *p & 0x1Fu;
    } else if ((*p & 0xF0) == 0xE0) {
        n = 2;
        cp = *p & 0x0Fu;
    } else if ((*p & 0xF8) == 0xF0) {
        n = 3;
        cp = *p & 0x07u;
    } else {
        *pp = p + 1;
        return -1;
    }
    if (p + n >= end) {          /* 截断的序列：只前进 1 字节，后面的字节还会各自再试 */
        *pp = p + 1;
        return -1;
    }
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp = p + 1;
            return -1;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *pp = p + n + 1;
    return (int)cp;
}

/* Python str.isspace() / re 的 \s 集合。写成表比写一长串 || 好对账：
   这是数据，不是逻辑，分支覆盖也不该被它污染。 */
static const unsigned WS_TAB[] = {
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
    0x85, 0xA0, 0x1680, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000
};

static int is_ws(unsigned c)
{
    size_t i;

    if (c >= 0x2000 && c <= 0x200A) return 1;   /* EN QUAD .. HAIR SPACE */
    for (i = 0; i < sizeof(WS_TAB) / sizeof(WS_TAB[0]); i++)
        if (WS_TAB[i] == c) return 1;
    return 0;
}

/* 各类破折号 → '-'（bomcore.DASHES） */
static const unsigned DASH_TAB[] = {
    0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, 0x2212, 0xFF0D
};

static int is_dash(unsigned c)
{
    size_t i;

    for (i = 0; i < sizeof(DASH_TAB) / sizeof(DASH_TAB[0]); i++)
        if (DASH_TAB[i] == c) return 1;
    return 0;
}

/* 位号分隔符（bomcore.REF_SPLIT：[,;、，；\s/\\|]+） */
static int is_sep(unsigned c)
{
    return c == ',' || c == ';' || c == '/' || c == '\\' || c == '|' ||
           c == 0xFF0C || c == 0xFF1B || c == 0x3001 || is_ws(c);
}

static int is_digit(unsigned c)
{
    return (c >= '0' && c <= '9') || (c >= 0xFF10 && c <= 0xFF19);
}

static unsigned to_upper(unsigned c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;   /* 全角 ａ-ｚ（中文表格常见） */
    return c;
}

/* 可增长码点数组。同样先用调用方给的栈块，不够再上堆 —— 逐格调用时
   这一步原来每格一次 malloc，是读取后分析里最大的单项开销。 */
typedef struct {
    unsigned *v;
    size_t n, cap;
    int owned;
} cpbuf;

static void cp_init(cpbuf *b, unsigned *buf, size_t cap)
{
    b->v = buf;
    b->n = 0;
    b->cap = cap;
    b->owned = 0;
}

static void cp_free(cpbuf *b)
{
    if (b->owned) free(b->v);
    b->v = NULL;
    b->n = b->cap = 0;
    b->owned = 0;
}

static void cp_push(cpbuf *b, unsigned c)
{
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        unsigned *q = (unsigned *)malloc(cap * sizeof(unsigned));
        if (!q) return;
        if (b->n) memcpy(q, b->v, b->n * sizeof(unsigned));
        if (b->owned) free(b->v);
        b->v = q;
        b->cap = cap;
        b->owned = 1;
    }
    b->v[b->n++] = c;
}

/* ---------------- norm ----------------
   全角转半角 + 去空白 + 小写。用于列名比对。 */
static void do_norm(const char *s, sbuf *o)
{
    const unsigned char *p, *end, *q;
    int cp;

    sb_reset(o);
    if (!s) return;
    p = (const unsigned char *)s;
    end = p + strlen(s);
    while (p < end) {
        q = p;
        cp = u8_next(&p, end);
        if (cp < 0) {
            sb_byte(o, *q);
            continue;
        }
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        else if (cp == 0x3000) cp = ' ';
        if (is_ws((unsigned)cp)) continue;
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        sb_cp(o, (unsigned)cp);
    }
}

/* ---------------- 料号 ----------------
   strip → 破折号归一 → 去空白 → 大写 → 去掉 pandas 遗留的 ".0" */
/* "xxx-xxxxxx-xxx" 共 14 个字符，连字符在下标 3 和 10。
   两个正则各判各的：PAT_WIDE 要求全串 ASCII alnum，PAT_STRICT 只要求前 9 位
   是数字（Python 的 \d 认全角数字）。所以全角数字能过 strict、过不了 wide ——
   这里必须分开算，否则对不上账。
   输入是码点序列：do_pn 的中间缓冲本来就是 unsigned[]，全角数字进来是 0xFF10 那种值。 */
static void pn_shape(const unsigned *s, size_t n, int *wide, int *strict)
{
    int okw = 1, okst = 1;
    size_t i;

    if (wide) *wide = 0;
    if (strict) *strict = 0;
    if (n != 14 || s[3] != '-' || s[10] != '-') return;
    for (i = 0; i < 14; i++) {
        unsigned c = s[i];
        int alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (i == 3 || i == 10) continue;
        if (!alnum) okw = 0;
        if (i < 3 || (i >= 4 && i <= 9)) {
            if (!is_digit(c)) okst = 0;
        } else {
            if (!alnum) okst = 0;
        }
    }
    if (wide) *wide = okw;
    if (strict) *strict = okst;
}

static void do_pn(const char *v, sbuf *o, int *wide, int *strict)
{
    const unsigned char *p, *end, *q;
    unsigned cpbuf_stack[32];
    cpbuf b;
    size_t i, i0, i1, n;
    int cp;

    sb_reset(o);
    if (wide) *wide = 0;
    if (strict) *strict = 0;
    if (!v) return;

    cp_init(&b, cpbuf_stack, sizeof(cpbuf_stack) / sizeof(cpbuf_stack[0]));
    p = (const unsigned char *)v;
    end = p + strlen(v);
    while (p < end) {
        q = p;
        cp = u8_next(&p, end);
        if (cp < 0) {
            cp_push(&b, RAW_MARK + *q);
            continue;
        }
        if (is_dash((unsigned)cp)) cp = '-';
        cp_push(&b, (unsigned)cp);
    }

    i0 = 0;
    i1 = b.n;
    while (i0 < i1 && is_ws(b.v[i0])) i0++;
    while (i1 > i0 && is_ws(b.v[i1 - 1])) i1--;

    /* s.endswith(".0") and s[:-2].isdigit() —— 前缀为空时 Python 的 "".isdigit()
       是 False，所以至少要有一位数字 */
    if (i1 - i0 >= 3 && b.v[i1 - 2] == '.' && b.v[i1 - 1] == '0') {
        int all_digit = 1;
        for (i = i0; i + 2 < i1; i++)
            if (!is_digit(b.v[i])) { all_digit = 0; break; }
        if (all_digit) i1 -= 2;
    }

    /* 形态判定：去掉空白后再数，所以先压成紧凑序列 */
    n = 0;
    for (i = i0; i < i1; i++) {
        unsigned c = to_upper(b.v[i]);
        if (is_ws(c)) continue;
        b.v[n++] = c;
        sb_unit(o, c);
    }
    /* 形态判定要读紧凑序列，必须先判再释放 cpbuf */
    pn_shape(b.v, n, wide, strict);
    cp_free(&b);
}

/* ---------------- 位号 ----------------
   大写 → 按 [,;、，；\s/\\|] 切 → 逐 token 形态校验 → 去重排序 → '|' 连接 */
static int tok_ok(const unsigned *v, size_t n)
{
    size_t i = 0, nd = 0, suf = 0;

    while (i < n && v[i] >= 'A' && v[i] <= 'Z' && i < 3) i++;
    if (i < 1 || i > 3) return 0;
    while (i < n && is_digit(v[i]) && nd < 5) { i++; nd++; }
    if (nd < 1) return 0;
    while (i < n && ((v[i] >= 'A' && v[i] <= 'Z') ||
                     (v[i] >= '0' && v[i] <= '9'))) { i++; suf++; }
    if (suf > 2) return 0;
    return i == n;
}

static int cmp_ptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* 小数组插入排序：位号行通常 2~4 个 token，qsort 的间接调用与分区开销比
   比较本身还贵。n >= 16 才退回 qsort。 */
static void sort_ptr(char **p, size_t n, int (*cmp)(const void *, const void *))
{
    size_t i, j;

    if (n >= 16) {
        qsort(p, n, sizeof(char *), cmp);
        return;
    }
    for (i = 1; i < n; i++) {
        char *v = p[i];
        for (j = i; j > 0 && cmp(&p[j - 1], &v) > 0; j--) p[j] = p[j - 1];
        p[j] = v;
    }
}

static void do_ref(const char *v, sbuf *o, int *n_ok, int *n_bad)
{
    const unsigned char *p, *end;
    char ptmp[1024];
    unsigned ttmp[32];
    sbuf pool;
    /* 栈上先备 32 个 token 的位置，一行位号超过 32 个才真去堆上要 */
    size_t offs_stack[32];
    size_t *offs = offs_stack;
    size_t offs_cap = sizeof(offs_stack) / sizeof(offs_stack[0]);
    char *ptrs_stack[16];
    char **ptrs = ptrs_stack;
    size_t n = 0, i, m;
    cpbuf tok;
    int cp, ok = 0, bad = 0, offs_heap = 0, ptrs_heap = 0;

    sb_reset(o);
    if (n_ok) *n_ok = 0;
    if (n_bad) *n_bad = 0;
    if (!v) return;

    sb_stack(&pool, ptmp, sizeof(ptmp));
    cp_init(&tok, ttmp, sizeof(ttmp) / sizeof(ttmp[0]));

    p = (const unsigned char *)v;
    end = p + strlen(v);
    for (;;) {
        const unsigned char *start = p;
        int at_end = (p >= end);
        cp = at_end ? -1 : u8_next(&p, end);
        if (!at_end && cp >= 0 && !is_sep((unsigned)cp)) {
            cp_push(&tok, to_upper((unsigned)cp));
            continue;
        }
        if (!at_end && cp < 0) {          /* 非法字节：当一个字符留在 token 里 */
            cp_push(&tok, RAW_MARK + *start);
            continue;
        }
        /* 切分点 */
        if (tok.n) {
            if (tok_ok(tok.v, tok.n)) {
                ok++;
                if (n == offs_cap) {
                    size_t nc = offs_cap * 2;
                    size_t *q2;
                    if (offs_heap) {
                        q2 = (size_t *)realloc(offs, nc * sizeof(size_t));
                    } else {
                        q2 = (size_t *)malloc(nc * sizeof(size_t));
                        if (q2) {
                            memcpy(q2, offs, n * sizeof(size_t));
                            offs_heap = 1;
                        }
                    }
                    if (q2) { offs = q2; offs_cap = nc; }
                }
                if (n < offs_cap) {
                    /* 每个 token 各带一个 '\0'，池扩容后偏移仍然有效 */
                    if (sb_reserve(&pool, pool.len + tok.n) == 0) {
                        offs[n++] = pool.len;
                        for (i = 0; i < tok.n; i++) sb_unit(&pool, tok.v[i]);
                        sb_byte(&pool, 0);
                    }
                }
            } else {
                bad++;
            }
            tok.n = 0;
        }
        if (at_end) break;
    }
    cp_free(&tok);

    /* 去重 + 排序后连接 */
    if (n) {
        if (n > sizeof(ptrs_stack) / sizeof(ptrs_stack[0])) {
            ptrs = (char **)malloc(n * sizeof(char *));
            ptrs_heap = ptrs != NULL;
        }
        if (ptrs) {
            for (i = 0; i < n; i++) ptrs[i] = pool.p + offs[i];
            sort_ptr(ptrs, n, cmp_ptr);
            m = 0;
            for (i = 0; i < n; i++) {
                if (i && strcmp(ptrs[i], ptrs[i - 1]) == 0) continue;
                if (m++) sb_byte(o, '|');
                sb_write(o, ptrs[i], strlen(ptrs[i]));
            }
            if (ptrs_heap) free(ptrs);
        }
    }
    if (offs_heap) free(offs);
    if (pool.owned) free(pool.p);

    if (n_ok) *n_ok = ok;
    if (n_bad) *n_bad = bad;
}

/* ---------------- 导出到调用方缓冲 ---------------- */
static int sb_export(sbuf *b, char *out, size_t cap)
{
    size_t n = b->len;
    int rc = (int)n;

    if (!out || cap == 0) {
        if (b->owned) free(b->p);
        b->p = NULL;
        return -1;
    }
    if (n + 1 > cap) { n = cap - 1; rc = -1; }
    if (n) memcpy(out, b->p, n);
    out[n] = 0;
    if (b->owned) free(b->p);
    b->p = NULL;
    b->cap = 0;
    return rc;
}

int bs_norm_str(const char *s, char *out, size_t cap)
{
    char tmp[256];
    sbuf b;

    /* 快路径：纯 ASCII、无空白、无大写 —— 此时 do_norm 的输出就是输入本身。
       表头打分要过一遍全表每个格子，而料号/数量/位号这些占绝大多数的格子
       恰好都落在这条路上；省掉的是「逐字符 UTF-8 解码再编码」的双份工。 */
    if (s && *s) {
        const unsigned char *q = (const unsigned char *)s;
        while (*q) {
            unsigned c = *q;
            if (c < 0x21 || c >= 0x80 || (c >= 'A' && c <= 'Z')) break;
            q++;
        }
        if (!*q) {
            size_t n = (size_t)(q - (const unsigned char *)s);
            if (!out || cap == 0) return -1;
            if (n + 1 > cap) {
                memcpy(out, s, cap - 1);
                out[cap - 1] = 0;
                return -1;
            }
            memcpy(out, s, n + 1);
            return (int)n;
        }
    }

    sb_stack(&b, tmp, sizeof(tmp));
    do_norm(s, &b);
    return sb_export(&b, out, cap);
}

int bs_pn_str(const char *v, char *out, size_t cap, int *wide, int *strict)
{
    char tmp[256];
    sbuf b;

    /* 快路径：纯 ASCII、无空白、无小写、不以 ".0" 收尾 —— 此时 do_pn 的等价
       结果就是「原样大写」（本来就是大写），不必逐字符解码再编码一遍。
       料号列 10 万行全走这条，省下的是 rowbuild 里最大的一笔。 */
    if (v && *v && out && cap) {
        const unsigned char *p = (const unsigned char *)v;
        size_t n = 0;
        int ok = 1;
        while (p[n]) {
            unsigned c = p[n];
            if (c >= 0x80 || c < 0x21 || (c >= 'a' && c <= 'z')) { ok = 0; break; }
            n++;
        }
        /* 结尾 ".0" 可能被剥掉，交给慢路径处理 */
        if (ok && n >= 2 && p[n - 2] == '.' && p[n - 1] == '0') ok = 0;
        if (ok && n + 1 <= cap) {
            memcpy(out, v, n + 1);
            if (n == 14) {          /* 只有 14 位才可能是 xxx-xxxxxx-xxx */
                unsigned cp14[14];
                size_t k;
                for (k = 0; k < 14; k++) cp14[k] = p[k];
                pn_shape(cp14, n, wide, strict);
            } else {
                if (wide) *wide = 0;
                if (strict) *strict = 0;
            }
            return (int)n;
        }
    }

    sb_stack(&b, tmp, sizeof(tmp));
    do_pn(v, &b, wide, strict);
    return sb_export(&b, out, cap);
}

int bs_ref_sig_str(const char *v, char *out, size_t cap, int *n_ok, int *n_bad)
{
    char tmp[256];
    sbuf b;

    sb_stack(&b, tmp, sizeof(tmp));
    do_ref(v, &b, n_ok, n_bad);
    return sb_export(&b, out, cap);
}

/* ---------------- 复用型：一次算齐一行的三个结果 ---------------- */
void bs_keys_init(bs_keys *k)
{
    if (!k) return;
    memset(k, 0, sizeof(*k));
}

void bs_keys_free(bs_keys *k)
{
    if (!k) return;
    free(k->pn);
    free(k->ref);
    free(k->key);
    memset(k, 0, sizeof(*k));
}

void bs_keys_make(bs_keys *k, const char *pn_raw, const char *ref_raw)
{
    sbuf a, r, y;

    if (!k) return;
    sb_wrap(&a, &k->pn, &k->pn_cap);
    sb_wrap(&r, &k->ref, &k->ref_cap);
    sb_wrap(&y, &k->key, &k->key_cap);

    do_pn(pn_raw, &a, &k->wide, &k->strict);
    do_ref(ref_raw, &r, &k->n_ok, &k->n_bad);

    if (a.len) {
        k->kind = 'P';
        sb_write(&y, "P:", 2);
        sb_write(&y, a.p, a.len);
    } else if (r.len) {
        k->kind = 'R';
        sb_write(&y, "R:", 2);
        sb_write(&y, r.p, r.len);
    } else {
        k->kind = 0;
    }

    sb_unwrap(&a, &k->pn, &k->pn_cap);
    sb_unwrap(&r, &k->ref, &k->ref_cap);
    sb_unwrap(&y, &k->key, &k->key_cap);
}

char bs_make_key(const char *pn_raw, const char *ref_raw, char *out, size_t cap)
{
    bs_keys k;
    char kind;

    bs_keys_init(&k);
    bs_keys_make(&k, pn_raw, ref_raw);
    kind = k.kind;
    if (out && cap) {
        size_t n = k.key ? strlen(k.key) : 0;
        if (n + 1 > cap) n = cap - 1;
        if (n) memcpy(out, k.key, n);
        out[n] = 0;
    }
    bs_keys_free(&k);
    return kind;
}
