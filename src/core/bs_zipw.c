#include "bs_zipw.h"

#include "bs_deflate.h"
#include "bs_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    char *data;
    size_t n;          /* 数据字节数（stored 时等于原始大小，压缩时是 csize） */
    size_t usize;      /* 原始大小 */
    unsigned method;   /* 0 = stored / 8 = deflate */
    unsigned crc;
    size_t off;        /* 本地头在文件里的偏移，写中央目录时回填 */
} zw_entry;

struct bs_zipw {
    zw_entry *e;
    int n, cap;
};

/* ── crc32（IEEE 802.3，zip 用的那个） ─────────────────────────── */
static unsigned crc_tab[256];
static int crc_ready;

static void crc_init(void)
{
    unsigned i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

static unsigned crc_calc(const unsigned char *p, size_t n)
{
    unsigned c = 0xFFFFFFFFu;
    size_t i;
    if (!crc_ready) crc_init();
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── 小端写出 ─────────────────────────────────────────────────── */
static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ── 生命周期 ─────────────────────────────────────────────────── */
bs_zipw *bs_zipw_new(void)
{
    return (bs_zipw *)calloc(1, sizeof(bs_zipw));
}

void bs_zipw_free(bs_zipw *z)
{
    int i;
    if (!z) return;
    for (i = 0; i < z->n; i++) {
        free(z->e[i].name);
        free(z->e[i].data);
    }
    free(z->e);
    free(z);
}

int bs_zipw_add(bs_zipw *z, const char *name, char *data, size_t n)
{
    unsigned crc = n ? crc_calc((const unsigned char *)data, n) : 0;

    /* 小部件压了反而变大（码表开销），256 字节以下直接存。
       压完不比原来小也一样退回 stored —— 永远不为了「用了压缩」而变大。 */
    if (n >= 256) {
        unsigned char *c = NULL;
        size_t cn = 0;
        if (bs_deflate_raw((const unsigned char *)data, n, &c, &cn) == 0 &&
            cn + 32 < n) {
            free(data);
            return bs_zipw_add_raw(z, name, (char *)c, cn, n, 8, crc);
        }
        free(c);
    }
    return bs_zipw_add_raw(z, name, data, n, n, 0, crc);
}

int bs_zipw_add_raw(bs_zipw *z, const char *name, char *data, size_t csize,
                    size_t usize, unsigned method, unsigned crc)
{
    zw_entry *e;
    size_t nl;

    if (!z || !name) {
        free(data);
        return -1;
    }
    nl = strlen(name);
    if (z->n == z->cap) {
        int cap = z->cap ? z->cap * 2 : 8;
        zw_entry *p = (zw_entry *)realloc(z->e, (size_t)cap * sizeof(*p));
        if (!p) {          /* 扩容失败：data 仍归调用方？不 —— 契约是接管，这里释放 */
            free(data);
            return -1;
        }
        z->e = p;
        z->cap = cap;
    }
    e = &z->e[z->n];
    memset(e, 0, sizeof(*e));
    e->name = (char *)malloc(nl + 1);
    if (!e->name) {
        free(data);
        return -1;
    }
    memcpy(e->name, name, nl + 1);
    e->data = data;
    e->n = csize;
    e->usize = usize;
    e->method = method;
    e->crc = crc;
    z->n++;
    return 0;
}

static int wr(FILE *f, const void *p, size_t n, size_t *off)
{
    if (n && fwrite(p, 1, n, f) != n) return -1;
    *off += n;
    return 0;
}

static int local_header(FILE *f, const zw_entry *e, size_t *off)
{
    unsigned char h[30];
    size_t nl = strlen(e->name);

    put32(h, 0x04034b50u);
    put16(h + 4, 20);          /* version needed */
    put16(h + 6, 0);           /* flags：无（ASCII 名，不需要 UTF-8 位） */
    put16(h + 8, (unsigned)e->method);   /* method：0 stored / 8 deflate */
    put16(h + 10, 0);          /* mod time 00:00 */
    put16(h + 12, 0x0021);     /* mod date 1980-01-01 */
    put32(h + 14, e->crc);
    put32(h + 18, (unsigned long)e->n);
    put32(h + 22, (unsigned long)e->usize);
    put16(h + 26, (unsigned)nl);
    put16(h + 28, 0);          /* extra len */
    if (wr(f, h, 30, off)) return -1;
    if (wr(f, e->name, nl, off)) return -1;
    if (e->n && wr(f, e->data, e->n, off)) return -1;
    return 0;
}

static int central_header(FILE *f, const zw_entry *e, size_t *off)
{
    unsigned char h[46];
    size_t nl = strlen(e->name);

    put32(h, 0x02014b50u);
    put16(h + 4, 20);          /* version made by */
    put16(h + 6, 20);          /* version needed */
    put16(h + 8, 0);
    put16(h + 10, (unsigned)e->method);
    put16(h + 12, 0);
    put16(h + 14, 0x0021);
    put32(h + 16, e->crc);
    put32(h + 20, (unsigned long)e->n);
    put32(h + 24, (unsigned long)e->usize);
    put16(h + 28, (unsigned)nl);
    put16(h + 30, 0);          /* extra */
    put16(h + 32, 0);          /* comment */
    put16(h + 34, 0);          /* disk start */
    put16(h + 36, 0);          /* internal attr */
    put32(h + 38, 0);          /* external attr */
    put32(h + 42, (unsigned long)e->off);
    if (wr(f, h, 46, off)) return -1;
    return wr(f, e->name, nl, off);
}

int bs_zipw_save(bs_zipw *z, const char *path_utf8, char *err, size_t errcap)
{
    FILE *f;
    size_t off = 0, cd_off, cd_size;
    int i;

    if (!z || !path_utf8) {
        if (err && errcap) snprintf(err, errcap, "内部错误：空指针");
        return -1;
    }
    f = bs_fopen_utf8(path_utf8, "wb");
    if (!f) {
        if (err && errcap) snprintf(err, errcap, "无法写入文件（被占用或无权限）：%s", strerror(errno));
        return -1;
    }
    for (i = 0; i < z->n; i++) {
        z->e[i].off = off;
        if (local_header(f, &z->e[i], &off)) goto fail;
    }
    cd_off = off;
    for (i = 0; i < z->n; i++)
        if (central_header(f, &z->e[i], &off)) goto fail;
    cd_size = off - cd_off;
    {
        unsigned char h[22];
        put32(h, 0x06054b50u);
        put16(h + 4, 0);
        put16(h + 6, 0);
        put16(h + 8, (unsigned)z->n);
        put16(h + 10, (unsigned)z->n);
        put32(h + 12, (unsigned long)cd_size);
        put32(h + 16, (unsigned long)cd_off);
        put16(h + 20, 0);
        if (wr(f, h, 22, &off)) goto fail;
    }
    if (fclose(f) != 0) {
        if (err && errcap) snprintf(err, errcap, "写入未完成（磁盘满？）");
        return -1;
    }
    return 0;

fail:
    fclose(f);
    remove(path_utf8);
    if (err && errcap) snprintf(err, errcap, "写入未完成（磁盘满？）");
    return -1;
}
