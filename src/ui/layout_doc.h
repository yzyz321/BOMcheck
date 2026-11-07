#ifndef BS_LAYOUT_DOC_H
#define BS_LAYOUT_DOC_H

#include <string>

namespace bs {

/* 把程序自己用的布局表导成 Markdown（--export-layout-doc）。

   坐标不是另写一份：直接遍历 kLayoutItems 并用 layout_rect() 现算，
   所以文档与程序不可能对不上。返回空串表示成功，否则是错误说明。 */
std::string export_layout_doc(const std::wstring &path, int dpi);

}  // namespace bs

#endif  // BS_LAYOUT_DOC_H
