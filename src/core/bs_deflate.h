#ifndef BS_DEFLATE_H
#define BS_DEFLATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DEFLATE 压缩（RFC 1951，raw 流，不含 zlib 头）。

   为什么自己写：MinGW 不带可静态链接的 zlib，而「零第三方运行库」是本项目的
   硬约束。解码端 bs_inflate 已经自己写了一份，编码端同理。

   算法：LZ77（32 KB 窗口、哈希链）+ **固定 Huffman**（BTYPE=01）。
   不做动态 Huffman —— 那要先统计词频再写码表，代码翻倍，而 xlsx 的 xml
   重复度极高，固定码已经能压到 1/8 左右，收益不值这个复杂度。

   输出缓冲由本函数 malloc，调用方 free。失败返回 -1。 */
int bs_deflate_raw(const unsigned char *src, size_t n,
                   unsigned char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* BS_DEFLATE_H */
