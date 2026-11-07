/* 布局自述文件导出（C8）。

   为什么要程序自己导：文档最怕「改了布局忘了改文档」。这里直接读 kLayoutItems
   并用 layout_rect() 现算，导出的表格天然等于程序此刻的行为；版本号也来自
   bs_version()，不会出现「文档写 V4.0.1、程序是 4.0.0」。

   输出是 UTF-8 的 Markdown：Windows 上双击用记事本/VS Code 都能看，
   git diff 也读得懂。 */
#include "layout_doc.h"

#include <cstdio>
#include <cstring>

#include "bs_version.h"
#include "layout.h"
#include "layout_gen.h"   // 颜色常量与 kLayoutItems 都在这里（生成文件）

namespace bs {
namespace {

const char *kSectionName(int sec) {
    switch (sec) {
        case 0: return "头部";
        case 1: return "文件卡";
        case 2: return "选项";
        case 3: return "操作区";
        case 4: return "结果区";
        case 5: return "状态栏";
        default: return "其他";
    }
}

const char *kTypeName(const char *type) {
    if (!type) return "-";
    if (!std::strcmp(type, "label")) return "静态文字";
    if (!std::strcmp(type, "badge")) return "版本徽标";
    if (!std::strcmp(type, "line")) return "分隔线";
    if (!std::strcmp(type, "frame")) return "卡片/框";
    if (!std::strcmp(type, "entry")) return "输入框";
    if (!std::strcmp(type, "combo")) return "下拉框";
    if (!std::strcmp(type, "button")) return "按钮";
    if (!std::strcmp(type, "check")) return "复选框";
    if (!std::strcmp(type, "spin")) return "数字框";
    if (!std::strcmp(type, "progress")) return "进度条";
    if (!std::strcmp(type, "tab")) return "页签";
    if (!std::strcmp(type, "table")) return "表格";
    if (!std::strcmp(type, "log")) return "日志";
    return type;
}

int scale(int v, int dpi) { return (v * dpi + 48) / 96; }

}  // namespace

std::string export_layout_doc(const std::wstring &path, int dpi) {
    if (path.empty()) return "没有给输出路径";
    if (dpi < 96) dpi = 96;

    std::FILE *f = _wfopen(path.c_str(), L"wb");
    if (!f) return "打不开输出文件（目录不存在或无写权限）";

    std::string s;
    char buf[512];

    std::snprintf(buf, sizeof(buf),
                  "# GUI 控件位置对照表（V%s）\n\n"
                  "> 本文件由 `BOMCheck_V%s.exe --export-layout-doc <路径>` 直接导出，"
                  "坐标取自程序正在使用的布局表，不是手抄。\n"
                  "> 改动界面后重新导出即可，不会出现「文档与程序对不上」。\n\n",
                  bs_version(), bs_version());
    s += buf;

    std::snprintf(buf, sizeof(buf),
                  "- 基准客户区：**%d × %d** 逻辑像素（96 DPI 下的 1:1 尺寸）\n"
                  "- 最小窗口：%d × %d；再小就按最小值夹住\n"
                  "- 拉伸规则：坐标 = 基准 + (当前尺寸 − 基准) × 拉伸系数；"
                  "系数为 0 表示钉死在左侧/顶部\n"
                  "- 本文档的「%d%%」列按当前显示器 DPI 换算出的物理像素给出\n"
                  "- 合计 **%d** 个控件\n\n",
                  kBaseLogicalW, kBaseLogicalH, kMinLogicalW, kMinLogicalH,
                  dpi * 100 / 96, layout_count());
    s += buf;

    s += "## 坐标总表\n\n";
    s += "| # | id | 区域 | 类型 | x | y | 宽 | 高 | 拉伸 (gx, gy, gw, gh) | ";
    std::snprintf(buf, sizeof(buf), "%d%% 像素 |\n", dpi * 100 / 96);
    s += buf;
    s += "|---|---|---|---|---|---|---|---|---|---|\n";

    int last_sec = -1;
    const int n = layout_count();
    for (int i = 0; i < n; ++i) {
        const LayoutItem *it = layout_at(i);
        if (!it) continue;
        if (last_sec >= 0 && it->sec != last_sec)
            s += "| | | | | | | | | | |\n";  // 区域之间留一行空行，肉眼好分块
        last_sec = it->sec;

        const RECT r = layout_rect(it, 0, 0);
        std::snprintf(buf, sizeof(buf),
                      "| %d | `%s` | %s | %s | %d | %d | %d | %d | %d, %d, %d, %d | "
                      "%d × %d（%d,%d） |\n",
                      i + 1, it->id, kSectionName(it->sec), kTypeName(it->type),
                      it->x, it->y, it->w, it->h, it->gx, it->gy, it->gw, it->gh,
                      scale(r.right - r.left, dpi), scale(r.bottom - r.top, dpi),
                      scale(r.left, dpi), scale(r.top, dpi));
        s += buf;
    }

    s += "\n## 三种窗口尺寸下的实际矩形\n\n";
    s += "同一套控件在最小 / 基准 / 最大化三种客户区下的位置，用于核对拉伸是否合理。\n\n";
    s += "| id | 最小 " "| 基准 " "| 大窗 |\n|---|---|---|---|\n";
    const int sizes[3][2] = {{kMinLogicalW, kMinLogicalH},
                             {kBaseLogicalW, kBaseLogicalH},
                             {1480, 1120}};
    for (int i = 0; i < n; ++i) {
        const LayoutItem *it = layout_at(i);
        if (!it) continue;
        std::snprintf(buf, sizeof(buf), "| `%s` |", it->id);
        s += buf;
        for (int k = 0; k < 3; ++k) {
            const RECT r = layout_rect(it, sizes[k][0], sizes[k][1]);
            std::snprintf(buf, sizeof(buf), " %d,%d %d×%d |", (int)r.left, (int)r.top,
                          (int)(r.right - r.left), (int)(r.bottom - r.top));
            s += buf;
        }
        s += "\n";
    }

    s += "\n## 颜色\n\n";
    struct Swatch {
        const char *name;
        unsigned rgb;
    };
    const Swatch sw[] = {
        {"窗口底", kColBg},        {"卡片", kColCard},      {"描边", kColBorder},
        {"正文", kColText},        {"次要文字", kColMuted}, {"提示文字", kColHint},
        {"主色", kColAccent},      {"主色按下", kColAccentDk}, {"主色浅底", kColAccentBg},
        {"警示", kColAmber},       {"成功", kColOk},        {"错误", kColErr},
        {"网格线", kColGrid},      {"新增行", kColRowAdd},  {"删除行", kColRowDel},
        {"变更行", kColRowChg},    {"重复料号", kColRowDup},
    };
    s += "| 用途 | 色值 | 预览 |\n|---|---|---|\n";
    for (const Swatch &c : sw) {
        std::snprintf(buf, sizeof(buf),
                      "| %s | `#%06X` | <span style=\"display:inline-block;width:48px;"
                      "height:14px;background:#%06X;border:1px solid #C8CDD4\"></span> |\n",
                      c.name, c.rgb, c.rgb);
        s += buf;
    }

    s += "\n---\n\n";
    std::snprintf(buf, sizeof(buf), "构建：%s　程序版本 V%s\n", bs_build_stamp(),
                  bs_version());
    s += buf;

    const size_t wrote = std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    if (wrote != s.size()) return "写入不完整（磁盘满？）";
    return std::string();
}

}  // namespace bs
