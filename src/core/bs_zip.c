#include "bs_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bs_file.h"
#include "bs_inflate.h"

#define SIG_EOCD 0x06054b50u
#define SIG_CDIR 0x02014b50u
#define SIG_LOCAL 0x04034b50u

typedef struct {
    char *name;
    unsigned method;
    unsigned crc;
    size_t csize;
    size_t usize;
    size_t local_off;
} ent_t;

struct bs_zip {
    unsigned char *data; /* 整个文件，简单可靠：BOM 文件都是几 MB 级 */
    size_t size;
    ent_t *ent;
    int n;
};

static unsigned rd16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

void bs_zip_close(bs_zip *z)
{
    int i;
    if (!z) return;
    for (i = 0; i < z->n; i++) free(z->ent[i].name);
    free(z->ent);
    free(z->data);
    free(z);
}

int bs_zip_count(const bs_zip *z) { return z ? z->n : 0; }

const char *bs_zip_entry_name(const bs_zip *z, int i)
{
    if (!z || i < 0 || i >= z->n) return "";
    return z->ent[i].name;
}

size_t bs_zip_entry_size(const bs_zip *z, int i)
{
    if (!z || i < 0 || i >= z->n) return 0;
    return z->ent[i].usize;
}

/* 从尾部往前找 EOCD（末尾可能有 64 KB 注释，所以最多回看 65557 字节） */
static long find_eocd(const unsigned char *d, size_t n)
{
    size_t i;
    if (n < 22) return -1;
    i = n - 22;
    for (;;) {
        if (rd32(d + i) == SIG_EOCD) return (long)i;
        if (i == 0) break;
        i--;
        if (n - i > 65557) break;
    }
    return -1;
}

int bs_zip_open(const char *path_utf8, bs_zip **out, char *err, size_t errcap)
{
    bs_zip *z;
    unsigned char *data = NULL;
    size_t size = 0;
    long eocd;
    size_t cd_off, cd_size, p;
    int total, i;

    if (out) *out = NULL;
    if (bs_read_file(path_utf8, &data, &size, err, errcap) != 0) return -1;

    if (size < 22 || !(data[0] == 'P' && data[1] == 'K')) {
        free(data);
        if (err) snprintf(err, errcap, "不是 .xlsx 文件（不是 zip 容器）");
        return -1;
    }

    eocd = find_eocd(data, size);
    if (eocd < 0) {
        free(data);
        if (err) snprintf(err, errcap, "zip 结构损坏（找不到目录结尾）");
        return -1;
    }

    total = (int)rd16(data + eocd + 10);
    cd_size = rd32(data + eocd + 12);
    cd_off = rd32(data + eocd + 16);
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || total == 0xFFFF) {
        free(data);
        if (err) snprintf(err, errcap, "不支持 ZIP64 格式（文件超过 4 GB）");
        return -1;
    }
    if (cd_off > size || cd_size > size - cd_off) {
        free(data);
        if (err) snprintf(err, errcap, "zip 结构损坏（目录越界）");
        return -1;
    }

    z = (bs_zip *)calloc(1, sizeof *z);
    if (!z) {
        free(data);
        if (err) snprintf(err, errcap, "内存不足");
        return -1;
    }
    z->data = data;
    z->size = size;
    z->ent = (ent_t *)calloc((size_t)(total > 0 ? total : 1), sizeof(ent_t));
    if (!z->ent) {
        bs_zip_close(z);
        if (err) snprintf(err, errcap, "内存不足（条目表）");
        return -1;
    }

    p = cd_off;
    for (i = 0; i < total; i++) {
        unsigned nlen, elen, clen;
        ent_t *e;
        if (p + 46 > size || rd32(data + p) != SIG_CDIR) break;
        nlen = rd16(data + p + 28);
        elen = rd16(data + p + 30);
        clen = rd16(data + p + 32);
        if (p + 46 + nlen > size) break;

        e = &z->ent[z->n];
        e->name = (char *)malloc(nlen + 1);
        if (!e->name) break;
        memcpy(e->name, data + p + 46, nlen);
        e->name[nlen] = 0;
        e->method = rd16(data + p + 10);
        e->crc = rd32(data + p + 16);
        e->csize = rd32(data + p + 20);
        e->usize = rd32(data + p + 24);
        e->local_off = rd32(data + p + 42);
        if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu ||
            e->local_off == 0xFFFFFFFFu) {
            free(e->name);
            e->name = NULL;
            break;
        }
        z->n++;
        p += 46 + nlen + elen + clen;
    }

    if (z->n == 0) {
        bs_zip_close(z);
        if (err) snprintf(err, errcap, "zip 里没有任何条目（文件可能损坏）");
        return -1;
    }
    if (out) *out = z;
    return 0;
}

/* 本地文件头里才有真实的数据起点（中央目录的 extra 与本地的不一定一样长） */
static int entry_data(bs_zip *z, const ent_t *e, const unsigned char **p, size_t *n,
                      char *err, size_t errcap)
{
    unsigned nlen, elen;
    size_t off;

    if (e->local_off + 30 > z->size || rd32(z->data + e->local_off) != SIG_LOCAL) {
        if (err) snprintf(err, errcap, "zip 条目损坏：%s", e->name);
        return -1;
    }
    nlen = rd16(z->data + e->local_off + 26);
    elen = rd16(z->data + e->local_off + 28);
    off = e->local_off + 30 + nlen + elen;
    if (off + e->csize > z->size) {
        if (err) snprintf(err, errcap, "zip 条目越界：%s", e->name);
        return -1;
    }
    *p = z->data + off;
    *n = e->csize;
    return 0;
}

int bs_zip_read_index(bs_zip *z, int i, unsigned char **buf, size_t *len,
                      char *err, size_t errcap)
{
    const ent_t *e;
    const unsigned char *src;
    size_t n;

    if (buf) *buf = NULL;
    if (len) *len = 0;
    if (!z || i < 0 || i >= z->n) {
        if (err) snprintf(err, errcap, "zip 条目序号越界");
        return -1;
    }
    e = &z->ent[i];
    if (entry_data(z, e, &src, &n, err, errcap) != 0) return -1;

    if (e->method == 0) {
        unsigned char *p = (unsigned char *)malloc(n + 1);
        if (!p) {
            if (err) snprintf(err, errcap, "内存不足（条目 %s）", e->name);
            return -1;
        }
        memcpy(p, src, n);
        p[n] = 0;
        if (buf) *buf = p;
        if (len) *len = n;
        return 0;
    }
    if (e->method == 8)
        return bs_inflate_raw(src, n, buf, len, e->usize, err, errcap);

    if (err) snprintf(err, errcap, "不支持的压缩方式 %u（部件 %s）", e->method, e->name);
    return -1;
}

int bs_zip_entry_raw(bs_zip *z, int i, const unsigned char **data, size_t *csize,
                     unsigned *method, unsigned *crc, size_t *usize,
                     char *err, size_t errcap)
{
    const ent_t *e;
    const unsigned char *src;
    size_t n;

    if (data) *data = NULL;
    if (csize) *csize = 0;
    if (usize) *usize = 0;
    if (!z || i < 0 || i >= z->n) {
        if (err) snprintf(err, errcap, "zip 条目序号越界");
        return -1;
    }
    e = &z->ent[i];
    if (entry_data(z, e, &src, &n, err, errcap) != 0) return -1;
    if (data) *data = src;
    if (csize) *csize = n;
    if (method) *method = e->method;
    if (crc) *crc = e->crc;
    if (usize) *usize = e->usize;
    return 0;
}

int bs_zip_read(bs_zip *z, const char *name, unsigned char **buf, size_t *len,
                char *err, size_t errcap)
{
    int i;
    size_t k;

    if (buf) *buf = NULL;
    if (len) *len = 0;
    if (!z || !name) return -1;

    for (i = 0; i < z->n; i++)
        if (strcmp(z->ent[i].name, name) == 0)
            return bs_zip_read_index(z, i, buf, len, err, errcap);

    /* 大小写不敏感兜底：同一个 xlsx 在不同工具里可能写成 Xl/Worksheets */
    for (i = 0; i < z->n; i++) {
        const char *a = z->ent[i].name;
        const char *b = name;
        int same = 1;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) { same = 0; break; }
            a++; b++;
        }
        if (same && !*a && !*b) return bs_zip_read_index(z, i, buf, len, err, errcap);
    }

    /* 路径里可能有 "./" 前缀，清一遍再找 */
    if (strncmp(name, "./", 2) == 0 && (k = strlen(name) - 2) > 0)
        return bs_zip_read(z, name + 2, buf, len, err, errcap);

    if (err) snprintf(err, errcap, "缺少部件 %s", name);
    return -1;
}
