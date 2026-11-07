#ifndef BS_TABLE_H
#define BS_TABLE_H

#include <windows.h>

namespace bs {

/* 结果表的数据源。界面不复制单元格 —— 表格只向源要「可见的那几十行」，
   行数再多内存也是常数（对应设计里 LVS_OWNERDATA 的虚拟模式意图）。

   字符串按 UTF-16 给，生命期由实现方保证到下一次 cell() 调用为止；
   绘制是即取即画，不做缓存。 */
class TableSource {
public:
    virtual ~TableSource() {}
    virtual int rows() const = 0;
    virtual int cols() const = 0;
    virtual const wchar_t *header(int c) const = 0;
    /* 列宽（96 DPI 逻辑像素） */
    virtual int width(int c) const = 0;
    virtual const wchar_t *cell(int r, int c) const = 0;
    /* 整行底色，0 = 不涂（按差异类型） */
    virtual unsigned row_bg(int r) const { (void)r; return 0; }
    /* 单元格底色，只在冻结列上生效（重复料号格浅紫） */
    virtual unsigned cell_bg(int r, int c) const { (void)r; (void)c; return 0; }
};

/* 自绘结果表：冻结首列 + 右 11 列横向滚动 + 共用一条竖向滚动条。
   不复用 SysListView32 的两个理由：
     1. 冻结首列要靠两个列表控件并排，滚动/选中要互相拉齐并防递归，
        C 版一次自绘就能做到，行高与表头高天然对齐；
     2. 单元格级浅紫底（导出文件的口径）在 ListView 上要走
        NM_CUSTOMDRAW 的子项矩形，反而绕。 */
class Table {
public:
    bool create(HWND parent, HINSTANCE inst);
    void destroy();

    void set_source(TableSource *src);
    void set_dpi(int dpi);
    /* 逻辑像素矩形（与布局表同一坐标）；内部换算成物理像素并 SetWindowPos */
    void set_logical_rect(int x, int y, int w, int h);
    void set_empty_text(const wchar_t *t);
    void set_visible(bool v);
    void invalidate();

    HWND hwnd() const { return hwnd_; }
    /* 探针用：首行 / 横向偏移（物理像素）/ 冻结列宽（逻辑像素） */
    int top_row() const { return top_; }
    int x_offset() const { return xoff_; }
    int frozen_width() const;          /* 逻辑像素 */
    int visible_rows() const;
    void scroll_to(int top, int xoff);
    void scroll_to_end();              /* 横向滚到最右，核对冻结列 */

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);

    void make_fonts();
    void drop_fonts();
    void metrics();                    /* 表头高 / 行高 / 冻结列宽 */
    void update_scrollbars();
    void paint();
    int  body_h() const;
    int  scroll_w() const;             /* 右 11 列可见宽度（物理像素） */
    int  total_w() const;              /* 右 11 列总宽（物理像素） */
    int  sc(int logical) const { return MulDiv(logical, dpi_, 96); }

    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    HFONT f_head_ = nullptr;
    HFONT f_cell_ = nullptr;
    TableSource *src_ = nullptr;
    wchar_t empty_[96] = L"";
    int top_ = 0;
    int xoff_ = 0;
    int hh_ = 0;        /* 表头高（物理像素） */
    int rh_ = 0;        /* 行高 */
    int frozen_px_ = 0; /* 冻结列宽（物理像素） */
};

}  // namespace bs

#endif  // BS_TABLE_H
