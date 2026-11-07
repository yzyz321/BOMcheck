#include "paint.h"

#include <cwchar>

#include "layout.h"

namespace bs {

COLORREF col(unsigned rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

RECT UiCtx::px(const RECT &logical) const {
    RECT r = {sc(logical.left), sc(logical.top), sc(logical.right), sc(logical.bottom)};
    return r;
}

RECT UiCtx::px(const char *id) const {
    RECT empty = {0, 0, 0, 0};
    const LayoutItem *it = layout_find(id);
    if (!it) return empty;
    return px(layout_rect(it, lw, lh));
}

void fill_rect(HDC dc, const RECT &r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void fill_round(HDC dc, const RECT &r, int radius_px, COLORREF fill, COLORREF border,
                int border_w) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = border == CLR_INVALID ? CreatePen(PS_NULL, 0, RGB(0, 0, 0))
                                  : CreatePen(PS_SOLID, border_w, border);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius_px, radius_px);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);
}

void draw_text(HDC dc, HFONT f, COLORREF c, const RECT &r, const wchar_t *s, UINT extra) {
    if (!s) return;
    HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
    SetTextColor(dc, c);
    RECT rr = r;
    /* DT_NOPREFIX 必须加：否则 DrawText 把 & 当加速键前缀吞掉，
       D:\R&D\BOM.xlsx 会显示成 D:\RD\BOM.xlsx，料号/描述里的 & 也会少字符 */
    DrawTextW(dc, s, -1, &rr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | extra);
    if (old) SelectObject(dc, old);
}

int text_w(HDC dc, HFONT f, const wchar_t *s) {
    if (!s) return 0;
    HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, s, (int)std::wcslen(s), &sz);
    if (old) SelectObject(dc, old);
    return sz.cx;
}

}  // namespace bs
