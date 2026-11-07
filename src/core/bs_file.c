#include "bs_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

FILE *bs_fopen_utf8(const char *path_utf8, const char *mode)
{
#if defined(_WIN32)
    wchar_t wpath[MAX_PATH * 4];
    wchar_t wmode[8];
    int n = MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath,
                                (int)(sizeof(wpath) / sizeof(wpath[0])));
    int m = MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8);
    if (n <= 0 || m <= 0) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path_utf8, mode);
#endif
}

int bs_file_exists(const char *path_utf8)
{
    FILE *f = bs_fopen_utf8(path_utf8, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int bs_read_file(const char *path_utf8, unsigned char **buf, size_t *len,
                 char *err, size_t errcap)
{
    FILE *f;
    long sz;
    unsigned char *p;
    size_t got;

    if (buf) *buf = NULL;
    if (len) *len = 0;

    f = bs_fopen_utf8(path_utf8, "rb");
    if (!f) {
        if (err) snprintf(err, errcap, "打不开文件（可能被占用或没有权限）");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err) snprintf(err, errcap, "文件无法定位（可能是设备或管道）");
        return -1;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        if (err) snprintf(err, errcap, "文件长度读取失败");
        return -1;
    }
    rewind(f);

    p = (unsigned char *)malloc((size_t)sz + 1);
    if (!p) {
        fclose(f);
        if (err) snprintf(err, errcap, "内存不足（文件 %ld 字节）", sz);
        return -1;
    }
    got = fread(p, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(p);
        if (err) snprintf(err, errcap, "文件读取不完整（%lu/%ld 字节）",
                          (unsigned long)got, sz);
        return -1;
    }
    p[sz] = 0;
    if (buf) *buf = p;
    if (len) *len = (size_t)sz;
    return 0;
}
