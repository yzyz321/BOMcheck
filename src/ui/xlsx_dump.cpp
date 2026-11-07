#include "xlsx_dump.h"

#include <cstdio>
#include <string>

#include "bs_xlsx.h"

namespace {

void put_escaped(std::string &out, const char *s) {
    for (; s && *s; ++s) {
        switch (*s) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += *s; break;
        }
    }
}

void line(void (*emit)(const char *), const std::string &s) { emit(s.c_str()); }

}  // namespace

namespace bs {

int dump_xlsx(const char *path_utf8, void (*emit)(const char *)) {
    bs_err e;
    bs_workbook *wb;
    char buf[256];

    wb = bs_xlsx_open(path_utf8, &e);
    if (!wb) {
        line(emit, std::string("FAIL ") + e.msg);
        return 1;
    }

    std::snprintf(buf, sizeof(buf), "sheets=%d", bs_xlsx_sheet_count(wb));
    line(emit, buf);

    for (int i = 0; i < bs_xlsx_sheet_count(wb); ++i) {
        std::string s = "sheet";
        s += std::to_string(i);
        s += "=";
        s += bs_xlsx_sheet_name(wb, i);
        s += "|";
        s += bs_xlsx_sheet_part(wb, i);
        line(emit, s);
    }

    std::snprintf(buf, sizeof(buf), "sst=%d fonts=%d xfs=%d", bs_xlsx_sst_count(wb),
                  bs_xlsx_font_count(wb), bs_xlsx_xf_count(wb));
    line(emit, buf);

    {
        std::string s = "strike_fonts=";
        for (int i = 0; i < bs_xlsx_font_count(wb); ++i) {
            if (i) s += ",";
            s += std::to_string(bs_xlsx_font_strike(wb, i));
        }
        line(emit, s);
    }

    for (int i = 0; i < bs_xlsx_sheet_count(wb); ++i) {
        bs_grid *g = nullptr;
        if (bs_xlsx_load_sheet(wb, i, &g, &e) != 0) {
            std::snprintf(buf, sizeof(buf), "SHEETFAIL %d %s", i, e.msg);
            line(emit, buf);
            continue;
        }
        std::snprintf(buf, sizeof(buf), "--- %d ---", i);
        line(emit, buf);
        std::snprintf(buf, sizeof(buf),
                      "stats rows=%d cols=%d cells=%d nrows=%d strike=%d merges=%d",
                      g->max_row, g->max_col, g->n_cells, g->n_rows, g->n_strike,
                      g->n_merges);
        line(emit, buf);

        for (int k = 0; k < g->n_cells; ++k) {
            const bs_cell &c = g->cells[k];
            std::snprintf(buf, sizeof(buf), "cell %d %d %d ", c.row, c.col, (int)c.strike);
            std::string s = buf;
            put_escaped(s, g->strs + c.off);
            line(emit, s);
        }
        bs_grid_free(g);
    }

    bs_xlsx_close(wb);
    return 0;
}

}  // namespace bs
