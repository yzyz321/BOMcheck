# -*- coding: utf-8 -*-
"""抓当前 BOMCheck 窗口的客户区截图（PrintWindow，不受桌面/遮挡影响）。

用法：python tools/shot.py build/c7_window.png [--full]
--full 抓整窗（含标题栏），默认只抓客户区。
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

from PIL import Image

u = ctypes.windll.user32
g = ctypes.windll.gdi32

# 进程不声明 DPI 感知时，GetClientRect 返回的是被虚拟化过的尺寸，截出来会糊
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    u.SetProcessDPIAware()


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def grab(hwnd, client_only):
    r = wt.RECT()
    if client_only:
        u.GetClientRect(hwnd, ctypes.byref(r))
        w, h = r.right, r.bottom
    else:
        u.GetWindowRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top

    hdc = u.GetWindowDC(hwnd)
    mdc = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(mdc, bmp)

    # PW_CLIENTONLY = 1
    u.PrintWindow(hwnd, mdc, 1 if client_only else 0)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    img = Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    u.ReleaseDC(hwnd, hdc)
    return img


def main():
    out = sys.argv[1]
    full = "--full" in sys.argv
    hwnd = 0
    for _ in range(60):
        hwnd = u.FindWindowW("BOMCheckMain", None)
        if hwnd:
            break
        time.sleep(0.1)
    if not hwnd:
        print("没找到窗口")
        return 1
    u.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    img = grab(hwnd, not full)
    img.save(out)
    print("saved", out, img.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
