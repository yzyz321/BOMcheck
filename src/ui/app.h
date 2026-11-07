#ifndef BS_APP_H
#define BS_APP_H

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "bs_pair.h"
#include "card.h"
#include "drop.h"
#include "paint.h"
#include "table.h"
#include "table_src.h"

namespace bs {

/* 写出口的开关：逐表比对要「一张文件装下所有表」，每涂完一张表，就以这一轮
   的产物为源接着涂下一张（src_old/src_new 非空时覆盖 slots_[x].path）。
   放在 App 外面是因为 GCC 不允许「嵌套类 + 默认实参用 NSDMI」这套组合。 */
struct WriteOpts {
    std::wstring res_tag;    // 差异结果文件名后缀（逐表：_<表名>）
    std::wstring mark_tag;   // 标注文件名后缀（逐表累积时是临时名，最后一轮才是正式名）
    std::wstring src_old;    // 空 = 用 slots_[0].path
    std::wstring src_new;    // 空 = 用 slots_[1].path
    bool write_res = true;
    bool write_marks = true;
};

class App : public DropSink {
public:
    explicit App(HINSTANCE inst);
    ~App();

    /* 写出过程中的进度回调：写三份文件时按步骤点亮进度条与日志。
       传 nullptr 就是纯探测路径（--write-all 用），不做任何界面动作。 */
    struct WriteStepSink {
        virtual ~WriteStepSink() {}
        virtual void on_writing(int pct, const wchar_t *what) = 0;
    };

    /* nCmdShow 传 SW_HIDE 时就只建窗不上屏（给 --probe / --dump-layout 用） */
    bool create(int nCmdShow);
    int run();

    /* 把 DPI / 感知级别 / 窗口尺寸交给回调，供脚本核对（不依赖 CRT 的 stdout） */
    void report_metrics(void (*emit)(const char *line)) const;
    /* 打印布局表的实际矩形（逻辑像素），供 tools/check_layout.py 与 Python 表对账 */
    void dump_layout(void (*emit)(const char *line)) const;
    /* 把客户区设成指定逻辑尺寸（--client-size 用），用于核对小/大尺寸下的排布 */
    void set_client_size(int lw, int lh);
    /* 打印两侧卡片状态（路径 / 是否通过校验 / 是否同一文件），--drop-test 后自证用 */
    void report_drop_state(void (*emit)(const char *line)) const;

    /* 测试钩子：绕过 OLE 直接走一遍拖放落点逻辑（--drop-test 用），
       内部调用的是与真拖放完全相同的 on_drop() */
    void apply_drop(const std::vector<std::wstring> &files, POINT client_pt);
    /* 按「落在旧卡片中心」跑一次，供截图脚本用 */
    void drop_test(const std::vector<std::wstring> &files);
    /* 只把拖放悬停态点亮（不落文件），用于核对高亮样式 */
    void drag_over_test();
    /* 直接把旧卡片的工作表下拉弹开（--open-menu），用于截图与自检 */
    void menu_test() { open_sheet_menu(0); }
    /* 切工作表（--pick-sheet old=2,new=2），走的是与点下拉菜单完全相同那条路 */
    void pick_sheet_test(int slot, int item) { pick_sheet(slot, item); }

    /* 把 12 列结果写进 xlsx（C5）。返回实际写入的路径，失败时 err 里是原因。
       目前只由 --write-result 调用；C6/C7 接自动写出时复用同一个入口。 */
    bool write_result(const std::wstring &path, std::wstring &out_path, std::wstring &err) const;

    /* 一次写三份（C6）：差异结果 + 旧标注 + 新标注，文件名与 bomcore.write_outputs
       一致。C7 的「开始比对」按钮点下去就是调这个。 */
    struct WriteAllResult {
        std::wstring res, old_mark, new_mark;
        std::wstring err;
        bool ok = false;
    };
    /* 逐表比对要「一张文件装下所有表」：每涂完一张表，就以这一轮的产物为源
       接着涂下一张（src_old/src_new 非空时覆盖 slots_[x].path）。所以这里能
       分别指定两个源文件、两个文件名后缀，以及这一轮要写哪几份。 */
    WriteAllResult write_all(const std::wstring &dir, WriteStepSink *sink = nullptr,
                             const WriteOpts &opt = WriteOpts()) const;

    /* ── C7 接线后的对外钩子 ─────────────────────────────────────
       run_compare() 是「开始比对 / 开始查重」按钮的同一条路径：写出三份（或
       查重标注），把每一步写进运行日志并推进进度条。探针与真点击共用它。 */
    bool run_compare();
    /* 逐表比对：两侧同名的工作表各比一遍，每张表各出一组文件（文件名带表名）。
       两侧都没有数据行的表跳过，日志里写一行说明。 */
    bool run_compare_all(const std::wstring &dir, WriteStepSink *sink);
    /* 逐表模式开关（工作表下拉的第一项「全部工作表」= 开） */
    void set_all_sheets(bool on);
    bool all_sheets() const { return all_sheets_; }
    /* 两侧都是多表工作簿、且有 ≥2 张同名表时自动进逐表模式：用户拖两个多页
       BOM 进来，默认就该全比 ——「只比一张」是 2.0 的老行为，2026-11-05 连报
       两次「多页没比」其实都是人以为已经全比、实际还停在单表模式。
       在下拉里显式选中某张表就关掉，不会绑死。 */
    void auto_all_sheets();

    /* ── ③ 比对选项 ─────────────────────────────────────────────
       双键（位号兜底）与近似阈值可以改；改完立刻重算并写日志。
       重复料号 / 删除线范围这两项仍按推荐值固定执行（dup_policy=SEQ、
       strike_scope=整行），界面上画成只读，不假装能点。 */
    void set_dual_key(bool on);
    void set_ref_min_overlap(double v);
    /* 整行逐列比较：除料号/位号/数量/删除线之外的列也比，任一格不同即算变更 */
    void set_row_diff_all(bool on);
    bool row_diff_all() const { return cfg_.row_diff_all != 0; }
    bool dual_key() const { return cfg_.dual_key != 0; }
    double ref_min_overlap() const { return cfg_.ref_min_overlap; }
    /* 帮助按钮：打开 exe 同目录的「用户使用手册.html」；文件不在时弹一段
       双键 / 阈值 / 逐列比较的简要说明。help_target 给探针用（不真开浏览器）。 */
    void help_target(std::wstring &path, bool &exists) const;
    void open_help();
    /* 输出目录：默认 <旧文件目录>\checkresults；不可写时（交互模式）弹目录选择 */
    void set_out_dir(const std::wstring &dir) { out_dir_ = dir; }
    void set_interactive(bool on) { interactive_ = on; }
    /* 页签：0 差异结果 / 1 运行日志 */
    void set_tab(int tab);
    /* 把结果表按界面口径导出（TSV + 行底色），供 tools/check_ui.py 对账 */
    void dump_table(void (*emit)(const char *line)) const;
    /* 运行日志原文（不含时间戳），探针与日志页共用同一份 */
    void dump_log(void (*emit)(const char *line)) const;
    const std::vector<std::wstring> &log_lines() const { return log_; }
    bool dup_mode() const { return dup_mode_; }
    /* 真点一下布局表里的控件（--click act.run）：走的是 hit 命中 → 分支那条路 */
    void click_layout(const char *id);
    /* 表格探针：横向滚到最右再读回冻结列是否还在（--table-check） */
    void table_scroll_probe(void (*emit)(const char *line));

    void on_drag_over(POINT client_pt, int file_count) override;
    void on_drag_leave() override;
    void on_drop(const std::vector<std::wstring> &files, POINT client_pt) override;

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    void apply_dpi(int dpi);
    void make_fonts();
    void drop_fonts();
    void paint(HWND hwnd);

    void paint_header(const UiCtx &ui);
    void paint_options(const UiCtx &ui);
    void paint_actions(const UiCtx &ui);
    void paint_results(const UiCtx &ui);
    void paint_status(const UiCtx &ui);

    UiCtx make_ctx(HDC dc) const;
    int scale(int logical) const;
    int to_logical(int px) const;
    int client_lw() const;   // 客户区逻辑宽
    int client_lh() const;   // 客户区逻辑高
    int hit_card(int lx, int ly) const;    // 0 旧 / 1 新 / -1 空白
    int drop_target(int lx, int ly) const; // 落点解析：空白处按「先填空的那侧」兜底
    int hit_browse(int lx, int ly) const;  // 同上
    int hit_sheet(int lx, int ly) const;   // 工作表下拉的点击区
    void set_slot(int idx, const std::wstring &path);
    /* 两张表并行载入：一次拖两个文件、以及 --ui-run 走的就是这条 */
    void load_pair(const std::wstring &old_path, const std::wstring &new_path);
    static DWORD WINAPI slot_load_proc(LPVOID p);
    /* 把「载入结果写进运行日志」单独抽出来，并行载入完成后统一打 */
    void note_slot(int idx);
    void browse(int idx);
    void refresh_status();
    /* 两份都分析完成时跑一遍四阶段配对（结果只留统计，12 列留给 C5 写出） */
    void recompute_pairs();

    /* ── C7：结果表 / 日志 / 进度 / 页签 ─────────────────────── */
    void create_children();
    void layout_children();          // 按当前客户区逻辑尺寸摆子窗口
    void make_table();               // 按当前模式重建数据源与列集
    void switch_tab(int tab, bool redraw = true);
    void append_log(const std::wstring &line);
    void refresh_log_view();
    void set_progress(int pct, const wchar_t *state);
    void set_busy(bool busy);
    bool resolve_out_dir(std::wstring &dir, std::wstring &err);
    void run_dups();                 // 两侧同一文件：解析目录 → write_dups
    /* 查重标注写到指定目录（「另存结果…」在查重模式下用它）。
       返回是否真的写出了一个文件；无重复时不写、返回 false。 */
    bool write_dups(const std::wstring &dir);
    wchar_t run_label_[24] = L"开始比对";
    bool can_run() const;
    int hit_action(int lx, int ly) const;   // 1 开始 / 2 另存 / 3 停止 / 4/5 页签
    /* ③ 选项区：1 = 双键复选框，2 = 近似阈值框，0 = 都不在 */
    int hit_options(int lx, int ly) const;
    /* 改完选项的统一收尾：重算配对 → 刷新状态栏 → 重画 */
    void after_option_change(const std::wstring &what);
    /* 阈值按档位走：0.40 / 0.50 / … / 0.90，到头回到 0.40（一个点击目标也能
       往两个方向调，不用再摆一对小箭头） */
    void click_threshold();
    /* 滚轮微调：±0.05，夹在 0.30–0.95 */
    void nudge_threshold(int dir);

    /* 工作表下拉：自绘弹出窗（跟主界面同一套字体与配色），
       不用系统菜单，免得一灰一白跟卡片对不上 */
    void open_sheet_menu(int idx);
    void close_sheet_menu();
    void pick_sheet(int slot, int item);
    int menu_item_at(int y) const;
    LRESULT menu_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK menu_proc_entry(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    HFONT f_h1_ = nullptr;
    HFONT f_h3_ = nullptr;
    HFONT f_body_ = nullptr;
    HFONT f_small_ = nullptr;
    wchar_t title_[96] = L"";
    wchar_t status_[192] = L"";
    bool status_warn_ = false;   // 状态栏是否用提醒色（如两侧同文件）

    FileSlot slots_[2];
    int hot_browse_ = -1;
    int hot_sheet_ = -1;
    bool ole_ok_ = false;

    /* 配对缓存：只在文件变化时重算，状态栏与探针都读它 */
    bs_pairstat pstat_;
    /* 比对选项（③ 区）。构造时 bs_cfg_default 填默认值，之后只由
       set_dual_key / set_ref_min_overlap 改，recompute_pairs 直接读它。 */
    bs_cfg cfg_;
    bool has_pairs_ = false;
    int n_res_ = 0;
    std::wstring pair_note_;     // 配对失败时的原因（严格模式等）
    std::wstring done_note_;     // 「已写出 3 份…」这类收尾说明，换文件时清空
    /* 写出口要吃 12 列，C6 标注还要按 pair 定位原表行，所以两者都留着；
       文件换掉时一起释放，峰值就是一次结果表的字符串量。 */
    bs_result *res_ = nullptr;
    bs_pair *pairs_ = nullptr;
    size_t n_pairs_ = 0;

    HWND menu_ = nullptr;      // 工作表下拉（弹出窗）
    int menu_slot_ = -1;       // 属于哪张卡片
    int menu_hover_ = -1;
    int menu_err_ = 0;         // 下拉创建失败时的 GetLastError，自检用
    int menu_press_ = -1;      // 按下时落在哪一项（松开时同项才算选中）
    bool menu_closing_ = false;

    /* ── C7 子窗口 ─────────────────────────────────────────── */
    Table table_;
    HWND log_hwnd_ = nullptr;       // 运行日志（只读多行 EDIT）
    HWND prog_ = nullptr;      // 进度条
    HFONT f_mono_ = nullptr;   // 日志用等宽字体
    int tab_ = 0;              // 0 差异结果 / 1 运行日志
    int progress_ = 0;
    wchar_t run_state_[32] = L"就绪";
    bool busy_ = false;
    int batch_ = 0;            // 一次拖两份时先都不重算，落完再算一遍
    bool interactive_ = true;
    bool dup_mode_ = false;    // 两侧同一文件 → 查重模式（8 列）
    bool all_sheets_ = false;  // 逐表比对模式（下拉里的「全部工作表」）
    bool auto_all_done_ = false;  // 本次载入已经做过「自动开逐表」的判断
    std::wstring out_dir_;     // --ui-run 指定的输出目录（空 = 用默认 checkresults）
    std::unique_ptr<DiffSource> diff_src_;
    std::unique_ptr<DupSource> dup_src_;
    std::vector<std::wstring> log_;
    int log_limit_ = 400;      // 日志最多留 400 行，避免大表把内存吃满
};

}  // namespace bs

#endif  // BS_APP_H
