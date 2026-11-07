# -*- coding: utf-8 -*-
"""干净目录验收（C8）：只把打包出来的 exe 拷到一个空目录，模拟「目标机只有 exe」。

真虚拟机不好自动化，这里做的是等价的几件事：
  - 空目录：没有任何伴随文件（没有 DLL、没有 _internal、没有 Python）
  - 净化环境：PATH 只留 system32 / Windows，TEMP 指到空目录
    （这样即便系统装了 MinGW/Python，也帮不上忙）
  - 全流程跑一遍：自检 → 版本 → 布局文档 → 两份 BOM 出三份文件
  - 用 openpyxl 读回三份文件，并核对源文件 sha256 没变

用法：python tools/check_pack.py [--keep]
"""
import glob
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "pyref"))

from findexe import find_exe  # noqa: E402
import samples  # noqa: E402  样本位置（tests/data，缺了退回 temp）

# WorkBuddy 自带的 3.13 没有 openpyxl；缺了就换成 miniconda 解释器重跑一遍
try:
    import openpyxl  # noqa: F401
except ImportError:
    _alt = r"D://miniconda3//python.exe"
    if os.path.exists(_alt) and os.path.abspath(sys.executable) != os.path.abspath(_alt):
        os.execv(_alt, [_alt, os.path.abspath(__file__)] + sys.argv[1:])

WORK = os.path.join(ROOT, "build", "tmp_pack")
OLD = samples.p("cases", "pair_old.xlsx")
NEW = samples.p("cases", "pair_new.xlsx")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def clean_env(tmp):
    env = {
        "SystemRoot": r"C:\Windows",
        "windir": r"C:\Windows",
        "PATH": r"C:\Windows\system32;C:\Windows;C:\Windows\System32\Wbem",
        "TEMP": os.path.join(tmp, "tmp"),
        "TMP": os.path.join(tmp, "tmp"),
        "USERPROFILE": tmp,
        "APPDATA": os.path.join(tmp, "appdata"),
        "LOCALAPPDATA": os.path.join(tmp, "localappdata"),
        "NUMBER_OF_PROCESSORS": "1",
    }
    for p in ("tmp", "appdata", "localappdata"):
        os.makedirs(os.path.join(tmp, p), exist_ok=True)
    return env


def packaged_exe():
    """优先用 dist 里打好的那份（这才是用户拿到手的文件）；没有就用 build 产物。"""
    hits = glob.glob(os.path.join(ROOT, "dist", "BOMCheck_V*", "BOMCheck_V*.exe"))
    if hits:
        return max(hits, key=lambda p: [int(x) for x in
                                        os.path.basename(p)[10:-4].split(".")])
    return find_exe()


def main():
    keep = "--keep" in sys.argv
    exe = packaged_exe()
    if not exe:
        print("找不到产物，先跑 build_c.bat")
        return 1
    print("被测产物 %s" % os.path.relpath(exe, ROOT))

    # 「干净目录」用带 pid 的新目录：不整目录删旧的一次删几十个文件会被安全策略
    # 拦下（SAFE_DELETE_BULK），整条构建链就断在这一步。新目录本来就是空的，
    # 验收意义不变；旧目录留在 build\ 下，build_c.bat clean 时一起没。
    clean = os.path.join(WORK, "clean_%d" % os.getpid())
    os.makedirs(clean)
    lone = os.path.join(clean, os.path.basename(exe))
    shutil.copy2(exe, lone)
    shutil.copy2(OLD, os.path.join(clean, "old.xlsx"))
    shutil.copy2(NEW, os.path.join(clean, "new.xlsx"))
    env = clean_env(clean)

    bad = 0

    def run(*args):
        p = subprocess.run([lone] + list(args), cwd=clean, env=env, capture_output=True)
        return p.returncode, p.stdout.decode("utf-8", "replace")

    rc, out = run("--version")
    ver = ""
    for ln in out.splitlines():
        if ln.startswith("BOMCheck "):
            ver = ln.split(None, 1)[1].strip()
    if rc != 0 or not ver:
        print("FAIL 版本自证 %r" % out[:120])
        bad += 1
    else:
        print("PASS 版本自证            %s" % ver)

    rc, out = run("--self-check")
    ok = rc == 0 and "RESULT PASS" in out.upper().replace("SELF-CHECK", "RESULT PASS")
    # --self-check 自己就打印 PASS/FAIL 表；这里只认退出码 + 没有 FAIL
    ok = rc == 0 and "FAIL" not in out
    if not ok:
        print("FAIL 自检  rc=%d\n%s" % (rc, out[:400]))
        bad += 1
    else:
        print("PASS 自检（资源 / manifest / DPI 声明）")

    # 布局文档：不带任何外部文件也要能导出
    docpath = os.path.join(clean, "layout.md")
    rc, out = run("--export-layout-doc", docpath)
    if rc != 0 or not os.path.exists(docpath) or os.path.getsize(docpath) < 2000:
        print("FAIL 布局文档导出 rc=%d %r" % (rc, out[:200]))
        bad += 1
    else:
        with open(docpath, encoding="utf-8") as f:
            head = f.read(200)
        if ver not in head:
            print("FAIL 布局文档版本号不是 %s：%r" % (ver, head[:80]))
            bad += 1
        else:
            print("PASS 布局文档            %.1f KB" % (os.path.getsize(docpath) / 1024.0))

    before = {p: sha256(os.path.join(clean, p)) for p in ("old.xlsx", "new.xlsx")}
    outdir = os.path.join(clean, "out")
    rc, out = run("--ui-run", "old.xlsx", "new.xlsx", outdir)
    files = sorted(f for f in os.listdir(outdir)) if os.path.isdir(outdir) else []
    if rc != 0 or len(files) != 3 or "ok=1" not in out:
        print("FAIL 全流程 rc=%d 产物=%s\n%s" % (rc, files, out[-600:]))
        bad += 1
    else:
        print("PASS 全流程出三份        %s" % " / ".join(files))

    if files:
        try:
            from openpyxl import load_workbook
        except ImportError:
            load_workbook = None
        for f in files:
            p = os.path.join(outdir, f)
            if load_workbook is None:
                # 缺 openpyxl 就验不了「写出的确实是能读的 xlsx」——
                # 这一项宁可红，也不能打印「0 个失败」骗自己
                print("FAIL 读回 %s：本机没有 openpyxl，干净目录验收不完整" % f)
                bad += 1
                continue
            try:
                wb = load_workbook(p)
            except Exception as exc:  # noqa: BLE001
                print("FAIL 读回 %s：%s" % (f, exc))
                bad += 1
                continue
            # 批注要按单元格数：openpyxl 的 ws._comments 在部分版本里是空的
            n_cmt = sum(1 for ws in wb.worksheets for row in ws.iter_rows()
                        for c in row if c.comment)
            print("PASS 读回 %-34s %d 页 · %d 行 · %d 条批注"
                  % (f, len(wb.sheetnames), wb[wb.sheetnames[0]].max_row, n_cmt))
            wb.close()

    after = {p: sha256(os.path.join(clean, p)) for p in ("old.xlsx", "new.xlsx")}
    if before != after:
        print("FAIL 源文件被改动 %s" % [k for k in before if before[k] != after[k]])
        bad += 1
    else:
        print("PASS 源文件未被改动（sha256 不变）")

    # 目录里除了我们自己放的，还有没有多出来的东西（解压残留之类）
    extra = [f for f in os.listdir(clean)
             if f not in ("old.xlsx", "new.xlsx", "out", "layout.md", "tmp", "appdata",
                          "localappdata", os.path.basename(exe))]
    if extra:
        print("注意 目录里多出 %s" % extra)

    print("\n干净目录验收：%d 个失败" % bad)
    if not keep and bad == 0:
        # 尽力清理：被安全策略拦下不影响结论（build\ 下本来就是临时区）
        try:
            shutil.rmtree(clean, ignore_errors=True)
        except BaseException:                    # noqa: BLE001
            pass
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
