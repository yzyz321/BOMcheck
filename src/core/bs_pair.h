#ifndef BS_PAIR_H
#define BS_PAIR_H

#include <stddef.h>

#include "bs_cfg.h"
#include "bs_sheet.h"
#include "bs_xlsx.h"   /* bs_err */

#ifdef __cplusplus
extern "C" {
#endif

/* 配对与差异（C4）。口径与 pyref/bomcore.py 的 pair() / build_result() 逐项一致：
     A 料号键精确（组内按行序一对一，min(旧数, 新数) 对）
     B 剩余行里位号键精确
     C 剩余行里位号 Jaccard >= ref_min_overlap，且至少一侧无料号；按 (相似度降序,
       旧行号, 新行号) 贪心取对，严格一对一
     D 仍未配对的按新增 / 删除
   结果 12 列的拼法（含空串位置）照 build_result，见 bs_result_build。 */

#define BS_CHG_REF     1u    /* 位号集合不同 */
#define BS_CHG_QTY     2u
#define BS_CHG_PN      4u
#define BS_CHG_STRIKE  8u    /* 删除线状态翻转；方向看 strike_dir */
#define BS_CHG_CELL   16u    /* 整行逐列比较：其余列里有格子不一样（opt.row_diff_all） */

typedef enum { BS_ACT_CHG = 0, BS_ACT_ADD = 1, BS_ACT_DEL = 2 } bs_act;
typedef enum {
    BS_KT_PN = 0,          /* 料号 */
    BS_KT_REF,             /* 位号（阶段 B） */
    BS_KT_REF_FUZZY,       /* 位号(近似) */
    BS_KT_REF_PNCHG        /* 位号(料号变更) */
} bs_keytype;

typedef struct {
    int oi, ni;                 /* 行索引（bs_sheet_row 的序号），-1 = 该侧不存在 */
    unsigned char act;          /* bs_act */
    unsigned char kt;           /* bs_keytype */
    unsigned char strike_dir;   /* 0 = 无变化 1 = 划除 2 = 恢复 */
    unsigned chg;               /* BS_CHG_* 位掩码；CHG 行为 0 表示无差异 */
} bs_pair;

typedef struct {
    int n_pn, n_ref, n_fuzzy, n_pnchg;   /* 配对方式统计（阶段 A/B/C） */
    int n_paired;                        /* 配对总数 = 上式四项之和 */
    int n_added, n_deleted;              /* 阶段 D */
    int n_skipped;                       /* 双键关闭时被跳过的无料号行（两侧合计） */
    int n_changed;                       /* 12 列里真正有差异的变更行数 */
    int cartesian_fallback;              /* dup_policy = CARTESIAN 时为 1（未实现全组合） */
} bs_pairstat;

/* 出参 p 由 malloc 分配，用完 bs_pair_free。CHG 行也包含「无差异」的配对
   （chg == 0），因为标注阶段要用它们定位行；生成 12 列时再过滤掉。 */
int  bs_pair_rows(const bs_sheet *o, const bs_sheet *n, const bs_cfg *c,
                  bs_pair **p, size_t *np, bs_pairstat *st, bs_err *e);
void bs_pair_free(bs_pair *p);

/* ── 12 列结果（2.6 定义，与 bomcore.RESULT_COLS 同序） ──────────
   所有字符串归 bs_result 所有，函数返回后即可读，bs_result_close 一起释放。 */
#define BS_RES_COLS 12

enum {
    BS_COL_PN = 0, BS_COL_KT, BS_COL_ACT, BS_COL_CHG,
    BS_COL_RO, BS_COL_RN, BS_COL_QO, BS_COL_QN,
    BS_COL_SO, BS_COL_SN, BS_COL_DUP, BS_COL_SRC
};

typedef struct {
    const char *col[BS_RES_COLS];
} bs_resrow;

typedef struct bs_result bs_result;

int    bs_result_build(const bs_sheet *o, const bs_sheet *n,
                       const bs_pair *p, size_t np, bs_result **out, bs_err *e);
size_t bs_result_count(const bs_result *r);
const bs_resrow *bs_result_row(const bs_result *r, size_t i);
const char *bs_result_header(int i);          /* 越界返回 "" */
void   bs_result_close(bs_result *r);

/* 数量文本：整数不带小数点，非整数取「转回 double 仍相等」的最短表示，
   -0.0 归成 0（与 bomcore 的 qty_text 同口径）。12 列与查重表共用。 */
void   bs_qty_text(const bs_row *r, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* BS_PAIR_H */
