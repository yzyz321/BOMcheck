# -*- coding: utf-8 -*-
"""按目录里的实际产物找 exe，不写死版本号。

版本号只在 CMakeLists.txt 的 project(VERSION ...) 里出现一次；工具脚本若把
BOMCheck_V3.2.0.exe 这类名字写死，每次升版本都得改一串脚本，忘一处就报
「找不到产物」。这里按版本号数值取最大的那个。
"""
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DIR = os.path.join(ROOT, "build", "c", "bin")


def _key(path):
    m = re.search(r"BOMCheck_V(\d+)\.(\d+)\.(\d+)\.exe$", os.path.basename(path))
    if not m:
        return (0, 0, 0)
    return tuple(int(x) for x in m.groups())


def find_exe(bin_dir=None):
    """返回 build/c/bin 下版本号最大的 BOMCheck_V*.exe；找不到返回空串。"""
    d = bin_dir or DEFAULT_DIR
    hits = [p for p in glob.glob(os.path.join(d, "BOMCheck_V*.exe"))
            if re.search(r"BOMCheck_V\d+\.\d+\.\d+\.exe$", os.path.basename(p))]
    if not hits:
        return ""
    return max(hits, key=_key)


def require_exe(bin_dir=None):
    exe = find_exe(bin_dir)
    if not exe:
        raise SystemExit("找不到产物：先在仓库根目录跑 build_c.bat（%s）"
                         % (bin_dir or DEFAULT_DIR))
    return exe


if __name__ == "__main__":
    print(require_exe())
