#ifndef BS_ZIPW_H
#define BS_ZIPW_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 只写 zip —— xlsx 的容器就是 zip，读取端是 bs_zip。

   method 按内容挑：`bs_zipw_add` 先试 deflate（bs_deflate，固定 Huffman +
   LZ77），**只有真变小才用**，否则退回 stored。10 万行差异表实测 3.58 MB →
   383 KB（9.4 : 1）；小部件压了会因码表开销变大，所以 256 字节以下直接存。

   压缩器是自己写的，不引 zlib：MinGW 不带可静态链接的 zlib，而「零第三方
   运行库」是本项目的硬约束。正确性由两条线守着 —— bstest 里「压完再用自家
   inflate 解回来逐字节比对」的回环用例，以及 check_write 的 Python 读回。

   部件内容一律由 add 接管（调用方 malloc，writer 负责 free）。sheet1.xml
   可能十几 MB，复制两遍的峰值没必要。 */

typedef struct bs_zipw bs_zipw;

bs_zipw *bs_zipw_new(void);

/* 接管 data（可为 NULL，n 为 0）。name 是 UTF-8，通常全 ASCII。
   内部会算 crc32 并复制名字。成功返回 0，内存不足返回 -1。 */
int bs_zipw_add(bs_zipw *z, const char *name, char *data, size_t n);

/* 搬已经压好的字节（典型来源是 bs_zip_entry_raw）：method / crc / usize 由
   调用方给。这样未修改的部件可以**原样拷贝压缩数据**，既快又不会因重压引入
   差异，附带的好处是 VBA、图表、图片全都无损。data 同样由本模块接管。 */
int bs_zipw_add_raw(bs_zipw *z, const char *name, char *data, size_t csize,
                    size_t usize, unsigned method, unsigned crc);

/* 落盘。失败返回 -1，err 写原因（含权限不足、磁盘满）。
   时间戳固定成 1980-01-01，同样的输入永远产出同样的字节，便于回归比对。 */
int bs_zipw_save(bs_zipw *z, const char *path_utf8, char *err, size_t errcap);

void bs_zipw_free(bs_zipw *z);

#ifdef __cplusplus
}
#endif

#endif /* BS_ZIPW_H */
