#include "bench.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "bs_cfg.h"
#include "bs_mark.h"
#include "bs_pair.h"
#include "bs_sheet.h"
#include "bs_write.h"

namespace bs {
namespace {

/* 微秒级计时：GetTickCount64 的 15.6 ms 粒度对「配对 40 ms」这种量级完全不够看 */
long long now_us() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c = {};
    QueryPerformanceCounter(&c);
    return static_cast<long long>(c.QuadPart * 1000000LL / freq.QuadPart);
}

void kv(void (*emit)(const char *), const char *key, long long v) {
    char line[128];
    std::snprintf(line, sizeof(line), "bench %s=%lld", key, v);
    emit(line);
}

void kvs(void (*emit)(const char *), const char *key, const char *v) {
    char line[512];
    std::snprintf(line, sizeof(line), "bench %s=%s", key, v ? v : "");
    emit(line);
}

/* 毫秒保留一位小数：10 万行压测里 0.1 ms 的差异也要看得出来 */
void ms(void (*emit)(const char *), const char *key, long long us) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", us / 1000.0);
    kvs(emit, key, buf);
}

/* 一张表的载入任务。两张表互不依赖，各自解析各自的 workbook，
   core 里没有跨实例的可变全局状态（解压的定长树已改成就地建）。 */
struct LoadJob {
    const std::string *path;
    bs_sheet *sheet;
    bs_err err;
    int rc;
};

DWORD WINAPI load_proc(LPVOID p) {
    LoadJob *j = static_cast<LoadJob *>(p);
    bs_cfg cfg;
    bs_cfg_default(&cfg);
    j->err.msg[0] = 0;
    j->sheet = nullptr;
    j->rc = bs_sheet_open(j->path->c_str(), 0, &cfg, &j->sheet, &j->err);
    return 0;
}

}  // namespace

int run_bench(const std::string &old_path, const std::string &new_path,
              const std::string &out_dir, bool parallel, void (*emit)(const char *line)) {
    if (!emit) return 1;

    bs_cfg cfg;
    bs_cfg_default(&cfg);
    bs_err e;
    e.msg[0] = 0;
    bs_sheet *so = nullptr;
    bs_sheet *sn = nullptr;
    bs_pair *pairs = nullptr;
    size_t np = 0;
    bs_pairstat st;
    std::memset(&st, 0, sizeof(st));
    bs_result *res = nullptr;
    int rc = 1;

    const long long t0 = now_us();
    double inf_old = 0, par_old = 0, inf_new = 0, par_new = 0;
    double sh_old[4] = {0, 0, 0, 0};
    long long t1 = 0;

    if (parallel) {
        LoadJob jo = {&old_path, nullptr, {}, 1};
        LoadJob jn = {&new_path, nullptr, {}, 1};
        HANDLE h1 = CreateThread(nullptr, 0, load_proc, &jo, 0, nullptr);
        HANDLE h2 = CreateThread(nullptr, 0, load_proc, &jn, 0, nullptr);
        if (h1) WaitForSingleObject(h1, INFINITE);
        if (h2) WaitForSingleObject(h2, INFINITE);
        if (h1) CloseHandle(h1);
        if (h2) CloseHandle(h2);
        if (jo.rc != 0 || jn.rc != 0) {
            kvs(emit, "error", jo.rc != 0 ? jo.err.msg : jn.err.msg);
            if (jo.sheet) bs_sheet_close(jo.sheet);
            if (jn.sheet) bs_sheet_close(jn.sheet);
            return 1;
        }
        so = jo.sheet;
        sn = jn.sheet;
        t1 = now_us();
        /* 并行时 bs_prof_* 是两边一起累加的，分不清是谁的，索性不报分解值 */
    } else {
        bs_prof_reset();
        std::memset(bs_prof_sheet, 0, sizeof(bs_prof_sheet));
        if (bs_sheet_open(old_path.c_str(), 0, &cfg, &so, &e) != 0) {
            kvs(emit, "error", e.msg);
            return 1;
        }
        t1 = now_us();
        inf_old = bs_prof_inflate_ms;
        par_old = bs_prof_parse_ms;
        std::memcpy(sh_old, bs_prof_sheet, sizeof(sh_old));

        bs_prof_reset();
        std::memset(bs_prof_sheet, 0, sizeof(bs_prof_sheet));
        if (bs_sheet_open(new_path.c_str(), 0, &cfg, &sn, &e) != 0) {
            kvs(emit, "error", e.msg);
            bs_sheet_close(so);
            return 1;
        }
        inf_new = bs_prof_inflate_ms;
        par_new = bs_prof_parse_ms;
    }
    const long long t2 = now_us();

    if (bs_pair_rows(so, sn, &cfg, &pairs, &np, &st, &e) != 0) {
        kvs(emit, "error", e.msg);
        bs_sheet_close(so);
        bs_sheet_close(sn);
        return 1;
    }
    const long long t3 = now_us();

    if (bs_result_build(so, sn, pairs, np, &res, &e) != 0) {
        kvs(emit, "error", e.msg);
        bs_pair_free(pairs);
        bs_sheet_close(so);
        bs_sheet_close(sn);
        return 1;
    }
    const long long t4 = now_us();

    long long t5 = t4;
    if (!out_dir.empty()) {
        /* 与界面同一条写出路径：结果表 + 诊断页，标注不在这里做（那是另一份产物） */
        char path[1024];
        std::snprintf(path, sizeof(path), "%s\\bench_result.xlsx", out_dir.c_str());
        bs_diag dg[2];
        bs_diag_fill(&dg[0], so, old_path.c_str());
        bs_diag_fill(&dg[1], sn, new_path.c_str());
        char real[1024] = "";
        if (bs_write_result(path, res, dg, 2, real, sizeof(real), &e) != 0) {
            kvs(emit, "write_error", e.msg);
        } else {
            t5 = now_us();
            kvs(emit, "out", real);
        }
    }

    kv(emit, "rows_old", (long long)bs_sheet_row_count(so));
    kv(emit, "rows_new", (long long)bs_sheet_row_count(sn));
    kv(emit, "pairs", (long long)np);
    kv(emit, "paired_pn", st.n_pn);
    kv(emit, "paired_ref", st.n_ref);
    kv(emit, "paired_fuzzy", st.n_fuzzy);
    kv(emit, "added", st.n_added);
    kv(emit, "deleted", st.n_deleted);
    kv(emit, "diffs", (long long)bs_result_count(res));
    ms(emit, "load_old_ms", t1 - t0);
    ms(emit, "load_new_ms", t2 - t1);
    ms(emit, "inflate_ms", (long long)((inf_old + inf_new) * 1000));
    ms(emit, "xml_ms", (long long)((par_old + par_new) * 1000));
    {
        const char *names[4] = {"label", "header", "rowbuild", "dups"};
        char buf[32];
        for (int q = 0; q < 4; q++) {
            std::snprintf(buf, sizeof(buf), "sheet_%s_ms", names[q]);
            ms(emit, buf, (long long)((sh_old[q] + bs_prof_sheet[q]) * 1000));
        }
    }
    ms(emit, "pair_ms", t3 - t2);
    ms(emit, "result_ms", t4 - t3);
    ms(emit, "write_ms", t5 - t4);
    ms(emit, "total_ms", t5 - t0);
    rc = 0;

    bs_result_close(res);
    bs_pair_free(pairs);
    bs_sheet_close(so);
    bs_sheet_close(sn);
    return rc;
}

}  // namespace bs
