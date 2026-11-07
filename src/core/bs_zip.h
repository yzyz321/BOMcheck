#ifndef BS_ZIP_H
#define BS_ZIP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 只读 zip（xlsx 就是 zip）。只做需要的三件事：列出条目、按名取一个、按序号取一个。
   支持 method 0（stored）与 8（deflate）。不支持加密与 ZIP64。 */
typedef struct bs_zip bs_zip;

int bs_zip_open(const char *path_utf8, bs_zip **out, char *err, size_t errcap);
void bs_zip_close(bs_zip *z);

int bs_zip_count(const bs_zip *z);
const char *bs_zip_entry_name(const bs_zip *z, int i);
/* 解压后的大小（中央目录里记的），用于预分配 */
size_t bs_zip_entry_size(const bs_zip *z, int i);

/* 按名读取并解压。名字先精确匹配，再忽略大小写匹配（不同写盘工具大小写不一致）。
   成功 *buf 由 malloc 分配、尾部补 '\0'；找不到返回 -1 并把 err 设为 "缺少部件 xxx" */
int bs_zip_read(bs_zip *z, const char *name, unsigned char **buf, size_t *len,
                char *err, size_t errcap);
int bs_zip_read_index(bs_zip *z, int i, unsigned char **buf, size_t *len,
                      char *err, size_t errcap);

/* 取条目的**原始压缩字节**（不解压），给「未修改的部件原样搬过去」用。
   返回的指针指向 zip 内部缓冲，zip 关闭即失效，不要 free。 */
int bs_zip_entry_raw(bs_zip *z, int i, const unsigned char **data, size_t *csize,
                     unsigned *method, unsigned *crc, size_t *usize,
                     char *err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* BS_ZIP_H */
