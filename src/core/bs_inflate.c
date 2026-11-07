/* DEFLATE 解压（RFC 1951）。
 *
 * 为什么自己写：MinGW 不带可静态链接的 zlib，样本里 12 个 zip 条目全是
 * method 8（deflate）；为 3 个函数拉一个第三方库，不如按规范写一遍。
 *
 * 算法是规范里的标准做法：范式 Huffman（canonical），按位从低到高读。
 * 解码用 count/symbol 表逐位比，不建快查表 —— xlsx 的 xml 解压速度不是瓶颈
 * （16 KB 的样本解压耗时在微秒级），代码短、好核对比快更重要。
 *
 * 不支持的：加密、ZIP64、preset dictionary。前两个在 xlsx 里不会出现，
 * 第三个 xlsx 不使用；真遇到会在 err 里说清楚，不静默出错。
 */
#include "bs_inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

/* ---------------- 输出缓冲 ---------------- */
typedef struct {
    unsigned char *p;
    size_t len, cap;
    int oom;
} outbuf;

static int out_need(outbuf *o, size_t extra)
{
    if (o->oom) return 0;
    if (o->len + extra <= o->cap) return 1;
    size_t cap = o->cap ? o->cap : 65536;
    while (cap < o->len + extra) {
        if (cap > (size_t)-1 / 2) { o->oom = 1; return 0; }
        cap *= 2;
    }
    unsigned char *np = (unsigned char *)realloc(o->p, cap);
    if (!np) { o->oom = 1; return 0; }
    o->p = np;
    o->cap = cap;
    return 1;
}

/* ---------------- 位读取（低位在前） ---------------- */
typedef struct {
    const unsigned char *in;
    size_t n, pos;
    unsigned bitbuf;
    int bitcnt;
    int overrun;
} bitrd;

/* 只保证「至少有 need 位可用」，不消耗。快表命中后才知道该丢几位 */
static int bitfill(bitrd *b, int need)
{
    while (b->bitcnt < need) {
        if (b->pos >= b->n) return -1;
        b->bitbuf |= (unsigned)b->in[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    return 0;
}

static int bits(bitrd *b, int need)
{
    unsigned val;
    if (bitfill(b, need)) { b->overrun = 1; return 0; }
    val = b->bitbuf & ((1u << need) - 1);
    b->bitbuf >>= need;
    b->bitcnt -= need;
    return (int)val;
}

/* ---------------- 范式 Huffman ---------------- */
#define FASTBITS 9
#define FASTSIZE (1 << FASTBITS)

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
    /* 快表：低 9 位直接给出 (符号 << 4) | 码长；0 表示这个 9 位前缀
       对应的是长码（或非法模式），走慢路径逐位解。短码一律能命中。 */
    unsigned short fast[FASTSIZE];
} huff;

/* 把 9 位以内的码展开进快表：一个 len 位的码占满所有低 len 位相同的位置 */
static void huff_fast(huff *h)
{
    unsigned next_code[MAXBITS + 1];
    unsigned bl[MAXBITS + 1];
    unsigned code = 0;
    int i, len, idx = 0;

    for (i = 0; i < FASTSIZE; i++) h->fast[i] = 0;
    /* count[0] 是「没有码的符号数」，算 next_code 时必须当 0 看（zlib 同样处理） */
    bl[0] = 0;
    for (len = 1; len <= MAXBITS; len++) bl[len] = (unsigned)h->count[len];
    for (len = 1; len <= MAXBITS; len++) {
        code = (code + bl[len - 1]) << 1;
        next_code[len] = code;
    }

    for (len = 1; len <= MAXBITS; len++) {
        int k;
        for (k = 0; k < h->count[len]; k++) {
            int sym = h->symbol[idx++];
            unsigned c = next_code[len]++;
            if (len <= FASTBITS) {
                unsigned rev = 0, t = c;
                int j;
                /* deflate 的位流是低位先出，规范码是高位先出 → 位序要翻 */
                for (j = 0; j < len; j++) { rev = (rev << 1) | (t & 1); t >>= 1; }
                for (j = (int)rev; j < FASTSIZE; j += 1 << len)
                    h->fast[j] = (unsigned short)((sym << 4) | len);
            }
        }
    }
}

/* 返回 >=0 表示成功（0 = 完整码，>0 = 不完整码，都可解）；
   -1 表示过度订阅（长度表本身非法）。 */
static int huff_build(huff *h, const short *length, int n)
{
    int i, left;
    short offs[MAXBITS + 1];

    for (i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) {          /* 全 0：空树，没有可解符号 */
        huff_fast(h);                /* 仍要清快表，否则解码器读到未初始化数据 */
        return 0;
    }

    left = 1;
    for (i = 1; i <= MAXBITS; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) {
            huff_fast(h);
            return -1;
        }
    }

    offs[1] = 0;
    for (i = 1; i < MAXBITS; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
    huff_fast(h);
    return left;
}

static int huff_decode(bitrd *b, const huff *h)
{
    int code = 0, first = 0, index = 0, len;

    /* 快路径：一次查表出一个码。短码（绝大多数）都走这里，
       长码查不到（fast 为 0）才退回逐位比对。 */
    if (bitfill(b, FASTBITS) == 0) {
        unsigned short e = h->fast[b->bitbuf & (FASTSIZE - 1)];
        if (e) {
            int n = e & 15;
            b->bitbuf >>= n;
            b->bitcnt -= n;
            return (int)(e >> 4);
        }
    }

    for (len = 1; len <= MAXBITS; len++) {
        int bit = bits(b, 1);
        if (b->overrun) return -1;
        code |= bit;
        {
            int count = h->count[len];
            if (code - first < count) return h->symbol[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
    }
    return -1;
}

/* 长度 / 距离基值与附加位（规范 3.2.5） */
static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                6145, 8193, 12289, 16385, 24577};
static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int inflate_codes(bitrd *b, outbuf *o, const huff *lencode, const huff *distcode)
{
    for (;;) {
        int sym = huff_decode(b, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (!out_need(o, 1)) return -2;
            o->p[o->len++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int len, dsym;
            size_t dist;
            unsigned char *dst;
            const unsigned char *src;

            sym -= 257;
            if (sym >= 29) return -1;
            len = lbase[sym] + bits(b, lext[sym]);
            if (b->overrun) return -1;

            dsym = huff_decode(b, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            dist = (size_t)dbase[dsym] + (size_t)bits(b, dext[dsym]);
            if (b->overrun) return -1;
            if (dist > o->len) return -1; /* 引用了还没输出的数据 */
            if (!out_need(o, (size_t)len)) return -2;

            dst = o->p + o->len;
            src = dst - dist;
            /* 允许重叠（dist < len），必须逐字节拷 */
            for (int i = 0; i < len; i++) dst[i] = src[i];
            o->len += (size_t)len;
        }
    }
}

static int inflate_stored(bitrd *b, outbuf *o)
{
    int len, nlen;
    b->bitbuf = 0;
    b->bitcnt = 0; /* 丢弃对齐用的残余位（bits() 最多预取到 7 位） */
    if (b->pos + 4 > b->n) return -1;
    len = b->in[b->pos] | (b->in[b->pos + 1] << 8);
    nlen = b->in[b->pos + 2] | (b->in[b->pos + 3] << 8);
    b->pos += 4;
    if ((len ^ 0xFFFF) != nlen) return -1;
    if (b->pos + (size_t)len > b->n) return -1;
    if (!out_need(o, (size_t)len)) return -2;
    memcpy(o->p + o->len, b->in + b->pos, (size_t)len);
    o->len += (size_t)len;
    b->pos += (size_t)len;
    return 0;
}

static int build_fixed(huff *lencode, huff *distcode)
{
    short lengths[FIXLCODES];
    int i;
    for (i = 0; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < FIXLCODES; i++) lengths[i] = 8;
    if (huff_build(lencode, lengths, FIXLCODES) < 0) return -1;

    for (i = 0; i < 32; i++) lengths[i] = 5;
    if (huff_build(distcode, lengths, 32) < 0) return -1;
    return 0;
}

static int inflate_fixed(bitrd *b, outbuf *o)
{
    /* 表就地建，不用 static：fixed 树构建约 1 µs，而一个 deflate 块
       最大 64 KB、整本 xlsx 也就几百块，摊下来可以忽略；
       换来的是本函数没有任何共享可变状态，两个线程各解各的互不干扰。 */
    huff lencode, distcode;

    if (build_fixed(&lencode, &distcode) != 0) return -1;
    return inflate_codes(b, o, &lencode, &distcode);
}

static int inflate_dynamic(bitrd *b, outbuf *o)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                    11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXCODES];
    huff lencode, distcode;
    int nlen, ndist, ncode, i, idx;

    nlen = bits(b, 5) + 257;
    ndist = bits(b, 5) + 1;
    ncode = bits(b, 4) + 4;
    if (b->overrun || nlen > MAXLCODES || ndist > MAXDCODES) return -1;

    for (i = 0; i < ncode; i++) lengths[order[i]] = (short)bits(b, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (b->overrun) return -1;
    if (huff_build(&lencode, lengths, 19) < 0) return -1;

    idx = 0;
    while (idx < nlen + ndist) {
        int sym = huff_decode(b, &lencode);
        int len = 0, rep;
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[idx++] = (short)sym;
            continue;
        }
        if (sym == 16) {
            if (idx == 0) return -1;
            len = lengths[idx - 1];
            rep = 3 + bits(b, 2);
        } else if (sym == 17) {
            rep = 3 + bits(b, 3);
        } else {
            rep = 11 + bits(b, 7);
        }
        if (b->overrun) return -1;
        if (idx + rep > nlen + ndist) return -1;
        while (rep--) lengths[idx++] = (short)len;
    }

    if (lengths[256] == 0) return -1; /* 必须有块结束符 */
    if (huff_build(&lencode, lengths, nlen) < 0) return -1;
    if (huff_build(&distcode, lengths + nlen, ndist) < 0) return -1;
    return inflate_codes(b, o, &lencode, &distcode);
}

int bs_inflate_raw(const unsigned char *src, size_t n,
                   unsigned char **out, size_t *out_len,
                   size_t hint, char *err, size_t errcap)
{
    bitrd b;
    outbuf o;
    int last;

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!src || n == 0) {
        if (err) snprintf(err, errcap, "压缩数据为空");
        return -1;
    }

    memset(&b, 0, sizeof b);
    memset(&o, 0, sizeof o);
    b.in = src;
    b.n = n;

    if (!out_need(&o, hint ? hint + 1 : 65536)) {
        if (err) snprintf(err, errcap, "内存不足（解压缓冲）");
        return -1;
    }

    do {
        int rc;
        last = bits(&b, 1);
        {
            int type = bits(&b, 2);
            if (b.overrun) { rc = -1; }
            else if (type == 0) rc = inflate_stored(&b, &o);
            else if (type == 1) rc = inflate_fixed(&b, &o);
            else if (type == 2) rc = inflate_dynamic(&b, &o);
            else rc = -1;
        }
        if (rc != 0) {
            free(o.p);
            if (err) {
                if (rc == -2) snprintf(err, errcap, "内存不足（解压中）");
                else snprintf(err, errcap, "压缩数据损坏（deflate 解码失败）");
            }
            return -1;
        }
    } while (!last && !b.overrun);

    if (b.overrun) {
        free(o.p);
        if (err) snprintf(err, errcap, "压缩数据被截断");
        return -1;
    }

    /* 补 '\0'：xlsx 的 xml 部件解压后都当文本用 */
    if (!out_need(&o, 1)) {
        free(o.p);
        if (err) snprintf(err, errcap, "内存不足（解压末尾）");
        return -1;
    }
    o.p[o.len] = 0;
    if (out) *out = o.p;
    if (out_len) *out_len = o.len;
    return 0;
}
