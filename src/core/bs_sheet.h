#ifndef BS_SHEET_H
#define BS_SHEET_H

#include <stddef.h>

#include "bs_cfg.h"
#include "bs_xlsx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 识别层（C3）：把网格变成「表头块 + 列定位 + 数据行」。
   口径与 pyref/bomcore.py 的 _header_blocks / _find_pn_column / _locate /
   _guess_ref_column / load_sheet 一致，逐项对账见 tools/check_sheet.py。

   与 Python 的两处有意差异：
     1. 表头扫描不设硬上限。Python 也没有（虽然报错文案写「前 50 行」）；
        header_scan_rows 只用于进度与文案。理由：表头在第 1000 行的文件也该读得出来，
        而多扫几千行只是一次线性遍历。
     2. 无法建档的行不保留细节（Python 存在 sd.unkeyable 列表里），只计数；
        C4 要列行号时再补，避免现在为它维持一份数组。 */

typedef struct {
    int hr;                        /* 表头行（1 基） */
    int start, end;                /* 数据区间 [start, end] */
    int col_pn, col_ref, col_qty;  /* 列号，0 = 未定位到 */
} bs_block;

typedef struct {
    int row;                  /* 1 基 */
    int block;                /* 所属表头块，1 基 */
    int c1, c2;               /* 该行有内容的列范围（标注整行用） */
    const char *pn;           /* 归一化料号，"" 表示无 */
    const char *ref;          /* 原位号原文，"" 表示无 */
    const char *ref_sig;      /* 位号签名，"" 表示无 */
    const char *key;          /* "P:..." / "R:..." / "" */
    char kind;                /* 'P' / 'R' / 0 */
    double qty;               /* 解析失败时无意义 */
    int has_qty;
    unsigned char strike;     /* 该行是否命中删除线（按 strike_scope） */
    int dup_i, dup_n;         /* 同键组内序号 / 组大小（1/1 = 唯一） */
} bs_row;

typedef struct bs_sheet bs_sheet;

/* 诊断：识别层分段耗时（累加毫秒）。
   0 = 合并单元格标题行  1 = 找表头/列定位  2 = 逐行建档  3 = 重复键分组 */
extern double bs_prof_sheet[4];

/* 用已打开的工作簿分析第 idx 张表；wb 由调用方持有 */
int bs_sheet_analyze(bs_workbook *wb, int idx, const bs_cfg *c,
                     bs_sheet **out, bs_err *e);
/* 直接接管一张已读好的网格（界面已经读过时用，避免再解析一遍）。
   无论成败，g 都归本函数处理：成功随 sheet 一起释放，失败当场释放。 */
int bs_sheet_from_grid(bs_grid *g, const bs_cfg *c, bs_sheet **out, bs_err *e);
/* 网格指针（只读，归 sheet 所有）。空表返回 NULL */
const bs_grid *bs_sheet_grid(const bs_sheet *s);
/* 便捷版：自己开关工作簿 */
int bs_sheet_open(const char *path_utf8, int idx, const bs_cfg *c,
                  bs_sheet **out, bs_err *e);
void bs_sheet_close(bs_sheet *s);

int bs_sheet_block_count(const bs_sheet *s);
const bs_block *bs_sheet_block(const bs_sheet *s, int i);

int bs_sheet_row_count(const bs_sheet *s);
/* 数据行 = 建档行 + 无法建档行 + 分组标题行（诊断页「数据行」的口径） */
int bs_sheet_data_rows(const bs_sheet *s);
const bs_row *bs_sheet_row(const bs_sheet *s, int i);
/* 某行某列的原位文本（空 / 越界返回 ""）。「整行逐列比较」用它读格 */
const char *bs_sheet_text_at(const bs_sheet *s, int row, int col);

int bs_sheet_labels(const bs_sheet *s);        /* 跨列合并的标题行数 */
int bs_sheet_unkeyable(const bs_sheet *s);     /* 无法建档的行数 */
double bs_sheet_hit_rate(const bs_sheet *s);   /* 料号列内容命中率 */

/* 列名（"H" / ""），kind 取 'P' / 'R' / 'Q' */
const char *bs_sheet_col_name(const bs_sheet *s, char kind);

/* 工作表名。bs_sheet_open / bs_sheet_analyze 时记下，bs_sheet_from_grid 为空串 */
const char *bs_sheet_name(const bs_sheet *s);
/* from_grid 之后补表名：界面先读网格再分析，标注回写（C6）要靠它找到部件 */
void bs_sheet_set_name(bs_sheet *s, const char *name);
/* 键类型为位号（'R'）的行数，诊断页用 */
int bs_sheet_ref_rows(const bs_sheet *s);

#ifdef __cplusplus
}
#endif

#endif /* BS_SHEET_H */
