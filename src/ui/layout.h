#ifndef BS_LAYOUT_H
#define BS_LAYOUT_H

#include <windows.h>

namespace bs {

/* 控件矩形：tbl 里的坐标是 96 DPI 逻辑像素、原点在客户区左上角。
   实际像素 = 逻辑值 × dpi / 96，与 Python 原型同一套约定。 */
struct LayoutItem {
    const char *id;
    int sec;                 // 0 header / 1 files / 2 options / 3 actions / 4 results / 5 status
    int x, y, w, h;          // 基准尺寸（1080×720）下的逻辑矩形
    int gx, gy, gw, gh;      // 每 1000 逻辑像素的增长量（拉伸系数），见 gen_layout_c.py
    const char *type;        // label / frame / button / combo / ...
};

/* 找不到返回 nullptr（拼错 id 时早点炸，别静默画到 (0,0)） */
const LayoutItem *layout_find(const char *id);

/* 按序号取（--dump-layout 遍历用） */
const LayoutItem *layout_at(int i);

/* 按当前客户区逻辑尺寸算出矩形（逻辑像素）。lw/lh 传 0 表示用基准尺寸 */
RECT layout_rect(const LayoutItem *it, int lw = 0, int lh = 0);

/* 便捷重载：id 不存在时返回空矩形 */
RECT layout_rect(const char *id, int lw = 0, int lh = 0);

int layout_count();

}  // namespace bs

#endif  // BS_LAYOUT_H
