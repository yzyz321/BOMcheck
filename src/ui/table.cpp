#include "table.h"

#include <algorithm>
#include <cwchar>

#include "layout_gen.h"
#include "paint.h"

namespace bs {
namespace {

constexpr wchar_t kClass[] = L"BOMCheckTable";

HFONT make_font(int px, bool bold, int dpi) {
    return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

}  // namespace

bool Table::create(HWND parent, HINSTANCE inst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Table::proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 全部自绘
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_HSCROLL | WS_VSCROLL,
                            0, 0, 10, 10, parent, nullptr, inst, this);
    if (!hwnd_) return false;
    dpi_ = 96;
    make_fonts();
    metrics();
    return true;
}

void Table::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    drop_fonts();
}

void Table::make_fonts() {
    drop_fonts();
    f_head_ = make_font(13, true, dpi_);
    f_cell_ = make_font(13, false, dpi_);
}

void Table::drop_fonts() {
    if (f_head_) DeleteObject(f_head_);
    if (f_cell_) DeleteObject(f_cell_);
    f_head_ = f_cell_ = nullptr;
}

void Table::set_dpi(int dpi) {
    if (dpi <= 0) dpi = 96;
    if (dpi == dpi_) return;
    dpi_ = dpi;
    make_fonts();
    xoff_ = 0;
    metrics();
    update_scrollbars();
    invalidate();
}

void Table::set_source(TableSource *src) {
    src_ = src;
    top_ = 0;
    xoff_ = 0;
    metrics();
    update_scrollbars();
    invalidate();
}

void Table::set_empty_text(const wchar_t *t) {
    std::wcsncpy(empty_, t ? t : L"", 95);
    empty_[95] = 0;
    invalidate();
}

void Table::set_visible(bool v) {
    if (hwnd_) ShowWindow(hwnd_, v ? SW_SHOW : SW_HIDE);
}

void Table::invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Table::set_logical_rect(int x, int y, int w, int h) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr, sc(x), sc(y), sc(w), sc(h), SWP_NOZORDER | SWP_NOACTIVATE);
    update_scrollbars();
}

/* 冻结列宽按「表头 + 前 80 行取样」实测，夹在 96–240 逻辑像素之间：
   只量表头会把料号截断（料号比表头长得多），全量测量又会在大表上白花时间。 */
void Table::metrics() {
    hh_ = sc(26);
    rh_ = sc(24);
    int want = 0;
    if (hwnd_ && src_ && src_->cols() > 0) {
        HDC dc = GetDC(hwnd_);
        if (dc) {
            /* text_w 自己会 SelectObject 并还原，这里不要再手动选字体：
               选了不还原，drop_fonts 的 DeleteObject 会因为「仍被 DC 选中」失败 */
            want = text_w(dc, f_head_, src_->header(0));
            const int n = std::min(src_->rows(), 80);
            for (int i = 0; i < n; ++i)
                want = std::max(want, text_w(dc, f_cell_, src_->cell(i, 0)));
            ReleaseDC(hwnd_, dc);
        }
        want += sc(16);
        want = std::max(sc(96), std::min(sc(240), want));
    }
    frozen_px_ = want;
}

int Table::frozen_width() const { return hwnd_ ? MulDiv(frozen_px_, 96, dpi_) : 0; }

int Table::body_h() const {
    RECT rc = {0, 0, 0, 0};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    return std::max(0, (int)(rc.bottom - hh_));
}

int Table::visible_rows() const {
    const int n = rh_ > 0 ? body_h() / rh_ : 0;
    return n > 0 ? n : 1;
}

int Table::scroll_w() const {
    RECT rc = {0, 0, 0, 0};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    return std::max(0, (int)(rc.right - frozen_px_));
}

int Table::total_w() const {
    if (!src_) return 0;
    int w = 0;
    for (int c = 1; c < src_->cols(); ++c) w += sc(src_->width(c));
    return w;
}

void Table::update_scrollbars() {
    if (!hwnd_) return;
    const int n = src_ ? src_->rows() : 0;
    const int vis = visible_rows();

    SCROLLINFO si = {sizeof(si)};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = n > 0 ? n - 1 : 0;
    si.nPage = (UINT)vis;
    if (top_ > std::max(0, n - vis)) top_ = std::max(0, n - vis);
    if (top_ < 0) top_ = 0;
    si.nPos = top_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);

    SCROLLINFO sh = {sizeof(sh)};
    sh.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    sh.nMin = 0;
    sh.nMax = std::max(0, total_w());
    sh.nPage = (UINT)std::max(1, scroll_w());
    const int maxoff = std::max(0, total_w() - scroll_w());
    if (xoff_ > maxoff) xoff_ = maxoff;
    if (xoff_ < 0) xoff_ = 0;
    sh.nPos = xoff_;
    SetScrollInfo(hwnd_, SB_HORZ, &sh, TRUE);
}

void Table::scroll_to(int top, int xoff) {
    const int n = src_ ? src_->rows() : 0;
    const int max_top = std::max(0, n - visible_rows());
    top_ = std::max(0, std::min(std::max(0, n - 1), top));
    if (top_ > max_top) top_ = max_top;
    xoff_ = std::max(0, std::min(std::max(0, total_w() - scroll_w()), xoff));
    update_scrollbars();
    invalidate();
}

void Table::scroll_to_end() {
    scroll_to(top_, std::max(0, total_w() - scroll_w()));
}

void Table::paint() {
    if (!hwnd_) return;
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc = {0, 0, 0, 0};
    GetClientRect(hwnd_, &rc);

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ old_bmp = SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);

    fill_rect(mem, rc, col(kColCard));

    const int n = src_ ? src_->rows() : 0;
    const int ncol = src_ ? src_->cols() : 0;
    const int vis = visible_rows();
    const int pad = sc(6);
    const int bottom = rc.bottom;

    /* 表头：整条先涂主色，再按横向偏移画各列，最后补冻结列 */
    RECT hdr = {0, 0, rc.right, hh_};
    fill_rect(mem, hdr, col(kColAccent));
    SetBkMode(mem, TRANSPARENT);

    IntersectClipRect(mem, frozen_px_, 0, rc.right, bottom);
    {
        int x = frozen_px_ - xoff_;
        for (int c = 1; c < ncol; ++c) {
            const int w = sc(src_->width(c));
            if (x + w > frozen_px_ && x < rc.right) {
                RECT r = {x + pad, 0, x + w - pad, hh_};
                draw_text(mem, f_head_, RGB(0xFF, 0xFF, 0xFF), r, src_->header(c),
                          DT_LEFT | DT_END_ELLIPSIS);
            }
            x += w;
        }
    }

    /* 数据行（右 11 列） */
    for (int i = top_; i < n && i < top_ + vis; ++i) {
        const int y = hh_ + (i - top_) * rh_;
        if (y + rh_ > bottom) break;
        const unsigned bg = src_->row_bg(i);
        if (bg) {
            RECT rr = {frozen_px_, y, rc.right, y + rh_};
            fill_rect(mem, rr, col(bg));
        }
        int x = frozen_px_ - xoff_;
        for (int c = 1; c < ncol; ++c) {
            const int w = sc(src_->width(c));
            if (x + w > frozen_px_ && x < rc.right) {
                RECT r = {x + pad, y, x + w - pad, y + rh_};
                draw_text(mem, f_cell_, col(kColText), r, src_->cell(i, c),
                          DT_LEFT | DT_END_ELLIPSIS);
            }
            x += w;
        }
    }
    SelectClipRgn(mem, nullptr);

    /* 行分隔线 */
    for (int i = top_; i < n && i < top_ + vis; ++i) {
        const int y = hh_ + (i - top_) * rh_;
        if (y + rh_ > bottom) break;
        RECT ln = {frozen_px_, y + rh_ - 1, rc.right, y + rh_};
        fill_rect(mem, ln, col(kColGrid));
    }

    /* 冻结列：最后画，盖住横向滚过来的内容，位置固定 */
    for (int i = top_; i < n && i < top_ + vis; ++i) {
        const int y = hh_ + (i - top_) * rh_;
        if (y + rh_ > bottom) break;
        unsigned bg = src_->cell_bg(i, 0);
        if (!bg) bg = src_->row_bg(i);
        RECT c0 = {0, y, frozen_px_, y + rh_};
        if (bg) fill_rect(mem, c0, col(bg));
        RECT r = {pad, y, frozen_px_ - pad, y + rh_};
        draw_text(mem, f_cell_, col(kColText), r, src_->cell(i, 0), DT_LEFT | DT_END_ELLIPSIS);
        RECT ln = {0, y + rh_ - 1, frozen_px_, y + rh_};
        fill_rect(mem, ln, col(kColGrid));
    }
    {
        RECT fh = {0, 0, frozen_px_, hh_};
        fill_rect(mem, fh, col(kColAccent));
        RECT r = {pad, 0, frozen_px_ - pad, hh_};
        draw_text(mem, f_head_, RGB(0xFF, 0xFF, 0xFF), r,
                  ncol > 0 ? src_->header(0) : L"", DT_LEFT | DT_END_ELLIPSIS);
        /* 冻结边界：一条实线 + 一条浅影，横向滚动时能看出「这列钉住了」 */
        RECT line = {frozen_px_ - 1, 0, frozen_px_, bottom};
        fill_rect(mem, line, col(kColBorder));
    }

    /* 空态：表格区居中一行灰字 */
    if (n == 0) {
        RECT r = {0, hh_, rc.right, bottom};
        draw_text(mem, f_cell_, col(kColHint), r, empty_, DT_CENTER | DT_END_ELLIPSIS);
    }

    RECT frame = {0, 0, rc.right, rc.bottom};
    HBRUSH fb = CreateSolidBrush(col(kColBorder));
    FrameRect(mem, &frame, fb);
    DeleteObject(fb);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

LRESULT Table::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            update_scrollbars();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_VSCROLL: {
            const int n = src_ ? src_->rows() : 0;
            int top = top_;
            switch (LOWORD(wp)) {
                case SB_LINEUP: top -= 1; break;
                case SB_LINEDOWN: top += 1; break;
                case SB_PAGEUP: top -= visible_rows(); break;
                case SB_PAGEDOWN: top += visible_rows(); break;
                case SB_TOP: top = 0; break;
                case SB_BOTTOM: top = n; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    /* HIWORD 只有 16 位，>65535 行的表拖到后面会跳回；
                       SIF_TRACKPOS 才是 SetScrollInfo 设的 32 位位置 */
                    SCROLLINFO si = {sizeof(si)};
                    si.fMask = SIF_TRACKPOS;
                    top = GetScrollInfo(hwnd_, SB_VERT, &si) ? (int)si.nTrackPos
                                                             : (int)HIWORD(wp);
                    break;
                }
                default: return 0;
            }
            scroll_to(top, xoff_);
            return 0;
        }
        case WM_HSCROLL: {
            int x = xoff_;
            switch (LOWORD(wp)) {
                case SB_LINELEFT: x -= sc(24); break;
                case SB_LINERIGHT: x += sc(24); break;
                case SB_PAGELEFT: x -= scroll_w(); break;
                case SB_PAGERIGHT: x += scroll_w(); break;
                case SB_LEFT: x = 0; break;
                case SB_RIGHT: x = total_w(); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si = {sizeof(si)};
                    si.fMask = SIF_TRACKPOS;
                    x = GetScrollInfo(hwnd_, SB_HORZ, &si) ? (int)si.nTrackPos
                                                           : (int)HIWORD(wp);
                    break;
                }
                default: return 0;
            }
            scroll_to(top_, x);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) {
                scroll_to(top_, xoff_ - delta / 120 * sc(48));
            } else {
                scroll_to(top_ - delta / 120 * 3, xoff_);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            return 0;
        case WM_KEYDOWN: {
            switch (wp) {
                case VK_UP: scroll_to(top_ - 1, xoff_); return 0;
                case VK_DOWN: scroll_to(top_ + 1, xoff_); return 0;
                case VK_PRIOR: scroll_to(top_ - visible_rows(), xoff_); return 0;
                case VK_NEXT: scroll_to(top_ + visible_rows(), xoff_); return 0;
                case VK_HOME: scroll_to(0, xoff_); return 0;
                case VK_END: scroll_to(src_ ? src_->rows() : 0, xoff_); return 0;
                case VK_LEFT: scroll_to(top_, xoff_ - sc(24)); return 0;
                case VK_RIGHT: scroll_to(top_, xoff_ + sc(24)); return 0;
                default: break;
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK Table::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Table *self = nullptr;
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        self = static_cast<Table *>(cs->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Table *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace bs
