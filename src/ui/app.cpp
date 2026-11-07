#include "app.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "bs_mark.h"
#include "bs_version.h"
#include "bs_version_gen.h"  /* BS_ICON_ID 与版本宏，由 CMake 生成 */
#include "bs_write.h"
#include "dpi.h"
#include "layout.h"
#include "layout_gen.h"
#include "utf8.h"

namespace bs {
namespace {

constexpr wchar_t kClass[] = L"BOMCheckMain";
constexpr wchar_t kMenuClass[] = L"BOMCheckSheetMenu";

HFONT make_font(int px, bool bold, int dpi) {
    return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

void wide_from_utf8(const char *src, wchar_t *dst, int cap) {
    if (cap <= 0) return;
    MultiByteToWideChar(CP_UTF8, 0, src ? src : "", -1, dst, cap);
    dst[cap - 1] = 0;
}

/* 小号灰字：骨架阶段大量用它标「哪一阶段接入」 */
constexpr COLORREF kDisabledFill = RGB(0xF7, 0xF8, 0xFA);
constexpr COLORREF kDisabledText = RGB(0x8A, 0x91, 0x99);

}  // namespace

App::App(HINSTANCE inst) : inst_(inst) {
    bs_cfg_default(&cfg_);
    /* 界面的初始状态（引擎默认值见 bs_cfg_default，批量/回归沿用那一份）：
       双键默认关 —— 料号齐全的 BOM 按位号硬配反而容易把两颗不同的料配成一对；
       近似阈值先按 0.90 起步，勾上双键时看到的就是这个保守值；
       整行逐列比较默认关 —— 只比料号/位号/数量/删除线是约定俗成的口径。 */
    cfg_.dual_key = 0;
    cfg_.ref_min_overlap = 0.90;
    cfg_.row_diff_all = 0;
    wchar_t ver[32] = L"";
    wide_from_utf8(bs_version(), ver, 32);
    /* 注意：MinGW 的 swprintf 里 %s 按窄字符处理，宽字符串必须用 %ls 或手工拼 */
    std::wcsncpy(title_, L"BOM 对比工具 V", 96);
    title_[95] = 0;
    std::wcsncat(title_, ver, 32);
    refresh_status();
}

App::~App() {
    close_sheet_menu();
    table_.destroy();
    card_unload(slots_[0]);
    card_unload(slots_[1]);
    bs_result_close(res_);
    bs_pair_free(pairs_);
    if (hwnd_) drop_revoke(hwnd_);
    drop_fonts();
    if (hwnd_) DestroyWindow(hwnd_);
}

int App::scale(int logical) const { return MulDiv(logical, dpi_, 96); }
int App::to_logical(int px) const { return MulDiv(px, 96, dpi_); }

int App::client_lw() const {
    RECT rc = {0, 0, 0, 0};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    const int v = to_logical(rc.right);
    return v > 0 ? v : kBaseLogicalW;
}

int App::client_lh() const {
    RECT rc = {0, 0, 0, 0};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    const int v = to_logical(rc.bottom);
    return v > 0 ? v : kBaseLogicalH;
}

bool App::create(int nCmdShow) {
    // 标题栏/任务栏用 .rc 里嵌的 app.ico；hIconSm 明确要 16px 那档，
    // 否则系统会拿 256 硬缩，在 100% 缩放下反而糊
    HICON ico_big = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(BS_ICON_ID), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                      LR_DEFAULTCOLOR);
    HICON ico_small = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(BS_ICON_ID), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!ico_big) ico_big = LoadIconW(nullptr, IDI_APPLICATION);
    if (!ico_small) ico_small = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &App::wnd_proc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ico_big;
    wc.hIconSm = ico_small;
    wc.hbrBackground = nullptr;  // 全部自绘，避免闪白
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW mc = {sizeof(mc)};
    mc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    mc.lpfnWndProc = &App::menu_proc_entry;
    mc.hInstance = inst_;
    mc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mc.hbrBackground = nullptr;
    mc.lpszClassName = kMenuClass;
    if (!RegisterClassExW(&mc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    dpi_ = dpi_for_system();

    RECT wr = {0, 0, scale(kBaseLogicalW), scale(kBaseLogicalH)};
    window_rect_for_client(scale(kBaseLogicalW), scale(kBaseLogicalH), WS_OVERLAPPEDWINDOW, 0, dpi_,
                           &wr);
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;

    RECT wa = {0, 0, 0, 0};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    /* WS_CLIPCHILDREN：父窗口的双缓冲 BitBlt 不能盖到结果表 / 日志上 */
    hwnd_ = CreateWindowExW(0, kClass, title_, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h,
                            nullptr, nullptr, inst_, this);
    if (!hwnd_) return false;

    /* 真实 DPI 以窗口为准（可能是从 96 拖到 150% 的屏上） */
    dpi_ = dpi_for_window(hwnd_);
    make_fonts();
    create_children();
    layout_children();
    switch_tab(0, false);

    /* OLE 拖放：拿得到 DragEnter / DragOver，能画悬停反馈。
       失败（极少见）就退回老式 DragAcceptFiles，功能仍在、只是没有悬停态。 */
    ole_ok_ = drop_register(hwnd_, this);
    if (!ole_ok_) DragAcceptFiles(hwnd_, TRUE);

    if (nCmdShow != SW_HIDE) {
        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);
    }
    return true;
}

int App::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void App::make_fonts() {
    drop_fonts();
    f_h1_ = make_font(24, true, dpi_);
    f_h3_ = make_font(15, true, dpi_);
    f_body_ = make_font(13, false, dpi_);
    f_small_ = make_font(12, false, dpi_);
    /* 日志用等宽：时间戳与路径对齐后，扫一眼能看出哪几步 */
    f_mono_ = CreateFontW(-MulDiv(12, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void App::drop_fonts() {
    if (f_h1_) DeleteObject(f_h1_);
    if (f_h3_) DeleteObject(f_h3_);
    if (f_body_) DeleteObject(f_body_);
    if (f_small_) DeleteObject(f_small_);
    if (f_mono_) DeleteObject(f_mono_);
    f_h1_ = f_h3_ = f_body_ = f_small_ = nullptr;
    f_mono_ = nullptr;
}

UiCtx App::make_ctx(HDC dc) const {
    UiCtx ui;
    ui.dc = dc;
    ui.dpi = dpi_;
    ui.h1 = f_h1_;
    ui.h3 = f_h3_;
    ui.body = f_body_;
    ui.small = f_small_;
    ui.lw = client_lw();
    ui.lh = client_lh();
    return ui;
}

/* ── 绘制 ─────────────────────────────────────────────────────── */

void App::paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    /* 双缓冲：卡片圆角 + 悬停换色，直接画屏会闪 */
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1,
                                         rc.bottom > 0 ? rc.bottom : 1);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    UiCtx ui = make_ctx(mem);
    fill_rect(mem, rc, col(kColBg));
    SetBkMode(mem, TRANSPARENT);

    paint_header(ui);
    card_paint(ui, "old", L"① 旧 BOM", slots_[0]);
    card_paint(ui, "new", L"② 新 BOM", slots_[1]);
    paint_options(ui);
    paint_actions(ui);
    paint_results(ui);
    paint_status(ui);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void App::paint_header(const UiCtx &ui) {
    HDC dc = ui.dc;
    draw_text(dc, ui.h1, col(kColText), ui.px("hdr.title"), L"BOM 对比工具", DT_LEFT);

    RECT b = ui.px("hdr.ver");
    fill_round(dc, b, ui.sc(11), col(kColAccentBg));
    wchar_t ver[32] = L"V";
    wide_from_utf8(bs_version(), ver + 1, 30);
    draw_text(dc, ui.h3, col(kColAccent), b, ver, DT_CENTER);

    draw_text(dc, ui.small, col(kColMuted), ui.px("hdr.sub"),
              L"双键比对 · 删除线入项 · 表头块自动识别 · 未配对的差异单独标注",
              DT_LEFT | DT_END_ELLIPSIS);

    wchar_t dpi_txt[96];
    std::swprintf(dpi_txt, 96, L"DPI %d · 缩放 %d%% · 每显示器感知", dpi_, dpi_ * 100 / 96);
    draw_text(dc, ui.small, col(kColMuted), ui.px("hdr.dpi"), dpi_txt, DT_RIGHT);
    draw_text(dc, ui.small, col(kColHint), ui.px("hdr.hint"),
              L"拖拽 .xlsx 到下方卡片，可直接载入", DT_RIGHT);

    RECT ln = ui.px("hdr.line");
    ln.bottom = ln.top + (ui.sc(1) > 0 ? ui.sc(1) : 1);
    fill_rect(dc, ln, col(kColGrid));

    /* 帮助按钮：打开 exe 同目录的用户使用手册；文件不在时弹简要说明 */
    RECT hb = ui.px("hdr.help");
    fill_round(dc, hb, ui.sc(6), col(kColCard), col(kColBorder), 1);
    draw_text(dc, ui.small, col(kColAccent), hb, L"使用手册", DT_CENTER);
}

void App::paint_options(const UiCtx &ui) {
    HDC dc = ui.dc;
    fill_round(dc, ui.px("opt.frame"), ui.sc(10), col(kColCard), col(kColBorder), 1);
    draw_text(dc, ui.h3, col(kColMuted), ui.px("opt.title"), L"③ 比对选项", DT_LEFT);

    auto pill = [&](const char *id, const wchar_t *text) {
        RECT r = ui.px(id);
        fill_round(dc, r, ui.sc(5), kDisabledFill, col(kColBorder), 1);
        RECT t = r;
        t.left += ui.sc(10);
        t.right -= ui.sc(10);
        draw_text(dc, ui.small, kDisabledText, t, text, DT_LEFT | DT_END_ELLIPSIS);
    };
    pill("opt.dup", L"顺序配对（推荐）");
    pill("opt.strike", L"整行任一格");

    /* 近似阈值：数字框画成可点的样子，双键关掉时置灰（它只对位号兜底生效） */
    const bool dual = cfg_.dual_key != 0;
    const bool thr_live = dual && cfg_.ref_fuzzy != 0;
    {
        RECT r = ui.px("opt.thr");
        fill_round(dc, r, ui.sc(5), thr_live ? kDisabledFill : RGB(0xF2, 0xF3, 0xF5),
                   thr_live ? col(kColBorder) : col(kColGrid), 1);
        wchar_t thr[16];
        std::swprintf(thr, 16, L"%.2f", cfg_.ref_min_overlap);
        draw_text(dc, ui.small, thr_live ? col(kColText) : kDisabledText, r, thr, DT_CENTER);
    }

    /* 复选框：勾上是 accent 实心方块 + 白勾，取消是空心方框 */
    auto checkbox = [&](const char *id, const wchar_t *text, bool on) {
        RECT cb = ui.px(id);
        const int s = ui.sc(15);
        RECT sq = {cb.left, (cb.top + cb.bottom - s) / 2, cb.left + s,
                   (cb.top + cb.bottom + s) / 2};
        fill_round(dc, sq, ui.sc(3), on ? col(kColAccent) : col(kColCard),
                   on ? col(kColAccentDk) : col(kColMuted), 1);
        if (on) {
            /* 白勾用两段线画：字体里不一定有 ✓，画线在任何机器上都不会缺字 */
            HPEN pen = CreatePen(PS_SOLID, (ui.sc(2) > 0 ? ui.sc(2) : 1),
                                 RGB(0xFF, 0xFF, 0xFF));
            HGDIOBJ old = SelectObject(dc, pen);
            const int cx = sq.left + s / 2, cy = sq.top + s / 2;
            MoveToEx(dc, cx - s / 4, cy, nullptr);
            LineTo(dc, cx - s / 12, cy + s / 4);
            LineTo(dc, cx + s / 3, cy - s / 4);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        RECT lt = cb;
        lt.left = sq.right + ui.sc(8);
        draw_text(dc, ui.small, on ? col(kColText) : col(kColMuted), lt, text, DT_LEFT);
    };
    checkbox("opt.dual", L"双键（位号兜底）", dual);
    checkbox("opt.cell", L"整行逐列比较", cfg_.row_diff_all != 0);

    draw_text(dc, ui.small, col(kColMuted), ui.px("opt.duplab"), L"重复料号", DT_LEFT);
    draw_text(dc, ui.small, col(kColMuted), ui.px("opt.strikelab"), L"删除线范围", DT_LEFT);
    draw_text(dc, ui.small, thr_live ? col(kColMuted) : kDisabledText, ui.px("opt.thrlab"),
              L"近似阈值", DT_LEFT);

    /* 提示行随当前状态变：一眼能看出「现在是什么、怎么改」 */
    const wchar_t *hint;
    if (!dual && !cfg_.row_diff_all)
        hint = L"双键已关：没料号的行不参与比对（跳过，日志会写跳过多少行）· 勾「整行逐列比较」连描述/规格等列一起比";
    else if (!dual)
        hint = L"整行逐列比较已开：料号/位号/数量之外的列也比 · 双键仍关着，没料号的行跳过";
    else if (!cfg_.ref_fuzzy)
        hint = L"双键开启（位号精确）· 近似关闭";
    else
        hint = L"双键开启：无料号的行按位号兜底配对 · 点阈值换档 · 滚轮 0.05 微调";
    draw_text(dc, ui.small, col(kColHint), ui.px("opt.hint"), hint,
              DT_LEFT | DT_END_ELLIPSIS);
}

void App::paint_actions(const UiCtx &ui) {
    HDC dc = ui.dc;
    const bool ready = can_run();

    RECT run = ui.px("act.run");
    if (ready && !busy_) {
        fill_round(dc, run, ui.sc(7), col(kColAccent), col(kColAccentDk), 1);
        draw_text(dc, ui.h3, RGB(0xFF, 0xFF, 0xFF), run, run_label_, DT_CENTER);
    } else {
        fill_round(dc, run, ui.sc(7), kDisabledFill, col(kColBorder), 1);
        draw_text(dc, ui.h3, kDisabledText, run, run_label_, DT_CENTER);
    }

    /* 外观与动作同源：能不能点，只看 can_run()（与 case 2 的判断完全一致），
       不再用「有没有行」单独猜一遍——查重模式此前就是画得能点、点了必失败 */
    const bool can_export = can_run() && !busy_;
    RECT ex = ui.px("act.export");
    fill_round(dc, ex, ui.sc(7), col(kColCard),
               can_export ? col(kColBorder) : col(kColGrid), 1);
    draw_text(dc, ui.body, can_export ? col(kColText) : kDisabledText, ex, L"另存结果…",
              DT_CENTER);

    RECT st = ui.px("act.stop");
    fill_round(dc, st, ui.sc(6), col(kColCard), col(kColGrid), 1);
    draw_text(dc, ui.small, kDisabledText, st, L"停止", DT_CENTER);

    RECT pg = ui.px("act.progress");
    fill_round(dc, pg, ui.sc(4), RGB(0xED, 0xEF, 0xF2));
    if (progress_ > 0) {
        RECT f = pg;
        f.right = pg.left + (int)((long long)(pg.right - pg.left) * progress_ / 100);
        if (f.right > f.left)
            fill_round(dc, f, ui.sc(4), col(progress_ >= 100 ? kColOk : kColAccent));
    }
    {
        wchar_t pct[16];
        std::swprintf(pct, 16, L"%d%%", progress_);
        draw_text(dc, ui.small, col(kColMuted), ui.px("act.pct"), pct, DT_LEFT);
        draw_text(dc, ui.small, col(kColHint), ui.px("act.state"), run_state_, DT_RIGHT);
    }
}

void App::paint_results(const UiCtx &ui) {
    HDC dc = ui.dc;
    fill_round(dc, ui.px("res.frame"), ui.sc(10), col(kColCard), col(kColBorder), 1);

    /* 页签：选中页白底 + 主色字 + 2px 下划线，未选页压灰 */
    const char *ids[2] = {"res.tab1", "res.tab2"};
    const wchar_t *names[2] = {L"差异结果", L"运行日志"};
    for (int i = 0; i < 2; ++i) {
        RECT t = ui.px(ids[i]);
        if (i == tab_) {
            fill_round(dc, t, ui.sc(5), col(kColAccentBg));
            RECT ul = {t.left + ui.sc(10), t.bottom - ui.sc(3), t.right - ui.sc(10), t.bottom};
            fill_rect(dc, ul, col(kColAccent));
            draw_text(dc, ui.small, col(kColAccent), t, names[i], DT_CENTER);
        } else {
            draw_text(dc, ui.small, col(kColHint), t, names[i], DT_CENTER);
        }
    }

    {
        wchar_t cnt[64] = L"";
        if (dup_mode_ && dup_src_ && dup_src_->rows() > 0)
            std::swprintf(cnt, 64, L"%d 行重复 · 涉及 %d 个料号", dup_src_->rows(),
                          dup_src_->n_groups());
        else if (has_pairs_)
            std::swprintf(cnt, 64, L"%d 条差异", n_res_);
        draw_text(dc, ui.small, col(kColHint), ui.px("res.count"), cnt, DT_RIGHT);
    }

    /* 表格与日志都是子窗口，这里只把底衬画好（子窗口会把这块盖住） */
    RECT tb = ui.px("res.table");
    fill_round(dc, tb, ui.sc(6), RGB(0xFA, 0xFB, 0xFC), col(kColBorder), 1);

    if (tab_ != 0) {
        /* 日志页：图例整条隐藏，免得「四色说明」挂在日志下面说不通 */
        return;
    }

    RECT lg = ui.px("res.legend");
    struct Chip {
        unsigned rgb;
        const wchar_t *name;
    };
    const Chip four[4] = {{kColRowAdd, L"新增"}, {kColRowDel, L"删除"},
                          {kColRowChg, L"变更"}, {kColRowDup, L"重复料号"}};
    const Chip one[1] = {{kColRowDup, L"重复行"}};
    const Chip *chips = dup_mode_ ? one : four;
    const int nchips = dup_mode_ ? 1 : 4;

    int x = lg.left;
    const int cy = (lg.top + lg.bottom) / 2;
    const int box = ui.sc(10);
    for (int i = 0; i < nchips; ++i) {
        RECT chip = {x, cy - box / 2, x + box, cy + box / 2};
        fill_rect(dc, chip, col(chips[i].rgb));
        HBRUSH fb = CreateSolidBrush(col(kColBorder));
        FrameRect(dc, &chip, fb);
        DeleteObject(fb);
        x += box + ui.sc(6);
        int tw = text_w(dc, ui.small, chips[i].name);
        RECT nr = {x, lg.top, x + tw + ui.sc(4), lg.bottom};
        draw_text(dc, ui.small, col(kColMuted), nr, chips[i].name, DT_LEFT);
        x += tw + ui.sc(18);
    }
    RECT tail = {x, lg.top, lg.right, lg.bottom};
    draw_text(dc, ui.small, col(kColHint), tail,
              dup_mode_ ? L"整行浅紫 · 料号格带批注 n/N"
                        : L"色值与导出标注文件一致 · 窗口按行着色，导出按格着色",
              DT_LEFT | DT_END_ELLIPSIS);
}

void App::paint_status(const UiCtx &ui) {
    HDC dc = ui.dc;
    RECT bar = ui.px("sta.bar");
    fill_rect(dc, bar, col(kColCard));
    RECT ln = {bar.left, bar.top, bar.right, bar.top + (ui.sc(1) > 0 ? ui.sc(1) : 1)};
    fill_rect(dc, ln, col(kColGrid));
    draw_text(dc, ui.small, status_warn_ ? col(kColAmber) : col(kColMuted),
              ui.px("sta.text"), status_, DT_LEFT | DT_END_ELLIPSIS);
}

/* ── C7：子窗口、结果表与「一次点击出三份」 ───────────────────── */

static std::wstring stem_of(const std::wstring &path);

void App::create_children() {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    table_.create(hwnd_, inst_);
    table_.set_dpi(dpi_);

    /* 日志用系统 EDIT：只读、多行、竖向滚动由系统管，长文本选中复制也现成 */
    log_hwnd_ = CreateWindowExW(0, L"EDIT", L"",
                           WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                               ES_NOHIDESEL | ES_AUTOVSCROLL,
                           0, 0, 10, 10, hwnd_, nullptr, inst_, nullptr);
    if (log_hwnd_) {
        SendMessageW(log_hwnd_, WM_SETFONT, (WPARAM)(f_mono_ ? f_mono_ : f_small_), TRUE);
        /* 多行 EDIT 默认 32767 字符上限：400 行带路径的日志会从尾部被截掉，
           丢的正好是最新、最该看的几步 */
        SendMessageW(log_hwnd_, EM_SETLIMITTEXT, 0, 0);
    }

    prog_ = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD, 0, 0, 10, 10, hwnd_, nullptr,
                            inst_, nullptr);
    if (prog_) {
        SendMessageW(prog_, PBM_SETRANGE32, 0, 100);
        SendMessageW(prog_, PBM_SETPOS, 0, 0);
    }
}

void App::layout_children() {
    if (!hwnd_) return;
    const int lw = client_lw(), lh = client_lh();

    RECT t = layout_rect("res.table", lw, lh);
    const int tw = t.right - t.left, th = t.bottom - t.top;
    table_.set_logical_rect(t.left, t.top, tw, th);
    if (log_hwnd_)
        SetWindowPos(log_hwnd_, nullptr, scale(t.left), scale(t.top), scale(tw), scale(th),
                     SWP_NOZORDER | SWP_NOACTIVATE);

    RECT p = layout_rect("act.progress", lw, lh);
    if (prog_)
        SetWindowPos(prog_, nullptr, scale(p.left), scale(p.top), scale(p.right - p.left),
                     scale(p.bottom - p.top), SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 表的数据源按当前模式换：比对 12 列 / 查重整 8 列。切模式就是换源，
   表格本身不重建 —— 冻结列、滚动位置、行高全都不动。 */
void App::make_table() {
    dup_mode_ = !slots_[0].path.empty() && !slots_[1].path.empty() &&
                card_same_file(slots_[0].path, slots_[1].path);

    table_.set_source(nullptr);
    diff_src_.reset();
    dup_src_.reset();

    if (dup_mode_) {
        std::wcsncpy(run_label_, L"开始查重", 23);
        run_label_[23] = 0;
        if (slots_[0].analysis) {
            dup_src_.reset(new DupSource());
            dup_src_->build(slots_[0].analysis);
        }
        table_.set_empty_text(L"这份 BOM 没有重复料号，不生成标注文件");
        table_.set_source(dup_src_ ? static_cast<TableSource *>(dup_src_.get()) : nullptr);
    } else {
        std::wcsncpy(run_label_, all_sheets_ ? L"逐表比对" : L"开始比对", 23);
        run_label_[23] = 0;
        if (res_) diff_src_.reset(new DiffSource(res_));
        table_.set_empty_text(has_pairs_ ? L"两份 BOM 没有差异"
                                        : L"两份 BOM 就位后自动配对，结果显示在这里");
        table_.set_source(diff_src_ ? static_cast<TableSource *>(diff_src_.get()) : nullptr);
    }
}

void App::switch_tab(int tab, bool redraw) {
    tab_ = tab ? 1 : 0;
    table_.set_visible(tab_ == 0);
    if (log_hwnd_) ShowWindow(log_hwnd_, tab_ == 1 ? SW_SHOW : SW_HIDE);
    if (redraw && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::set_tab(int tab) { switch_tab(tab); }

void App::append_log(const std::wstring &line) {
    log_.push_back(line);
    if ((int)log_.size() > log_limit_)
        log_.erase(log_.begin(), log_.begin() + ((int)log_.size() - log_limit_));
    refresh_log_view();
}

void App::refresh_log_view() {
    if (!log_hwnd_) return;
    std::wstring all;
    all.reserve(log_.size() * 64);
    for (size_t i = 0; i < log_.size(); ++i) {
        all += log_[i];
        all += L"\r\n";
    }
    SetWindowTextW(log_hwnd_, all.c_str());
    const int lines = (int)SendMessageW(log_hwnd_, EM_GETLINECOUNT, 0, 0);
    SendMessageW(log_hwnd_, EM_LINESCROLL, 0, lines);
}

void App::set_progress(int pct, const wchar_t *state) {
    progress_ = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    if (state) {
        std::wcsncpy(run_state_, state, 31);
        run_state_[31] = 0;
    }
    if (prog_) {
        SendMessageW(prog_, PBM_SETPOS, (WPARAM)progress_, 0);
        SendMessageW(prog_, PBM_SETSTATE,
                     progress_ >= 100 ? (WPARAM)PBST_NORMAL : (WPARAM)PBST_NORMAL, 0);
        UpdateWindow(prog_);
    }
    /* 进度条是子窗口，进度数字与状态文字在父窗口上，要单独刷一次 */
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
}

void App::set_busy(bool busy) {
    busy_ = busy;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::can_run() const {
    if (slots_[0].path.empty() || slots_[1].path.empty()) return false;
    if (!slots_[0].ok || !slots_[1].ok) return false;
    if (dup_mode_) return slots_[0].analysis != nullptr;
    return has_pairs_ && res_ != nullptr;
}

int App::hit_action(int lx, int ly) const {
    POINT p = {lx, ly};
    struct {
        const char *id;
        int code;
    } kHit[] = {{"act.run", 1}, {"act.export", 2},
                {"res.tab1", 4}, {"res.tab2", 5}, {"hdr.help", 6}};
    /* 注意：act.stop 故意不在这里 —— 比对与写出都在 UI 线程同步执行，
       没有可取消的检查点，按钮恒为禁用态（见 layout_spec 的同名控件说明） */
    for (const auto &h : kHit) {
        RECT r = layout_rect(h.id, client_lw(), client_lh());
        if (PtInRect(&r, p)) return h.code;
    }
    return 0;
}

int App::hit_options(int lx, int ly) const {
    POINT p = {lx, ly};
    RECT r = layout_rect("opt.dual", client_lw(), client_lh());
    if (PtInRect(&r, p)) return 1;
    if (cfg_.dual_key && cfg_.ref_fuzzy) {
        r = layout_rect("opt.thr", client_lw(), client_lh());
        if (PtInRect(&r, p)) return 2;
    }
    r = layout_rect("opt.cell", client_lw(), client_lh());
    if (PtInRect(&r, p)) return 3;
    return 0;
}

void App::after_option_change(const std::wstring &what) {
    append_log(what);
    /* 有表就立刻按新参数重算：差异表、标注、统计一起变，不留在半新半旧的状态 */
    if (slots_[0].analysis && slots_[1].analysis) {
        recompute_pairs();
        done_note_.clear();
    }
    refresh_status();
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
}

void App::set_dual_key(bool on) {
    const int v = on ? 1 : 0;
    if (v == cfg_.dual_key) return;
    cfg_.dual_key = v;
    wchar_t buf[160];
    if (on)
        std::swprintf(buf, 160,
                      L"比对选项：双键开启 —— 没料号的行按位号兜底配对，近似阈值 %.2f",
                      cfg_.ref_min_overlap);
    else
        std::swprintf(buf, 160, L"比对选项：双键关闭 —— 没料号的行跳过，不参与比对");
    after_option_change(buf);
}

void App::set_row_diff_all(bool on) {
    const int v = on ? 1 : 0;
    if (v == cfg_.row_diff_all) return;
    cfg_.row_diff_all = v;
    after_option_change(on ? L"比对选项：整行逐列比较开启 —— 料号/位号/数量之外的列也比，"
                             L"任一格不同即算变更并涂黄"
                           : L"比对选项：整行逐列比较关闭 —— 只比料号/位号/数量/删除线");
}

void App::set_ref_min_overlap(double v) {
    if (v < 0.30) v = 0.30;
    if (v > 0.95) v = 0.95;
    v = (double)((int)(v * 100.0 + 0.5)) / 100.0;
    if (v == cfg_.ref_min_overlap) return;
    cfg_.ref_min_overlap = v;
    wchar_t buf[128];
    std::swprintf(buf, 128, L"比对选项：近似阈值 %.2f —— 位号重合度低于它的不再配对",
                  cfg_.ref_min_overlap);
    after_option_change(buf);
}

void App::click_threshold() {
    /* 七档循环：0.40 0.50 0.60 0.70 0.80 0.90 0.95 → 回到 0.40。
       当前值不在档位上（滚轮调过）时，跳到「比它大的下一档」。
       档位与滚轮上下限（0.30–0.95）一致，界面初始值 0.90 也在档上。 */
    static const double kLadder[] = {0.40, 0.50, 0.60, 0.70, 0.80, 0.90, 0.95};
    const int n = (int)(sizeof(kLadder) / sizeof(kLadder[0]));
    double next = kLadder[0];
    for (int i = 0; i < n; ++i) {
        if (cfg_.ref_min_overlap < kLadder[i] - 1e-9) {
            next = kLadder[i];
            break;
        }
    }
    set_ref_min_overlap(next);
}

void App::nudge_threshold(int dir) {
    set_ref_min_overlap(cfg_.ref_min_overlap + (dir >= 0 ? 0.05 : -0.05));
}

/* ── 帮助 ───────────────────────────────────────────────────────────
   手册随交付包放在 exe 同目录（pack.py 会把 md 转出来的单文件 html 拷进去）。
   找不到文件时不当成错误：弹一段「双键 / 阈值 / 逐列比较」的速查，比什么都没有强。 */
void App::help_target(std::wstring &path, bool &exists) const {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring dir;
    if (n > 0 && n < (DWORD)(sizeof(buf) / sizeof(buf[0]))) {
        std::wstring exe(buf, n);
        const size_t k = exe.find_last_of(L"\\/");
        if (k != std::wstring::npos) dir = exe.substr(0, k);
    }
    path = dir.empty() ? std::wstring(L"用户使用手册.html")
                       : dir + L"\\" + L"用户使用手册.html";
    const DWORD attr = GetFileAttributesW(path.c_str());
    exists = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

namespace {

/* 用系统默认程序打开一个文件。三条路依次试，任一条成功就返回 0：
   ① ShellExecuteExW —— 首选，失败时能拿到具体错误码（ShellExecuteW 只回一个
      魔数，>32 就算成功，出问题完全看不出原因）；
   ② rundll32 url.dll,FileProtocolHandler —— 走另一条关联链，专治「.html 的
      文件关联被改坏 / 只剩一个没注册的 ProgId」；
   ③ 从注册表读默认浏览器的命令行，自己 CreateProcess —— 前两条都黄了才用。
   真失败时把 Win32 错误码交给调用方，别再静默。 */
/* 打开前先确认自己读得到。放在 C:\Program Files、或目录 ACL 只给了管理员时，
   浏览器（普通权限）一样读不到，表现出来就是「点了一下，什么都没发生」。 */
UINT file_unreadable_code(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return 0;
    }
    const DWORD e = GetLastError();
    /* 文件不存在 / 路径有问题已经在上一步排除了，这里只报真读不了的情况 */
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 0 : e;
}

UINT shell_open_with(const std::wstring &path, HWND owner) {
    SHELLEXECUTEINFOW sei;
    std::memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return 0;
    const UINT e1 = GetLastError();

    {
        std::wstring cmd = L"rundll32.exe url.dll,FileProtocolHandler \"" + path + L"\"";
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        std::memset(&si, 0, sizeof(si));
        std::memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
    }

    /* 注册表兜底：HKCU\...\UrlAssociations\http\UserChoice 的 ProgId →
       HKCR\<ProgId>\shell\open\command（默认值形如 "C:\...\msedge.exe" --single-argument %1） */
    HKEY k = nullptr;
    wchar_t progid[128] = L"";
    DWORD cb = sizeof(progid) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations"
                      L"\\http\\UserChoice", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"ProgId", nullptr, &type, reinterpret_cast<LPBYTE>(progid), &cb);
        RegCloseKey(k);
    }
    if (progid[0]) {
        wchar_t cmd[1024] = L"";
        cb = sizeof(cmd) - sizeof(wchar_t);
        const std::wstring sub = std::wstring(progid) + L"\\shell\\open\\command";
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub.c_str(), 0, KEY_QUERY_VALUE, &k) ==
            ERROR_SUCCESS) {
            RegQueryValueExW(k, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(cmd), &cb);
            RegCloseKey(k);
        }
        std::wstring line(cmd);
        const size_t at = line.find(L"%1");
        if (at != std::wstring::npos && !line.empty()) {
            std::wstring quoted = L"\"" + path + L"\"";
            line.replace(at, 2, quoted);
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            std::memset(&si, 0, sizeof(si));
            std::memset(&pi, 0, sizeof(pi));
            si.cb = sizeof(si);
            if (CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return 0;
            }
        }
    }
    return e1 ? e1 : 1;
}

}  // namespace

void App::open_help() {
    std::wstring path;
    bool exists = false;
    help_target(path, exists);

    if (exists) {
        /* 先分清楚「读不到」和「打不开」：这两种的系统错误码长得很像，但让用户
           做的事完全不同 —— 前者是文件/目录权限，后者是 .html 关联 */
        const UINT deny = file_unreadable_code(path);
        if (deny != 0) {
            wchar_t code[32];
            std::swprintf(code, 32, L"%u", deny);
            std::wstring msg =
                L"手册文件在，但当前账户读不了它。\n\n文件位置：\n" + path +
                L"\n\n系统返回的错误码：" + code +
                L"（5 = 拒绝访问，32 = 文件被占用）\n\n"
                L"常见原因：程序放在 C:\\Program Files 之类的受保护目录、文件从别处复制过来时"
                L"带了只允许管理员读取的权限、或者安全软件正在扫描/隔离它。\n\n"
                L"处理办法：把整个程序文件夹（exe + 用户使用手册.html）挪到桌面或 D 盘，"
                L"右键 html → 属性 → 解除锁定后再点一次。";
            MessageBoxW(hwnd_, msg.c_str(), L"打开用户使用手册失败", MB_OK | MB_ICONWARNING);
            append_log(L"手册读不到（错误码 " + std::wstring(code) + L"）：" + path);
            return;
        }

        const UINT err = shell_open_with(path, hwnd_);
        if (err == 0) {
            append_log(L"已用系统默认程序打开用户使用手册：" + path);
            return;
        }
        /* 打不开就把原因说清楚：错误码 + 常见成因（受保护目录 / 关联被改坏 /
           安全软件拦截），别让用户对着一个没反应的程序猜 */
        wchar_t code[32];
        std::swprintf(code, 32, L"%u", err);
        std::wstring msg = L"手册文件找到了，但系统没能打开它。\n\n文件位置：\n" + path +
                           L"\n\n系统返回的错误码：" + code + L"\n\n";
        msg += L"常见的三种原因：\n"
               L"· 程序放在 C:\\Program Files、C:\\Windows 之类的受保护目录里，"
               L"浏览器读不到这个文件 —— 把整个程序文件夹挪到桌面或 D 盘再试；\n"
               L"· .html 的默认打开方式被改坏（双击 html 也没反应）—— "
               L"设置 → 应用 → 默认应用，把 .html 指回浏览器；\n"
               L"· 安全软件拦了这次「程序打开网页」的动作 —— 放行本程序后重试。\n\n"
               L"临时办法：把上面那行路径复制到浏览器的地址栏，或把这个文件直接拖进浏览器窗口。";
        MessageBoxW(hwnd_, msg.c_str(), L"打开用户使用手册失败", MB_OK | MB_ICONWARNING);
        append_log(L"手册打开失败（错误码 " + std::wstring(code) + L"）：" + path);
        return;
    }

    const wchar_t *brief =
        L"没找到「用户使用手册.html」（应与本程序放在同一目录）。\n"
        L"下面先把最容易设错的几个地方说清楚：\n\n"
        L"【双键（位号兜底）】默认关闭\n"
        L"料号对不上时，再用位号集合核对一遍。开启后「料号换了、位号没变」能配成一对并标为"
        L"「位号(料号变更)」；关闭后没有料号的行直接跳过（日志里会写跳过多少行），"
        L"不会硬凑成一条新增 + 一条删除。\n\n"
        L"【近似阈值】只在双键开启时生效，默认 0.90\n"
        L"衡量两行位号的重合度（交集 ÷ 并集），≥ 阈值才配成一对。0.90 表示几乎要求位号完全"
        L"一致；调到 0.60 会把「差一两个位号」的行也算成同一颗料。点数字框换档，滚轮 ±0.05。\n\n"
        L"【整行逐列比较】默认关闭\n"
        L"勾上后，除料号 / 位号 / 数量 / 删除线之外的列（描述、规格、厂商等）也比一遍，"
        L"任一格不同即算「变更」，并把这些格子涂黄。\n\n"
        L"【新旧选同一个文件 → 料号查重】\n"
        L"旧 BOM 和新 BOM 拖进同一份文件时，程序不做版本比对，而是把这份表里的重复料号找出来："
        L"按料号分组列出（查重表 8 列），并写出「<文件名>_查重标注.xlsx」。"
        L"用来在发版前清掉一颗料多个编号、同料重复入项这类问题；这时「开始比对」按钮会变成"
        L"「开始查重」。\n\n"
        L"完整说明（操作步骤、12 列含义、四种颜色、常见问题、命令行参数）见随包的用户使用手册。";
    MessageBoxW(hwnd_, brief, L"帮助：双键 / 近似阈值 / 逐列比较 / 同文件查重",
                MB_OK | MB_ICONINFORMATION);
    append_log(L"帮助：手册缺失，已弹出简要说明（" + path + L"）");
}

/* ── 输出目录 ─────────────────────────────────────────────────── */

namespace {

std::wstring dir_of(const std::wstring &path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring() : path.substr(0, sep);
}

bool ensure_dir(const std::wstring &dir) {
    if (dir.empty()) return false;
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* 真写一个临时文件判断可写：只读盘、ACL 拒绝、U 盘写保护都会在这里露出来 */
bool dir_writable(const std::wstring &dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const std::wstring probe = dir + L"\\~bomcheck.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

}  // namespace

bool App::resolve_out_dir(std::wstring &dir, std::wstring &err) {
    if (!out_dir_.empty()) {
        dir = out_dir_;
        if (!ensure_dir(dir)) {
            err = L"目录不能创建：" + dir;
            return false;
        }
        if (!dir_writable(dir)) {
            err = L"目录不可写：" + dir;
            return false;
        }
        return true;
    }

    const std::wstring want = dir_of(slots_[0].path) + L"\\checkresults";
    if (ensure_dir(want) && dir_writable(want)) {
        dir = want;
        return true;
    }

    /* 只读目录：按 2.8 的要求让用户改选；探针模式不弹窗，直接报错 */
    if (!interactive_) {
        err = L"默认目录不可写：" + want;
        return false;
    }
    BROWSEINFOW bi = {0};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"checkresults 不可写，请选择结果输出目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) {
        err = L"已取消，本次不写文件（界面结果仍在）";
        return false;
    }
    wchar_t picked[MAX_PATH * 2] = L"";
    const bool got = SHGetPathFromIDListW(idl, picked);
    CoTaskMemFree(idl);
    if (!got || !picked[0]) {
        err = L"没有选中目录";
        return false;
    }
    if (!dir_writable(picked)) {
        err = std::wstring(L"选中的目录不可写：") + picked;
        return false;
    }
    dir = picked;
    return true;
}

/* ── 一次点击：比对 → 三份文件 ────────────────────────────────── */

bool App::run_compare() {
    if (busy_) return false;
    if (!can_run()) {
        append_log(L"还不能开始：需要两份已识别完成的 BOM（状态栏有原因）");
        status_warn_ = true;
        std::wcsncpy(status_, L"还不能开始：两份 BOM 尚未就绪", 191);
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    if (dup_mode_) {
        run_dups();
        return true;
    }

    std::wstring dir, err;
    if (!resolve_out_dir(dir, err)) {
        append_log(L"未写出：" + err);
        status_warn_ = true;
        std::wcsncpy(status_, err.c_str(), 191);
        status_[191] = 0;
        set_progress(0, L"未写出");
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }

    set_busy(true);

    struct Sink : WriteStepSink {
        App *app = nullptr;
        void on_writing(int pct, const wchar_t *what) override {
            app->set_progress(pct, what);
            app->append_log(std::wstring(L"  · ") + what);
        }
    } sink;
    sink.app = this;

    /* 逐表模式：同名表一张一张过，每张各出一组文件 */
    if (all_sheets_) {
        set_progress(5, L"逐表比对");
        append_log(L"开始逐表比对 · 输出目录 " + dir);
        const bool any = run_compare_all(dir, &sink);
        set_busy(false);
        if (!any) {
            set_progress(progress_, L"未写出");
            status_warn_ = true;
            std::wcsncpy(status_, L"逐表比对没有写出任何文件（两侧没有同名表，或都为空）", 191);
            status_[191] = 0;
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return false;
        }
        set_progress(100, L"完成");
        done_note_ = L"逐表比对完成（" + dir + L"）";
        status_warn_ = false;
        refresh_status();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    set_progress(5, L"准备写出");
    /* 单表模式把工作表名写进日志：多页工作簿只比一张时，用户要能一眼看出
       「现在比的是哪一张、其它页没参与」，而不是比对完了才发现只有一页有颜色 */
    append_log(L"开始比对 · 工作表「" + slots_[0].sheet + L"」· 输出目录 " + dir);
    if (slots_[0].sheets.size() > 1 || slots_[1].sheets.size() > 1) {
        append_log(L"（本次只比这一张；要比全部同名表，请在工作表下拉里选「全部工作表（逐表比对）」）");
    }

    const WriteAllResult wa = write_all(dir, &sink);
    set_busy(false);

    if (!wa.ok) {
        append_log(L"写出失败：" + wa.err);
        set_progress(progress_, L"失败");
        status_warn_ = true;
        std::wcsncpy(status_, (L"写出失败：" + wa.err).c_str(), 191);
        status_[191] = 0;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }

    set_progress(100, L"完成");
    append_log(L"差异结果 " + wa.res);
    append_log(L"旧表标注 " + wa.old_mark);
    append_log(L"新表标注 " + wa.new_mark);
    done_note_ = L"已写出 3 份（" + dir + L"）";
    status_warn_ = false;
    refresh_status();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

/* 两侧同一个文件：只出一份查重标注；没有重复料号就一个文件都不生成 */
void App::run_dups() {
    if (!slots_[0].analysis) return;

    std::wstring dir, err;
    if (!resolve_out_dir(dir, err)) {
        append_log(L"未写出：" + err);
        status_warn_ = true;
        std::wcsncpy(status_, err.c_str(), 191);
        status_[191] = 0;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    write_dups(dir);
}

bool App::write_dups(const std::wstring &dir) {
    if (!slots_[0].analysis) return false;
    const int n = dup_src_ ? dup_src_->rows() : 0;
    if (n == 0) {
        append_log(L"未发现重复料号，不生成标注文件");
        set_progress(100, L"无重复");
        done_note_ = L"无重复料号";
        refresh_status();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }

    set_busy(true);
    set_progress(30, L"写查重标注");
    const std::wstring stem = stem_of(slots_[0].path);
    const std::wstring dst = dir + L"\\" + stem + L"_查重标注.xlsx";
    append_log(L"开始查重 · 输出目录 " + dir);

    bs_err e;
    e.msg[0] = 0;
    char real[1024] = "";
    const std::string src = to_utf8(slots_[0].path);
    const int rc = bs_mark_dups(slots_[0].analysis, src.c_str(), to_utf8(dst).c_str(), real,
                                sizeof(real), &e);
    set_busy(false);

    if (rc != 0) {
        append_log(L"查重标注失败：" + from_utf8(e.msg));
        set_progress(progress_, L"失败");
        status_warn_ = true;
        std::wcsncpy(status_, (L"查重标注失败：" + from_utf8(e.msg)).c_str(), 191);
        status_[191] = 0;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }

    set_progress(100, L"完成");
    append_log(L"查重标注 " + from_utf8(real));
    wchar_t note[256];
    std::swprintf(note, 256, L"已写出查重标注（%d 行重复 · %d 个料号）", n, dup_src_->n_groups());
    done_note_ = note;
    status_warn_ = false;
    refresh_status();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

/* ── 探针 ─────────────────────────────────────────────────────── */

void App::dump_table(void (*emit)(const char *line)) const {
    if (!emit) return;
    TableSource *src = dup_mode_ ? static_cast<TableSource *>(dup_src_.get())
                                 : static_cast<TableSource *>(diff_src_.get());
    char buf[64];
    if (!src) {
        emit("table rows=0 cols=0 dup=0");
        return;
    }
    std::snprintf(buf, sizeof(buf), "table rows=%d cols=%d frozen=%d dup=%d xoff=%d top=%d",
                  src->rows(), src->cols(), table_.frozen_width(), dup_mode_ ? 1 : 0,
                  table_.x_offset(), table_.top_row());
    emit(buf);
    for (int r = 0; r < src->rows(); ++r) {
        char head[64];
        const unsigned bg = src->row_bg(r);
        const unsigned pn = src->cell_bg(r, 0);
        std::snprintf(head, sizeof(head), "row %d bg=%06X pnbg=%06X\t", r, bg ? bg : 0,
                      pn ? pn : 0);
        std::string line = head;
        for (int c = 0; c < src->cols(); ++c) {
            if (c) line += "\t";
            line += to_utf8(src->cell(r, c));
        }
        emit(line.c_str());
    }
}

void App::dump_log(void (*emit)(const char *line)) const {
    if (!emit) return;
    for (size_t i = 0; i < log_.size(); ++i) emit(to_utf8(log_[i]).c_str());
}

void App::click_layout(const char *id) {
    if (!id || !hwnd_) return;
    RECT r = layout_rect(id, client_lw(), client_lh());
    const int x = (r.left + r.right) / 2;
    const int y = (r.top + r.bottom) / 2;
    SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(scale(x), scale(y)));
}

void App::table_scroll_probe(void (*emit)(const char *line)) {
    if (!emit) return;
    TableSource *src = dup_mode_ ? static_cast<TableSource *>(dup_src_.get())
                                 : static_cast<TableSource *>(diff_src_.get());
    if (!src) {
        emit("scroll no-table");
        return;
    }
    const int frozen = table_.frozen_width();
    table_.scroll_to(0, 0);
    char a[160];
    std::snprintf(a, sizeof(a), "scroll start xoff=%d frozen=%d header0=%s", table_.x_offset(),
                  frozen, to_utf8(src->header(0)).c_str());
    emit(a);
    table_.scroll_to_end();
    char b[200];
    std::snprintf(b, sizeof(b), "scroll end   xoff=%d frozen=%d rows=%d cols=%d", table_.x_offset(),
                  table_.frozen_width(), src->rows(), src->cols());
    emit(b);
}

/* ── 状态与落点 ───────────────────────────────────────────────── */

void App::refresh_status() {
    const bool a = !slots_[0].path.empty();
    const bool b = !slots_[1].path.empty();
    const wchar_t *s;
    status_warn_ = false;
    if (!a && !b) {
        s = L"就绪 · 拖入或「浏览…」选择两份 .xlsx · 单份也可先放进来";
    } else if (!a || !b) {
        s = L"已选 1 份，还差另 1 份 · 拖到哪张卡片就落到哪一侧，拖到空白处按先空的那侧兜底";
    } else if (card_same_file(slots_[0].path, slots_[1].path)) {
        wchar_t buf[192];
        if (!done_note_.empty())
            std::swprintf(buf, 192, L"查重模式 · %ls", done_note_.c_str());
        else
            std::swprintf(buf, 192,
                          L"两侧是同一个文件 → 查重模式：%d 行重复 · 涉及 %d 个料号",
                          dup_src_ ? dup_src_->rows() : 0,
                          dup_src_ ? dup_src_->n_groups() : 0);
        status_warn_ = true;
        std::wcsncpy(status_, buf, 191);
        status_[191] = 0;
        return;
    } else {
        wchar_t buf[320];
        if (has_pairs_) {
            if (!done_note_.empty())
                std::swprintf(buf, 320, L"差异 %d 行 · %ls", n_res_, done_note_.c_str());
            else
                std::swprintf(buf, 320,
                              L"已配对 %d 对（料号 %d · 位号 %d · 近似 %d · 料号变更 %d）· 新增 %d · 删除 %d · 差异 %d 行",
                              pstat_.n_paired, pstat_.n_pn, pstat_.n_ref, pstat_.n_fuzzy,
                              pstat_.n_pnchg, pstat_.n_added, pstat_.n_deleted, n_res_);
            /* 双键关着时把「跳过了多少无料号行」摆到状态栏：否则用户只看见
               新增/删除变少，不知道少在哪 */
            if (pstat_.n_skipped > 0 && done_note_.empty()) {
                wchar_t tail[64];
                std::swprintf(tail, 64, L" · 跳过 %d 行(无料号)", pstat_.n_skipped);
                std::wcsncat(buf, tail, 319 - std::wcslen(buf));
            }
        } else if (!pair_note_.empty()) {
            std::swprintf(buf, 192, L"配对中止：%ls", pair_note_.c_str());
            status_warn_ = true;
            std::wcsncpy(status_, buf, 191);
            status_[191] = 0;
            return;
        } else {
            std::swprintf(buf, 192, L"两份已就位 · 旧 %d 行 / 新 %d 行 · 未找到可配对的行",
                          slots_[0].data_rows, slots_[1].data_rows);
        }
        std::wcsncpy(status_, buf, 191);
        status_[191] = 0;
        return;
    }
    std::wcsncpy(status_, s, 191);
    status_[191] = 0;
}

int App::hit_card(int lx, int ly) const {
    POINT p = {lx, ly};
    RECT r0 = layout_rect("card.old", client_lw(), client_lh());
    RECT r1 = layout_rect("card.new", client_lw(), client_lh());
    if (PtInRect(&r0, p)) return 0;
    if (PtInRect(&r1, p)) return 1;
    return -1;
}

int App::hit_browse(int lx, int ly) const {
    POINT p = {lx, ly};
    RECT b0 = layout_rect("old.browse", client_lw(), client_lh());
    RECT b1 = layout_rect("new.browse", client_lw(), client_lh());
    if (PtInRect(&b0, p)) return 0;
    if (PtInRect(&b1, p)) return 1;
    return -1;
}

int App::hit_sheet(int lx, int ly) const {
    POINT p = {lx, ly};
    RECT s0 = layout_rect("old.sheet", client_lw(), client_lh());
    RECT s1 = layout_rect("new.sheet", client_lw(), client_lh());
    if (slots_[0].ok && slots_[0].sheets.size() > 1 && PtInRect(&s0, p)) return 0;
    if (slots_[1].ok && slots_[1].sheets.size() > 1 && PtInRect(&s1, p)) return 1;
    return -1;
}

int App::drop_target(int lx, int ly) const {
    const int t = hit_card(lx, ly);
    if (t >= 0) return t;
    if (slots_[0].path.empty()) return 0;
    if (slots_[1].path.empty()) return 1;
    return 0;  // 两侧都有：覆盖旧侧，符合「再拖一份进来替换」的直觉
}

void App::note_slot(int idx) {
    wchar_t buf[768];
    const wchar_t *who = idx == 0 ? L"旧表" : L"新表";

    if (slots_[idx].ok) {
        const char *pn = slots_[idx].analysis ? bs_sheet_col_name(slots_[idx].analysis, 'P') : "";
        wchar_t wpn[16] = L"-";
        if (pn && *pn) from_utf8(pn, wpn, 16);
        /* 「数据行」按诊断页同一口径给（建档 + 无键 + 分组标题），
           括号里单列无键行数；slot.data_rows 保持「建档行」，控制流还在用它 */
        const int total_rows = slots_[idx].analysis
                                   ? bs_sheet_data_rows(slots_[idx].analysis)
                                   : slots_[idx].data_rows;
        std::swprintf(buf, 768,
                      L"载入%ls %ls · 表「%ls」· 数据行 %d（无键 %d）· 料号列 %ls · %lld ms",
                      who, slots_[idx].path.c_str(), slots_[idx].sheet.c_str(),
                      total_rows, slots_[idx].unkeyable, wpn, slots_[idx].ms);
    } else {
        std::swprintf(buf, 768, L"载入%ls %ls 失败：%ls", who, slots_[idx].path.c_str(),
                      slots_[idx].status.c_str());
    }
    append_log(buf);
}

void App::set_slot(int idx, const std::wstring &path) {
    if (idx < 0 || idx > 1) return;
    slots_[idx].path = path;
    card_load(slots_[idx]);
    note_slot(idx);
    auto_all_done_ = false;   /* 换了输入：要不要自动逐表，重新判断一次 */

    if (!batch_) recompute_pairs();
    refresh_status();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::load_pair(const std::wstring &old_path, const std::wstring &new_path) {
    /* 两张表互不依赖，各解析各的 workbook；core 里没有跨实例的可变状态
       （解压的定长树已改成就地建），所以可以真并行。
       10 万行时这一步从 ~0.9 s 降到 ~0.55 s，代价只是多两个线程。 */
    HANDLE th[2] = {nullptr, nullptr};

    slots_[0].path = old_path;
    slots_[1].path = new_path;
    auto_all_done_ = false;   /* 换了输入：要不要自动逐表，重新判断一次 */
    th[0] = CreateThread(nullptr, 0, &App::slot_load_proc, &slots_[0], 0, nullptr);
    th[1] = CreateThread(nullptr, 0, &App::slot_load_proc, &slots_[1], 0, nullptr);

    /* 万一线程没起来（内存紧张），当前线程把它干完，功能不受影响 */
    for (int i = 0; i < 2; i++)
        if (!th[i]) card_load(slots_[i]);
    for (int i = 0; i < 2; i++) {
        if (th[i]) {
            WaitForSingleObject(th[i], INFINITE);
            CloseHandle(th[i]);
        }
    }
    note_slot(0);
    note_slot(1);
}

DWORD WINAPI App::slot_load_proc(LPVOID p) {
    card_load(*static_cast<FileSlot *>(p));
    return 0;
}

void App::recompute_pairs() {
    has_pairs_ = false;
    n_res_ = 0;
    pair_note_.clear();
    done_note_.clear();
    std::memset(&pstat_, 0, sizeof(pstat_));
    bs_result_close(res_);
    res_ = nullptr;
    bs_pair_free(pairs_);
    pairs_ = nullptr;
    n_pairs_ = 0;

    const bool same = !slots_[0].path.empty() && !slots_[1].path.empty() &&
                      card_same_file(slots_[0].path, slots_[1].path);

    if (slots_[0].analysis && slots_[1].analysis && !same) {
        bs_err e;
        e.msg[0] = 0;
        if (bs_pair_rows(slots_[0].analysis, slots_[1].analysis, &cfg_, &pairs_, &n_pairs_,
                         &pstat_, &e) != 0) {
            pair_note_ = from_utf8(e.msg);
        } else {
            has_pairs_ = true;
            if (bs_result_build(slots_[0].analysis, slots_[1].analysis, pairs_, n_pairs_,
                                &res_, &e) == 0) {
                n_res_ = (int)bs_result_count(res_);
            } else {
                /* 失败不能继续假装“已配对、差异 0 行”——那是内存不足 */
                has_pairs_ = false;
                pair_note_ = from_utf8(e.msg);
            }
        }
    }

    make_table();

    /* 配对结果写进运行日志：数字与状态栏同源，评审时不用来回切页签 */
    if (has_pairs_) {
        wchar_t buf[320];
        std::swprintf(buf, 320,
                      L"配对 %d 对（料号 %d · 位号 %d · 近似 %d · 料号变更 %d）· "
                      L"新增 %d · 删除 %d · 差异 %d 行",
                      pstat_.n_paired, pstat_.n_pn, pstat_.n_ref, pstat_.n_fuzzy,
                      pstat_.n_pnchg, pstat_.n_added, pstat_.n_deleted, n_res_);
        append_log(buf);
        if (pstat_.n_skipped > 0) {
            std::swprintf(buf, 320,
                          L"跳过了 %d 行「无料号」的行（双键关闭）：这些行不参与比对；"
                          L"要让它们按位号配对，勾上「双键（位号兜底）」",
                          pstat_.n_skipped);
            append_log(buf);
        }
    } else if (!pair_note_.empty()) {
        append_log(L"配对中止：" + pair_note_);
    } else if (same && slots_[0].analysis) {
        wchar_t buf[192];
        std::swprintf(buf, 192, L"查重 %d 行重复 · 涉及 %d 个料号（%d 行有料号 / %d 行仅有位号）",
                      dup_src_ ? dup_src_->rows() : 0, dup_src_ ? dup_src_->n_groups() : 0,
                      dup_src_ ? dup_src_->n_pn_rows() : 0,
                      dup_src_ ? dup_src_->n_ref_rows() : 0);
        append_log(buf);
    }

    if (hwnd_) {
        layout_children();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    /* 放在最后：两侧都就绪才判断得出来，而且要在 make_table 之后（表格列集
       跟模式无关，但状态栏 / 按钮文字得跟上） */
    auto_all_sheets();
}

bool App::write_result(const std::wstring &path, std::wstring &out_path,
                       std::wstring &err) const {
    out_path.clear();
    err.clear();
    if (!res_ || !slots_[0].analysis || !slots_[1].analysis) {
        err = L"还没有可写出的比对结果";
        return false;
    }
    bs_diag dg[2];
    const std::string file_o = to_utf8(slots_[0].path);
    const std::string file_n = to_utf8(slots_[1].path);
    bs_diag_fill(&dg[0], slots_[0].analysis, file_o.c_str());
    bs_diag_fill(&dg[1], slots_[1].analysis, file_n.c_str());

    const std::string p = to_utf8(path);
    char real[1024] = "";
    bs_err e;
    e.msg[0] = 0;
    if (bs_write_result(p.c_str(), res_, dg, 2, real, sizeof(real), &e) != 0) {
        err = from_utf8(e.msg);
        return false;
    }
    out_path = from_utf8(real);
    return true;
}

/* 与 Python 的 os.path.splitext(os.path.basename(p))[0] 等价 */
static std::wstring stem_of(const std::wstring &path) {
    size_t sep = path.find_last_of(L"\\/");
    std::wstring base = (sep == std::wstring::npos) ? path : path.substr(sep + 1);
    size_t dot = base.find_last_of(L'.');
    return (dot == std::wstring::npos || dot == 0) ? base : base.substr(0, dot);
}

App::WriteAllResult App::write_all(const std::wstring &dir, WriteStepSink *sink,
                                      const WriteOpts &opt) const {
    WriteAllResult r;
    if (!res_ || !slots_[0].analysis || !slots_[1].analysis) {
        r.err = L"还没有可写出的比对结果";
        return r;
    }

    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    if (d.empty()) {
        r.err = L"输出目录为空";
        return r;
    }

    const std::wstring ob = stem_of(slots_[0].path);
    const std::wstring nb = stem_of(slots_[1].path);

    /* 逐表比对时 tag = "_<表名>"：同一次比对里每张表各一组，不能互相覆盖 */
    r.res = d + L"\\" + ob + L"_vs_" + nb + opt.res_tag + L"_差异结果.xlsx";
    r.old_mark = d + L"\\" + ob + L"_差异标注_旧" + opt.mark_tag + L".xlsx";
    r.new_mark = d + L"\\" + nb + L"_差异标注_新" + opt.mark_tag + L".xlsx";

    std::wstring got, err;
    if (opt.write_res) {
        if (sink) sink->on_writing(20, L"写差异结果");
        if (!write_result(r.res, got, err)) {
            r.err = err;
            return r;
        }
        r.res = got;
    } else {
        r.res.clear();
    }

    if (!opt.write_marks) {
        r.ok = true;
        return r;
    }

    bs_err e;
    e.msg[0] = 0;
    char real[1024] = "";
    /* 源文件默认是原始表；逐表累积时传上一轮的产物，前面涂好的页才不会丢 */
    const std::string src_o = to_utf8(opt.src_old.empty() ? slots_[0].path : opt.src_old);
    const std::string src_n = to_utf8(opt.src_new.empty() ? slots_[1].path : opt.src_new);

    if (sink) sink->on_writing(60, L"标注旧表");
    if (bs_mark_workbook(slots_[0].analysis, slots_[1].analysis, src_o.c_str(),
                         pairs_, n_pairs_, BS_SIDE_OLD, to_utf8(r.old_mark).c_str(),
                         real, sizeof(real), &e) != 0) {
        r.err = L"旧表标注失败：" + from_utf8(e.msg);
        return r;
    }
    r.old_mark = from_utf8(real);

    e.msg[0] = 0;
    if (sink) sink->on_writing(85, L"标注新表");
    if (bs_mark_workbook(slots_[1].analysis, slots_[0].analysis, src_n.c_str(),
                         pairs_, n_pairs_, BS_SIDE_NEW, to_utf8(r.new_mark).c_str(),
                         real, sizeof(real), &e) != 0) {
        r.err = L"新表标注失败：" + from_utf8(e.msg);
        return r;
    }
    r.new_mark = from_utf8(real);
    if (sink) sink->on_writing(100, L"写完了");
    r.ok = true;
    return r;
}

namespace {

/* 工作表名进文件名：\ / : * ? " < > | 与控制字符统一换成下划线 */
std::wstring file_safe(const std::wstring &s) {
    std::wstring out = s;
    for (wchar_t &c : out) {
        if (c < 32 || std::wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out.empty() ? std::wstring(L"sheet") : out;
}

}  // namespace

bool App::run_compare_all(const std::wstring &dir, WriteStepSink *sink) {
    if (!slots_[0].ok || !slots_[1].ok) return false;

    /* 同名表配对：只比两侧都有的表，顺序按旧表。名字对不上就不比 ——
       硬按序号配对会把「Main ↔ FPC-KEY板」凑成一对，出来全是假差异。 */
    struct Job {
        std::wstring name;
        int io;
        int in;
    };
    std::vector<Job> jobs;
    for (size_t i = 0; i < slots_[0].sheets.size(); ++i) {
        for (size_t j = 0; j < slots_[1].sheets.size(); ++j) {
            if (_wcsicmp(slots_[0].sheets[i].c_str(), slots_[1].sheets[j].c_str()) == 0) {
                Job jb;
                jb.name = slots_[0].sheets[i];
                jb.io = (int)i;
                jb.in = (int)j;
                jobs.push_back(jb);
                break;
            }
        }
    }
    if (jobs.empty()) {
        append_log(L"两侧没有同名工作表，逐表比对做不了（请改回单表比对）");
        return false;
    }

    const int keep_o = slots_[0].sheet_index;
    const int keep_n = slots_[1].sheet_index;
    const int total = (int)jobs.size();
    int done = 0, skipped = 0;

    /* 标注要「一份文件装下所有表」：每轮以上一轮的**产物**为源接着涂。
       每轮都从原始文件重新复制的话，前面涂好的页会全部退回原始状态 ——
       用户 2026-11-05 报的「只剩最后一次比对的工作表有颜色」就是这个。
       中间轮写临时名 .partN，全部比完再把最后一份改名成正式名。 */
    const std::wstring base_o = stem_of(slots_[0].path);
    const std::wstring base_n = stem_of(slots_[1].path);
    std::wstring acc_o = slots_[0].path;   // 第一轮从原始旧表起
    std::wstring acc_n = slots_[1].path;
    std::wstring last_o, last_n;           // 最后一轮的累积文件
    std::vector<std::wstring> parts;       // 中间产物，收尾要清掉

    for (int k = 0; k < total; ++k) {
        const Job &jb = jobs[(size_t)k];
        set_progress(5 + 90 * k / total, (L"逐表比对：" + jb.name).c_str());
        card_pick_sheet(slots_[0], jb.io);
        card_pick_sheet(slots_[1], jb.in);
        recompute_pairs();

        if (slots_[0].data_rows == 0 && slots_[1].data_rows == 0) {
            append_log(L"跳过「" + jb.name + L"」：两侧都没有数据行");
            ++skipped;
            continue;
        }

        WriteOpts o;
        o.res_tag = L"_" + file_safe(jb.name);
        o.mark_tag = L".part" + std::to_wstring(k);
        o.src_old = acc_o;
        o.src_new = acc_n;
        const WriteAllResult wa = write_all(dir, sink, o);
        if (!wa.ok) {
            append_log(L"「" + jb.name + L"」写出失败：" + wa.err);
            /* 本轮可能已经落了结果表或 .partK 标注：全部清掉，别留孤儿 */
            if (!wa.res.empty()) DeleteFileW(wa.res.c_str());
            if (!wa.old_mark.empty()) DeleteFileW(wa.old_mark.c_str());
            if (!wa.new_mark.empty()) DeleteFileW(wa.new_mark.c_str());
            continue;
        }
        acc_o = wa.old_mark;               // 下一轮以它为源
        acc_n = wa.new_mark;
        last_o = wa.old_mark;
        last_n = wa.new_mark;
        parts.push_back(wa.old_mark);
        parts.push_back(wa.new_mark);
        ++done;
        append_log(L"「" + jb.name + L"」差异 " + std::to_wstring(n_res_) + L" 行 · 已并入标注");
    }

    /* 收尾：把最后一份累积文件改名成正式名（不带表名后缀），其余临时件删掉。
       目标被 Excel 打开时 MoveFileEx 会失败 —— 那就留着临时名并写清楚，别假装成功。
       两个文件分开记成败：以前共用一个 renamed，旧表改名成功、新表失败时会把
       final_o 重置成一份已经被移走的路径，日志与收尾说明全指向不存在的文件。 */
    std::wstring final_o, final_n;
    if (done > 0) {
        final_o = dir + L"\\" + base_o + L"_差异标注_旧.xlsx";
        final_n = dir + L"\\" + base_n + L"_差异标注_新.xlsx";
        const bool ok_o =
            MoveFileExW(last_o.c_str(), final_o.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        const bool ok_n =
            MoveFileExW(last_n.c_str(), final_n.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok_o) {
            final_o = last_o;
            append_log(L"旧表标注改名失败（目标可能正被占用），保留临时名：" + last_o);
        }
        if (!ok_n) {
            final_n = last_n;
            append_log(L"新表标注改名失败（目标可能正被占用），保留临时名：" + last_n);
        }
    }
    for (const std::wstring &p : parts) {
        if (_wcsicmp(p.c_str(), final_o.c_str()) != 0 && _wcsicmp(p.c_str(), final_n.c_str()) != 0) {
            DeleteFileW(p.c_str());
        }
    }

    /* 比完把原来的选中项放回去：界面状态别莫名其妙停在最后一张表 */
    if (keep_o >= 0 && keep_o < (int)slots_[0].sheets.size()) card_pick_sheet(slots_[0], keep_o);
    if (keep_n >= 0 && keep_n < (int)slots_[1].sheets.size()) card_pick_sheet(slots_[1], keep_n);
    recompute_pairs();

    wchar_t buf[256];
    std::swprintf(buf, 256, L"逐表比对：同名表 %d 张 · 写出 %d 张 · 跳过 %d 张", total, done, skipped);
    append_log(buf);
    if (done > 0) {
        append_log(L"标注：旧表合并 " + std::to_wstring(done) + L" 张表 → " + final_o);
        append_log(L"标注：新表合并 " + std::to_wstring(done) + L" 张表 → " + final_n);
    }
    return done > 0;
}

void App::set_all_sheets(bool on) {
    all_sheets_ = on;
    slots_[0].all_sheets = on;
    slots_[1].all_sheets = on;
    /* 按钮文字跟着模式走。make_table() 里虽然也设了一次，但切模式不重建表，
       不在这里补一下就会出现「已经自动进逐表了、按钮还写着开始比对」
       —— 用户更看不出来现在的模式了 */
    if (!dup_mode_) {
        std::wcsncpy(run_label_, all_sheets_ ? L"逐表比对" : L"开始比对", 23);
        run_label_[23] = 0;
    }
    refresh_status();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void App::auto_all_sheets() {
    if (auto_all_done_ || all_sheets_ || dup_mode_ || batch_) return;
    if (!slots_[0].ok || !slots_[1].ok) return;
    if (slots_[0].sheets.size() < 2 || slots_[1].sheets.size() < 2) return;

    /* 一次载入只判断一次：之后用户在下拉里怎么选，都不再自动插手 */
    auto_all_done_ = true;

    int same = 0;
    for (const std::wstring &a : slots_[0].sheets) {
        for (const std::wstring &b : slots_[1].sheets) {
            if (_wcsicmp(a.c_str(), b.c_str()) == 0) {
                ++same;
                break;
            }
        }
    }
    if (same < 2) return;

    set_all_sheets(true);
    append_log(L"两侧各有多张工作表（同名 " + std::to_wstring(same) +
               L" 张），已自动切到逐表比对；只想比一张可在工作表下拉里选");
}

void App::browse(int idx) {
    wchar_t file[MAX_PATH * 2] = L"";
    if (!slots_[idx].path.empty()) std::wcsncpy(file, slots_[idx].path.c_str(), MAX_PATH * 2 - 1);

    OPENFILENAMEW ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Excel 工作簿 (*.xlsx)\0*.xlsx\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"选择 BOM 文件（.xlsx）";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;  // 取消
    set_slot(idx, file);
}

/* ── 拖放 ─────────────────────────────────────────────────────── */

void App::on_drag_over(POINT pt, int file_count) {
    if (!hwnd_) return;
    const int lx = to_logical(pt.x), ly = to_logical(pt.y);
    bool want[2] = {false, false};
    if (file_count >= 2) {
        want[0] = want[1] = true;  // 一次拖两份：两侧都要收
    } else if (file_count == 1) {
        want[drop_target(lx, ly)] = true;
    }

    bool changed = false;
    for (int i = 0; i < 2; ++i) {
        if (slots_[i].drop_hot != want[i]) {
            slots_[i].drop_hot = want[i];
            changed = true;
        }
    }
    if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::on_drag_leave() {
    bool changed = false;
    for (int i = 0; i < 2; ++i) {
        if (slots_[i].drop_hot) {
            slots_[i].drop_hot = false;
            changed = true;
        }
    }
    if (changed && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::on_drop(const std::vector<std::wstring> &files, POINT client_pt) {
    if (files.empty()) return;
    on_drag_leave();

    /* 两份一起拖：按资源管理器的显示顺序给「旧 / 新」，与落点无关。
       两份都落好再算一次配对，免得日志里出现「只配了一半」的中间态。 */
    if (files.size() >= 2) {
        batch_ = 1;
        load_pair(files[0], files[1]);
        batch_ = 0;
        recompute_pairs();
        refresh_status();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const int lx = to_logical(client_pt.x), ly = to_logical(client_pt.y);
    set_slot(drop_target(lx, ly), files[0]);
}

void App::apply_drop(const std::vector<std::wstring> &files, POINT client_pt) {
    on_drop(files, client_pt);
}

void App::drag_over_test() {
    /* 悬停态没法用脚本演一遍真实的资源管理器拖拽，就单独给个入口：
       走的是 OLE 回调同一条 on_drag_over() */
    const RECT c = layout_rect("card.old", client_lw(), client_lh());
    POINT p = {scale((c.left + c.right) / 2), scale((c.top + c.bottom) / 2)};
    on_drag_over(p, 1);
}

void App::drop_test(const std::vector<std::wstring> &files) {
    /* 落在旧卡片中心：验证「按落点选卡片」这条路径 */
    const RECT c = layout_rect("card.old", client_lw(), client_lh());
    POINT p = {scale((c.left + c.right) / 2), scale((c.top + c.bottom) / 2)};
    apply_drop(files, p);
}

/* ── DPI / 消息 ───────────────────────────────────────────────── */

void App::apply_dpi(int dpi) {
    if (dpi <= 0 || dpi == dpi_) return;
    dpi_ = dpi;
    make_fonts();
    /* 字体对象换了，子控件还拿着旧句柄：重新下发一次，否则跨屏后日志页
       用的是已经被 DeleteObject 掉的 HFONT */
    if (log_hwnd_)
        SendMessageW(log_hwnd_, WM_SETFONT, (WPARAM)(f_mono_ ? f_mono_ : f_small_), TRUE);
    if (prog_)
        SendMessageW(prog_, WM_SETFONT, (WPARAM)f_small_, TRUE);
}

LRESULT App::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED: {
            close_sheet_menu();
            apply_dpi((int)LOWORD(wp));
            table_.set_dpi(dpi_);
            layout_children();
            const RECT *r = reinterpret_cast<const RECT *>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_SIZE:
            layout_children();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);
            RECT r = {0, 0, scale(kMinLogicalW), scale(kMinLogicalH)};
            window_rect_for_client(scale(kMinLogicalW), scale(kMinLogicalH), WS_OVERLAPPEDWINDOW,
                                   0, dpi_, &r);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int lx = to_logical(GET_X_LPARAM(lp)), ly = to_logical(GET_Y_LPARAM(lp));
            const int hb = hit_browse(lx, ly);
            if (hb != hot_browse_) {
                hot_browse_ = hb;
                slots_[0].hot_browse = (hb == 0);
                slots_[1].hot_browse = (hb == 1);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            /* 拖放悬停态下光标要回到箭头，别让手型留在卡上 */
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p = {0, 0};
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                const int hb = hit_browse(to_logical(p.x), to_logical(p.y));
                const int hs = hit_sheet(to_logical(p.x), to_logical(p.y));
                const int ha = hit_action(to_logical(p.x), to_logical(p.y));
                const int ho = hit_options(to_logical(p.x), to_logical(p.y));
                SetCursor(LoadCursorW(nullptr, (hb >= 0 || hs >= 0 || ha > 0 || ho > 0)
                                                   ? IDC_HAND
                                                   : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEWHEEL: {
            /* 滚轮只在近似阈值框上生效：上下 0.05 微调，焦点在哪都行 */
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            if (hit_options(to_logical(p.x), to_logical(p.y)) == 2) {
                nudge_threshold(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            const int lx = to_logical(GET_X_LPARAM(lp)), ly = to_logical(GET_Y_LPARAM(lp));
            /* ③ 选项区：点复选框开关双键，点阈值框换档（都立刻重算） */
            switch (hit_options(lx, ly)) {
                case 1:
                    set_dual_key(cfg_.dual_key == 0);
                    return 0;
                case 2:
                    click_threshold();
                    return 0;
                case 3:
                    set_row_diff_all(cfg_.row_diff_all == 0);
                    return 0;
                default:
                    break;
            }
            const int hs = hit_sheet(lx, ly);
            if (hs >= 0) {
                open_sheet_menu(hs);
                return 0;
            }
            const int hb = hit_browse(lx, ly);
            if (hb >= 0) {
                browse(hb);
                return 0;
            }
            switch (hit_action(lx, ly)) {
                case 1:  // 开始比对 / 开始查重
                    run_compare();
                    return 0;
                case 2:  // 另存结果：选目录再写一遍（查重模式只写那一份标注）
                    if (!can_run()) return 0;
                    {
                        std::wstring dir, err;
                        if (!resolve_out_dir(dir, err)) {
                            append_log(L"另存取消：" + err);
                            return 0;
                        }
                        if (dup_mode_) {
                            /* 查重没有差异结果表，write_all 里会直接报「没有可写出的比对结果」 */
                            const bool ok = write_dups(dir);
                            append_log(ok ? (L"查重标注另存到 " + dir)
                                          : std::wstring(L"另存失败：查重标注没有写出"));
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        const WriteAllResult wa = write_all(dir, nullptr);
                        append_log(wa.ok ? (L"另存到 " + dir) : (L"另存失败：" + wa.err));
                        if (wa.ok) {
                            done_note_ = L"已另存到 " + dir;
                            refresh_status();
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                    }
                    return 0;
                case 4:
                    switch_tab(0);
                    return 0;
                case 5:
                    switch_tab(1);
                    return 0;
                case 6:
                    open_help();
                    return 0;
                default:
                    break;
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            return 0;
        case WM_DROPFILES: {  // OLE 注册失败时的兜底路径
            HDROP hd = reinterpret_cast<HDROP>(wp);
            POINT p = {0, 0};
            DragQueryPoint(hd, &p);
            std::vector<std::wstring> files;
            const UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                const UINT len = DragQueryFileW(hd, i, nullptr, 0);
                if (!len) continue;
                std::wstring s(len, L'\0');
                DragQueryFileW(hd, i, &s[0], len + 1);
                s.resize(len);
                files.push_back(s);
            }
            DragFinish(hd);
            if (!files.empty()) on_drop(files, p);
            return 0;
        }
        case WM_DESTROY:
            /* 拖放必须在窗口还活着、OLE 还没反初始化时撤销：
               放到 ~App 里做，那时窗口已销毁、OleUninitialize 已调用，
               RevokeDragDrop 必失败，RegisterDragDrop 加的引用就漏了 */
            drop_revoke(hwnd_);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK App::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *self = nullptr;
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        self = static_cast<App *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::report_metrics(void (*emit)(const char *line)) const {
    if (!emit) return;
    RECT rc = {0, 0, 0, 0};
    RECT wr = {0, 0, 0, 0};
    if (hwnd_) {
        GetClientRect(hwnd_, &rc);
        GetWindowRect(hwnd_, &wr);
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "version=%s", bs_version());
    emit(buf);
    std::snprintf(buf, sizeof(buf), "build=%s", bs_build_stamp());
    emit(buf);
    std::snprintf(buf, sizeof(buf), "dpi=%d", dpi_);
    emit(buf);
    std::snprintf(buf, sizeof(buf), "scale=%d", dpi_ * 100 / 96);
    emit(buf);
    std::snprintf(buf, sizeof(buf), "awareness=%s", awareness_name());
    emit(buf);
    std::snprintf(buf, sizeof(buf), "client=%ldx%ld", (long)(rc.right - rc.left),
                  (long)(rc.bottom - rc.top));
    emit(buf);
    std::snprintf(buf, sizeof(buf), "window=%ldx%ld", (long)(wr.right - wr.left),
                  (long)(wr.bottom - wr.top));
    emit(buf);
    std::snprintf(buf, sizeof(buf), "dragdrop=%s", ole_ok_ ? "ole" : "dropfiles");
    emit(buf);
    std::snprintf(buf, sizeof(buf), "layout_items=%d", layout_count());
    emit(buf);
}

void App::report_drop_state(void (*emit)(const char *line)) const {
    if (!emit) return;
    char buf[512];
    for (int i = 0; i < 2; ++i) {
        const std::wstring &w = slots_[i].path;
        char narrow[MAX_PATH * 3] = "";
        if (!w.empty())
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
        std::snprintf(buf, sizeof(buf),
                      "slot%d ok=%d sheets=%d sheetidx=%d rows=%d cols=%d cells=%d strike=%d "
                      "merges=%d ms=%d hdr=%d blocks=%d drows=%d unkey=%d hit=%.3f pn=%s path=%s",
                      i, slots_[i].ok ? 1 : 0, (int)slots_[i].sheets.size(),
                      slots_[i].sheet_index, slots_[i].rows, slots_[i].cols,
                      slots_[i].n_cells, slots_[i].n_strike, slots_[i].n_merges,
                      (int)slots_[i].ms, slots_[i].header_row, slots_[i].n_blocks,
                      slots_[i].data_rows, slots_[i].unkeyable, slots_[i].hit_rate,
                      slots_[i].analysis ? bs_sheet_col_name(slots_[i].analysis, 'P') : "",
                      narrow);
        emit(buf);
        {
            std::string names;
            for (size_t k = 0; k < slots_[i].sheets.size(); ++k) {
                if (k) names += "|";
                names += to_utf8(slots_[i].sheets[k]);
            }
            std::snprintf(buf, sizeof(buf), "slot%d.sheets=%s", i, names.c_str());
            emit(buf);
        }
    }
    std::snprintf(buf, sizeof(buf), "same=%d",
                  card_same_file(slots_[0].path, slots_[1].path) ? 1 : 0);
    emit(buf);
        std::snprintf(buf, sizeof(buf),
                      "pair ok=%d pairs=%d pn=%d ref=%d fuzzy=%d pnchg=%d add=%d del=%d "
                      "chg=%d res=%d skip=%d note=%s",
                      has_pairs_ ? 1 : 0, pstat_.n_paired, pstat_.n_pn, pstat_.n_ref,
                      pstat_.n_fuzzy, pstat_.n_pnchg, pstat_.n_added, pstat_.n_deleted,
                      pstat_.n_changed, n_res_, pstat_.n_skipped,
                      to_utf8(pair_note_).c_str());
    emit(buf);
    std::snprintf(buf, sizeof(buf), "layout_items=%d", layout_count());
    emit(buf);
    std::snprintf(buf, sizeof(buf), "menu=%d hover=%d err=%d", menu_ ? 1 : 0, menu_hover_,
                  menu_err_);
    emit(buf);
}

/* ── 工作表下拉 ──────────────────────────────────────────────── */

int App::menu_item_at(int y) const {
    if (menu_slot_ < 0 || menu_slot_ > 1) return -1;
    const int pad = scale(4), ih = scale(24);
    if (ih <= 0 || y < pad) return -1;
    const int i = (y - pad) / ih;
    /* 第 0 行是「全部工作表（逐表比对）」，之后才是各张表，共 n+1 行 */
    if (i < 0 || i > (int)slots_[menu_slot_].sheets.size()) return -1;
    return i;
}

void App::close_sheet_menu() {
    if (menu_) {
        HWND h = menu_;
        menu_ = nullptr;  // 先清，DestroyWindow 会回调 WM_CAPTURECHANGED
        menu_closing_ = true;
        if (GetCapture() == h) ReleaseCapture();
        DestroyWindow(h);
        menu_closing_ = false;
    }
    if (menu_slot_ >= 0 && menu_slot_ <= 1) slots_[menu_slot_].menu_open = false;
    menu_slot_ = -1;
    menu_hover_ = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void App::pick_sheet(int slot, int item) {
    if (slot < 0 || slot > 1) return;
    FileSlot &s = slots_[slot];
    if (item < 0 || item > (int)s.sheets.size()) return;

    /* 第 0 项：逐表比对。它不是某张表，是一个模式开关 */
    if (item == 0) {
        if (!all_sheets_) {
            set_all_sheets(true);
            append_log(L"工作表选择：全部工作表（逐表比对）");
            done_note_.clear();
        }
        return;
    }
    if (all_sheets_) set_all_sheets(false);
    auto_all_done_ = true;   /* 用户显式选了某张表：别再自动开逐表 */

    const int idx = item - 1;
    if (idx == s.sheet_index) return;
    if (!card_pick_sheet(s, idx)) {
        /* 这张表读不出来：照样重算，把上一张表的结果清掉，不能留着旧结果冒充 */
        append_log(L"切换工作表失败：" + s.sheet + L" · " + s.status);
    }

    /* 换表 ＝ 换了输入：配对、结果表、诊断、标注全部得按新表重算。
       原来这里只刷状态不重算，于是「差异结果」还是上一张表的配对，
       而诊断页与两份标注用的是新表 —— 用户看到的就是差异表有 8 行、
       标注文件里一个色块都没有，而且错得悄无声息。
       批量模式（--drop-test 之类连点）仍由调用方统一重算，见 set_slot。 */
    if (!batch_) recompute_pairs();
    done_note_.clear();          /* 上一份「已写出 3 份」不再对应当前输入 */
    status_warn_ = !s.ok;
    refresh_status();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void App::open_sheet_menu(int idx) {
    if (idx < 0 || idx > 1) return;
    FileSlot &s = slots_[idx];
    if (!s.ok || s.sheets.size() < 2) return;
    close_sheet_menu();

    const RECT combo = layout_rect(idx == 0 ? "old.sheet" : "new.sheet", client_lw(), client_lh());
    POINT tl = {scale(combo.left), scale(combo.top)};
    ClientToScreen(hwnd_, &tl);

    const int pad = scale(4);
    const int ih = scale(24);
    int w = scale(combo.right - combo.left);
    if (w < scale(180)) w = scale(180);
    int h = ih * ((int)s.sheets.size() + 1) + pad * 2;   /* +1：第一行「全部工作表」 */
    int x = tl.x;
    int y = tl.y + scale(combo.bottom - combo.top) + scale(2);

    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (y + h > mi.rcWork.bottom) y = tl.y - h - scale(2);  // 下面放不下就往上弹
        if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }

    menu_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kMenuClass, L"", WS_POPUP, x, y, w, h,
                            hwnd_, nullptr, inst_, this);
    if (!menu_) {
        menu_err_ = (int)GetLastError();
        return;
    }
    menu_slot_ = idx;
    menu_hover_ = all_sheets_ ? 0 : s.sheet_index + 1;
    s.menu_open = true;
    SetWindowPos(menu_, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    /* 不用 SetFocus 收菜单：窗口不是前台时系统会立刻把焦点抢回去，
       菜单刚弹就收。改成抓鼠标，点外面（包括别的程序）由 WM_CAPTURECHANGED 兜底。 */
    SetCapture(menu_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK App::menu_proc_entry(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto *self = reinterpret_cast<App *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->menu_proc(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT App::menu_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_MOUSEMOVE: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc;
            GetClientRect(h, &rc);
            const int it = PtInRect(&rc, pt) ? menu_item_at(pt.y) : -1;
            if (it != menu_hover_) {
                menu_hover_ = it;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc;
            GetClientRect(h, &rc);
            if (!PtInRect(&rc, pt)) {
                close_sheet_menu();  // 点菜单外面：只收起，不穿透
                return 0;
            }
            menu_press_ = menu_item_at(pt.y);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc;
            GetClientRect(h, &rc);
            if (PtInRect(&rc, pt)) {
                const int it = menu_item_at(pt.y);
                if (it >= 0 && it == menu_press_) pick_sheet(menu_slot_, it);
            }
            close_sheet_menu();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (!menu_closing_) close_sheet_menu();
            return 0;
        case WM_KEYDOWN: {
            const int n = (menu_slot_ >= 0)
                              ? (int)slots_[menu_slot_].sheets.size() + 1   /* 含「全部工作表」 */
                              : 0;
            if (wp == VK_ESCAPE) {
                close_sheet_menu();
                return 0;
            }
            if (wp == VK_UP) {
                if (menu_hover_ > 0) --menu_hover_;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_DOWN) {
                if (menu_hover_ + 1 < n) ++menu_hover_;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_RETURN) {
                if (menu_hover_ >= 0) pick_sheet(menu_slot_, menu_hover_);
                close_sheet_menu();
                return 0;
            }
            return 0;
        }
        case WM_KILLFOCUS:
            close_sheet_menu();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            UiCtx ui = make_ctx(dc);
            const int pad = scale(4);
            const int ih = scale(24);

            fill_rect(dc, rc, col(kColCard));
            if (menu_slot_ >= 0 && menu_slot_ <= 1) {
                FileSlot &s = slots_[menu_slot_];
                const int rows = (int)s.sheets.size() + 1;   /* 第 0 行「全部工作表」 */
                for (int i = 0; i < rows; ++i) {
                    RECT r = {pad, pad + i * ih, rc.right - pad, pad + (i + 1) * ih};
                    const bool all_row = (i == 0);
                    const bool cur = all_row ? all_sheets_ : (i - 1 == s.sheet_index);
                    if (i == menu_hover_) fill_round(dc, r, scale(3), col(kColAccentBg));
                    RECT tr = r;
                    tr.left += scale(8);
                    tr.right -= scale(6);
                    draw_text(dc, ui.small, col(cur ? kColAccentDk : kColText), tr,
                              all_row ? L"全部工作表（逐表比对）" : s.sheets[(size_t)(i - 1)].c_str(),
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (cur) {
                        RECT mr = r;
                        mr.right -= scale(8);
                        draw_text(dc, ui.small, col(kColAccent), mr, L"\u5f53\u524d",
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    }
                    if (all_row) {   /* 和下面的具体表分开一条细线 */
                        RECT ln = {r.left + scale(6), r.bottom - 1, r.right - scale(6), r.bottom};
                        fill_rect(dc, ln, col(kColGrid));
                    }
                }
            }
            HBRUSH fb = CreateSolidBrush(col(kColBorder));
            FrameRect(dc, &rc, fb);
            DeleteObject(fb);
            EndPaint(h, &ps);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void App::set_client_size(int lw, int lh) {
    if (!hwnd_ || lw <= 0 || lh <= 0) return;
    RECT r = {0, 0, scale(lw), scale(lh)};
    window_rect_for_client(scale(lw), scale(lh), WS_OVERLAPPEDWINDOW, 0, dpi_, &r);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void App::dump_layout(void (*emit)(const char *line)) const {
    if (!emit) return;
    char buf[160];
    const int lw = client_lw(), lh = client_lh();
    for (int i = 0; i < layout_count(); ++i) {
        const LayoutItem *it = layout_at(i);
        const RECT r = layout_rect(it, lw, lh);
        std::snprintf(buf, sizeof(buf), "%s\t%d\t%d\t%d\t%d", it->id, (int)r.left, (int)r.top,
                      (int)(r.right - r.left), (int)(r.bottom - r.top));
        emit(buf);
    }
}

}  // namespace bs
