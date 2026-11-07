#ifndef BS_WRITE_H
#define BS_WRITE_H

#include <stddef.h>

#include "bs_pair.h"
#include "bs_sheet.h"
#include "bs_xlsx.h"   /* bs_err */

#ifdef __cplusplus
extern "C" {
#endif

/* 结果写出（C5）。口径与 pyref/bomcore.py 的 export_xlsx 一致：
     第 1 页「差异结果」12 列 + 表头底色 185FA5 + 新增/删除整行涂色 +
       变更只涂「变更项」与有差异的旧/新成对列 + 重复料号只涂料号格 +
       冻结首行 + autoFilter + 数据区下方一行的颜色说明栏
     第 2 页「识别诊断」12 列（文件 / 工作表 / 表头行 / … / 料号命中率）

   与 Python 的三处差异：
     1. 不写 docProps，也不写主题与共享字符串表（单元格用 inlineStr）。
        openpyxl 和 Excel 都能读，体积略大一点，换来部件数从 10 降到 7。
     2. 按内容挑 method：压缩后确实变小才用 deflate，否则 stored（见 bs_zipw.c）。
     3. 目标被占用时同样自动改名 _2…_19，但判断依据是「打不开文件」，
        不是 Python 的 PermissionError 异常类型。 */

typedef struct {
    char file[512];        /* 源文件路径（诊断页第 1 列） */
    char sheet[72];        /* 工作表名 */
    char header[128];      /* 表头行，多块用 "/" 连接，如 "5/18/28" */
    int data_rows;         /* 数据行：块区间内有内容的行 = 建档 + 无键 + 分组标题 */
    int built_rows;        /* 建档行：真正进了 bs_row 的行（旧实现两列写的是同一个数） */
    char col_pn[8], col_ref[8], col_qty[8];
    int ref_rows;          /* 位号键行数 */
    int labels;            /* 跨列合并的标题行数 */
    int unkeyable;
    double hit_rate;       /* 0..1，写出时按 "%.0f%%" 格式化 */
} bs_diag;

/* 从识别结果填一行诊断。file 为 NULL 时第 1 列留空 */
void bs_diag_fill(bs_diag *d, const bs_sheet *s, const char *file);

/* 写 xlsx。out_path/out_cap 收到实际写入的路径（可能带 _2 后缀）。
   失败返回 -1 并填 e。 */
int bs_write_result(const char *path_utf8, const bs_result *r,
                    const bs_diag *diag, int n_diag,
                    char *out_path, size_t out_cap, bs_err *e);

#ifdef __cplusplus
}
#endif

#endif /* BS_WRITE_H */
