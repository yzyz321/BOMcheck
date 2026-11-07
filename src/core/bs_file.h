#ifndef BS_FILE_H
#define BS_FILE_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* core 层只用这一处跟平台文件 API 打交道：其余文件都是纯内存逻辑，
   单测可以直接喂 buffer。Windows 下走 _wfopen，路径按 UTF-8 传入 ——
   fopen 会按 ANSI 代码页解释，中文路径必然打不开。 */
FILE *bs_fopen_utf8(const char *path_utf8, const char *mode);

/* 整个文件读进内存；成功返回 0，*buf 由 malloc 分配（尾部补 '\0'） */
int bs_read_file(const char *path_utf8, unsigned char **buf, size_t *len,
                 char *err, size_t errcap);

/* 文件是否存在且是普通文件 */
int bs_file_exists(const char *path_utf8);

#ifdef __cplusplus
}
#endif

#endif /* BS_FILE_H */
