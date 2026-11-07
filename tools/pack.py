# -*- coding: utf-8 -*-
"""打包 C 版：dist/BOMCheck_V<版本>/ + 同名 zip。

与 Python 版 build_exe.py 的区别：C 版是**单个静态链接 exe**，不需要
PyInstaller、不需要 _internal 目录、不需要目标机有 Python。这里做的只是
「挑出产物 → 配上说明与布局对照表 → 打 zip → 去掉 Zone.Identifier」。

版本号不写死：从 exe 自己的 --version 读，保证「包名 = 程序自报版本」。
打包完顺带核对三件事：README 里的版本号、zip 里的条目、exe 的 DLL 依赖。

用法（仓库根目录）：
    python tools/pack.py                 # 打当前产物
    python tools/pack.py --dist D:\\out   # 换输出目录
    python tools/pack.py --no-zip        # 只出目录，不打压缩包
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from findexe import find_exe  # noqa: E402

SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "shell32.dll", "comctl32.dll",
    "comdlg32.dll", "ole32.dll", "uuid.dll", "msimg32.dll", "uxtheme.dll",
    "version.dll", "advapi32.dll", "shlwapi.dll", "gdiplus.dll",
}


def exe_version(exe):
    out = subprocess.run([exe, "--version"], capture_output=True).stdout.decode("utf-8", "replace")
    for ln in out.splitlines():
        if ln.strip().startswith("BOMCheck "):
            return ln.split(None, 1)[1].strip()
    raise SystemExit("读不到版本号：%r" % out[:200])


def dll_deps(exe):
    # 优先用环境变量 / PATH 里的 objdump，最后才退回本机默认安装位置
    objdump = (os.environ.get("BOMCHECK_OBJDUMP")
               or shutil.which("objdump")
               or os.path.join(os.environ.get("BOMCHECK_MINGW", r"D:\AICS\mingw64\bin"),
                               "objdump.exe"))
    if not os.path.exists(objdump):
        return []
    out = subprocess.run([objdump, "-p", exe], capture_output=True, text=True).stdout
    return sorted({ln.split(":", 1)[1].strip() for ln in out.splitlines() if "DLL Name" in ln})


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def unblock(path):
    """删掉 Zone.Identifier，避免本机 SmartScreen 反复拦自己刚打的包。"""
    try:
        os.remove(path + ":Zone.Identifier")
        return True
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--no-zip", action="store_true")
    args = ap.parse_args()

    exe = find_exe()
    if not exe:
        raise SystemExit("找不到产物，先跑 build_c.bat")
    ver = exe_version(exe)
    name = "BOMCheck_V%s" % ver
    out = os.path.join(args.dist, name)

    readme = os.path.join(ROOT, "README.txt")
    with open(readme, encoding="utf-8") as f:
        readme_txt = f.read()
    if ver not in readme_txt:
        raise SystemExit("README.txt 里没有 %s —— 先改说明再打包，否则用户看到的版本对不上"
                         % ver)

    # 交付包里带版本号的文件全部要对得上。只校验 README 的话，升版本时
    # README 会红、手册与对照表静默停留在旧版本，照样进 zip 发出去。
    for rel in ("README.txt", "docs/用户使用手册.md", "docs/用户使用手册.html",
                "docs/GUI控件位置对照表.md"):
        path = os.path.join(ROOT, *rel.split("/"))
        if not os.path.exists(path):
            print("警告：交付清单里缺 %s" % rel)
            continue
        with open(path, encoding="utf-8") as f:
            if ver not in f.read():
                raise SystemExit("%s 里没有版本号 %s —— 先更新这份文档再打包"
                                 % (rel, ver))

    # 先清空上一次的目录再重建；删不掉（有的环境对批量删除有拦截）也不算错，
    # 下面三个文件都是整份覆盖，覆盖后按白名单核对有没有多余残留
    if os.path.isdir(out):
        try:
            shutil.rmtree(out, ignore_errors=True)
        except BaseException:                    # noqa: BLE001
            pass
    os.makedirs(out, exist_ok=True)

    dst_exe = os.path.join(out, name + ".exe")
    shutil.copy2(exe, dst_exe)
    shutil.copy2(readme, os.path.join(out, "README.txt"))

    doc = os.path.join(ROOT, "docs", "GUI控件位置对照表.md")
    if os.path.exists(doc):
        shutil.copy2(doc, os.path.join(out, "GUI控件位置对照表.md"))

    # 用户手册一起分发：md 给技术同事，html 给业务同事（单文件、图片已内联）
    manual = ("用户使用手册.md", "用户使用手册.html")
    shipped_manual = []
    for fn in manual:
        p = os.path.join(ROOT, "docs", fn)
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(out, fn))
            shipped_manual.append(fn)
    if not shipped_manual:
        print("警告：docs/ 里没有《用户使用手册》，交付包里将不含手册")

    # 目录清理被拦下时留下的旧文件会一起进 zip，这里点名报警
    expect = {name + ".exe", "README.txt"}
    if os.path.exists(doc):
        expect.add("GUI控件位置对照表.md")
    expect.update(shipped_manual)
    extra = sorted(f for f in os.listdir(out) if f not in expect)
    if extra:
        print("警告：%s 里有多余文件 %s（上次打包残留）" % (name, ", ".join(extra)))

    unblock(dst_exe)

    deps = dll_deps(dst_exe)
    bad = [d for d in deps if d.lower() not in SYSTEM_DLLS
           and not d.lower().startswith("api-ms-win-crt-")]
    if bad:
        print("警告：有非系统 DLL 依赖 %s（目标机可能缺）" % ", ".join(bad))

    print("目录 %s" % out)
    for f in sorted(os.listdir(out)):
        p = os.path.join(out, f)
        print("  %-28s %8.1f KB" % (f, os.path.getsize(p) / 1024.0))

    zip_path = os.path.join(args.dist, name + ".zip")
    if not args.no_zip:
        # 不用先删旧的：ZipFile("w") 会整份截断重写。删这一步在某些环境里
        # 会撞上批量删除拦截（SAFE_DELETE_BULK），没必要冒这个险
        # 固定时间戳：同一份产物重复打包，zip 的 sha256 不变，便于分发核对
        stamp = (2026, 1, 1, 0, 0, 0)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(os.listdir(out)):
                p = os.path.join(out, f)
                info = zipfile.ZipInfo(name + "/" + f, date_time=stamp)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                with open(p, "rb") as fh:
                    z.writestr(info, fh.read())
        unblock(zip_path)
        print("压缩包 %s  %.1f KB" % (zip_path, os.path.getsize(zip_path) / 1024.0))

    print("exe sha256 %s" % sha256(dst_exe))
    print("PACK %s" % name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
