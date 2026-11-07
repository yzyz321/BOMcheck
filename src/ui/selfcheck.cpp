#include "selfcheck.h"

#include <windows.h>
#include <winver.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "bs_version.h"

namespace bs {
namespace {

struct ManifestInfo {
    int count = 0;
    std::string text;
};

BOOL CALLBACK enum_manifest(HMODULE, LPCWSTR, LPWSTR, LONG_PTR param) {
    auto *mi = reinterpret_cast<ManifestInfo *>(param);
    ++mi->count;
    return TRUE;
}

ManifestInfo read_manifest() {
    ManifestInfo mi;
    EnumResourceNamesW(nullptr, MAKEINTRESOURCEW(24 /* RT_MANIFEST */), enum_manifest,
                       reinterpret_cast<LONG_PTR>(&mi));

    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    if (!res) return mi;
    HGLOBAL h = LoadResource(nullptr, res);
    if (!h) return mi;
    const char *p = static_cast<const char *>(LockResource(h));
    DWORD n = SizeofResource(nullptr, res);
    if (p && n) mi.text.assign(p, n);
    return mi;
}

/* ── 图标资源 ───────────────────────────────────────────────────
   只统计 RT_GROUP_ICON 的档位，不解码像素：目的是证明「.ico 被 windres
   完整嵌进来了」，而不是证明它好看。256 档在 GRPICONDIR 里宽高写作 0。 */
struct IconInfo {
    int groups = 0;
    std::string sizes;   /* 形如 "16,20,24,..."，便于脚本逐档比对 */
};

BOOL CALLBACK enum_icon(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) {
    auto *ii = reinterpret_cast<IconInfo *>(param);
    ++ii->groups;
    HRSRC res = FindResourceW(nullptr, name, RT_GROUP_ICON);
    if (!res) return TRUE;
    HGLOBAL h = LoadResource(nullptr, res);
    const BYTE *p = h ? static_cast<const BYTE *>(LockResource(h)) : nullptr;
    if (!p) return TRUE;

    /* GRPICONDIR：WORD reserved, type, count；每项 14 字节，首字节是宽（0 表示 256） */
    const int count = *reinterpret_cast<const WORD *>(p + 4);
    std::string s;
    for (int i = 0; i < count; i++) {
        const BYTE *e = p + 6 + i * 14;
        int w = e[0] ? e[0] : 256;
        if (!s.empty()) s += ',';
        s += std::to_string(w);
    }
    if (!s.empty()) ii->sizes = s;
    return TRUE;
}

IconInfo read_icons() {
    IconInfo ii;
    EnumResourceNamesW(nullptr, RT_GROUP_ICON, enum_icon, reinterpret_cast<LONG_PTR>(&ii));
    return ii;
}

std::string wide_to_utf8(const wchar_t *w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1));
    return s;
}

/* 读版本资源里的字符串字段 */
std::string version_string(const wchar_t *field) {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return {};
    std::string buf(size, '\0');
    if (!GetFileVersionInfoW(path, 0, size, &buf[0])) return {};

    struct LangCp {
        WORD lang;
        WORD cp;
    } *tr = nullptr;
    UINT trlen = 0;
    /* 注意：查 Translation 时 puLen 常返回 0（指针有效、长度不可信），不能拿它做判据 */
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void **)&tr, &trlen) || !tr)
        return {};

    wchar_t sub[128];
    std::swprintf(sub, 128, L"\\StringFileInfo\\%04x%04x\\", tr[0].lang, tr[0].cp);
    std::wcsncat(sub, field, 60);
    wchar_t *val = nullptr;
    UINT vlen = 0;
    if (!VerQueryValueW(buf.data(), sub, (void **)&val, &vlen) || !val) return {};
    return wide_to_utf8(val);
}

void emit_kv(void (*emit)(const char *), const char *key, const char *val) {
    char line[512];
    std::snprintf(line, sizeof(line), "%s=%s", key, val ? val : "");
    emit(line);
}

void emit_kv(void (*emit)(const char *), const char *key, long long val) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", val);
    emit_kv(emit, key, buf);
}

}  // namespace

int run_self_check(void (*emit)(const char *line)) {
    if (!emit) return 1;

    ManifestInfo mi = read_manifest();
    const std::string &t = mi.text;

    const bool has_pmv2 = t.find(">PerMonitorV2<") != std::string::npos ||
                          t.find(">PerMonitorV2</dpiAwareness>") != std::string::npos;
    const bool has_dpi_v1 = t.find(">true/pm<") != std::string::npos;
    const bool has_as_invoker = t.find("level=\"asInvoker\"") != std::string::npos;
    const bool has_cc6 = t.find("Microsoft.Windows.Common-Controls") != std::string::npos;
    const bool has_long_path = t.find("longPathAware") != std::string::npos;

    emit_kv(emit, "version", bs_version());
    emit_kv(emit, "build", bs_build_stamp());

    emit_kv(emit, "manifest_count", (long long)mi.count);
    emit_kv(emit, "manifest_bytes", (long long)t.size());
    emit_kv(emit, "manifest_dpi_pmv2", has_pmv2 ? 1 : 0);
    emit_kv(emit, "manifest_dpi_v1", has_dpi_v1 ? 1 : 0);
    emit_kv(emit, "manifest_as_invoker", has_as_invoker ? 1 : 0);
    emit_kv(emit, "manifest_cc6", has_cc6 ? 1 : 0);
    emit_kv(emit, "manifest_long_path", has_long_path ? 1 : 0);

    std::string fv = version_string(L"FileVersion");
    std::string fn = version_string(L"OriginalFilename");
    emit_kv(emit, "res_file_version", fv.c_str());
    emit_kv(emit, "res_original_name", fn.c_str());

    IconInfo ii = read_icons();
    /* 按逗号精确匹配，避免 "16" 命中 "256" 这种子串误判 */
    const std::string wrapped = "," + ii.sizes + ",";
    auto has_size = [&wrapped](const char *tok) {
        return wrapped.find(std::string(",") + tok + ",") != std::string::npos;
    };
    const bool icon_ok = ii.groups == 1 && has_size("16") && has_size("32") &&
                         has_size("48") && has_size("256");
    emit_kv(emit, "res_icon_groups", ii.groups);
    emit_kv(emit, "res_icon_sizes", ii.sizes.c_str());

    const bool ok = mi.count == 1 && has_pmv2 && has_dpi_v1 && has_as_invoker &&
                    has_cc6 && !fv.empty() && icon_ok;
    emit_kv(emit, "selfcheck", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace bs
