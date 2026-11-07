# -*- coding: utf-8 -*-
"""样本文件的统一入口。

样本以前放在 temp/ —— 那是用户随手放当次 BOM 的临时目录，用户清理它完全正常，
于是「清一次 temp 就把整条构建链打红」。现在样本跟项目一起走，放在 tests/data：

    tests/data/*.xlsx         真实 BOM 样本（读取 / 识别 / 配对 / 标注对账用）
    tests/data/cases/*.xlsx   构造出来的边角样本

调用方只写相对名：samples.p("cases", "pair_old.xlsx")。tests/data 不存在时
退回 temp/（老布局）；两边都没有就返回 tests/data 下的路径，报错信息里能看出该放哪。
"""
import glob
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tests", "data")
TEMP = os.path.join(ROOT, "temp")


def base():
    """样本根目录：有 tests/data 就用它，否则退回 temp。"""
    return DATA if os.path.isdir(DATA) else TEMP


def p(*parts):
    """样本路径，可传多段：p("cases", "pair_old.xlsx")。"""
    for root in (DATA, TEMP):
        cand = os.path.join(root, *parts)
        if os.path.exists(cand):
            return cand
    return os.path.join(DATA, *parts)


def xlsx(*parts):
    """样本目录下的 xlsx 列表；parts 传子目录或不传。"""
    return sorted(glob.glob(os.path.join(base(), *(parts + ("*.xlsx",)))))
