#include "bs_xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ATTR 32

typedef struct {
    char *buf;
    size_t cap;
} scratch;

static char *sc_reserve(scratch *s, size_t need)
{
    if (need <= s->cap) return s->buf;
    {
        size_t cap = s->cap ? s->cap : 512;
        char *np;
        while (cap < need) cap *= 2;
        np = (char *)realloc(s->buf, cap);
        if (!np) return NULL;
        s->buf = np;
        s->cap = cap;
    }
    return s->buf;
}

static int is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* 写一个码点到 UTF-8 */
static size_t put_utf8(char *d, unsigned long cp)
{
    if (cp < 0x80) { d[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        d[0] = (char)(0xC0 | (cp >> 6));
        d[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        d[0] = (char)(0xE0 | (cp >> 12));
        d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        d[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    d[0] = (char)(0xF0 | (cp >> 18));
    d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    d[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    d[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* 实体解码。未知实体原样保留 —— Excel 的文本里出现裸 & 时按这种做法不至于炸。 */
static size_t unescape(const unsigned char *s, size_t n, char *d)
{
    size_t i = 0, o = 0;

    /* 快路径：既没有实体也没有 CR 的段落直接整段拷。xlsx 里绝大多数文本
       （料号、位号、描述）都落在这一档，省掉的是逐字节的搬砖。
       短串不走 memchr —— 单元格文本平均不到 16 字节，函数调用与跳转开销
       比逐个字节看还贵；长串才交给 SIMD。 */
    if (n < 24) {
        const unsigned char *q = s, *e = s + n;
        while (q < e && *q != '&' && *q != '\r') q++;
        if (q == e) {
            memcpy(d, s, n);
            return n;
        }
    } else if (!memchr(s, '&', n) && !memchr(s, '\r', n)) {
        memcpy(d, s, n);
        return n;
    }

    while (i < n) {
        /* XML 1.0 §2.11：换行归一化。CRLF / CR 都算一个 LF。
           不做的话单元格文本里会多出一个 CR，
           与 Python ElementTree 的解析结果对不上（对账会在这里分叉）。 */
        if (s[i] == '\r') {
            d[o++] = '\n';
            i += (i + 1 < n && s[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        if (s[i] != '&') { d[o++] = (char)s[i++]; continue; }
        {
            size_t j = i + 1;
            while (j < n && j - i < 12 && s[j] != ';') j++;
            if (j < n && s[j] == ';') {
                size_t len = j - i - 1;
                const unsigned char *e = s + i + 1;
                if (len == 3 && memcmp(e, "amp", 3) == 0) { d[o++] = '&'; i = j + 1; continue; }
                if (len == 2 && memcmp(e, "lt", 2) == 0) { d[o++] = '<'; i = j + 1; continue; }
                if (len == 2 && memcmp(e, "gt", 2) == 0) { d[o++] = '>'; i = j + 1; continue; }
                if (len == 4 && memcmp(e, "quot", 4) == 0) { d[o++] = '"'; i = j + 1; continue; }
                if (len == 4 && memcmp(e, "apos", 4) == 0) { d[o++] = '\''; i = j + 1; continue; }
                if (len >= 2 && e[0] == '#') {
                    unsigned long cp = 0;
                    size_t k = 1;
                    int hex = 0;
                    if (e[1] == 'x' || e[1] == 'X') { hex = 1; k = 2; }
                    if (k < len) {
                        int ok = 1;
                        for (; k < len; k++) {
                            int c = e[k];
                            int v;
                            if (c >= '0' && c <= '9') v = c - '0';
                            else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
                            else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
                            else { ok = 0; break; }
                            cp = cp * (hex ? 16 : 10) + (unsigned long)v;
                            if (cp > 0x10FFFF) { ok = 0; break; }
                        }
                        if (ok) { o += put_utf8(d + o, cp); i = j + 1; continue; }
                    }
                }
            }
        }
        d[o++] = '&';
        i++;
    }
    return o;
}

typedef struct {
    const unsigned char *p;
    size_t n, i;
    scratch sc;
    const bsx_cb *cb;
    int depth;
} parser;

static void emit_text(parser *ps, const unsigned char *s, size_t n)
{
    char *d;
    size_t o;
    if (!ps->cb->on_text || n == 0) return;
    d = sc_reserve(&ps->sc, n + 8);
    if (!d) return;
    o = unescape(s, n, d);
    if (o) ps->cb->on_text(ps->cb->ud, d, o);
}

/* 找 s[i..] 里的终止串，返回其起始下标；找不到返回 n */
static size_t find_seq(const unsigned char *p, size_t n, size_t i, const char *needle)
{
    size_t k = strlen(needle);
    if (k == 0) return i;
    while (i + k <= n) {
        if (p[i] == (unsigned char)needle[0] && memcmp(p + i, needle, k) == 0) return i;
        i++;
    }
    return n;
}

int bsx_parse(const unsigned char *p, size_t n, const bsx_cb *cb, char *err, size_t errcap)
{
    parser ps;
    const char *attr[MAX_ATTR * 2];
    int rc = 0;

    if (!p || !cb) {
        if (err) snprintf(err, errcap, "xml 输入为空");
        return -1;
    }
    memset(&ps, 0, sizeof ps);
    ps.p = p;
    ps.n = n;
    ps.cb = cb;

    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        if (err) snprintf(err, errcap, "不支持 UTF-16 编码的 xml");
        return -1;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ps.i = 3;

    while (ps.i < n) {
        if (p[ps.i] != '<') {
            size_t j = ps.i;
            while (j < n && p[j] != '<') j++;
            emit_text(&ps, p + ps.i, j - ps.i);
            ps.i = j;
            continue;
        }
        if (ps.i + 1 >= n) break;

        if (p[ps.i + 1] == '!') {
            if (ps.i + 3 < n && p[ps.i + 2] == '-' && p[ps.i + 3] == '-') {
                size_t j = find_seq(p, n, ps.i + 3, "-->");
                ps.i = (j >= n) ? n : j + 3;
            } else if (ps.i + 8 < n && memcmp(p + ps.i, "<![CDATA[", 9) == 0) {
                size_t j = find_seq(p, n, ps.i + 9, "]]>");
                if (cb->on_text && j > ps.i + 9)
                    cb->on_text(cb->ud, (const char *)(p + ps.i + 9), j - (ps.i + 9));
                ps.i = (j >= n) ? n : j + 3;
            } else { /* DOCTYPE 之类，跳过 */
                size_t j = ps.i;
                while (j < n && p[j] != '>') j++;
                ps.i = (j >= n) ? n : j + 1;
            }
            continue;
        }

        if (p[ps.i + 1] == '?') {
            size_t j = find_seq(p, n, ps.i + 2, "?>");
            ps.i = (j >= n) ? n : j + 2;
            continue;
        }

        if (p[ps.i + 1] == '/') {
            size_t s = ps.i + 2, j = s;
            char *nm;
            while (j < n && p[j] != '>' && !is_space(p[j])) j++;
            nm = sc_reserve(&ps.sc, j - s + 1);
            if (nm) {
                memcpy(nm, p + s, j - s);
                nm[j - s] = 0;
                if (cb->on_close) cb->on_close(cb->ud, nm);
            }
            while (j < n && p[j] != '>') j++;
            ps.i = (j >= n) ? n : j + 1;
            continue;
        }

        {
            /* 起始标签 */
            size_t s = ps.i + 1, j = s;
            size_t need;
            char *nm;
            int nattr = 0, self_close = 0;

            while (j < n && p[j] != '>' && p[j] != '/' && !is_space(p[j])) j++;
            need = (j - s) + 2 + (n - s) / 2 + 64;
            nm = sc_reserve(&ps.sc, need > 65536 ? 65536 : need);
            if (!nm) { rc = -1; break; }

            /* 名字直接拷在 scratch 头部；属性值随后追加（scratch 可能被 realloc，
               所以先把整个标签的原始字节跨度算出来，一次性预留够） */
            {
                size_t tag_end = j;
                int in_quote = 0;
                char q = 0;
                while (tag_end < n) {
                    char c = (char)p[tag_end];
                    if (in_quote) {
                        if (c == q) in_quote = 0;
                    } else if (c == '"' || c == '\'') {
                        in_quote = 1;
                        q = c;
                    } else if (c == '>') {
                        break;
                    }
                    tag_end++;
                }
                if (!sc_reserve(&ps.sc, (tag_end - s) * 2 + 128)) { rc = -1; break; }
                nm = ps.sc.buf;
            }

            memcpy(nm, p + s, j - s);
            nm[j - s] = 0;
            {
                char *w = nm + (j - s) + 1;
                char *lim = ps.sc.buf + ps.sc.cap;
                ps.i = j;
                for (;;) {
                    while (ps.i < n && is_space(p[ps.i])) ps.i++;
                    if (ps.i >= n) break;
                    if (p[ps.i] == '/') { self_close = 1; ps.i++; continue; }
                    if (p[ps.i] == '>') { ps.i++; break; }

                    {
                        size_t a0 = ps.i, v0, v1;
                        while (ps.i < n && p[ps.i] != '=' && p[ps.i] != '>' &&
                               p[ps.i] != '/' && !is_space(p[ps.i]))
                            ps.i++;
                        if (ps.i >= n) break;
                        while (ps.i < n && is_space(p[ps.i])) ps.i++;
                        if (ps.i >= n || p[ps.i] != '=') {
                            /* 没有 '=' 的裸属性：跳过 */
                            if (ps.i < n && p[ps.i] != '>') ps.i++;
                            continue;
                        }
                        ps.i++;
                        while (ps.i < n && is_space(p[ps.i])) ps.i++;
                        if (ps.i >= n) break;
                        {
                            char q = (char)p[ps.i];
                            if (q != '"' && q != '\'') break;
                            ps.i++;
                            v0 = ps.i;
                            while (ps.i < n && p[ps.i] != (unsigned char)q) ps.i++;
                            v1 = ps.i;
                            if (ps.i < n) ps.i++;
                        }

                        /* 名字 + 值都要 NUL 结尾，写进 scratch 尾部 */
                        if (w + (ps.i - a0) + 8 >= lim) { /* 属性太多，放弃后续 */
                            continue;
                        }
                        {
                            size_t kl = 0;
                            /* 属性名 */
                            {
                                size_t e = a0;
                                while (e < n && p[e] != '=' && p[e] != '>' &&
                                       p[e] != '/' && !is_space(p[e]))
                                    e++;
                                kl = e - a0;
                            }
                            if (nattr < MAX_ATTR) {
                                char *wn = w;
                                memcpy(w, p + a0, kl);
                                w[kl] = 0;
                                w += kl + 1;
                                attr[nattr * 2] = wn;
                                {
                                    char *wv = w;
                                    w += unescape(p + v0, v1 - v0, w);
                                    *w++ = 0;
                                    attr[nattr * 2 + 1] = wv;
                                }
                                nattr++;
                            }
                        }
                    }
                }
            }

            if (cb->on_open) cb->on_open(cb->ud, nm, attr, nattr);
            if (self_close && cb->on_close) cb->on_close(cb->ud, nm);
            if (ps.i == s) ps.i++; /* 保险：绝不原地打转 */
        }
    }

    free(ps.sc.buf);
    if (rc != 0 && err) snprintf(err, errcap, "内存不足（xml 解析缓冲）");
    return rc;
}
