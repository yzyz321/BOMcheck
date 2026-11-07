#ifndef BS_XLSX_H
#define BS_XLSX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* xlsx 读取层。口径与 pyref/bomcore.py 的 _load_sst / _load_styles / _Grid 一致：
   - 共享字符串按 <si> 顺序，取其中所有 <t> 文本拼接（含 rPh，跟 ElementTree 的 iter 一致）
   - 删除线看 cellXfs[单元格 s].fontId → fonts[fontId] 里有没有 <strike>
   - 数值不做本地化格式还原，保留 <v> 原文（Python 侧也是原文，格式化留给显示层）
   差异只有一处：缺 r 属性时按顺序补，Python 会抛异常 —— 这是我们故意的容错。 */

typedef struct {
    char msg[256];
} bs_err;

/* Excel 的硬边界。解析 r="..." 时按这两个值裁剪：
   - 列名超过 3 个字母 / 列号 > 16384：按非法引用处理（col_from_ref 返回 0，整格跳过）
   - 行号 > 1048576：钳到上限（row_from_ref / <row r> 都走这条）
   不加这两道闸，7 个 'Z' 的列号会 int 溢出成负数，负下标写位图就是堆越界。 */
#define BS_MAX_ROW 1048576
#define BS_MAX_COL 16384

typedef struct bs_workbook bs_workbook;

typedef struct {
    int row, col;          /* 1 基 */
    unsigned off, len;     /* 指向 grid->strs */
    unsigned char strike;
    unsigned ord;          /* 文档顺序；同一格写两次时后写的生效 */
} bs_cell;

typedef struct {
    int r0, c0, r1, c1;
} bs_merge;

typedef struct {
    char *strs;
    size_t strs_len, strs_cap;
    bs_cell *cells;
    int n_cells, cap_cells;
    bs_merge *merges;
    int n_merges, cap_merges;
    int max_row, max_col;  /* max_col 按出现过的 <c> 算，空值也算（同 Python） */
    int n_rows;            /* <row> 数（= Python 里 cells 字典长度） */
    int n_strike;          /* 带删除线的格子数 */
} bs_grid;

bs_workbook *bs_xlsx_open(const char *path_utf8, bs_err *e);
void bs_xlsx_close(bs_workbook *wb);

/* 诊断：累计的「解压」与「xml 解析」毫秒数。只给 --bench / 性能排查用，
   不参与任何判断。读之前先 bs_prof_reset()。 */
extern double bs_prof_inflate_ms;
extern double bs_prof_parse_ms;
void bs_prof_reset(void);

int bs_xlsx_sheet_count(const bs_workbook *wb);
const char *bs_xlsx_sheet_name(const bs_workbook *wb, int i);
const char *bs_xlsx_sheet_part(const bs_workbook *wb, int i);
int bs_xlsx_find_sheet(const bs_workbook *wb, const char *name);

int bs_xlsx_sst_count(const bs_workbook *wb);
int bs_xlsx_font_count(const bs_workbook *wb);
int bs_xlsx_font_strike(const bs_workbook *wb, int i);
int bs_xlsx_xf_count(const bs_workbook *wb);

/* 读一张表的全部格子。*out 由 bs_grid_free 释放 */
int bs_xlsx_load_sheet(bs_workbook *wb, int idx, bs_grid **out, bs_err *e);
void bs_grid_free(bs_grid *g);

/* 取格子文本（返回内部指针，空值返回 ""）；越界返回 NULL */
const char *bs_grid_get(const bs_grid *g, int row, int col);
int bs_grid_strike(const bs_grid *g, int row, int col);

#ifdef __cplusplus
}
#endif

#endif /* BS_XLSX_H */
