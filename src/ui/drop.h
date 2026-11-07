#ifndef BS_DROP_H
#define BS_DROP_H

#include <windows.h>

#include <string>
#include <vector>

namespace bs {

/* 拖放回调。坐标已换成客户区物理像素。 */
class DropSink {
public:
    virtual ~DropSink() {}
    virtual void on_drag_over(POINT client_pt, int file_count) = 0;
    virtual void on_drag_leave() = 0;
    virtual void on_drop(const std::vector<std::wstring> &files, POINT client_pt) = 0;
};

/* 注册 OLE 拖放目标（能拿到 DragEnter/DragOver 的悬停反馈）。
   失败返回 false，调用方应退回 DragAcceptFiles + WM_DROPFILES。 */
bool drop_register(HWND hwnd, DropSink *sink);
void drop_revoke(HWND hwnd);

/* 从 IDataObject 取 CF_HDROP 文件列表 */
std::vector<std::wstring> drop_files(IDataObject *obj);

}  // namespace bs

#endif  // BS_DROP_H
