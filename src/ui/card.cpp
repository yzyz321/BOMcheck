#include "card.h"

#include <cstdio>
#include <cwchar>
#include <string>

#include "layout.h"
#include "layout_gen.h"
#include "utf8.h"

namespace bs {
namespace {

RECT by_id(const UiCtx &ui, const char *prefix, const char *suffix) {
    char key[48];
    std::snprintf(key, sizeof(key), "%s.%s", prefix, suffix);
    return ui.px(key);
}

bool ends_with_xlsx(const std::wstring &p) {
    const size_t n = p.size();
    if (n < 5) return false;
    const wchar_t *e = p.c_str() + n - 5;
    return _wcsicmp(e, L".xlsx") == 0;
}

/* 第一张表数据行到这个数，就不再去翻后面的表了：绝大多数 BOM 的主表就在第一页，
   而一本 20 张表的大工作簿把每张都解析一遍是白花时间（10 万行时每张 ~0.4 s）。
   封面 / 修订记录页通常只有几行，翻不动这个门槛，于是会触发全本扫描。 */
const int AUTOSCAN_ROWS = 20;

void fmt_size(long long bytes, wchar_t *out, int cap) {
    if (bytes >= 1024LL * 1024)
        std::swprintf(out, cap, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        std::swprintf(out, cap, L"%.0f KB", (double)bytes / 1024.0);
    else
        std::swprintf(out, cap, L"%d B", (int)bytes);
}

long long now_ms() {
    static LARGE_INTEGER freq = {};
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart * 1000 / (freq.QuadPart ? freq.QuadPart : 1));
}

/* 状态行在 C3 之后才有信息量：表头在哪、料号列是哪一列、命中率多少、多少行 */
void set_status(FileSlot &slot) {
    std::wstring s = L"已载入 · ";
    wchar_t t[64];

    if (slot.sheets.size() > 1) {
        std::swprintf(t, 64, L"%d 表 · ", (int)slot.sheets.size());
        s += t;
    }
    if (slot.header_row > 0) {
        if (slot.n_blocks > 1)
            std::swprintf(t, 64, L"表头 r%d 等 %d 块", slot.header_row, slot.n_blocks);
        else
            std::swprintf(t, 64, L"表头 r%d", slot.header_row);
        s += t;
        const std::wstring pn_col = from_utf8(bs_sheet_col_name(slot.analysis, 'P'));
        if (!pn_col.empty()) {
            std::swprintf(t, 64, L" · 料号列 %ls", pn_col.c_str());
            s += t;
        }
        std::swprintf(t, 64, L" · 命中率 %d%%", (int)(slot.hit_rate * 100.0 + 0.5));
        s += t;
        std::swprintf(t, 64, L" · %d 行", slot.data_rows);
        s += t;
        if (slot.unkeyable > 0) {
            std::swprintf(t, 64, L" · 无法建档 %d", slot.unkeyable);
            s += t;
        }
    } else {
        std::swprintf(t, 64, L"%d 行 × %d 列", slot.rows, slot.cols);
        s += t;
    }
    std::swprintf(t, 64, L" · %d ms", (int)slot.ms);
    s += t;
    slot.status = s;
}

}  // namespace

bool card_is_zip(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char head[4] = {0, 0, 0, 0};
    DWORD got = 0;
    ReadFile(h, head, 4, &got, nullptr);
    CloseHandle(h);
    return got >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4;
}

void card_unload(FileSlot &slot) {
    if (slot.analysis) {
        bs_sheet_close(slot.analysis);
        slot.analysis = nullptr;
    }
    if (slot.wb) {
        bs_xlsx_close(slot.wb);
        slot.wb = nullptr;
    }
    slot.sheets.clear();
    slot.sheet_index = -1;
    slot.rows = slot.cols = slot.n_cells = slot.n_strike = slot.n_merges = 0;
    slot.header_row = slot.n_blocks = slot.data_rows = slot.unkeyable = 0;
    slot.hit_rate = 0.0;
    slot.ms = 0;
}

bool card_pick_sheet(FileSlot &slot, int idx) {
    if (!slot.wb) return false;
    if (idx < 0 || idx >= (int)slot.sheets.size()) return false;

    const long long t0 = now_ms();
    bs_grid *g = nullptr;
    bs_err e;
    const int rc = bs_xlsx_load_sheet(slot.wb, idx, &g, &e);

    if (slot.analysis) {
        bs_sheet_close(slot.analysis);
        slot.analysis = nullptr;
    }
    slot.sheet_index = idx;
    slot.sheet = slot.sheets[(size_t)idx];
    slot.rows = slot.cols = slot.n_cells = slot.n_strike = slot.n_merges = 0;
    slot.header_row = slot.n_blocks = slot.data_rows = slot.unkeyable = 0;
    slot.hit_rate = 0.0;

    if (rc != 0) {
        slot.status = L"× " + from_utf8(e.msg);
        slot.ok = false;
        slot.ms = now_ms() - t0;
        return false;
    }

    /* 网格交给识别层，读完就是「表头 / 料号列 / 行」三件事；
       识别失败（没有表头）也算读到了文件，只是没东西可比。
       这里传 nullptr = 引擎默认配置：识别层只用到固定阈值（命中率 0.60、
       表头别名 1 个、删除线整行），与界面上能改的三项（双键 / 阈值 / 逐列
       比较）无关，所以不必把 App::cfg_ 递进来。 */
    bs_sheet *sh = nullptr;
    const bool analysed = bs_sheet_from_grid(g, nullptr, &sh, &e) == 0;
    const bs_grid *grid = analysed ? bs_sheet_grid(sh) : nullptr;

    if (analysed) {
        slot.analysis = sh;
        /* from_grid 不知道表名，标注回写（C6）要靠它去找 sheet 部件 */
        bs_sheet_set_name(sh, to_utf8(slot.sheet).c_str());
        slot.header_row = bs_sheet_block_count(sh) > 0 ? bs_sheet_block(sh, 0)->hr : 0;
        slot.n_blocks = bs_sheet_block_count(sh);
        slot.data_rows = bs_sheet_row_count(sh);
        slot.unkeyable = bs_sheet_unkeyable(sh);
        slot.hit_rate = bs_sheet_hit_rate(sh);
        slot.rows = grid->max_row;
        slot.cols = grid->max_col;
        slot.n_cells = grid->n_cells;
        slot.n_strike = grid->n_strike;
        slot.n_merges = grid->n_merges;
        slot.ok = true;
        slot.ms = now_ms() - t0;
        set_status(slot);
        return true;
    }

    /* 没有表头：给出原因，文件本身仍算选中（可以换一张表再试） */
    slot.status = L"× " + from_utf8(e.msg);
    slot.ok = false;
    slot.sheet = slot.sheets[(size_t)idx];
    slot.ms = now_ms() - t0;
    return true;
}

void card_load(FileSlot &slot) {
    card_unload(slot);
    slot.ok = false;

    if (slot.path.empty()) {
        slot.status = L"未载入";
        slot.sheet = L"（载入后选择）";
        return;
    }
    if (!ends_with_xlsx(slot.path)) {
        slot.status = L"× 不是 .xlsx 文件，请重新选择";
        slot.sheet = L"（无）";
        return;
    }

    /* 先用 Win32 判「能不能开、是不是 zip」，这样被占用 / 传错文件的
       错误信息比 zip 层的更贴近用户实际遇到的情况 */
    HANDLE h = CreateFileW(slot.path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        slot.status = L"× 打不开：文件被占用或没有读取权限";
        slot.sheet = L"（无）";
        return;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    CloseHandle(h);

    if (!card_is_zip(slot.path)) {
        slot.status = L"× 文件头不是 PK，可能损坏或并非 xlsx";
        slot.sheet = L"（无）";
        return;
    }

    const long long t0 = now_ms();
    bs_err e;
    bs_workbook *wb = bs_xlsx_open(to_utf8(slot.path).c_str(), &e);
    if (!wb) {
        slot.status = L"× " + from_utf8(e.msg);
        slot.sheet = L"（无）";
        return;
    }
    slot.wb = wb;

    const int n = bs_xlsx_sheet_count(wb);
    if (n <= 0) {
        card_unload(slot);
        slot.status = L"× 工作簿里没有工作表";
        slot.sheet = L"（无）";
        return;
    }
    slot.sheets.clear();
    for (int i = 0; i < n; ++i) slot.sheets.push_back(from_utf8(bs_xlsx_sheet_name(wb, i)));

    const int n_sheets = (int)slot.sheets.size();
    card_pick_sheet(slot, 0);   /* 先按第一张来，下面的自动挑表可能改主意 */

    /* 自动挑表（说明里承诺过「默认自动挑最像 BOM 的那张」）。
       第一张是封面 / 修订记录 / 说明页的情况很常见，只认 index 0 会静默比对
       一张没有数据的表 —— 用户看到的就是「什么都没比出来」。 */
    bool any_ok = slot.ok;
    int best_idx = slot.sheet_index;
    int best_rows = slot.data_rows;
    double best_hit = slot.hit_rate;
    if ((!slot.ok || slot.data_rows < AUTOSCAN_ROWS) && n_sheets > 1) {
        for (int i = 1; i < n_sheets; ++i) {
            if (!card_pick_sheet(slot, i)) continue;
            any_ok = true;
            if (slot.data_rows > best_rows ||
                (slot.data_rows == best_rows && slot.hit_rate > best_hit)) {
                best_idx = i;
                best_rows = slot.data_rows;
                best_hit = slot.hit_rate;
            }
        }
        if (slot.sheet_index != best_idx || !slot.ok) card_pick_sheet(slot, best_idx);
    }

    if (!any_ok || !slot.ok) {
        card_unload(slot);
        slot.sheet = L"（读取失败）";
        return;
    }
    slot.ms = now_ms() - t0;  // 含工作簿解析，卡片上给的是总耗时
    set_status(slot);

    wchar_t sz[32] = L"";
    fmt_size(size.QuadPart, sz, 32);
    (void)sz;  // 体积暂时不占状态行，路径框已经够看
}

bool card_same_file(const std::wstring &a, const std::wstring &b) {
    if (a.empty() || b.empty()) return false;
    if (_wcsicmp(a.c_str(), b.c_str()) == 0) return true;

    auto open = [](const std::wstring &p) {
        return CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    };
    HANDLE ha = open(a), hb = open(b);
    if (ha == INVALID_HANDLE_VALUE || hb == INVALID_HANDLE_VALUE) {
        if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
        if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION fa = {}, fb = {};
    const bool ga = GetFileInformationByHandle(ha, &fa) != 0;
    const bool gb = GetFileInformationByHandle(hb, &fb) != 0;
    CloseHandle(ha);
    CloseHandle(hb);
    if (!ga || !gb) return false;
    return fa.dwVolumeSerialNumber == fb.dwVolumeSerialNumber &&
           fa.nFileIndexHigh == fb.nFileIndexHigh && fa.nFileIndexLow == fb.nFileIndexLow;
}

void card_paint(const UiCtx &ui, const char *key, const wchar_t *cap, const FileSlot &slot) {
    HDC dc = ui.dc;

    char id[48];
    std::snprintf(id, sizeof(id), "card.%s", key);
    RECT box = ui.px(id);

    /* 卡片本体：拖放悬停时整卡变浅蓝、描边加粗到 2px，提示「这里能接」 */
    const COLORREF bg = slot.drop_hot ? col(kColAccentBg) : col(kColCard);
    const COLORREF bd = slot.drop_hot ? col(kColAccent) : col(kColBorder);
    fill_round(dc, box, ui.sc(10), bg, bd, slot.drop_hot ? ui.sc(2) : 1);

    /* 标题 + 右侧拖放提示 */
    RECT t = by_id(ui, key, "title");
    draw_text(dc, ui.h3, col(kColText), t, cap, DT_LEFT);

    RECT hint = by_id(ui, key, "hint");
    draw_text(dc, ui.small, col(kColHint), hint, slot.path.empty()
                                                    ? L"拖入文件，或点「浏览…」"
                                                    : L"已就位",
              DT_LEFT);

    /* 路径框 */
    RECT p = by_id(ui, key, "path");
    fill_round(dc, p, ui.sc(6), col(kColCard), col(kColBorder), 1);
    RECT pt = p;
    pt.left += ui.sc(10);
    pt.right -= ui.sc(10);
    if (slot.path.empty()) {
        draw_text(dc, ui.small, col(kColHint), pt, L"尚未选择文件", DT_LEFT | DT_END_ELLIPSIS);
    } else {
        draw_text(dc, ui.body, col(kColText), pt, slot.path.c_str(),
                  DT_LEFT | DT_PATH_ELLIPSIS);
    }

    /* 浏览按钮 */
    RECT b = by_id(ui, key, "browse");
    if (slot.hot_browse) {
        fill_round(dc, b, ui.sc(6), col(kColAccentBg), col(kColAccent), 1);
        draw_text(dc, ui.body, col(kColAccent), b, L"浏览…", DT_CENTER);
    } else {
        fill_round(dc, b, ui.sc(6), col(kColCard), col(kColBorder), 1);
        draw_text(dc, ui.body, col(kColAccentDk), b, L"浏览…", DT_CENTER);
    }

    /* 工作表：读到工作簿后才是活的下拉，之前保持禁用态 */
    RECT sl = by_id(ui, key, "sheetlab");
    draw_text(dc, ui.small, col(kColMuted), sl, L"工作表", DT_LEFT);

    const bool can_pick = slot.ok && slot.sheets.size() > 1;
    RECT sc = by_id(ui, key, "sheet");
    const bool hot = can_pick && (slot.hot_sheet || slot.menu_open);
    fill_round(dc, sc, ui.sc(4), col(hot ? kColAccentBg : 0xF7F8FA),
               col(hot ? kColAccent : kColBorder), 1);

    RECT st = sc;
    st.left += ui.sc(8);
    st.right -= can_pick ? ui.sc(18) : ui.sc(8);
    const bool all_mode = slot.all_sheets && slot.sheets.size() > 1;
    draw_text(dc, ui.small, can_pick || slot.ok ? col(kColText) : col(kColHint), st,
              all_mode ? L"全部工作表（逐表比对）" : slot.sheet.c_str(),
              DT_LEFT | DT_END_ELLIPSIS);

    if (can_pick) {
        POINT tri[3] = {{sc.right - ui.sc(12), (sc.top + sc.bottom) / 2 - ui.sc(2)},
                        {sc.right - ui.sc(6), (sc.top + sc.bottom) / 2 - ui.sc(2)},
                        {sc.right - ui.sc(9), (sc.top + sc.bottom) / 2 + ui.sc(3)}};
        HBRUSH tb = CreateSolidBrush(col(hot ? kColAccent : kColHint));
        HGDIOBJ ob = SelectObject(dc, tb);
        HPEN tp = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
        HGDIOBJ op = SelectObject(dc, tp);
        Polygon(dc, tri, 3);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(tb);
        DeleteObject(tp);
    }

    /* 识别摘要 */
    RECT s = by_id(ui, key, "status");
    COLORREF sc2 = col(kColHint);
    if (slot.drop_hot)
        sc2 = col(kColAccent);
    else if (slot.ok)
        sc2 = col(kColOk);
    else if (!slot.path.empty())
        sc2 = col(kColErr);
    draw_text(dc, ui.small, sc2, s, slot.drop_hot ? L"松开即载入此侧" : slot.status.c_str(),
              DT_LEFT | DT_END_ELLIPSIS);
}

}  // namespace bs
