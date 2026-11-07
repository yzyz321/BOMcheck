#ifndef BS_INFLATE_H
#define BS_INFLATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 解压一段原始 DEFLATE 流（RFC 1951，不含 zlib / gzip 头）。

   hint 是预期的解压长度（zip 中央目录里有），只用于一次性分配，不影响正确性。
   成功返回 0，*out 由 malloc 分配（尾部多留一个 '\0'，便于当文本用），
   调用方 free()；*out_len 为真实长度。
   失败返回 -1，err 里是给用户看的原因。 */
int bs_inflate_raw(const unsigned char *src, size_t n,
                   unsigned char **out, size_t *out_len,
                   size_t hint, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* BS_INFLATE_H */
