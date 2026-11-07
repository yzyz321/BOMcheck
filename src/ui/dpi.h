/* 每显示器 DPI：所有 API 动态取地址，避免老系统上直接调用不存在的导出 */
#ifndef BS_DPI_H
#define BS_DPI_H

#include <windows.h>

namespace bs {

/* 96 / 120 / 144 / 192 …；取不到时退回系统 DPI */
int dpi_for_window(HWND h);
int dpi_for_system(void);

/* 按 dpi 把「客户区尺寸」换算成「窗口尺寸」；新系统用 ForDpi 版本，老系统按比例放缩 */
bool window_rect_for_client(int cw, int ch, DWORD style, DWORD ex, int dpi, RECT *out);

/* "PerMonitorV2" / "PerMonitor" / "System" / "Unaware" / "Unknown" */
const char *awareness_name(void);

}  // namespace bs

#endif  // BS_DPI_H
