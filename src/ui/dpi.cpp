#include "dpi.h"

namespace bs {
namespace {

typedef UINT(WINAPI *FnGetDpiForWindow)(HWND);
typedef UINT(WINAPI *FnGetDpiForSystem)(void);
typedef BOOL(WINAPI *FnAdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef DPI_AWARENESS_CONTEXT(WINAPI *FnGetThreadDpiAwarenessContext)(void);
typedef DPI_AWARENESS(WINAPI *FnGetAwarenessFromDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);

struct Api {
    FnGetDpiForWindow get_dpi_for_window = nullptr;
    FnGetDpiForSystem get_dpi_for_system = nullptr;
    FnAdjustWindowRectExForDpi adjust_for_dpi = nullptr;
    FnGetThreadDpiAwarenessContext thread_ctx = nullptr;
    FnGetAwarenessFromDpiAwarenessContext awareness = nullptr;
    bool loaded = false;
};

const Api &api() {
    static Api a;
    if (!a.loaded) {
        a.loaded = true;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) {
            a.get_dpi_for_window = (FnGetDpiForWindow)(void *)GetProcAddress(u, "GetDpiForWindow");
            a.get_dpi_for_system = (FnGetDpiForSystem)(void *)GetProcAddress(u, "GetDpiForSystem");
            a.adjust_for_dpi =
                (FnAdjustWindowRectExForDpi)(void *)GetProcAddress(u, "AdjustWindowRectExForDpi");
            a.thread_ctx = (FnGetThreadDpiAwarenessContext)(void *)GetProcAddress(
                u, "GetThreadDpiAwarenessContext");
            a.awareness = (FnGetAwarenessFromDpiAwarenessContext)(void *)GetProcAddress(
                u, "GetAwarenessFromDpiAwarenessContext");
        }
    }
    return a;
}

/* 无 DPI 上下文时的兜底：桌面 DPI */
int screen_dpi() {
    HDC dc = GetDC(nullptr);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d > 0 ? d : 96;
}

}  // namespace

int dpi_for_window(HWND h) {
    const Api &a = api();
    if (a.get_dpi_for_window && h) {
        UINT d = a.get_dpi_for_window(h);
        if (d) return (int)d;
    }
    return dpi_for_system();
}

int dpi_for_system(void) {
    const Api &a = api();
    if (a.get_dpi_for_system) {
        UINT d = a.get_dpi_for_system();
        if (d) return (int)d;
    }
    return screen_dpi();
}

bool window_rect_for_client(int cw, int ch, DWORD style, DWORD ex, int dpi, RECT *out) {
    if (!out) return false;
    RECT r = {0, 0, cw, ch};
    const Api &a = api();
    if (a.adjust_for_dpi && dpi > 0) {
        if (a.adjust_for_dpi(&r, style, ex ? TRUE : FALSE, ex, (UINT)dpi)) {
            *out = r;
            return true;
        }
    }
    /* 兜底：按 96 基准算边框，再按比例放大 */
    if (!AdjustWindowRectEx(&r, style, FALSE, ex)) return false;
    int ref = 96;
    r.left = MulDiv(r.left, dpi, ref);
    r.top = MulDiv(r.top, dpi, ref);
    r.right = MulDiv(r.right, dpi, ref);
    r.bottom = MulDiv(r.bottom, dpi, ref);
    *out = r;
    return true;
}

const char *awareness_name(void) {
    const Api &a = api();
    if (!a.thread_ctx || !a.awareness) return "Unknown";
    switch (a.awareness(a.thread_ctx())) {
        case DPI_AWARENESS_UNAWARE: return "Unaware";
        case DPI_AWARENESS_SYSTEM_AWARE: return "System";
        case DPI_AWARENESS_PER_MONITOR_AWARE: return "PerMonitor";
        default: return "Unknown";
    }
}

}  // namespace bs
