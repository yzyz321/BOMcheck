// BOM 对比工具 · C/C++ 版入口
// C0：窗口 / PerMonitorV2 DPI / 单份 manifest / 版本资源 / --version 与 --probe-dpi
// C7：布局表 / 卡片骨架 / OLE 拖放 / --dump-layout 与 --drop-test
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "app.h"
#include "bench.h"
#include "layout_doc.h"
#include "bs_version.h"
#include "selfcheck.h"
#include "utf8.h"
#include "xlsx_dump.h"

namespace {

HANDLE g_out = INVALID_HANDLE_VALUE;

/* GUI 子系统没有 stdout：先试继承来的句柄，再挂父控制台，最后开个新的 */
void init_output() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
        g_out = h;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
        return;
    }
    if (AllocConsole()) {
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    }
}

void emit(const char *line) {
    if (!line) return;
    if (g_out == INVALID_HANDLE_VALUE) init_output();
    if (g_out == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(g_out, line, (DWORD)std::strlen(line), &n, nullptr);
    WriteFile(g_out, "\r\n", 2, &n, nullptr);
}

bool has_arg(int argc, wchar_t **argv, const wchar_t *a, const wchar_t *b = nullptr) {
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], a) == 0) return true;
        if (b && std::wcscmp(argv[i], b) == 0) return true;
    }
    return false;
}

/* 取某个开关后面跟着的位置参数（到下一个 -- 开头为止） */
std::vector<std::wstring> args_after(int argc, wchar_t **argv, const wchar_t *flag) {
    std::vector<std::wstring> out;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], flag) != 0) continue;
        for (int j = i + 1; j < argc; ++j) {
            if (argv[j][0] == L'-' && argv[j][1] == L'-') break;
            out.push_back(argv[j]);
        }
        break;
    }
    return out;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int nCmdShow) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    const bool want_version = has_arg(argc, argv, L"--version", L"-v");
    const bool want_probe = has_arg(argc, argv, L"--probe-dpi");
    const bool want_self = has_arg(argc, argv, L"--self-check");
    const bool want_layout = has_arg(argc, argv, L"--dump-layout");
    const std::vector<std::wstring> doc_arg = args_after(argc, argv, L"--export-layout-doc");
    const bool want_drag_over = has_arg(argc, argv, L"--drag-over");
    const std::vector<std::wstring> size_arg = args_after(argc, argv, L"--client-size");
    const std::vector<std::wstring> dump_arg = args_after(argc, argv, L"--xlsx-dump");
    const std::vector<std::wstring> drop_files = args_after(argc, argv, L"--drop-test");
    const bool want_drop_test = !drop_files.empty();
    const std::vector<std::wstring> write_arg = args_after(argc, argv, L"--write-result");
    const std::vector<std::wstring> write_all_arg = args_after(argc, argv, L"--write-all");
    const std::vector<std::wstring> ui_run_arg = args_after(argc, argv, L"--ui-run");
    const std::vector<std::wstring> bench_arg = args_after(argc, argv, L"--bench");
    const std::vector<std::wstring> out_dir_arg = args_after(argc, argv, L"--out-dir");
    /* 切工作表探针：--pick-sheet old=2,new=2（走的是点下拉菜单同一条路） */
    const std::vector<std::wstring> pick_arg = args_after(argc, argv, L"--pick-sheet");
    const std::vector<std::wstring> click_arg = args_after(argc, argv, L"--click");
    const bool want_table = has_arg(argc, argv, L"--dump-table", L"--table-dump");
    const bool want_scroll = has_arg(argc, argv, L"--table-scroll");
    const bool want_tab_log = has_arg(argc, argv, L"--tab-log");
    const bool want_stay = has_arg(argc, argv, L"--stay");      // 探针跑完不退出（截图用）
    const bool want_show = has_arg(argc, argv, L"--visible");   // 有探针也把窗口显示出来

    if (!doc_arg.empty()) {
        /* 布局自述文件：不需要窗口，导完就退（打包时生成的对照表就走这条） */
        const std::string err = bs::export_layout_doc(doc_arg[0], 96);
        if (err.empty())
            emit(("layout-doc " + bs::to_utf8(doc_arg[0])).c_str());
        else
            emit(("ERROR: 布局文档导出失败：" + err).c_str());
        if (argv) LocalFree(argv);
        return err.empty() ? 0 : 1;
    }

    if (want_version || want_probe || want_self || want_layout || !dump_arg.empty() ||
        !ui_run_arg.empty() || want_table || want_scroll || !bench_arg.empty())
        init_output();

    /* 纯计算基准：不建窗口，直接量 core 各段耗时（C9 压测用） */
    if (!bench_arg.empty()) {
        if (bench_arg.size() < 2) {
            emit("ERROR: --bench 需要 <旧.xlsx> <新.xlsx> [输出目录]");
            if (argv) LocalFree(argv);
            return 2;
        }
        const std::string out_dir =
            bench_arg.size() >= 3 ? bs::to_utf8(bench_arg[2]) : std::string();
        /* 默认并行载入；--serial 用来取分段耗时（并行时两张表的计数混在一起） */
        const bool serial = has_arg(argc, argv, L"--serial");
        const int rc = bs::run_bench(bs::to_utf8(bench_arg[0]), bs::to_utf8(bench_arg[1]),
                                     out_dir, !serial, &emit);
        if (argv) LocalFree(argv);
        return rc;
    }

    if (want_version) {
        char line[96];
        std::snprintf(line, sizeof(line), "BOMCheck %s", bs_version());
        emit(line);
        std::snprintf(line, sizeof(line), "build %s", bs_build_stamp());
        emit(line);
        if (argv) LocalFree(argv);
        return 0;
    }

    /* 读取链自检：不需要窗口 / OLE */
    if (!dump_arg.empty()) {
        int rc = bs::dump_xlsx(bs::to_utf8(dump_arg[0]).c_str(), &emit);
        if (argv) LocalFree(argv);
        return rc;
    }

    if (want_self) {
        int rc = bs::run_self_check(&emit);
        if (argv) LocalFree(argv);
        return rc;
    }

    /* OLE 拖放需要先 OleInitialize；顺便让文件对话框也能用现代样式 */
    const HRESULT hr_ole = OleInitialize(nullptr);

    bs::App app(inst);
    const bool quiet = (want_probe || want_layout || !ui_run_arg.empty() || want_table ||
                        want_scroll || !click_arg.empty());
    const int show = (quiet && !want_show) ? SW_HIDE : nCmdShow;
    if (!app.create(show)) {
        emit("ERROR: 窗口创建失败");
        if (SUCCEEDED(hr_ole)) OleUninitialize();
        if (argv) LocalFree(argv);
        return 1;
    }

    if (!size_arg.empty()) {
        int cw = 0, ch = 0;
        if (std::swscanf(size_arg[0].c_str(), L"%dx%d", &cw, &ch) == 2)
            app.set_client_size(cw, ch);
        else
            emit("ERROR: --client-size 需要 宽x高，例如 980x620");
    }

    if (want_layout) {
        app.dump_layout(&emit);
        if (SUCCEEDED(hr_ole)) OleUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }

    /* ③ 比对选项：--dual-key 0|1 关掉位号兜底，--min-overlap 0.65 改近似阈值。
       要在 --drop-test 之前设，否则载入时已经按默认值算过一遍。 */
    {
        const std::vector<std::wstring> dual_arg = args_after(argc, argv, L"--dual-key");
        if (!dual_arg.empty())
            app.set_dual_key(dual_arg[0] != L"0" && dual_arg[0] != L"off" &&
                             dual_arg[0] != L"false");
        const std::vector<std::wstring> ov_arg = args_after(argc, argv, L"--min-overlap");
        if (!ov_arg.empty()) {
            double v = wcstod(ov_arg[0].c_str(), nullptr);
            if (v > 0.0) app.set_ref_min_overlap(v);
        }
        /* --row-diff-all 0|1：整行逐列比较（界面上是「整行逐列比较」复选框） */
        const std::vector<std::wstring> cell_arg = args_after(argc, argv, L"--row-diff-all");
        if (!cell_arg.empty())
            app.set_row_diff_all(cell_arg[0] != L"0" && cell_arg[0] != L"off" &&
                                 cell_arg[0] != L"false");
    }

    /* 帮助落点：只报路径与文件在不在，不真去开浏览器（回归用） */
    if (has_arg(argc, argv, L"--help-probe")) {
        std::wstring path;
        bool exists = false;
        app.help_target(path, exists);
        char utf[600] = "";
        const std::string u = bs::to_utf8(path);
        std::snprintf(utf, sizeof(utf), "%s", u.c_str());
        char line[700];
        std::snprintf(line, sizeof(line), "help path=%s exists=%d", utf, exists ? 1 : 0);
        emit(line);
        if (SUCCEEDED(hr_ole)) OleUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }

    /* 切工作表：old=2,new=2 这种写法，可以一次切一侧也可以两侧都切。
       切完把运行日志吐出来 —— 换表有没有重新配对，看日志里的「配对 N 对」。
       封装成 lambda 是因为 --ui-run 也要用：那边 drop 完就自己比了，
       不在这里再切一次，--pick-sheet 会被「多表自动逐表」盖掉。 */
    auto pick_sheets = [&]() {
        if (pick_arg.empty()) return;
        const std::string spec = bs::to_utf8(pick_arg[0]);
        size_t pos = 0;
        while (pos <= spec.size()) {
            const size_t comma = spec.find(',', pos);
            const std::string one = spec.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const size_t eq = one.find('=');
            if (eq != std::string::npos) {
                const std::string who = one.substr(0, eq);
                const std::string val = one.substr(eq + 1);
                const int slot = (who == "new") ? 1 : 0;
                /* old=2 → 第 2 张表（0 起算）；old=all → 下拉第一项「全部工作表」 */
                const bool all = (val.find("all") != std::string::npos);
                int idx = 0;
                for (char ch : val) {
                    if (ch >= '0' && ch <= '9') idx = idx * 10 + (ch - '0');
                }
                app.pick_sheet_test(slot, all ? 0 : idx + 1);
                char buf[64];
                if (all)
                    std::snprintf(buf, sizeof(buf), "pick-sheet %s=all", who.c_str());
                else
                    std::snprintf(buf, sizeof(buf), "pick-sheet %s=%d", who.c_str(), idx);
                emit(buf);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        app.dump_log(&emit);
    };

    if (want_drop_test) app.drop_test(drop_files);
    /* 逐表比对模式（等价于在下拉里选「全部工作表」） */
    if (has_arg(argc, argv, L"--all-sheets")) app.set_all_sheets(true);
    if (has_arg(argc, argv, L"--open-menu")) app.menu_test();
    if (want_drop_test) app.report_drop_state(&emit);
    pick_sheets();

    if (!write_arg.empty()) {
        std::wstring got, err;
        if (app.write_result(write_arg[0], got, err)) {
            const std::string line = "write ok=1 path=" + bs::to_utf8(got);
            emit(line.c_str());
        } else {
            const std::string line = "write ok=0 err=" + bs::to_utf8(err);
            emit(line.c_str());
        }
    }
    if (!write_all_arg.empty()) {
        const bs::App::WriteAllResult wa = app.write_all(write_all_arg[0]);
        if (wa.ok) {
            emit(("write-all ok=1 res=" + bs::to_utf8(wa.res)).c_str());
            emit(("write-all old=" + bs::to_utf8(wa.old_mark)).c_str());
            emit(("write-all new=" + bs::to_utf8(wa.new_mark)).c_str());
        } else {
            emit(("write-all ok=0 err=" + bs::to_utf8(wa.err)).c_str());
        }
    }
    /* C7：--ui-run 走的就是「开始比对」按钮那条路（drop → 配对 → 写出 → 日志），
       区别只是输出目录可指定、不弹目录选择框 */
    if (!ui_run_arg.empty()) {
        std::vector<std::wstring> files = ui_run_arg;
        std::wstring dir;
        if (files.size() >= 3) {
            dir = files[2];
            files.resize(2);
        }
        app.set_interactive(false);
        if (!dir.empty()) app.set_out_dir(dir);
        app.drop_test(files);
        pick_sheets();                   /* 落文件后「多表自动逐表」已生效，这里再钉一次 */
        const bool ok = app.run_compare();
        emit(ok ? "ui-run ok=1" : "ui-run ok=0");
        app.dump_log(&emit);
    }
    if (!out_dir_arg.empty()) {
        app.set_interactive(false);
        app.set_out_dir(out_dir_arg[0]);
    }
    if (!click_arg.empty()) {
        /* 支持 --click a,b,c：按顺序各点一下（页签 → 页签 → 按钮 这样串起来） */
        std::string ids = bs::to_utf8(click_arg[0]);
        size_t pos = 0;
        while (pos <= ids.size()) {
            const size_t comma = ids.find(',', pos);
            const std::string one = ids.substr(pos, comma == std::string::npos ? std::string::npos
                                                                              : comma - pos);
            if (!one.empty()) {
                app.click_layout(one.c_str());
                emit(("click " + one).c_str());
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        app.dump_log(&emit);
    }
    if (want_tab_log) app.set_tab(1);
    if (want_table) app.dump_table(&emit);
    if (want_scroll) app.table_scroll_probe(&emit);
    if (want_drag_over) app.drag_over_test();  // 先落文件再点亮，否则悬停态被 on_drag_leave 清掉
    if (argv) LocalFree(argv);

    /* 这几条都是「跑一遍就退」的探针：不能落到下面的消息循环里 */
    if (!want_stay &&
        (want_probe || !ui_run_arg.empty() || want_table || want_scroll || !click_arg.empty())) {
        if (want_probe) app.report_metrics(&emit);
        if (SUCCEEDED(hr_ole)) OleUninitialize();
        return 0;  // 析构里销毁窗口
    }

    const int rc = app.run();
    if (SUCCEEDED(hr_ole)) OleUninitialize();
    return rc;
}
