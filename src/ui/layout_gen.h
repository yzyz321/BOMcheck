// 由 tools/gen_layout_c.py 从 pyref/layout_spec.py 生成，请勿手改。
// 数据源：layout_spec.resolve(1080, 720) 与 resolve(1480, 1120) 两次采样。
#ifndef BS_LAYOUT_GEN_H
#define BS_LAYOUT_GEN_H

#include "layout.h"

namespace bs {

constexpr int kBaseLogicalW = 1080;
constexpr int kBaseLogicalH = 720;
constexpr int kMinLogicalW  = 980;
constexpr int kMinLogicalH  = 620;

// layout_spec.COLORS 的同源副本（ARGB 无关，统一 0xRRGGBB）
constexpr unsigned kColBg = 0xF4F6F8;
constexpr unsigned kColCard = 0xFFFFFF;
constexpr unsigned kColBorder = 0xE1E5EA;
constexpr unsigned kColText = 0x1F2328;
constexpr unsigned kColMuted = 0x6B7280;
constexpr unsigned kColHint = 0x8A9199;
constexpr unsigned kColAccent = 0x185FA5;
constexpr unsigned kColAccentDk = 0x124A80;
constexpr unsigned kColAccentBg = 0xE6F1FB;
constexpr unsigned kColAmber = 0xBA7517;
constexpr unsigned kColOk = 0x0F6E56;
constexpr unsigned kColErr = 0xA32D2D;
constexpr unsigned kColGrid = 0xEDEFF2;
constexpr unsigned kColRowAdd = 0xC6EFCE;
constexpr unsigned kColRowDel = 0xFFC7CE;
constexpr unsigned kColRowChg = 0xFFEB9C;
constexpr unsigned kColRowDup = 0xD9D2E9;

static const LayoutItem kLayoutItems[] = {
    {"hdr.title", 0, 20, 10, 180, 28, 0, 0, 0, 0, "label"},
    {"hdr.ver", 0, 206, 15, 78, 22, 0, 0, 0, 0, "badge"},
    {"hdr.sub", 0, 20, 38, 560, 18, 0, 0, 0, 0, "label"},
    {"hdr.dpi", 0, 740, 10, 320, 18, 1000, 0, 0, 0, "label"},
    {"hdr.hint", 0, 740, 32, 320, 16, 1000, 0, 0, 0, "label"},
    {"hdr.help", 0, 294, 15, 84, 22, 0, 0, 0, 0, "button"},
    {"hdr.line", 0, 0, 60, 1080, 1, 0, 0, 1000, 0, "line"},
    {"card.old", 1, 16, 72, 516, 152, 0, 0, 500, 0, "frame"},
    {"old.title", 1, 32, 84, 110, 22, 0, 0, 0, 0, "label"},
    {"old.hint", 1, 148, 87, 200, 18, 0, 0, 0, 0, "label"},
    {"old.path", 1, 32, 112, 396, 30, 0, 0, 500, 0, "entry"},
    {"old.browse", 1, 436, 112, 80, 30, 500, 0, 0, 0, "button"},
    {"old.sheetlab", 1, 32, 154, 60, 20, 0, 0, 0, 0, "label"},
    {"old.sheet", 1, 94, 150, 422, 28, 0, 0, 500, 0, "combo"},
    {"old.status", 1, 32, 186, 484, 20, 0, 0, 500, 0, "label"},
    {"card.new", 1, 548, 72, 516, 152, 500, 0, 500, 0, "frame"},
    {"new.title", 1, 564, 84, 110, 22, 500, 0, 0, 0, "label"},
    {"new.hint", 1, 680, 87, 200, 18, 500, 0, 0, 0, "label"},
    {"new.path", 1, 564, 112, 396, 30, 500, 0, 500, 0, "entry"},
    {"new.browse", 1, 968, 112, 80, 30, 1000, 0, 0, 0, "button"},
    {"new.sheetlab", 1, 564, 154, 60, 20, 500, 0, 0, 0, "label"},
    {"new.sheet", 1, 626, 150, 422, 28, 500, 0, 500, 0, "combo"},
    {"new.status", 1, 564, 186, 484, 20, 500, 0, 500, 0, "label"},
    {"opt.frame", 2, 16, 236, 1048, 92, 0, 0, 1000, 0, "frame"},
    {"opt.title", 2, 32, 246, 140, 20, 0, 0, 0, 0, "label"},
    {"opt.duplab", 2, 32, 282, 64, 18, 0, 0, 0, 0, "label"},
    {"opt.dup", 2, 100, 276, 140, 28, 0, 0, 0, 0, "combo"},
    {"opt.strikelab", 2, 252, 282, 64, 18, 0, 0, 0, 0, "label"},
    {"opt.strike", 2, 320, 276, 120, 28, 0, 0, 0, 0, "combo"},
    {"opt.dual", 2, 456, 276, 152, 28, 0, 0, 0, 0, "check"},
    {"opt.thrlab", 2, 618, 282, 64, 18, 0, 0, 0, 0, "label"},
    {"opt.thr", 2, 686, 276, 62, 28, 0, 0, 0, 0, "spin"},
    {"opt.cell", 2, 764, 276, 176, 28, 0, 0, 0, 0, "check"},
    {"opt.hint", 2, 32, 308, 1004, 16, 0, 0, 1000, 0, "label"},
    {"act.run", 3, 16, 336, 140, 44, 0, 0, 0, 0, "button"},
    {"act.export", 3, 168, 336, 120, 44, 0, 0, 0, 0, "button"},
    {"act.stop", 3, 300, 342, 88, 32, 0, 0, 0, 0, "button"},
    {"act.progress", 3, 410, 350, 480, 18, 0, 0, 1000, 0, "progress"},
    {"act.pct", 3, 900, 350, 60, 18, 1000, 0, 0, 0, "label"},
    {"act.state", 3, 964, 350, 100, 18, 1000, 0, 0, 0, "label"},
    {"res.frame", 4, 16, 388, 1048, 304, 0, 0, 1000, 1000, "frame"},
    {"res.tab1", 4, 28, 398, 92, 26, 0, 0, 0, 0, "tab"},
    {"res.tab2", 4, 126, 398, 88, 26, 0, 0, 0, 0, "tab"},
    {"res.count", 4, 840, 402, 212, 20, 1000, 0, 0, 0, "label"},
    {"res.table", 4, 28, 428, 1024, 224, 0, 0, 1000, 1000, "table"},
    {"res.legend", 4, 28, 658, 1024, 22, 0, 1000, 1000, 0, "legend"},
    {"res.log", 4, 28, 428, 1024, 224, 0, 0, 1000, 1000, "log"},
    {"sta.bar", 5, 0, 696, 1080, 24, 0, 1000, 1000, 0, "frame"},
    {"sta.text", 5, 16, 701, 1048, 18, 0, 1000, 1000, 0, "label"},
};

constexpr int kLayoutCount = 49;

}  // namespace bs

#endif  // BS_LAYOUT_GEN_H
