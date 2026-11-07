#ifndef BS_TABLE_SRC_H
#define BS_TABLE_SRC_H

#include <string>
#include <vector>

#include "bs_pair.h"
#include "bs_sheet.h"
#include "table.h"

namespace bs {

/* 12 列差异结果的数据源：直接读 bs_result，不复制单元格。
   列宽按导出表 2.6 的字符数换算（×7 + 6 逻辑像素），界面与导出比例一致。 */
class DiffSource : public TableSource {
public:
    explicit DiffSource(const bs_result *r);

    int rows() const override { return n_; }
    int cols() const override { return BS_RES_COLS; }
    const wchar_t *header(int c) const override;
    int width(int c) const override;
    const wchar_t *cell(int r, int c) const override;
    unsigned row_bg(int r) const override;
    unsigned cell_bg(int r, int c) const override;

private:
    const bs_result *r_ = nullptr;
    int n_ = 0;
    std::wstring head_[BS_RES_COLS];
    mutable wchar_t buf_[1024] = L"";
};

/* 8 列查重结果（两侧同一个文件时）：口径照 bomcore.find_dups ——
   只按料号判重，分组「出现次数降序、料号升序」，组内按原表行号。 */
class DupSource : public TableSource {
public:
    /* 从一张已分析的表生成；返回重复行数 */
    int build(const bs_sheet *s);

    int rows() const override { return (int)rows_.size(); }
    int cols() const override { return 8; }
    const wchar_t *header(int c) const override;
    int width(int c) const override;
    const wchar_t *cell(int r, int c) const override;
    unsigned row_bg(int r) const override;
    unsigned cell_bg(int r, int c) const override;

    int n_groups() const { return n_groups_; }
    int n_pn_rows() const { return n_pn_rows_; }
    int n_ref_rows() const { return n_ref_rows_; }
    const std::wstring &top_pn() const { return top_pn_; }
    int max_count() const { return max_count_; }

private:
    std::vector<std::vector<std::wstring>> rows_;
    std::vector<std::wstring> head_;
    std::vector<int> width_;
    int n_groups_ = 0;
    int n_pn_rows_ = 0;
    int n_ref_rows_ = 0;
    int max_count_ = 0;
    std::wstring top_pn_;
};

}  // namespace bs

#endif  // BS_TABLE_SRC_H
