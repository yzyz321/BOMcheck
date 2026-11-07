#include "layout.h"

#include <cstring>

#include "layout_gen.h"

namespace bs {
namespace {

int grow(int base, int delta, int ppm) {
    return base + (int)((long long)delta * ppm / 1000);
}

}  // namespace

const LayoutItem *layout_find(const char *id) {
    if (!id) return nullptr;
    for (int i = 0; i < kLayoutCount; ++i) {
        if (std::strcmp(kLayoutItems[i].id, id) == 0) return &kLayoutItems[i];
    }
    return nullptr;
}

int layout_count() { return kLayoutCount; }

const LayoutItem *layout_at(int i) {
    if (i < 0 || i >= kLayoutCount) return nullptr;
    return &kLayoutItems[i];
}

RECT layout_rect(const LayoutItem *it, int lw, int lh) {
    RECT r = {0, 0, 0, 0};
    if (!it) return r;
    if (lw <= 0) lw = kBaseLogicalW;
    if (lh <= 0) lh = kBaseLogicalH;
    const int dx = lw - kBaseLogicalW;
    const int dy = lh - kBaseLogicalH;
    r.left = grow(it->x, dx, it->gx);
    r.top = grow(it->y, dy, it->gy);
    r.right = r.left + grow(it->w, dx, it->gw);
    r.bottom = r.top + grow(it->h, dy, it->gh);
    return r;
}

RECT layout_rect(const char *id, int lw, int lh) {
    return layout_rect(layout_find(id), lw, lh);
}

}  // namespace bs
