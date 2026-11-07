/* DEFLATE 压缩（固定 Huffman + LZ77）。规范见 RFC 1951 §3.2。
 *
 * 为什么只用固定 Huffman：动态码要先扫两遍（统计词频 + 写码表），代码量翻倍；
 * xlsx 的 sheet xml 重复度极高，固定码已经能压到 1/8 上下，不值得为再省
 * 几个百分点引入一棵可变的码表 —— 那也是最容易写错、最难回归的部分。
 *
 * 位序：deflate 的位流是「低位先出」，而 Huffman 码在规范里按高位先写，
 * 所以 put_code() 先把码反转再交给 put_bits()。
 */
#include "bs_deflate.h"

#include <stdlib.h>
#include <string.h>

#define MIN_MATCH 3
#define MAX_MATCH 258
#define WSIZE 32768
#define HBITS 15
#define HSIZE (1 << HBITS)
#define MAX_CHAIN 24

/* ---------------- 位输出 ---------------- */
typedef struct {
    unsigned char *p;
    size_t len, cap;
    unsigned buf;
    int cnt;
    int oom;
} bitout;

static void put_bits(bitout *b, unsigned v, int n)
{
    if (b->oom) return;
    b->buf |= (v & ((1u << n) - 1)) << b->cnt;
    b->cnt += n;
    while (b->cnt >= 8) {
        if (b->len >= b->cap) {   /* 预留量算错才会走到这里，按失败处理 */
            b->oom = 1;
            return;
        }
        b->p[b->len++] = (unsigned char)(b->buf & 0xFFu);
        b->buf >>= 8;
        b->cnt -= 8;
    }
}

/* 写一个 Huffman 码：码是高位先写，位流是低位先出，故先反转 */
static void put_code(bitout *b, unsigned code, int n)
{
    unsigned r = 0;
    int i;

    for (i = 0; i < n; i++) {
        r = (r << 1) | (code & 1u);
        code >>= 1;
    }
    put_bits(b, r, n);
}

/* ---------------- 固定 Huffman 表（规范 3.2.6） ---------------- */
static void emit_ll(bitout *b, int sym)
{
    if (sym < 144) put_code(b, (unsigned)(0x30 + sym), 8);
    else if (sym < 256) put_code(b, (unsigned)(0x190 + sym - 144), 9);
    else if (sym < 280) put_code(b, (unsigned)(sym - 256), 7);
    else put_code(b, (unsigned)(0xC0 + sym - 280), 8);
}

static const unsigned short LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577};
static const unsigned char DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* 长度 -> 码序号。每次匹配都要查，用表而不是二分 */
static unsigned char LEN_IDX[MAX_MATCH + 1];
static int len_idx_ready = 0;

static void init_len_idx(void)
{
    int i, k;

    if (len_idx_ready) return;   /* 两个线程同时进来也是写同一份值，无害 */
    for (i = 0; i < 29; i++) {
        int hi = (i == 28) ? MAX_MATCH : (int)LEN_BASE[i + 1] - 1;
        for (k = LEN_BASE[i]; k <= hi && k <= MAX_MATCH; k++) LEN_IDX[k] = (unsigned char)i;
    }
    len_idx_ready = 1;
}

static void emit_match(bitout *b, int len, int dist)
{
    const int li = LEN_IDX[len];
    int di;

    emit_ll(b, 257 + li);
    if (LEN_EXTRA[li]) put_bits(b, (unsigned)(len - LEN_BASE[li]), LEN_EXTRA[li]);

    for (di = 29; di > 0; di--)
        if (dist >= DIST_BASE[di]) break;
    put_code(b, (unsigned)di, 5);
    if (DIST_EXTRA[di]) put_bits(b, (unsigned)(dist - DIST_BASE[di]), DIST_EXTRA[di]);
}

/* ---------------- LZ77 ---------------- */
static unsigned hash3(const unsigned char *p)
{
    return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ (unsigned)p[2]) & (HSIZE - 1);
}

int bs_deflate_raw(const unsigned char *src, size_t n,
                   unsigned char **out, size_t *out_len)
{
    bitout b;
    int *head = NULL;
    int *prev = NULL;
    size_t i, cap;

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!src || n == 0) return -1;

    init_len_idx();

    /* 最坏情况：每个字节都当字面量（9 位）→ n*9/8，再加块头与结束码 */
    cap = n + n / 4 + 64;
    memset(&b, 0, sizeof b);
    b.p = (unsigned char *)malloc(cap);
    if (!b.p) return -1;
    b.cap = cap;

    head = (int *)malloc((size_t)HSIZE * sizeof(int));
    prev = (int *)malloc(n * sizeof(int));
    if (!head || !prev) {
        free(head);
        free(prev);
        free(b.p);
        return -1;
    }
    for (i = 0; i < HSIZE; i++) head[i] = -1;

    put_bits(&b, 1, 1);      /* BFINAL = 1（整段一个块） */
    put_bits(&b, 1, 2);      /* BTYPE  = 01（固定 Huffman） */

    i = 0;
    while (i < n && !b.oom) {
        int best_len = 0, best_dist = 0;

        if (i + MIN_MATCH <= n) {
            const unsigned h = hash3(src + i);
            size_t maxlen = n - i;
            int j = head[h];
            int chain = MAX_CHAIN;

            if (maxlen > MAX_MATCH) maxlen = MAX_MATCH;
            while (j >= 0 && chain-- > 0) {
                size_t dist = i - (size_t)j;
                if (dist > WSIZE) break;
                /* 先比「已知最长位置上的那个字节」，多数候选第一步就被否掉 */
                if (src[j + (size_t)best_len] == src[i + (size_t)best_len]) {
                    size_t l = 0;
                    while (l < maxlen && src[j + l] == src[i + l]) l++;
                    if (l > (size_t)best_len) {
                        best_len = (int)l;
                        best_dist = (int)dist;
                        if (l == maxlen) break;
                    }
                }
                j = prev[j];
            }
        }

        if (best_len >= MIN_MATCH) {
            int k;
            emit_match(&b, best_len, best_dist);
            /* 匹配段里的每个位置都要进哈希链，否则后续会漏掉可行匹配 */
            for (k = 0; k < best_len; k++) {
                const size_t pos = i + (size_t)k;
                if (pos + MIN_MATCH <= n) {
                    const unsigned h = hash3(src + pos);
                    prev[pos] = head[h];
                    head[h] = (int)pos;
                }
            }
            i += (size_t)best_len;
        } else {
            emit_ll(&b, src[i]);
            if (i + MIN_MATCH <= n) {
                const unsigned h = hash3(src + i);
                prev[i] = head[h];
                head[h] = (int)i;
            }
            i++;
        }
    }

    emit_ll(&b, 256);        /* 块结束 */
    if (b.cnt > 0 && !b.oom) { /* 补到字节边界 */
        if (b.len < b.cap) b.p[b.len++] = (unsigned char)(b.buf & 0xFFu);
        else b.oom = 1;
    }

    free(head);
    free(prev);
    if (b.oom) {
        free(b.p);
        return -1;
    }
    if (out) *out = b.p;
    if (out_len) *out_len = b.len;
    return 0;
}
