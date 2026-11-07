#include "table_src.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <map>

#include "layout_gen.h"

namespace bs {
namespace {

/* UTF-8 → UTF-16，写进调用方给的缓冲区（表格绘制是即取即画，不持有） */
const wchar_t *widen(const char *s, wchar_t *buf, int cap) {
    if (cap <= 0) return L"";
    if (!s) s = "";
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, cap);
    buf[cap - 1] = 0;
    return buf;
}

std::wstring wstr(const char *s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

/* 导出表 2.6 的列宽（字符数）→ 逻辑像素：×7 + 6，与 xlsx 的宽度观感一致 */
int px_from_chars(int chars) { return chars * 7 + 6; }

}  // namespace

/* ── 12 列差异结果 ──────────────────────────────────────────────── */

DiffSource::DiffSource(const bs_result *r) : r_(r) {
    n_ = r ? (int)bs_result_count(r) : 0;
    for (int c = 0; c < BS_RES_COLS; ++c) head_[c] = wstr(bs_result_header(c));
}

const wchar_t *DiffSource::header(int c) const {
    return (c >= 0 && c < BS_RES_COLS) ? head_[c].c_str() : L"";
}

int DiffSource::width(int c) const {
    static const int kW[BS_RES_COLS] = {18, 14, 9, 24, 22, 22, 11, 11, 10, 10, 10, 22};
    if (c < 0 || c >= BS_RES_COLS) return 80;
    return px_from_chars(kW[c]);
}

const wchar_t *DiffSource::cell(int r, int c) const {
    if (!r_ || r < 0 || r >= n_ || c < 0 || c >= BS_RES_COLS) return L"";
    const bs_resrow *row = bs_result_row(r_, (size_t)r);
    return row ? widen(row->col[c], buf_, 1024) : L"";
}

unsigned DiffSource::row_bg(int r) const {
    if (!r_ || r < 0 || r >= n_) return 0;
    const bs_resrow *row = bs_result_row(r_, (size_t)r);
    if (!row) return 0;
    const char *act = row->col[BS_COL_ACT];
    if (!act) return 0;
    if (std::strcmp(act, "新增") == 0) return kColRowAdd;
    if (std::strcmp(act, "删除") == 0) return kColRowDel;
    if (std::strcmp(act, "变更") == 0) return kColRowChg;
    return 0;
}

/* 重复料号只涂料号格（与导出、标注三处同口径）。整行底色被差异类型占着，
   不能再压一层紫，否则「这行为什么黄」会被盖掉。 */
unsigned DiffSource::cell_bg(int r, int c) const {
    if (c != BS_COL_PN || !r_ || r < 0 || r >= n_) return 0;
    const bs_resrow *row = bs_result_row(r_, (size_t)r);
    if (!row || !row->col[BS_COL_DUP] || !*row->col[BS_COL_DUP]) return 0;
    return kColRowDup;
}

/* ── 8 列查重 ──────────────────────────────────────────────────── */

int DupSource::build(const bs_sheet *s) {
    rows_.clear();
    n_groups_ = 0;
    n_pn_rows_ = 0;
    n_ref_rows_ = 0;
    max_count_ = 0;
    top_pn_.clear();
    if (!s) return 0;

    const int nr = bs_sheet_row_count(s);
    std::map<std::string, std::vector<const bs_row *>> by_pn;
    for (int i = 0; i < nr; ++i) {
        const bs_row *r = bs_sheet_row(s, i);
        if (!r) continue;
        if (r->pn && *r->pn) {
            by_pn[r->pn].push_back(r);
            ++n_pn_rows_;
        } else {
            ++n_ref_rows_;
        }
    }
    std::vector<std::pair<std::string, std::vector<const bs_row *>>> groups;
    for (auto &kv : by_pn) {
        if (kv.second.size() > 1) groups.push_back(kv);
    }
    /* 出现次数降序、料号升序；组内保持原表行序（map 的 vector 本就是行序追加） */
    std::stable_sort(groups.begin(), groups.end(),
                     [](const std::pair<std::string, std::vector<const bs_row *>> &a,
                        const std::pair<std::string, std::vector<const bs_row *>> &b) {
                         if (a.second.size() != b.second.size())
                             return a.second.size() > b.second.size();
                         return a.first < b.first;
                     });
    n_groups_ = (int)groups.size();
    if (n_groups_ > 0) {
        max_count_ = (int)groups[0].second.size();
        top_pn_ = wstr(groups[0].first.c_str());
    }

    const std::wstring sheet = wstr(bs_sheet_name(s));
    char qty[64];
    for (auto &g : groups) {
        const int cnt = (int)g.second.size();
        for (const bs_row *r : g.second) {
            std::vector<std::wstring> row;
            row.reserve(8);
            bs_qty_text(r, qty, sizeof(qty));
            wchar_t num[32];
            std::swprintf(num, 32, L"%d", cnt);
            wchar_t line[32];
            std::swprintf(line, 32, L"r%d", r->row);
            row.push_back(wstr(g.first.c_str()));
            row.push_back(num);
            row.push_back(L"重复");
            row.push_back(wstr(r->ref));
            row.push_back(wstr(qty));
            row.push_back(r->strike ? L"有" : L"无");
            row.push_back(line);
            row.push_back(sheet);
            rows_.push_back(std::move(row));
        }
    }
    return (int)rows_.size();
}

const wchar_t *DupSource::header(int c) const {
    static const wchar_t *kHead[8] = {L"Part Number", L"重复次数", L"查重结果", L"位号",
                                      L"数量", L"删除线", L"行号", L"工作表"};
    return (c >= 0 && c < 8) ? kHead[c] : L"";
}

int DupSource::width(int c) const {
    static const int kW[8] = {140, 84, 84, 160, 76, 70, 62, 130};
    return (c >= 0 && c < 8) ? kW[c] : 80;
}

const wchar_t *DupSource::cell(int r, int c) const {
    if (r < 0 || r >= (int)rows_.size() || c < 0 || c >= 8) return L"";
    return rows_[(size_t)r][(size_t)c].c_str();
}

/* 查重整行浅紫：这里没有「差异类型」占位，紫就是唯一语义 */
unsigned DupSource::row_bg(int r) const {
    (void)r;
    return kColRowDup;
}

unsigned DupSource::cell_bg(int r, int c) const {
    (void)r;
    (void)c;
    return 0;
}

}  // namespace bs
