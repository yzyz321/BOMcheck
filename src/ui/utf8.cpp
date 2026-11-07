#include "utf8.h"

#include <windows.h>

namespace bs {

std::string to_utf8(const std::wstring &s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(const char *s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out((size_t)(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

void from_utf8(const char *src, wchar_t *dst, int cap) {
    if (!dst || cap <= 0) return;
    dst[0] = 0;
    if (!src) return;
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap);
    dst[cap - 1] = 0;
}

}  // namespace bs
