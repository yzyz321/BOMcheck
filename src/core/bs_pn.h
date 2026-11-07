#ifndef BS_PN_H
#define BS_PN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 料号 / 位号 / 键的纯函数层。口径与 pyref/bomcore.py 的 norm / pn_key /
   ref_tokens / ref_sig / make_key 逐一对应，两边必须逐字节一致（tools/check_pn.py
   拿真实样本对账）。这里不碰文件、不碰 Win32，只有字符串。

   四个已知的口径差异（都只在“不该出现的脏数据”上体现，写在这里免得以后对不上账时
   再查一遍）：
     1. 大小写：Python 的 str.lower()/upper() 是全 Unicode 的；这里只折 ASCII
        A-Z 与全角 ａ-ｚ（中文表格里真会出现全角字母）。希腊/西里尔等不折。
     2. 数字：Python 的 \d / str.isdigit() 认所有 Unicode 十进制数字；这里只认
        ASCII 0-9（外加全角 ０-９，中文 Excel 里真会出现），别的按非数字算。
     3. 非法 UTF-8 字节：原样保留，Python 侧遇到这种输入会直接抛异常。
     4. 内存：全部由调用方给缓冲 / 用完 free，不藏静态状态。 */

#define BS_PN_MAX 256

/* 一次算齐一行的键。料号与位号两组结果都在里面，供逐行循环复用：
       bs_keys k;  bs_keys_init(&k);
       for (...) bs_keys_make(&k, pn, ref);
       bs_keys_free(&k);
   内部缓冲只增不减，所以只在第一次调用时分配。 */
typedef struct bs_keys {
    char  *pn;      size_t pn_cap;    /* 归一化后的料号，可能为 "" */
    char  *ref;     size_t ref_cap;   /* 位号签名，可能为 "" */
    char  *key;     size_t key_cap;   /* "P:xxx" / "R:xxx" / "" */
    int    wide, strict;              /* 料号是否合规范形态 / 严格形态 */
    int    n_ok, n_bad;               /* 位号：合法 token 数 / 被拒数 */
    char   kind;                      /* 'P' / 'R' / 0 */
} bs_keys;

void bs_keys_init(bs_keys *k);
void bs_keys_free(bs_keys *k);
void bs_keys_make(bs_keys *k, const char *pn_raw, const char *ref_raw);

/* 单步函数（单测与对账用），结果写进调用方缓冲，cap 含结尾 '\0'。
   返回写入的字节数；缓冲不够时截断、仍然 '\0' 结尾，返回 -1。
   NULL 输入一律当空串（和 Python 的 `if v is None: return ...` 一致）。 */
int bs_norm_str(const char *s, char *out, size_t cap);
int bs_pn_str(const char *v, char *out, size_t cap, int *wide, int *strict);
int bs_ref_sig_str(const char *v, char *out, size_t cap, int *n_ok, int *n_bad);

/* 便捷版：一次算 key（"P:" 优先，否则 "R:"），返回 'P' / 'R' / 0 */
char bs_make_key(const char *pn_raw, const char *ref_raw, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* BS_PN_H */
