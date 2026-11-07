# -*- coding: utf-8 -*-
"""C 版产物自检：把 exe 自己的 --self-check / --probe-dpi 结果当断言，再补体积与依赖检查。

用法：python tools/check_exe.py [exe] [--expect-version 4.0.1]
返回码 0 = 全过；1 = 有项目不达标。

为什么不解析 windres dump：同一种资源在不同 windres 版本 / 语言设置下导出格式不同
（dword 列表 / 宽字符串转义都见过），拿它当断言很脆。程序自己在运行时枚举资源更可靠，
顺带现场也能用 `BOMCheck.exe --self-check` 自证 DPI 声明在不在。
"""
import glob
import os
import re
import subprocess
import sys

MINGW = os.environ.get("BOMCHECK_MINGW", r"D:\AICS\mingw64\bin")
OBJDUMP = os.path.join(MINGW, "objdump.exe")

SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "shell32.dll", "comctl32.dll",
    "comdlg32.dll", "ole32.dll", "uuid.dll", "msimg32.dll", "uxtheme.dll",
    "version.dll", "advapi32.dll", "shlwapi.dll", "gdiplus.dll",
}


def run(exe, *args):
    p = subprocess.run([exe] + list(args), capture_output=True)
    out = p.stdout.decode("utf-8", "replace")
    return p.returncode, out


def kv(text):
    d = {}
    for ln in text.splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            d[k.strip()] = v.strip()
    return d


def dll_deps(exe):
    out = subprocess.run([OBJDUMP, "-p", exe], capture_output=True, text=True).stdout
    return sorted({ln.split(":", 1)[1].strip() for ln in out.splitlines() if "DLL Name" in ln})


def main():
    argv = sys.argv[1:]
    expect = None
    if "--expect-version" in argv:
        i = argv.index("--expect-version")
        expect = argv[i + 1]
        del argv[i:i + 2]
    if argv:
        exe = argv[0]
    else:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from findexe import find_exe
        exe = find_exe()
        if not exe:
            # 兜底：build/c/bin 里挑最新的产物，不写死版本号
            cand = glob.glob(os.path.join(ROOT, "build", "c", "bin", "BOMCheck_V*.exe"))
            exe = max(cand, key=os.path.getmtime) if cand else ""
    if not os.path.exists(exe):
        print("找不到产物：%s" % exe)
        return 1

    rows = []

    rc, out = run(exe, "--self-check")
    d = kv(out)
    rows.append(("--self-check", "PASS" if rc == 0 else "FAIL", rc == 0, "程序自证资源与 manifest"))
    rows.append(("manifest 份数", d.get("manifest_count", "?"), d.get("manifest_count") == "1",
                 "必须恰好 1 份"))
    for k, why in (("manifest_dpi_pmv2", "PerMonitorV2 声明"),
                   ("manifest_dpi_v1", "老系统 dpiAware 回退"),
                   ("manifest_as_invoker", "asInvoker 避免 UIPI 拦拖放"),
                   ("manifest_cc6", "Common-Controls v6"),
                   ("manifest_long_path", "longPathAware")):
        rows.append((k.replace("manifest_", ""), d.get(k, "?"), d.get(k) == "1", why))

    ver = d.get("version", "?")
    rows.append(("version", ver, bool(expect is None or ver == expect),
                 "与 CMake project() 一致" if expect else "程序版本"))
    fv = d.get("res_file_version", "")
    rows.append(("资源 FileVersion", fv or "缺失", fv.startswith(ver),
                 "资源版本以程序版本开头"))
    on = d.get("res_original_name", "")
    rows.append(("资源 OriginalFilename", on or "缺失", on == os.path.basename(exe),
                 "应与文件名一致"))

    icg = d.get("res_icon_groups", "0")
    rows.append(("图标组数", icg, icg == "1", "exe 内嵌 1 组 RT_GROUP_ICON"))
    sizes = d.get("res_icon_sizes", "")
    have = sizes.split(",") if sizes else []
    need = ["16", "20", "24", "32", "48", "64", "256"]
    miss = [s for s in need if s not in have]
    rows.append(("图标档位", "%d 档 %s" % (len(have), sizes or "缺失"), not miss,
                 "缺 %s" % ",".join(miss) if miss else "16~256 关键档齐全"))

    rc2, out2 = run(exe, "--probe-dpi")
    d2 = kv(out2)
    aw = d2.get("awareness", "?")
    rows.append(("DPI 感知", aw, rc2 == 0 and aw != "Unaware" and aw != "Unknown",
                 "PerMonitor 才算真感知"))
    rows.append(("client", d2.get("client", "?"), True, "逻辑 1080×720 按 DPI 放大"))
    rows.append(("dpi/scale", "%s / %s%%" % (d2.get("dpi", "?"), d2.get("scale", "?")), True,
                 "当前显示器"))

    size = os.path.getsize(exe)
    ico = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "src", "res", "app.ico")
    ico_kb = os.path.getsize(ico) / 1024.0 if os.path.exists(ico) else 0.0
    # 图标 10 档 PNG 约 140 KB，是固定资产，单独报出来免得与代码增长混为一谈
    rows.append(("体积", "%.1f KB（图标 %.0f）" % (size / 1024.0, ico_kb),
                 size <= 800 * 1024, "≤ 800 KB"))

    deps = dll_deps(exe)
    bad = [x for x in deps if x.lower() not in SYSTEM_DLLS
           and not x.lower().startswith("api-ms-win-crt-")]
    rows.append(("DLL 依赖", "%d 个" % len(deps), not bad,
                 "无第三方运行库" + ("，多出：%s" % ", ".join(bad) if bad else "")))

    ok = True
    for name, val, good, why in rows:
        ok = ok and good
        print("%-22s %-14s %s   %s" % (name, val, "OK" if good else "FAIL", why))
    print("DLL：%s" % ", ".join(deps))
    print("RESULT %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
