#include "drop.h"

#include <ole2.h>
#include <shellapi.h>

namespace bs {
namespace {

/* 同一个进程里只有一个主窗口，用单个注册表就够了；
   真要多窗口时这里换成 hwnd -> Target* 的映射。 */
HWND g_hwnd = nullptr;
IDropTarget *g_target = nullptr;

class Target : public IDropTarget {
public:
    Target(HWND h, DropSink *s) : hwnd_(h), sink_(s) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *obj, DWORD, POINTL ptl, DWORD *eff) override {
        files_ = drop_files(obj);
        want(eff);
        report(ptl);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL ptl, DWORD *eff) override {
        want(eff);
        report(ptl);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        files_.clear();
        sink_->on_drag_leave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *obj, DWORD, POINTL ptl, DWORD *eff) override {
        std::vector<std::wstring> files = drop_files(obj);
        POINT p = {ptl.x, ptl.y};
        ScreenToClient(hwnd_, &p);
        if (eff) *eff = files.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        sink_->on_drag_leave();
        if (!files.empty()) sink_->on_drop(files, p);
        files_.clear();
        return S_OK;
    }

private:
    void want(DWORD *eff) {
        if (eff) *eff = files_.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    }
    void report(POINTL ptl) {
        POINT p = {ptl.x, ptl.y};
        ScreenToClient(hwnd_, &p);
        sink_->on_drag_over(p, (int)files_.size());
    }

    LONG ref_ = 1;
    HWND hwnd_ = nullptr;
    DropSink *sink_ = nullptr;
    std::vector<std::wstring> files_;
};

}  // namespace

std::vector<std::wstring> drop_files(IDataObject *obj) {
    std::vector<std::wstring> out;
    if (!obj) return out;

    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg = {};
    if (FAILED(obj->GetData(&fmt, &stg))) return out;

    HDROP hd = static_cast<HDROP>(GlobalLock(stg.hGlobal));
    if (hd) {
        const UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(hd, i, nullptr, 0);
            if (!len) continue;
            std::wstring s(len, L'\0');
            DragQueryFileW(hd, i, &s[0], len + 1);
            s.resize(len);
            out.push_back(s);
        }
        GlobalUnlock(stg.hGlobal);
    }
    ReleaseStgMedium(&stg);
    return out;
}

bool drop_register(HWND hwnd, DropSink *sink) {
    if (!hwnd || !sink) return false;
    Target *t = new Target(hwnd, sink);
    if (FAILED(RegisterDragDrop(hwnd, t))) {
        t->Release();
        return false;
    }
    g_hwnd = hwnd;
    g_target = t;
    return true;
}

void drop_revoke(HWND hwnd) {
    if (hwnd && hwnd == g_hwnd) {
        RevokeDragDrop(hwnd);
        if (g_target) g_target->Release();
        g_target = nullptr;
        g_hwnd = nullptr;
    }
}

}  // namespace bs
