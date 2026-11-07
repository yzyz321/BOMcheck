#ifndef BS_PAINT_H
#define BS_PAINT_H

#include <windows.h>

namespace bs {

/* 一次绘制的上下文：DC + 当前 DPI + 客户区逻辑尺寸 + 三档字体。
   px() 把布局表里的逻辑矩形换算成物理像素，所有绘制都走它。 */
struct UiCtx {
    HDC dc = nullptr;
    int dpi = 96;
    int lw = 1080;   // 客户区逻辑宽
    int lh = 720;    // 客户区逻辑高
    HFONT h1 = nullptr;      // 18pt 粗：窗口主标题
    HFONT h3 = nullptr;      // 11pt 粗：卡片标题 / 按钮
    HFONT body = nullptr;    // 10pt：正文
    HFONT small = nullptr;   // 9pt：注释

    int sc(int logical) const { return MulDiv(logical, dpi, 96); }
    RECT px(const RECT &logical) const;
    RECT px(const char *id) const;   // id 查布局表；找不到返回空矩形
};

COLORREF col(unsigned rgb);
void fill_rect(HDC dc, const RECT &r, COLORREF c);
void fill_round(HDC dc, const RECT &r, int radius_px, COLORREF fill, COLORREF border = CLR_INVALID,
                int border_w = 1);
void draw_text(HDC dc, HFONT f, COLORREF c, const RECT &r, const wchar_t *s, UINT extra = 0);
int text_w(HDC dc, HFONT f, const wchar_t *s);

}  // namespace bs

#endif  // BS_PAINT_H
