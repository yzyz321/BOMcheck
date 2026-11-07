#ifndef BS_CARD_H
#define BS_CARD_H

#include <windows.h>

#include <string>
#include <vector>

#include "bs_sheet.h"
#include "bs_xlsx.h"
#include "paint.h"

namespace bs {

/* 一张文件卡片的全部状态。
   C7 只到「选好文件、验一下是不是 xlsx」；C2 接上读取链后，
   载入即解析工作簿、列出工作表、并把首表读成网格，卡片才算「活」。 */
struct FileSlot {
    std::wstring path;
    std::wstring status = L"未载入";
    std::wstring sheet = L"（载入后选择）";

    std::vector<std::wstring> sheets;  // 工作表名（UTF-16，给界面用）
    int sheet_index = -1;

    bool ok = false;
    bool hot_browse = false;  // 「浏览…」悬停
    bool drop_hot = false;    // 拖放悬停
    bool hot_sheet = false;   // 工作表下拉悬停
    bool menu_open = false;   // 下拉正展开
    bool all_sheets = false;  // 下拉选了「全部工作表（逐表比对）」

    /* 当前工作表统计，卡片状态行与 --probe 都用它 */
    int rows = 0;      // 最大行号
    int cols = 0;      // 最大列号
    int n_cells = 0;   // 有 <c> 的格子数
    int n_strike = 0;  // 带删除线的格子数
    int n_merges = 0;  // 合并区数
    long long ms = 0;  // 解析耗时

    /* 识别结果（C3）：表头块、料号列、命中率、数据行。analysis 接管了网格，
       所以这里不再单独持有 grid —— 避免同一张表被解析两遍 */
    bs_sheet *analysis = nullptr;
    int header_row = 0;    // 第一个表头行
    int n_blocks = 0;      // 表头块数
    int data_rows = 0;     // 能建档的数据行
    int unkeyable = 0;     // 无法建档的行
    double hit_rate = 0.0; // 料号列内容命中率

    bs_workbook *wb = nullptr;  // 工作簿句柄（可换表）
};

/* 载入：校验扩展名 / 能否打开 / 是不是 zip，然后解析工作簿并读首表。
   失败时 status 写明原因，ok=false，wb/analysis 全部释放。 */
void card_load(FileSlot &slot);

/* 释放工作簿与网格（换文件、关窗口时调用） */
void card_unload(FileSlot &slot);

/* 切到第 idx 张表并重读网格；越界或失败返回 false */
bool card_pick_sheet(FileSlot &slot, int idx);

/* 读文件头判断是不是 xlsx（PK zip），只做「不解析」的快速校验 */
bool card_is_zip(const std::wstring &path);

/* 两条路径是否指向同一个文件：卷序列号 + 文件索引，与 os.path.samefile 同口径 */
bool card_same_file(const std::wstring &a, const std::wstring &b);

/* 画一张卡片。key 取 "old" / "new" */
void card_paint(const UiCtx &ui, const char *key, const wchar_t *cap, const FileSlot &slot);

}  // namespace bs

#endif  // BS_CARD_H
