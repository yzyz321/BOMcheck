#ifndef BS_MARK_H
#define BS_MARK_H

#include <stddef.h>

#include "bs_cfg.h"
#include "bs_pair.h"
#include "bs_sheet.h"
#include "bs_xlsx.h"   /* bs_err */

#ifdef __cplusplus
extern "C" {
#endif

/* 标注回写（C6）：在源工作簿的**副本**上涂四色，源文件只读不改。

   做法与 pyref/bomcore.py 的 mark_workbook 不同：Python 用 openpyxl 整本加载再
   另存，图片 / 图表 / 条件格式 / 数据透视会丢；这里走文档 6.4 的「XML 叠加」：
     1. 只重写两个部件 —— 目标 sheet 与 styles.xml；其余部件按中央目录里的
        压缩字节原样搬过去（VBA、图表、图片、其他工作表都无损）
     2. 样式是**叠加**不是替换：复制原 <xf> 只改 fillId，字体 / 边框 / 数字
        格式全部保留
     3. 空单元格没有 <c> 节点，整行标色时按列字母补齐

   一期只做底色；批注（C6b）在此之上追加：重复料号的料号格写一条
   「重复料号 n/N（本页共 N 处）」，走 xl/comments1.xml + VML 形状 +
   sheet 关系三件套。源表若已有批注（自带 <legacyDrawing>），保留原样、
   不再追加 —— schema 里 legacyDrawing 只能有一个，硬塞会写坏文件。 */

typedef enum { BS_MARK_ADD = 0, BS_MARK_DEL, BS_MARK_CHG, BS_MARK_DUP } bs_markcolor;

typedef enum { BS_SIDE_OLD = 0, BS_SIDE_NEW = 1 } bs_side;

/* 差异四色。与 bs_write.c 的结果表、界面 theme 三处同值（附录 A 唯一来源）。 */
const char *bs_mark_color_hex(int color);   /* 返回 "C6EFCE" 这类 6 位大写；越界返回 "" */
int bs_mark_color_count(void);

/* 把配对结果涂到 s 这一侧的副本上：
     - 删除 → 旧表整行红；新增 → 新表整行绿；变更 → 两侧各自变化的列黄；
     - 重复料号 → 仅料号格浅紫（与 2.5 一致，涂整行会跟红绿打架）
   s 必须来自 src 的某张表，且知道表名（bs_sheet_from_grid 之后要调
   bs_sheet_set_name，否则只能靠 bs_sheet_open / bs_sheet_analyze）。
   dst 被占用时自动改名 _2/_3…，实际路径写进 out_path。
   返回 0 成功；失败时 err 里是中文原因，dst 不会被留下半截文件。 */
int bs_mark_workbook(const bs_sheet *s, const bs_sheet *other,
                     const char *src_utf8, const bs_pair *p,
                     size_t np, int side, const char *dst_utf8,
                     char *out_path, size_t out_cap, bs_err *e);

/* 查重（R6）用：只涂重复料号格，整行浅紫 + 料号格标记。
   与 bs_mark_workbook 共用同一套 XML 叠加，只是不读配对结果。 */
int bs_mark_dups(const bs_sheet *s, const char *src_utf8, const char *dst_utf8,
                 char *out_path, size_t out_cap, bs_err *e);

#ifdef __cplusplus
}
#endif

#endif /* BS_MARK_H */
