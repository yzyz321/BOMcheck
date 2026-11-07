# BOMCheck V4.1.6 代码审查报告

| 项 | 内容 |
|---|---|
| 被审对象 | BOMCheck V4.1.6（C/C++ 重写版，单 exe） |
| 源码基线 | `D:\AI_Workspace\workbuddy_spa\BOMcheck`（`CMakeLists.txt:2` `project(BOMCheck VERSION 4.1.6)`） |
| 二进制 | `build\c\bin\BOMCheck_V4.1.6.exe` / `dist\BOMCheck_V4.1.6\BOMCheck_V4.1.6.exe`（705,024 B，2026-11-07 09:31:23） |
| 审查日期 | 2026-11-07 |
| 审查方式 | 审查阶段**只读**：read / grep / glob / 只读 PowerShell（文件清单、字节数、版本、运行已编译 exe 做黑盒验证），未修改源文件、未重跑 `build_c.bat`。审查结束后按 §7 执行了第一批（10:40）与第二批（11:10）修复，改动与验证见 §9.3 / §9.4 |
| 记录位置 | `BOMcheck\CodeReview\`（本报告 + 问题清单 CSV + `repro\` 复现样本） |

---

## 0 结论摘要

代码整体工程质量偏上：core 层纯 C、零第三方依赖、模块边界清晰，自研的 zip / inflate / deflate / XML / xlsx 写回链路在**正常输入**下表现稳健，`-Wall -Wextra` 下 core 只报出 1 条告警。标注「在原包内改样式、其余部件按压缩字节搬运」的设计是这套代码里最有价值的部分，实现也基本兑现了承诺。

但有两类问题在本次审查中被**实际复现**，都属于「文件内容可以决定程序行为」的输入信任问题：

1. **打开一份 1.5 KB 的构造 xlsx 会直接崩溃（堆越界写）**，根因是 Excel 列号 `"ZZZZZZZ1"` 在 `col_from_ref` 里整数溢出成负数，随后被当作数组下标写进 `present_cols` 的位图（§2.1）。
2. **另一份 1.5 KB 的构造 xlsx 会让程序永久占满一个 CPU 核**，根因是 `<row r="2147483647">` 把 `max_row` 抬到 `INT_MAX`，行扫描 `r++` 溢出，循环条件被优化成恒真（§2.2）。

除此之外，口径层面有三处值得注意的「同源缺陷」：合并单元格虽然被解析但从没被用来补全取值（与设计文档相悖）；诊断页的「数据行」与「建档行」在实现和 oracle 测试里是同一个值；「跳过 N 行」这个 10.21 教训的核心语义完全没有进入对账。

| 级别 | 数量 | 说明 |
|---|---|---|
| **P0** 崩溃 / 内存破坏 / 可被构造输入触发 | 1 | §2.1 列号溢出越界写 |
| **P1** 功能错误 / 卡死 / 数据口径错误 | 10 | §2.2–§2.11 |
| **P2** 健壮性 / 资源 / 文档-实现不符 | 28 | §3 |
| **P3** 卫生 / 可维护性 | 36 | §4（表格） |

界面层单独说明：`src/ui` 下 29 个文件已全部读完，其中 15 个由第二路逐行审查、关键路径（状态机、写出、逐表比对、拖放、菜单）由我复核原文。UI 侧没有发现 P0，但找到 4 个 P1：查重模式下「另存结果」必失败、大表滚动条 16 位截断、`DrawTextW` 缺 `DT_NOPREFIX` 导致 `&` 被吞、OLE 反初始化早于窗口销毁。

**修复进度（2026-11-07）**：三批已全部执行完 —— 第一批 **P0-01、P1-01、P3-04**；第二批 **P1-02/05/06/07/08/09/10**（P1-04 部分修复）；第三批 **P1-03、P2-14～P2-23** 以及 §5 的测试/文档三项。39 条 P0–P2 中 **20 条已修复**（10 条有回归或功能实测、10 条只做了静态修复待实测），1 条部分修复，其余为未排期的 P2 项。全量回归 0 失败（`check_sheet` 33/0、`check_xlsx` 7/0、`check_pair` 34/0、`check_write` 26+2/0、`check_mark` 8+4/0、`check_ui` 23/0、`check_fuzz` 18/0、`ctest` 4/4），逐条状态见 **§9**。

> 级别定义：P0 = 正常使用或恶意样本可触发的崩溃/内存破坏/数据损坏；P1 = 功能错误、可复现卡死、用户可见的错误口径；P2 = 边界健壮性、资源泄漏、文档与实现不符；P3 = 可维护性/风格。

---

## 1 审查范围与方法

### 1.1 覆盖范围

| 层 | 文件 | 行数 | 本次处理 |
|---|---|---|---|
| core | 15 个 `.c`（bs_zip / bs_inflate / bs_deflate / bs_xml / bs_xlsx / bs_sheet / bs_pn / bs_pair / bs_write / bs_zipw / bs_mark / bs_arena / bs_file / bs_version） | 6,843 | 全文逐行审阅 + 静态分析 |
| core 头 | 12 个 `.h`（含 `bs_cfg.h` 契约） | 516 | 全读，用于核对文档/接口承诺 |
| ui | 14 个 `.cpp`（app / main / table / table_src / card / drop / layout / dpi / paint / bench / selfcheck / xlsx_dump / layout_doc / utf8） | 4,079 | 全部读完（app.cpp 的关键路径由我逐行复核：状态机、写出、逐表、拖放、菜单、帮助；其余由第二路全文审阅，我复核了全部引用行号） |
| ui 头 + 生成 | 15 个 `.h` + `layout_gen.h`（49 控件） | 700 | 全读，与 `gui/layout_spec.py` 对账 |
| 测试/脚本 | `tests/test_core.c`（实测 863 行）、`tools/*.py`、`build_c.bat`、`CMakeLists.txt`、`gui/bomcore.py` | — | 由第二路审查覆盖，本报告只收录经我复核的条目 |
| 文档 | `README.txt`、`docs/用户使用手册.md`、`docs/BOM对比工具_V4_设计与实施.md`、`docs/BOM流程图_V4.html`、`docs/GUI控件位置对照表.md` | — | 逐条与实现对照 |

### 1.2 静态检查（只读，未落产物到源码树）

```text
gcc 16.2.0 (MinGW-w64 UCRT) -std=c99 -O2 -Wall -Wextra -Wshadow -Wformat=2 -fsyntax-only
  core: 1 条告警  bs_mark.c:1009: -Wformat-nonliteral（free_part_no 把 fmt 当格式串）
g++ -std=c++17 -O2 -Wall -Wextra -Wshadow -fsyntax-only
  ui:   1 条实质告警  drop.cpp:31: delete this（多态类无虚析构）
        其余均为 {sizeof(x)} 初始化器的 -Wmissing-field-initializers 噪声
gcc -fanalyzer（12 个 core 模块）
  bs_pair.c:716/741/775  NULL 'a' 解引用（防御性缺口，正常路径不可达）
  bs_pn.c:242            疑似未初始化读（经复核为分析器误报：cp_push 只读已写元素）
  bs_xlsx.c / bs_file.c  疑似泄漏（条件所有权转移，误报；bs_file.c:78 对 buf==NULL 真实存在）
```

### 1.3 动态验证（黑盒，使用已编译 exe）

没有 ASan（MinGW 不可用），因此采用两种手段：

1. **真实 exe + 构造 xlsx**：`--drop-test` / `--dump-table` / `--ui-run` 探针路径，观察退出码与耗时；
2. **guard-page 版 `bs_xml.c`**：把 `realloc` 换成「分配 n 字节、紧跟一页 `PAGE_NOACCESS`」的分配器，任何越界写立即 AV。用它验证过「超长 XML 标签名」的怀疑（`<` + 7 万字符标签名）——**结论是不存在越界**：代码在 `memcpy` 之前先按 `(tag_end-s)*2+128` 二次 `sc_reserve`，容量始终够。这条怀疑已排除，不写入问题清单。

| 实验 | 样本 | 结果 |
|---|---|---|
| 非法列号 `r="ZZZZZZZ1"` | `repro\crash_bad_colref.xlsx`（1,537 B） | **exit 0xC0000005**（访问违例）；对照样本 exit 0 |
| 行号 `r="2147483647"` | `repro\hang_row_intmax.xlsx`（1,541 B） | **20 s 未退出，被强杀**；`r="2147483646"` 同样输入 6.4 s 正常退出 |
| 行号 1e9 | 同上改参数 | 3.1 s 正常退出（证明是 O(max_row) 线性扫描） |
| 超长 XML 标签名 | 7 万 / 20 万字符 | guard-page 下无越界（排除） |
| 双键近似阶段复杂度 | 5000 行 × 2（2000 个无料号行） | 双键关 0.16 s；双键开 0.59 s |

### 1.4 明确未做的事

- 没有跑 `build_c.bat`（会写 `build\`、`dist\`，属于修改工作区），因此**没有验证「168 PASS / ctest 4/4」基线**；§5 对它的讨论只基于脚本静态阅读。
- 没有做长时间压力、并发竞态注入（无 TSan）、没有在真实 Excel/WPS 里打开产出文件做兼容性核对。
- 没有在 150%/200% 多屏环境里实测 DPI 相关条目（§3.6 P2-20、P3-24/27），这些是代码阅读结论。
- `src/ui/bench.cpp`、`selfcheck.cpp`、`layout_doc.cpp` 由第二路读过并给出 P3/S 级观察，我没有逐行复核这三份（其余 11 个 `.cpp` 与 15 个 `.h` 均已复核）。

---

## 2 P0 / P1 问题详情

### 2.1 【P0】列号整数溢出 → 越界写 → 打开文件即崩溃

**位置**：`src\core\bs_xlsx.c:92-99`（`col_from_ref`）、`src\core\bs_xlsx.c:395-423`（`<c>` 处理）、`src\core\bs_sheet.c:260-270`（`present_cols`）

```c
/* bs_xlsx.c:92 */
static int col_from_ref(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (*s >= 'A' && *s <= 'Z') n = n * 26 + (*s - 'A' + 1), s++;   /* 无上限检查 */
    if (n == 0 || *s < '0' || *s > '9') return 0;
    return n;
}
```

```c
/* bs_xlsx.c:399 */
int col = r ? col_from_ref(r) : 0;
if (r && col == 0) return;          /* 只挡住“非法→0”，挡不住溢出成负数 */
...
c->cur_col = col;                   /* 负数被原样存进 cell->col */
```

```c
/* bs_sheet.c:265 */
p = (char *)calloc((size_t)g->max_col + 1, 1);
for (i = 0; i < g->n_cells; i++) p[g->cells[i].col] = 1;   /* ← 负下标越界写 */
```

**复现**：`repro\crash_bad_colref.xlsx` 里放一个 `<c r="ZZZZZZZ1">`：

```powershell
BOMCheck_V4.1.6.exe --drop-test control_clean.xlsx crash_bad_colref.xlsx --dump-table
# exit = 0xC0000005 (STATUS_ACCESS_VIOLATION)
```

`"ZZZZZZZ"` 的列号真值是 `8,353,082,582`，`int` 回绕后是 **-236,852,010**。`present_cols` 分配的位图只有 `max_col+1`（本例 4）字节，于是向缓冲区**前**约 226 MB 处写 1 —— 几乎必然落在未映射页上，直接访问违例；如果恰好命中已映射内存，则是静默的堆破坏。

**影响**：任何人发来一份 xlsx 就能让程序崩在打开阶段（不需要用户点「开始比对」，拖进来即触发：`card_load → card_pick_sheet → bs_sheet_from_grid → find_pn_column → present_cols`）。这是典型的「解析不可信输入」缺陷，也是本报告唯一的 P0。

**建议**：

1. `col_from_ref` 用 `long long` 累加，超过 `16384`（Excel 最大列 XFD）或超过 `INT_MAX` 一律返回 0；
2. `row_from_ref`（`bs_xlsx.c:101-108`）同样处理（最大 1,048,576），见 P2-?；
3. `present_cols` 循环里加 `if (col <= 0 || col > g->max_col) continue;` 作为第二道防线；
4. 回归用例：`tests/data/cases/` 增加 `case_bad_ref`（`ZZZZZZZ1`、`A2147483647`、`AAAAAAA1`），并断言「打开成功或明确报错，绝不崩溃」。

**修复（2026-11-07，已验证）**：

- `bs_xlsx.h` 新增 `BS_MAX_ROW 1048576` / `BS_MAX_COL 16384`（Excel 硬边界）；
- `col_from_ref`：字母超过 3 个或列号 > 16384 直接返回 0（不再累加，彻底消除溢出）；
- `row_from_ref`：`long` 累加，> 1048576 返回 0（`mergeCells` 里由 `if (r0 && c0 && r1 && c1)` 丢弃）；
- `present_cols`：写位图前判 `col > 0 && col <= g->max_col`（第二道防线）；
- 复现样本 `repro\crash_bad_colref.xlsx` 从 `exit 0xC0000005` 变为 **exit 0，0.12 s**，`check_sheet` 33 张表仍 0 失败。

---

### 2.2 【P1】`max_row = INT_MAX` → 行扫描溢出 → 永久卡死

**位置**：`src\core\bs_xlsx.c:381-391`（`<row r>` 的 `atoi`）、`src\core\bs_sheet.c:761-779`（行循环）

```c
/* bs_sheet.c:764 */
for (r = b.start; r <= b.end; r++) {
    while (cur < g->n_cells && g->cells[cur].row < r) cur++;
    ...
}
```

`b.end` 直接取 `g->max_row`，而 `max_row` 完全由文件的 `<row r="…">` 决定（`atoi` 无上限）。当 `max_row == INT_MAX` 时，`r++` 在 `r == INT_MAX` 处有符号溢出（UB），`-O2` 下 GCC 按「不发生溢出」优化，`r <= INT_MAX` 恒真 → **死循环**。

**复现**（`repro\hang_row_intmax.xlsx`）：

| 文件里的行号 | 结果 |
|---|---|
| `r="100000"` | 0.0 s，exit 0 |
| `r="10000000"` | 0.0 s，exit 0 |
| `r="1000000000"` | 3.1 s，exit 0 |
| `r="2147483646"` | 6.4 s，exit 0 |
| `r="2147483647"` | **>20 s 仍在跑（强杀）** |

前四行证明这是**与行号成正比**的全区间扫描（1.5 KB 的文件换 3～6 秒卡顿），最后一行证明 `INT_MAX` 时已经退化成不终止。

**影响**：① 恶意/损坏文件 → 单核 100% 永久占用，只能杀进程；② 即使不是 `INT_MAX`，只要行号虚高（例如从别的系统导出的表里带了 `r="1048577"` 这种），每张表就要多扫几十万到十亿次空行；③ 逐表比对时每张表都会重复这一代价。

**建议**：

1. 解析层钳制：`if (rn > 1048576) rn = 1048576;`（或直接判为坏文件并报错）；`row_from_ref` 同理；
2. 循环改成 `for (r = b.start; r < b.end + 1 && r < kRowLimit; r++)`，或改用 `long long` 计数；
3. `b.end` 用「实际最后一个有内容的行」而不是 `max_row`（`g->cells[g->n_cells-1].row`），顺带解决空行扫描的性能问题。

**修复（2026-11-07，已验证）**：

- `row_from_attr()` 取代 `<row r>` 的 `atoi`：`long` 累加，> 1048576 返回 -1；
- `sheet_ctx.skip_row`：越界行整行忽略（连同它的 `<c>`），不再把行号钳进表格制造假数据；
- `bs_sheet.c` 新增 `last_content_row()`，行扫描上界取 `min(b.end, 最后一个有内容的行)`——**`b.end` 本身不改**（诊断页与 Python oracle 比的是它，改了小表会对不上）；
- `row_from_ref` 同时限幅，`mergeCells` 里的超限引用被安全丢弃。

> 过程记录：第一次实现把 `b.end` 直接收到了「最后一个有内容的行」，`check_sheet` 立刻红了 6 张（3 个真实样本 × 2 个视角）——因为 `block ... end=` 是对账字段。改成只收扫描上界后 33 张全绿。这也说明了 §5 的价值：对账能挡住「语义被顺手改掉」。

复现样本 `repro\hang_row_intmax.xlsx` 从「>20 s 未退出」变为 **exit 0，0.13 s**；`r="1000000000"` 从 3.1 s 降到 0.0 s。

---

### 2.3 【P1】合并单元格只被解析、从未被使用 —— 与设计文档相反

**位置**：`src\core\bs_xlsx.c:360-377`（记录 `bs_merge`）、`src\core\bs_sheet.c:425-452`（唯一使用处：识别跨列标题行）、`src\core\bs_sheet.c:866-879`（`bs_sheet_text_at`）、`src\core\bs_xlsx.c:727-741`（`bs_grid_get`）

设计文档 §4.1 明确写：

> `bs_grid`（bs_xlsx）是原始网格：…**合并单元格以 span 记录，取值走 span 头**。

实际代码里 `g->merges` 只被 `collect_label_rows()` 读了一次（判断 `m->r1 == m->r0 && m->c1 - m->c0 >= 3` 的标题行），`bs_grid_get` / `bs_grid_strike` / `bs_sheet_text_at` 都不做「落在合并区但非左上角 → 取 span 头」的映射。

**影响**：中文 BOM 里「同一颗料的行合并料号格」很常见（`A2:A4` 合并、只写一次料号）。当前实现下，第 3、4 行读到的料号是空串 → `kind == 0` → 双键关时计入「跳过」、双键开时按位号兜底硬配，**这几行会从差异结果里消失或配错**。`case_merge_vertical.xlsx` 只合并了序号列，恰好绕开了这条路径，所以对账一直绿。

**建议**：在 `bs_xlsx_load_sheet` 末尾做一次 span 展开（把 span 头的值/删除线复制到合并区内所有格，或让 `bs_grid_get` 在未命中时查一次合并表）；然后在 `check_sheet` 里加「合并料号列」样本。

---

### 2.4 【P1】双键近似阶段是 O(旧×新)，候选表没有上限

**位置**：`src\core\bs_pair.c:446-506`

```c
for (i = 0; i < nro && cs; i++) {
    for (k = 0; k < nrn; k++) {          /* 全笛卡尔积 */
        ...
        j = jaccard(a->ref_sig, b->ref_sig);   /* 每对都跑字符串归并 */
        if (j < c->ref_min_overlap) continue;
        if (nc == ccap) { ccap *= 2; cs = realloc(cs, ccap * sizeof(cand)); }  /* 无上限 */
        cs[nc++] = ...;
    }
}
```

`a->kind == 'P' && b->kind == 'P'` 会被短路，所以常规「料号齐全」的 BOM 不会触发——这也是 README 里「10 万行 0.65 s」成立的原因。但只要**一侧存在大量无料号行**（旧表料号列大面积空着、或识别错列），就是完整笛卡尔积。

**实测**：旧表 5000 行（3000 行有料号、2000 行空料号）、新表 5000 行、双键开：

| 配置 | 耗时 |
|---|---|
| 双键关 | 0.16 s |
| 双键开 | **0.59 s**（≈2000×5000 = 1e7 次 jaccard，约 35 ns/次） |

按此线性外推：双方各 10 万行、其中 5 万行无料号 → 2.5e9 次 ≈ **90 秒起**；若用户把阈值从 0.90 降到 0.60，候选表可能装下接近全部组合（100k×100k），`cand` 每项 24 B，内存直接爆掉。整个过程中 UI 线程被占死（见 §2.5），没有取消入口。

**建议**：① 给 `nro*nrn` 设一个上限（如 5e7），超限时降级为「按位号 token 建倒排索引，只比有共同 token 的行」；② 给候选表设容量上限；③ 在状态栏/日志里提示「近似配对规模过大，已降级」。

---

### 2.5 【P1】「停止」按钮没有任何实现；所有比较与写盘都跑在 UI 线程

**位置**：`src\ui\app.cpp:603-615`（`hit_action` 返回 3）、`src\ui\app.cpp:1862-1894`（`WM_LBUTTONUP` 的 `switch` 里**没有 case 3**）、`src\ui\layout_spec.py:198-199`（`note="中断当前任务（引擎协作式取消）"`）

```cpp
switch (hit_action(lx, ly)) {
    case 1: run_compare(); return 0;      // 开始比对 / 查重
    case 2: ...                            // 另存结果
    case 4: ... case 5: ... case 6: ...
    default: break;                        // ← 3（停止）落到这里，什么都不做
}
```

同时 `run_compare()` → `run_compare_all()` → `write_all()` 全在主线程同步执行：`load_pair` 用 `WaitForSingleObject(INFINITE)`，逐表比对是 N 张表 × 3 份文件串行，10 万行约 2.4 s、多张表线性叠加。期间没有消息泵（`set_progress` 里只有 `UpdateWindow` 直接画，`UpdateWindow` 会同步派发 `WM_PAINT`，因此进度条能走），但**点击、拖动、关闭窗口都没反应**，Windows 会弹「未响应」。

`layout_spec.py` 里 `act.stop` 的 `state="disabled"`，`paint_actions`（`app.cpp:369-371`）也确实把它画成禁用色——所以现状是「一个画成禁用、点了也没反应的按钮」，与 README/手册完全不提它是一致的；但布局 spec 的 note 声称「引擎协作式取消」，属于虚构的实现描述。

**建议**：要么删掉 `act.stop`（连同 `hit_action` 的 3 和 `run_label`），要么真正实现：把 `run_compare_all` 放到工作线程，`WM_APP+1` 回报进度，`act.stop` 置原子取消标志，`write_all` 每张表之间检查一次。在实现之前，至少把 `layout_spec` 的 note 改成「未实现」。

---

### 2.6 【P1】诊断页的「数据行」与「建档行」永远是同一个数

**位置**：`src\core\bs_write.c:333-353`（写诊断页）、`src\core\bs_write.c:376`（`bs_diag_fill`）

```c
cell_int (b, 4, rr, x->data_rows, XF_BODY);   /* 第 4 列：数据行  */
...
cell_int (b, 8, rr, x->data_rows, XF_BODY);   /* 第 8 列：建档行  */
```

`bs_diag_fill` 里 `d->data_rows = bs_sheet_row_count(s)`，而 `bs_sheet_row_count` 只统计**成功建档**的行（`add_row` 在 `kind == 0` 时 `s->unkeyable++` 后直接 return，不进 `rows`）。于是「数据行」被实现成了「建档行 + 0」，两列恒等。

**影响**：手册 §十 说「识别诊断页…数据行多少…识别结果不对时先看这一页」。用户看到「数据行 157 / 建档行 157 / 无法建档 3」会以为 157 已包含那 3 行，实际总数据行是 160。卡片状态行的「数据行 N（无键 M）」同源，也是同一个偏小的数。

**更值得警惕的是对账**：`tools/check_write.py:127-132` 的 Python 侧把同一个错误公式抄了一遍——

```python
diag.append([..., len(sd.rows), ... , len(sd.rows), ...])   # 第 4 与第 8 列同值
```

所以 `check_write` 逐格比对永远通过。这是「C 与 Python 共享同一份错误理解 → 对账无法互证」的教科书例子（同类问题见 §5）。

**建议**：`data_rows` 改为「块区间内有内容的行数」（含 `unkeyable` 与 `labels`），并在 `check_write` 里加一条**独立于两侧实现**的断言：`数据行 == 建档行 + 无法建档 + 分组标签`。

---

### 2.7 【P1】`bs_write_result` 在分配失败路径上双重释放

**位置**：`src\core\bs_write.c:463-467` + `481-486`；`src\core\bs_zipw.c:89-99`、`108-121`

```c
/* bs_write.c:464 */
if (bs_zipw_add(z, "xl/worksheets/sheet1.xml", s1.p, s1.n) != 0) goto oom;
s1.p = NULL;                       /* 只有成功才交所有权 */
...
oom:
    free(s1.p);                    /* 失败时 zipw 内部已经 free 过同一块 */
```

`bs_zipw_add` → `bs_zipw_add_raw` 的契约是「接管 data，任何失败路径都 `free(data)`」（`bs_zipw.c:109/117/127` 三处），而 `bs_zipw_add` 在走了 deflate 分支时还会先 `free(data)` 再传压缩块（`bs_zipw.c:94`）。因此 `bs_write_result` 失败时对 `s1.p` 的 `free` 是第二次释放。

**影响**：只在内存分配失败时触发，触发即堆破坏（CRT 可能直接 abort，也可能静默损坏后继续跑）。考虑到结果表 10 万行时 `sb` 缓冲能到几十 MB，在 32 位兼容环境或低内存机器上不是理论问题。

**建议**：统一所有权约定——要么 `bs_zipw_add` 失败时**不**接管（调用方负责），要么调用方在所有分支都把指针置 NULL。顺带把 `bs_mark.c:915-948` 的同类写法（那里是 `buf` 局部变量，安全）一起对齐。

---

### 2.8 【P1】查重模式下「另存结果…」必定失败

**位置**：`src\ui\app.cpp:361-367`（外观）、`src\ui\app.cpp:1866-1875`（动作）、`src\ui\app.cpp:1433-1439`（`write_all` 入口）

```cpp
/* app.cpp:1436 —— write_all 只认差异结果 */
if (!res_ || !slots_[0].analysis || !slots_[1].analysis) {
    r.err = L"还没有可写出的比对结果";
    return r;
}
```

查重模式（两侧同一文件）下 `recompute_pairs` 走 `&& !same` 分支，**不会生成 `res_`**（`app.cpp:1346`），查重的唯一产物是 `bs_mark_dups` 写出的 `<主名>_查重标注.xlsx`。而「另存结果」的点击处理（`app.cpp:1866-1875`）只判 `can_run()`——查重模式下 `can_run()` 因为 `slots_[0].analysis` 非空而返回 true——然后直接调 `write_all(dir, nullptr)`，必然命中上面那条 `return r`。

同时按钮外观（`app.cpp:361-367`）用 `has_rows`（包含 `dup_src_`）判断，所以查重有重复行时按钮是**可点的高亮态**，点下去只写一条「另存失败：还没有可写出的比对结果」的日志。也就是说：查重结果无法另存到用户指定目录，只能写到默认 `checkresults\`。

**建议**：`case 2` 先判 `dup_mode_` 转 `run_dups()` 并支持传入目录；外观判断与命中判断统一走 `can_run()`，避免「画得能点、点了必败」。

---

### 2.9 【P1】超过 65535 行的表格，竖向滚动条拖不过去

**位置**：`src\ui\table.cpp:150-175`（设置范围）、`src\ui\table.cpp:319-337`（处理 `WM_VSCROLL`）

```cpp
si.nMax = n > 0 ? n - 1 : 0;          /* 32 位范围，n = 结果行数 */
...
case SB_THUMBTRACK:
case SB_THUMBPOSITION:
    top = (int)HIWORD(wp);            /* 只有低 16 位有效 */
```

`SetScrollInfo` 支持 32 位位置，但老式 `WM_VSCROLL` 的 `HIWORD(wParam)` 被截断在 16 位（65535）。10 万行的结果表（项目自己的压测基线）拖到 6.5 万行之后，滚动位置会回跳到 16 位范围内的值。同一缺陷在 `WM_HSCROLL`（`table.cpp:347-350`）上表现为总宽度超过 32767 像素时拖动错位。

**建议**：`SB_THUMBTRACK/SB_THUMBPOSITION` 一律用 `GetScrollInfo(hwnd_, SB_VERT, &si)` 取 `si.nTrackPos`；或用 `SetScrollInfo` 后统一读 `SIF_TRACKPOS`。

---

### 2.10 【P1】全站文本绘制缺 `DT_NOPREFIX`：`&` 被吞掉

**位置**：`src\ui\paint.cpp:45-52`（唯一的 `DrawTextW` 出口）

```cpp
DrawTextW(dc, s, -1, &rr, DT_SINGLELINE | DT_VCENTER | extra);
```

Win32 的 `DrawText` 默认把 `&` 当加速键前缀（`&R` 显示成带下划线的 `R`，单独的 `&` 直接消失）。全工程 grep 不到一处 `DT_NOPREFIX`，而 `draw_text()` 被卡片路径、状态栏、表格单元格（`table.cpp:244/269`）、日志页之外的几乎所有文字复用。

**影响**：`D:\R&D\BOM.xlsx` 显示成 `D:\RD\BOM.xlsx`；描述列里的 `SCREW & NUT` 少一个字符；料号里带 `&` 时用户看到的与实际数据不一致——而这是一个「对着料号核对」的工具，显示失真比样式问题严重。

**修复**：在 `draw_text()` 里统一加 `DT_NOPREFIX`（一处改完，所有调用点受益）。

---

### 2.11 【P1】OLE 反初始化早于窗口销毁 / 析构，拖放对象泄漏

**位置**：`src\ui\main.cpp:160`、`328-334`；`src\ui\app.cpp:66-76`

```cpp
const HRESULT hr_ole = OleInitialize(nullptr);
bs::App app(inst);
...
const int rc = app.run();                 // 消息循环，WM_DESTROY 已把窗口销毁
if (SUCCEEDED(hr_ole)) OleUninitialize(); // 先反初始化 OLE
return rc;                                // 再析构 App → drop_revoke(hwnd_)
```

`~App`（`app.cpp:73`）里的 `drop_revoke(hwnd_)` 在窗口已经 `WM_DESTROY`、OLE 已经 `OleUninitialize` 之后才执行，`RevokeDragDrop` 必然失败。`RegisterDragDrop` 给 `Target` 加的引用因此不会被释放，`IDropTarget`（含它持有的 `std::vector<std::wstring> files_`）泄漏一次。

**建议**：在 `WM_DESTROY` 里先 `drop_revoke(hwnd_)`；或把 `App` 放进内层作用域，先析构再 `OleUninitialize()`；最稳的是两者都做（`drop_revoke` 幂等）。

---

## 3 P2 问题详情

### 3.1 识别层

**P2-1 `header_scan_rows` 是死配置，错误文案却写「前 50 行内」。**
`bs_cfg.h:27` 注释自认「只用于文案/进度，不做硬截断」，`bs_sheet.h:18-20` 也解释了「表头扫描不设硬上限」。但 `bs_sheet.c:722-724` 的错误文案仍然是：

```c
snprintf(e->msg, sizeof(e->msg),
         "未识别到表头行：前 %d 行内没有「列名 + 料号格式」的双证据", c->header_scan_rows);
```

用户据此去挪表头，挪到第 60 行依然不行时会以为工具坏了（实际 `case_header_at60` 证明第 60 行能识别）。建议改成「全表扫描未找到」，或真的按该值截断并同步测试。

**P2-2 六个 `bs_cfg` 字段完全没被读取。** grep 结果：`block_mode`、`alias_exact`、`ref_require_token`、`skip_label_rows`、`seg1/seg2/seg3` 只在 `bs_cfg_default` 里被赋值，任何逻辑都不读。其中 `block_mode` 最容易被误解——文档说「block_mode=1 时允许多块」，实际实现**永远**多块；`seg1..seg3` 声称控制料号形态（3-6-3），实际 `pn_shape` 把 `14 / 3 / 10` 写死（`bs_pn.c:291`）。建议删字段或在 `bs_cfg.h` 注明「保留未用」。

**P2-3 自动挑表用 `AUTOSCAN_ROWS = 20` 提前收敛，与文档「数据行最多」不符。**
`card.cpp:27-30` + `card.cpp:245-257`：第一张表只要 ≥20 个数据行就不再往后翻。手册 §二 写「默认自动挑『数据行最多』的那张，封面/修订记录页不会顶替真正的 BOM 表」。若封面页恰好是一张 25 行的修订记录表，它会被当成 BOM。建议把门槛提到「至少覆盖后续表的一张」或把文案改成「挑第一张像 BOM 的表」。

**P2-4 `card_pick_sheet` 分析失败仍返回 `true`。**
`card.cpp:172-177`：`bs_sheet_from_grid` 失败时设置 `slot.status`、`slot.ok = false`，然后 `return true`。调用方 `card_load` 用返回值判断「这张表读成功了吗」（`card.cpp:247` `if (!card_pick_sheet(slot, i)) continue;`），于是把失败页当成成功页参与 `best_rows` 比较。目前因为 `data_rows` 都是 0 才没出事故，建议改成 `return false` 并让 `card_load` 用 `slot.ok` 判断。

### 3.2 配对与统计

**P2-5 「跳过 N 行」没有进入任何对账。**
`bs_pair.c:438-444` 统计 `s.n_skipped`，`app.cpp:1212-1214` 与 `:1371-1375` 把它显示在状态栏与日志里——这是「同一份文件自比不会冒出假差异」的唯一解释性数字。但：

- `tests/test_core.c:567` 的 stat 行只有 `pairs/pn/ref/fuzzy/pnchg/add/del/chg/cart`，**没有 skip**；
- `tools/check_pair.py` 全文没有 `skip`；
- `check_ui` 也不断言状态栏这句文案。

双计、漏计、恒为 0 都能全绿。而设计文档 §8.2 明写 check_pair「含…跳过语义」。建议 stat 行加 `skip=`，`py_dump` 补 `st["skipped"]`，`check_ui` 断言日志里的 N。

**P2-6 阶段 A/B 对同键重复行是 O(k²)。**
`bs_pair.c:195-214`：对每条旧行都 `htab_find` 从桶头线性找同键节点，再 `while (un[...])` 往后挪。k 条同料号行 → k²/2 次 `strcmp`。常规 BOM 重复行个位数，无感；构造一份 5 万行同料号的表就是 1.25e9 次比较（分钟级）。`bs_sheet.c` 的 `group_dups` 已经用链表规避了同类问题，这里可以照抄「桶内游标」。

**P2-7 `bs_pair.c` 用文件级全局 `g_o/g_n` 传行数组，接口不可重入。**
`bs_pair.c:94` 两个 `static const bs_row *g_o, *g_n`，在 `bs_pair_rows`（`:402/432`）和 `bs_result_build`（`:701-702`）里赋值，`cmp_oref`/`pair_pn` 依赖它们。注释写「单线程，够用」，但 `load_pair` 已经证明「core 可以并行」（`app.cpp:1302-1304`），一旦以后并行做两张表的配对就会串数据。建议把 `g_o/g_n` 塞进 `bs_result`/上下文结构体。

### 3.3 内存与并发

**P2-8 并行载入会并发写全局耗时计数器（数据竞争）。**
`app.cpp:1302-1304` 的注释说「core 里没有跨实例的可变状态（解压的定长树已改成就地建），所以可以真并行」。但 `bs_xlsx.c:13-14` 的 `bs_prof_inflate_ms` / `bs_prof_parse_ms` 是 `double` 全局，`load_part`（`:496-497`）在两个载入线程里各自 `+=` —— 非原子读改写，形式上是 UB，实际会丢计数。建议改成 `_Thread_local` 或只在单线程模式下累计。

**P2-9 inflate 输出没有上限（zip bomb）。**
`bs_inflate.c:32-46` 的 `out_need` 只受「剩余内存」限制，`bs_inflate_raw` 对 `hint`（中央目录里的 usize）不做校验，也不校验解压结果是否超过 `usize`。一个 200 KB 的压缩包可以解到 2 GB。虽然 `bs_zip_open` 拒绝 ZIP64，但 `usize` 仍可写 4 GB。建议：解压时以 `usize` 为硬上限，超过即报「压缩数据与目录不符」。

**P2-10 zip 读取不校验 CRC。**
`bs_zip.c:145` 把 `crc` 读进 `ent_t`，但 `bs_zip_read_index` / `entry_data` 从不比对。自研 inflate 一旦有边界 bug，坏数据会静默进入解析器。建议对每个部件解压后校验 CRC32（`bs_zipw.c` 已有现成的 `crc_calc`，导出一个只读版本即可）。

### 3.4 写出与标注

**P2-11 逐表比对靠「以上一轮产物为源」累积，styles.xml 每轮增长。**
`app.cpp:1550-1593` 每轮把上一轮的标注文件当源；`bs_mark.c:1348-1371` 每次都往 `fills` 追加 4 个填充、往 `cellXfs` 追加本轮克隆的 xf。N 张表后 `styles.xml` 里有 4N 个填充和若干 N 份 xf，文件逐轮变大；且每轮都要把整包解压/重写一遍（10 万行 × 4 张表 ≈ 4 次全量重写）。功能正确，代价线性叠加。建议最后一次性合并，或在累积时复用上一轮的 `fill_base`。

**P2-12 输出改名重试把「非重名错误」也当成重名。**
`bs_write.c:469-476` 与 `bs_mark.c:950-970`：`bs_zipw_save` 失败就换 `_2`、`_3`……最多 20 次。磁盘满、目录只读、文件被 Excel 占用，都会被重试 20 次，最后报的 `err` 是第 20 次的错误，日志里出现一串没人看过的文件名。建议先判断「目标是否存在」（`GetFileAttributes`），只有存在才改名；其余错误直接上报。

**P2-13 逐列比较把列范围硬截在 512。**
`bs_pair.c:349` 与 `bs_mark.c:1463`：`if (c2 > 512) c2 = 512;`。防御意图可以理解，但超过 512 列的 BOM（宽表很常见）会**静默漏比**，用户勾了「整行逐列比较」也发现不了。建议改成可配置上限，并在日志里写明「仅比较前 512 列」。

### 3.5 文档与实现不符（经复核）

| # | 文档说法 | 实现 | 影响 |
|---|---|---|---|
| P2-14 | 设计文档 §4.1「合并单元格…取值走 span 头」 | 只用于识别标题行（§2.3） | 用户按文档预期使用合并料号会丢行 |
| P2-15 | `bs_write.h`「整包用 stored（不压缩）」 | `bs_zipw.c:88-99` 变小就用 deflate；README/设计文档都是 deflate 口径 | 头文件注释与同一模块的 `.c` 自相矛盾 |
| P2-16 | `gui/layout_spec.py:196-197` 按钮文案「重写结果…」 | `app.cpp:366` 实绘「另存结果…」；README/手册也写「另存结果…」 | 交付包里的《控件位置对照图》渲染的是「重写结果…」，按图找按钮找不到 |
| P2-17 | README「单个 exe 约 660 KB」 | 实测 705,024 B ≈ 688 KB；设计文档写「约 700 KB」 | 三个口径三种说法 |
| P2-18 | 设计文档 §7.2「168 PASS / 0 FAIL」 | 没有任何脚本汇总总数；`check_ui` 第 23 项是条件性的（样本无重复料号时才 `n += 1`，`check_ui.py:793-808`） | 基线数字不可复现，样本一改就静默变 22 项 |

### 3.6 界面层（全量走查，本人复核原文）

**P2-19 日志页 `EDIT` 没有解除长度上限，最新几行会被截掉。**
`app.cpp:482-487` 创建 `ES_MULTILINE` 的只读 EDIT，`app.cpp:559-570` 每打一条日志就整段 `SetWindowTextW`。全文件没有 `EM_SETLIMITTEXT`（已 grep：0 处），多行 EDIT 默认上限 32767 字符。`log_` 保留 400 行，按每行一条带路径的「载入旧表 D:\… · 表『Main』…」约 100 字符算就是 4 万字符——**超出部分按 EDIT 的语义是从尾部截断**，恰好把最新、最该看的收尾日志（写完了 / 失败原因）截没。建议 `SendMessageW(log_hwnd_, EM_SETLIMITTEXT, 0, 0)`。

**P2-20 DPI 变化后日志页持有已删除的字体。**
`app.cpp:175-195`（`make_fonts` → `drop_fonts` 删掉 `f_mono_`）、`app.cpp:486-487`（只在创建时 `WM_SETFONT`）、`app.cpp:1775-1784`（`WM_DPICHANGED` 重造字体但没重发 `WM_SETFONT`）。跨屏后 EDIT 用的 HFONT 已被删除，字号不随 DPI 变化。建议 `apply_dpi()` 末尾重新 `WM_SETFONT`（`prog_`/`log_hwnd_` 都要）。

**P2-21 `Table::metrics()` 借 DC 不还原字体，`set_dpi` 每次漏一个 HFONT。**
`table.cpp:107-119`：`GetDC` → `SelectObject(f_head_)` → `SelectObject(f_cell_)` → `ReleaseDC`，没有恢复原字体。之后 `drop_fonts()` 的 `DeleteObject` 因为对象仍被缓存 DC 选中而失败，字体句柄泄漏。建议保存 `old` 并在 `ReleaseDC` 前 `SelectObject(dc, old)`。

**P2-22 `bs_result_build` 失败被静默吞掉，表现为「两份 BOM 没有差异」。**
`app.cpp:1352-1357`：

```cpp
has_pairs_ = true;
if (bs_result_build(..., &res_, &e) == 0)
    n_res_ = (int)bs_result_count(res_);      /* 没有 else */
```

失败时 `has_pairs_` 仍为 true、`res_` 为 NULL：状态栏显示「已配对 N 对 · 差异 0 行」，主按钮因 `can_run()` 要求 `res_` 而变灰。用户看到的是「没有差异 + 按钮不可用」，实际是内存不足。建议 `else { pair_note_ = from_utf8(e.msg); has_pairs_ = false; append_log(...); }`。

**P2-23 逐表比对失败时会留下 `.partK` 孤儿文件，日志指向不存在的路径。**
`app.cpp:1579-1591`：某张表 `write_all` 失败时只 `DeleteFileW(wa.res)`，已经落盘的 `wa.old_mark`（`.partK`）既不加入 `parts` 也不删除。`app.cpp:1599-1610` 两次 `MoveFileEx` 共用一个 `renamed`：旧表改名成功、新表改名失败时，`last_o` 已经被移走，却把 `final_o` 重置成 `last_o`——日志里的「已留在临时名：<不存在的路径>」和收尾的「标注：旧表合并 N 张表 → <不存在的路径>」都是错的。建议 `ok_o`/`ok_n` 分别记录，失败轮把自己产生的文件全部登记进 `parts`。

**P2-24 下拉菜单绘制没设透明背景，悬停行是白字底。**
`app.cpp:2203-2226` 的 `menu_proc`/`WM_PAINT` 没有 `SetBkMode`（全文件只有 `app.cpp:226`、`table.cpp:201/214` 设过）。悬停行先 `fill_round(kColAccentBg)` 再 `DrawTextW`，默认 OPAQUE 会先填一层白底，浅蓝高亮上出现白字。建议在 `menu_proc` 的绘制开头 `SetBkMode(hdc, TRANSPARENT)`。

**P2-25 表格滚轮必须先点一下表格才生效。**
`app.cpp:1826-1834` 的父窗口 `WM_MOUSEWHEEL` 只处理近似阈值框，其余 `break` 交给默认处理；`table.cpp:356-364` 的滚轮处理只在表格窗口收到消息时执行，而表格要 `WM_LBUTTONDOWN`（`table.cpp:365-367`）才 `SetFocus`。刚出结果时鼠标在表格上滚轮无效。建议父窗口把未消费的滚轮转发给 `table_`。

**P2-26 `GetOpenFileName` 失败一律当「用户取消」。**
`app.cpp:1673-1685`：

```cpp
wchar_t file[MAX_PATH * 2] = L"";        /* 520 */
ofn.nMaxFile = MAX_PATH * 2;
if (!GetOpenFileNameW(&ofn)) return;     // 取消
```

manifest 声明了 longPathAware，但这里缓冲只有 520 宽字符；选中超过 519 字符的路径时 `GetOpenFileNameW` 以 `FNERR_BUFFERTOOSMALL` 失败，被当成取消，界面毫无反应。建议判 `CommDlgExtendedError()` 并提示，或换 `IFileOpenDialog`。

**P2-27 `AdjustWindowRectExForDpi` 第三参传错（潜在）。**
`dpi.cpp:73` `a.adjust_for_dpi(&r, style, ex ? TRUE : FALSE, ex, dpi)`——第三个参数是 `bMenu`，不是「ex 是否非 0」。当前三个调用点（`app.cpp:130/1793/2254`）都传 `ex = 0`，恰好等于 `FALSE` 才没出事；一旦有人传非 0 的 ex style，算出来的窗口会多一个菜单栏高度，`--client-size` 与最小尺寸校验都会失准。兜底分支 `dpi.cpp:79` 用的反而是正确的 `FALSE`。建议改成 `FALSE`。

**P2-28 载入阶段同样阻塞且没有进度。**
`app.cpp:1289-1299` 单文件直接同步 `card_load`；`app.cpp:1301-1324` 起了两个线程却仍在 UI 线程 `WaitForSingleObject(th[i], INFINITE)`。网络盘或 20 张表的工作簿在载入期窗口假死，且没有 `set_busy`、没有进度。建议 `MsgWaitForMultipleObjects` 边等边抽消息，至少把光标改成等待并显示「载入中…」。

---

## 4 P3 汇总

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| 1 | `app.cpp:1906-1912`、`drop.cpp:90-97` | `std::wstring s(len, L'\0'); DragQueryFileW(..., &s[0], len+1)` 写入 `len+1` 个 wchar，越过了 `size()`（libstdc++ 恰好分配了终止符槽位，实际安全，形式上是 UB） | 先构造 `len+1` 再 `resize(len)` |
| 2 | `drop.cpp:29-33` | 多态类 `delete this` 且无虚析构（`-Wdelete-non-virtual-dtor`） | 给 `Target` 加 `virtual ~Target()` |
| 3 | `bs_sheet.c:454-468` | `parse_qty` 接受 `nan`/`inf`；`bs_pair.c:621-624` 输出 `"nan"` | 显式拒绝非有限值 |
| 4 | `bs_xlsx.c:101-108` | `row_from_ref` 同样无溢出保护（影响 `mergeCells`） | 同 P0 建议 |
| 5 | `bs_xml.c:35-53` | `put_utf8` 对 `0xD800-0xDFFF` 输出 CESU-8 非法序列；`&#0;` 会写入 NUL 截断字符串 | 拒绝代理区与 0 码点 |
| 6 | `bs_write.c:93-111` | `sb_esc` 把控制字符直接丢弃、`\t\n\r` 换成空格 | 至少把 `\t` 保留为 `&#9;`，或在日志里提示「已清理控制字符」 |
| 7 | `bs_mark.c:1457-1468` | 逐列涂色与 `bs_pair.c:340-356` 各自实现了一遍「列范围取并集」，两处上限（512）与归一化须手工同步 | 抽成 `bs_sheet_diff_cells()` 共用 |
| 8 | `bs_mark.c:1448-1453` | `mark_push` 返回值全部忽略，OOM 时静默少涂 | 累计失败标志，最后报「部分涂色失败」 |
| 9 | `bs_mark.c:1230-1239` | 为了取一个部件名，把整个 workbook 重新 `bs_xlsx_open` 一遍（每次标注 2 次全量解析） | 从调用方传 `sheet_part` 进来 |
| 10 | `bs_mark.c:912-913` | 替换部件按 `strcmp` 精确匹配；源包大小写不同（`xl/Worksheets/...`）时会追加同名不同大小写的条目 | 用已有的 `zip_name_ci` 结果做替换名 |
| 11 | `bs_write.c:398-412` | `alt_path` 的 `%.*s_%d%s` 在 `stem` 超长时静默截断 | 检查返回值 |
| 12 | `bs_file.c:78-80` | `bs_read_file` 在 `buf == NULL` 时泄漏整块缓冲（`-fanalyzer` 报出） | 加 `if (!buf) { free(p); return -1; }` |
| 13 | `bs_xlsx.c:284-300` | 字符串池 `strs_len` 用 `unsigned` 偏移，超 4 GB 溢出（理论上不可达） | 用 `size_t` |
| 14 | `app.cpp:2117-2119`、`2175-2199` | 弹出菜单用 `SetCapture` 而不取焦点，`VK_ESC/UP/DOWN/RETURN` 分支永远收不到（Esc 关不掉下拉） | 要么取焦点并处理 `WM_KEYDOWN`，要么删掉死分支 |
| 15 | `table.cpp:356-364` | `delta / 120` 在高精度滚轮（delta < 120）下为 0，滚不动 | 累加余量 |
| 16 | `card.cpp:267-269` | `fmt_size(...)` 算完 `(void)sz` 丢掉，死代码 | 删除或显示到卡片上 |
| 17 | `app.cpp:1442-1454` | `write_all` 里的文件名拼接没做长度检查，超长路径会截断到 260/1024 | 用 `\\?\` 前缀或提前报错 |
| 18 | `bs_zip.c:266-282` | 大小写不敏感兜底用逐字符循环 + 递归 `./`，逻辑重复 | 抽 `name_eq_ci()` |
| 19 | `bs_inflate.c:279-324` | `inflate_dynamic` 不校验 code-length 树是否「完整」，依赖后续解码失败兜底 | 加 `left == 0 || 单符号` 的显式判断，错误信息更准 |
| 20 | `bs_cfg.h:8-10` | 注释说「C3 只用到识别相关几项，其余留着给 C4/C5」，实际 C4–C6 已实现而这些字段仍未用 | 与 P2-2 一并清理 |
| 21 | `app.cpp:489-494` | 进度条子控件没有 `WS_VISIBLE`、全文件也没有 `ShowWindow(prog_)`，真正显示的是 `paint_actions` 自绘的那条——两套实现，一套是死代码 | 删子控件或改自绘 |
| 22 | `app.cpp:580-581` | `PBM_SETSTATE` 两个分支同为 `PBST_NORMAL`（`cond ? PBST_NORMAL : PBST_NORMAL`） | 失败态应设 `PBST_ERROR`，或删掉该行 |
| 23 | `app.h:217`、`card.h:29`、`card.cpp:347` | `hot_sheet` 只被读、全工程无赋值，下拉悬停态永远是 false | 在 `WM_MOUSEMOVE` 里设置 |
| 24 | `dpi.h:16` | 注释称可返回 `"PerMonitorV2"`，`dpi.cpp:92-97` 只能返回 PerMonitor/System/Unaware | 改注释 |
| 25 | `layout.h:18` vs `layout.cpp:45-47`、`paint.cpp:18-23` | 注释说「找不到 id 要早点炸」，实际返回全 0 矩形，静默画到 (0,0) | 至少 `OutputDebugString` 或断言 |
| 26 | `app.cpp:1127-1139` | `dump_table` 不转义 `\t`/`\n`，与 `xlsx_dump.cpp:10-20` 的 `put_escaped` 不一致，含换行的描述列会拆散探针协议 | 复用 `put_escaped` |
| 27 | `main.cpp:106`、`layout_doc.cpp:78-81` | 导出布局文档时 DPI 恒传 96，文案却说「按当前显示器 DPI」 | 改文案或真取 DPI |
| 28 | `app.cpp:752-765`、`798-803`、`806`、`838` | 帮助回退把「`CreateProcess` 返回成功」当「已打开」；失败兜底错误码 `e1 ? e1 : 1` 可能显示不存在的错误码 1 | 用 `ShellExecuteEx` 的 `hProcess` 句柄判存活 |
| 29 | `app.cpp:703-712` | exe 路径 ≥520 宽字符时 `dir` 为空，落到相对路径「用户使用手册.html」（依赖 CWD） | 用 `\\?\` 或 `GetModuleFileNameW` 动态缓冲 |
| 30 | `app.cpp:1799-1809` | `WM_MOUSEMOVE` 无 `TrackMouseEvent`，鼠标直接移出窗口时 `hot_browse_` 残留高亮 | 加 `TME_LEAVE` |
| 31 | `table_src.cpp:14-19` | `widen()` 转换失败时不写 `buf[0]`（对比 `utf8.cpp:25-31` 先置空），会显示上一格文字 | 进入函数先 `buf[0] = 0` |
| 32 | `app.cpp:166-173` | `MSG` 未初始化、`GetMessageW` 返回 -1 未处理 | `MSG msg = {}`，-1 时退出 |
| 33 | `app.cpp:1506-1513` | `file_safe` 把 `\ / : * ? " < > \|` 统一换成 `_`，`A/B` 与 `A:B` 会撞名，后写的覆盖先写的 | 撞名时追加序号 |
| 34 | `app.h:189-191` vs `app.cpp:683` | 注释说档位到 0.90 为止，实际 `{…, 0.90, 0.95}` 七档 | 改注释 |
| 35 | `app.cpp:1563` vs `1458/1480/1490/1498` | 逐表比对外层进度 `5+90k/total` 与内层 `20/60/85/100` 互相覆盖，进度条来回跳 | 统一成「按表分段」的单一刻度 |
| 36 | `app.cpp:1337-1360` | 先 `bs_result_close(res_)`，后 `make_table()->set_source(nullptr)`，中间 `table_.src_` 悬空（当前无消息泵，属顺序脆弱点） | 先 `table_.set_source(nullptr)` 再释放 |

---

## 5 测试与质量体系的观察（经复核的部分）

这一节只收录我**亲自复核过**的条目；`build_c.bat` 未执行，因此旧文档里那个「168 PASS」基线本身当时无法验证。
**2026-11-07 第三批已处理 5.1–5.3**（负样本白名单、`check_pack` 的 SKIP、全交付文档版本断言），细节见 §9.5。

**5.1 「两边都拒 = PASS」是一条共谋通道（已复核）。**
`tools/check_pair.py:87-92`：

```python
except B.BomError as e:
    # 只要求「两边都拒」，错误文案不逐字对齐
    if got and got[0].startswith("ERR "):
        return True, "两边都拒（%s）%s" % (str(e)[:30], note), got
```

`check_sheet.py:100-104`、`check_write.py:423-431` 同款。对 `case_no_header` 这类负样本合理，但对真实样本也一视同仁——只要 C 和 Python 同时对某张真实 BOM 判定「无表头」，就打印 PASS。而「改口径必须两侧同改」恰恰是这套体系规定的日常操作，一次同步回归可以让整链全绿。建议加「必须识别成功」的样本白名单，只允许负样本走这条分支。

**5.2 `check_pack` 缺 openpyxl 时把「读回」降级为 SKIP 且不计失败（已复核）。**
`tools/check_pack.py:147-154`：`load_workbook is None` → `print("SKIP 读回 ...")` → `continue`，`bad` 不变。干净目录验收于是只剩「退出码 0 + 三个文件名」。建议把环境缺失计为 FAIL（或至少让整轮退出码非零）。

**5.3 版本号单一来源只护住了 README（已复核）。**
`pack.py:83-88` 只断言 `ver in readme_txt`；`docs/用户使用手册.md` 里写死 8 处 `V4.1.6`，`GUI控件位置对照表.md`/`对照图.html` 也是。升版本时 README 会红，手册静默过期。建议手册用占位符，或 pack 前对全部交付文档做版本断言。

**5.4 12 列对账的粒度确实很高（正面）。**
`check_pair` 用 `row\t...` 逐字段全等，`check_write` 用 openpyxl 读回比单元格文本 + 填充色 + 列宽 + 冻结窗格 + 筛选范围。这是本项目最扎实的部分，也是为什么 §2.6 那种「两边抄同一个错」才格外值得警惕——越严的对账，越依赖「两侧不是同一个作者的同一次误解」。

**5.5 `check_ui` 的项数是条件性的（已复核）。**
`check_ui.py:793-808`：样本里没有重复料号时才 `n += 1`，否则总数变 22 而脚本仍 PASS（文档写 23 项）。

---

## 6 逐模块走查结论

| 模块 | 结论 | 主要问题 |
|---|---|---|
| `bs_file.c` | 良好 | 无 `\\?\` 长路径支持；`buf==NULL` 泄漏 |
| `bs_zip.c` | 良好（边界检查到位） | 不校验 CRC；不支持 ZIP64（已文档化） |
| `bs_inflate.c` | 良好（块型/huffman/距离检查齐全） | 无输出上限；对 code-length 树的完整性只做隐式检查 |
| `bs_deflate.c` | 良好（缓冲按最坏情况预留并二次校验） | 全局懒初始化表非线程安全（当前只写路径用） |
| `bs_xml.c` | 中等 | 不校验 UTF-8/代理区；属性区指针比较存在形式上的越界指针算术（实测安全） |
| `bs_xlsx.c` | **薄弱** | 列号/行号溢出（P0/P1 根源）、`atoi` 无校验、诊断数值 |
| `bs_sheet.c` | 中等 | `present_cols` 越界（P0 落点）、行扫描上界、合并单元格、死配置 |
| `bs_pn.c` | 良好 | 全角/空白归一、Jaccard、快路径都正确；仅 `to_upper` 覆盖面小于 Python `.upper()` |
| `bs_pair.c` | 良好但有两个结构风险 | 阶段 C 复杂度、全局 `g_o/g_n`、阶段 A/B 的 O(k²) |
| `bs_write.c` | 中等 | 诊断列重复；OOM 双释放；改名重试策略 |
| `bs_zipw.c` | 良好 | 契约「失败也 free(data)」与调用方理解不一致（P1 根源） |
| `bs_mark.c` | 良好（最复杂也最克制） | 批注三件套、样式克隆、legacyDrawing 位置都正确；逐列涂色与 pair 重复实现 |
| `bs_arena.c` | 良好 | 无 |
| `app.cpp` | 中等 | 停止按钮空实现、长任务阻塞 UI、查重模式另存失效、`bs_result_build` 失败被吞、逐表残留 |
| `table.cpp` | 良好 | 大行数滚动条 `HIWORD`（§2.9）；`metrics()` 不还原 DC 字体 |
| `card.cpp` | 中等 | `AUTOSCAN_ROWS` 早退、失败返回 true、自动挑表无提前退出 |
| `main.cpp` / `drop.cpp` / `dpi.cpp` / `paint.cpp` | 良好 | OLE 生命周期（§2.11）、`delete this` 无虚析构、`wstring` 写入 NUL、`DT_NOPREFIX`（§2.10）、`AdjustWindowRectExForDpi` 参数 |
| `layout.cpp` / `layout_gen.h` / `table_src.cpp` / `utf8.cpp` | 良好 | 静默空矩形；`widen()` 不清缓冲 |

---

## 7 修复优先级建议

**第一批（可以直接消除崩溃/挂死面，建议当轮发版）—— ✅ 2026-11-07 已完成**

1. ✅ §2.1 列号/行号范围校验（`bs_xlsx.h` 加 `BS_MAX_ROW/BS_MAX_COL`；`col_from_ref` / `row_from_ref` / `row_from_attr` 三个入口 + `present_cols` 防御）；
2. ✅ §2.2 行扫描上界（越过上限的行整行忽略；扫描上界取「最后一个有内容的行」，`b.end` 保持不变以免破坏诊断页契约）；
3. ✅ 回归样本进 `tools/check_fuzz.py`（新增 4 例：列号溢出、行号 INT_MAX、行号 1048577、行号 1e9；超时计为 `HANG` 失败），并跑通全量对账。

> 验证：`check_sheet` 33/0、`check_pair` 34/0、`check_write` 26+2/0、`check_mark` 8+4/0、`check_fuzz` 18 例 0 崩溃 0 挂死、`ctest` 4/4；两个 repro 样本均 `exit 0`、约 0.13 s。

**第二批（用户当天就能撞到的功能错误）—— ✅ 2026-11-07 已完成（P1-04 部分）**

4. ✅ §2.8 查重模式另存 + 把「外观/可点」统一到 `can_run()`；
5. ✅ §2.9 滚动条 `SIF_TRACKPOS`；
6. ✅ §2.10 `draw_text` 加 `DT_NOPREFIX`（一处改动，全局收益）；
7. ✅ §2.11 OLE 生命周期（`WM_DESTROY` 里 `drop_revoke`）；
8. ✅ §2.6 诊断页「数据行」语义 + `check_write` 的独立恒等式断言；
9. ✅ §2.3 合并单元格 span 取值（C 与 `bomcore._Grid` 同步改，`check_xlsx` 逐格对账仍绿）；
10. ◐ §2.5 停止按钮：已移出命中表、说明改为「占位」；UI 线程阻塞与取消仍未实现。✅ §2.7 所有权契约统一（`s1/s2` 先置空）。

**第三批（体系与文档）—— ✅ 2026-11-07 已完成**

11. ✅ §5.1 三套脚本加负样本白名单（白名单外「两边都拒」= FAIL）；§5.2 `check_pack` 缺 openpyxl 改计 FAIL；§5.3 `pack.py` 对 README/手册 md+html/对照表统一断言版本号；
12. ✅ §2.4 阶段 C 加 `FUZZY_MAX_PAIRS=5e7` / `FUZZY_MAX_CAND=20万` 护栏与降级（C 与 oracle 同值）；
13. ✅ §3.6 的 P2-19～P2-23（日志 `EM_SETLIMITTEXT`、DPI 重发字体、`metrics` 不再泄漏 HFONT、`bs_result_build` 失败可见、逐表失败清理与改名分别记成功）；
14. ✅ §3.5 的六处文档-实现不符（合并单元格已实现、`stored` 注释、按钮文案与对照图、exe 体积、168 基线、check_ui 条件项）。

---

## 8 附录：复现材料与命令

`repro\` 目录：

| 文件 | 用途 |
|---|---|
| `control_clean.xlsx` | 对照组：结构相同的正常 BOM（表头 + 2 行数据） |
| `crash_bad_colref.xlsx` | 含 `<c r="ZZZZZZZ1">`，触发 §2.1 的越界写 |
| `hang_row_intmax.xlsx` | 含 `<row r="2147483647">`，触发 §2.2 的死循环 |

```powershell
$exe = 'D:\AI_Workspace\workbuddy_spa\BOMcheck\build\c\bin\BOMCheck_V4.1.6.exe'
$d   = 'D:\AI_Workspace\workbuddy_spa\BOMcheck\CodeReview\repro'

# 1) 对照组：约 0.1 s，exit 0
& $exe --drop-test "$d\control_clean.xlsx" "$d\control_clean.xlsx" --dump-table

# 2) P0：exit 0xC0000005
& $exe --drop-test "$d\control_clean.xlsx" "$d\crash_bad_colref.xlsx" --dump-table

# 3) P1：20 s 内不退出（Ctrl+C 或 Stop-Process 结束）
& $exe --drop-test "$d\control_clean.xlsx" "$d\hang_row_intmax.xlsx" --dump-table
```

复现样本只包含 `<workbook>`/`<worksheet>`/`<rels>`/`[Content_Types].xml` 五个部件，均为手写最小包，不含宏、不含外部链接，可安全打开/删除。

---

## 9 问题状态跟踪表

> 状态取值：`未开始` / `进行中` / `部分修复` / `已修复（待验证）` / `已修复 / 已验证`。
> 第一批（2026-11-07 10:40）P0-01、P1-01、P3-04；第二批（11:10）P1-02/05/06/07/08/09/10，P1-04 部分；第三批（11:20）P1-03、P2-14～P2-23 与 §5 的测试/文档三项。验证见 §9.3。

### 9.1 P0 / P1 / P2（39 条）

| 编号 | 级别 | 问题 | 状态 | 修复 / 验证记录 |
|---|---|---|---|---|
| P0-01 | P0 | 列号整数溢出：<c r="ZZZZZZZ1"> 回绕成 -236852010，present_cols 负下标越界写 | 已修复 / 已验证 | 已修复（已验证）：col_from_ref 限 3 字母/16384 列，row_from_ref 限 1048576；present_cols 加边界；repro 样本 exit 0 |
| P1-01 | P1 | max_row=INT_MAX 时行扫描 r++ 溢出，循环条件被优化成恒真 | 已修复 / 已验证 | 已修复（已验证）：行号>1048576 整行忽略；扫描上界取“最后一个有内容的行”；repro 样本 0.13s 退出 |
| P1-02 | P1 | 合并单元格只用于识别标题行，从未按 span 头补全取值 | 已修复 / 已验证 | bs_xlsx.c 新增 expand_merges（span 头铺满合并区），bomcore._Grid 同口径；check_sheet 33/0、check_xlsx 7/0；合并料号格样本现在 3 行全部建档（dup 1/3..3/3） |
| P1-03 | P1 | 双键近似阶段是完整笛卡尔积，候选表无上限 | 已修复（待验证） | bs_pair.c 加 FUZZY_MAX_PAIRS=5e7 / FUZZY_MAX_CAND=20万 护栏，超限跳过近似、交给阶段 D；bomcore 同值；未构造 5e7 级样本实测 |
| P1-04 | P1 | “停止”按钮无 case 3 处理；比较与写盘全在 UI 线程，无取消 | 部分修复 | act.stop 已移出命中表并改为「占位」说明（不再假装可取消）；比对/写出仍在 UI 线程同步执行，取消与进度未实现 |
| P1-05 | P1 | 诊断页“数据行”与“建档行”写同一个 data_rows；oracle 测试硬编码同一错误公式 | 已修复 / 已验证 | bs_sheet_data_rows = 建档+无键+分组标题；诊断第 4/8 列分开；check_write 新增独立恒等式断言，26+2/0；主样本（未随仓库分发）诊断由 157/157 变为 168/157 |
| P1-06 | P1 | bs_zipw_add 失败路径已 free(data)，调用方 oom 分支再次 free(s1.p) | 已修复（待验证） | bs_write 把 s1/s2 所有权交给 bs_zipw_add 前先置空，oom 分支不再二次 free；OOM 路径本身未实测 |
| P1-07 | P1 | 查重模式不生成 res_，但“另存结果”仍调 write_all → 必失败；按钮外观却显示可用 | 已修复 / 已验证 | 拆出 write_dups(dir)；查重模式点「另存结果」实测写出 dup_sample_查重标注.xlsx；差异模式仍写 3 份 |
| P1-08 | P1 | SetScrollInfo 用 32 位范围，SB_THUMBTRACK 用 HIWORD 只有 16 位 | 已修复（待验证） | WM_VSCROLL / WM_HSCROLL 的 SB_THUMBTRACK 改用 GetScrollInfo(SIF_TRACKPOS) 的 32 位位置 |
| P1-09 | P1 | 唯一的 DrawTextW 出口没有 DT_NOPREFIX，& 被当加速键前缀吞掉 | 已修复（待验证） | paint.cpp 的 draw_text 统一加 DT_NOPREFIX（唯一 DrawTextW 出口），& 不再被当加速键吞掉 |
| P1-10 | P1 | OleUninitialize 先于 ~App，drop_revoke 在窗口销毁/OLE 反初始化之后执行 | 已修复（待验证） | WM_DESTROY 里先 drop_revoke 再 PostQuitMessage，RevokeDragDrop 不再发生在 OleUninitialize 之后 |
| P2-01 | P2 | header_scan_rows 是死配置，错误文案仍写“前 50 行内”，实际全表扫描 | 未开始 | — |
| P2-02 | P2 | block_mode / alias_exact / ref_require_token / skip_label_rows / seg1-3 六个字段从未被读 | 未开始 | — |
| P2-03 | P2 | 自动挑表以 AUTOSCAN_ROWS=20 提前收敛，并遍历全部工作表无提前退出 | 未开始 | — |
| P2-04 | P2 | 分析失败仍 return true，调用方按返回值判成功 | 未开始 | — |
| P2-05 | P2 | n_skipped 未进 stat 行、未进 py_dump、check_ui 也不断言 | 未开始 | — |
| P2-06 | P2 | 阶段 A/B 对同键重复行是 O(k²)：每行都从桶头 htab_find | 未开始 | — |
| P2-07 | P2 | 文件级全局 g_o/g_n 传行数组，bs_pair_rows 与 bs_result_build 均不可重入 | 未开始 | — |
| P2-08 | P2 | 并行载入两个线程并发 += 全局 double 计数器（非原子） | 未开始 | — |
| P2-09 | P2 | 解压输出无上限，也不校验是否超过目录里的 usize（zip bomb） | 未开始 | — |
| P2-10 | P2 | 读取条目时从不校验 CRC32 | 未开始 | — |
| P2-11 | P2 | 逐表比对以“上一轮产物为源”累积，每轮往 styles.xml 追加 4 fill + N xf | 未开始 | — |
| P2-12 | P2 | 保存失败一律当“重名”，换 _2…_19 重试 20 次 | 未开始 | — |
| P2-13 | P2 | 逐列比较与涂色把列范围硬截在 512 | 未开始 | — |
| P2-14 | P2 | 文档称“合并单元格以 span 记录，取值走 span 头”，实现没有（见 P1-02） | 已修复 / 已验证 | 随 P1-02：合并单元格已按 span 头取值，文档描述与实现一致 |
| P2-15 | P2 | 头文件称“整包用 stored（不压缩）”，实现是变小才 deflate | 已修复 / 已验证 | bs_write.h 注释放改为「按内容挑 method：变小才 deflate，否则 stored」 |
| P2-16 | P2 | 布局单一数据源写“重写结果…”，实绘与 README/手册都是“另存结果…” | 已修复 / 已验证 | layout_spec 文案改为「另存结果…」，docs/GUI控件位置对照图_v4.html 已重新生成并 grep 确认 |
| P2-17 | P2 | README 写“约 660 KB”，实测 705,024 B≈688 KB，设计文档写“约 700 KB” | 已修复 / 已验证 | README 660KB→690KB（以 --self-check 的 size 为准），设计文档 700→690 |
| P2-18 | P2 | “168 PASS”无脚本汇总；check_ui 第 23 项条件性计数，可静默变 22 项仍 PASS | 已修复 / 已验证 | 设计文档不再写死 168；check_ui 第 23 项改为无条件计数（前提不成立即 FAIL），实测 23 项 0 失败 |
| P2-19 | P2 | 日志 EDIT 未设 EM_SETLIMITTEXT（全仓 0 处），默认 32767 字符上限 | 已修复（待验证） | 创建日志 EDIT 后 EM_SETLIMITTEXT=0，32767 上限解除 |
| P2-20 | P2 | WM_DPICHANGED 重建字体但不重发 WM_SETFONT | 已修复（待验证） | apply_dpi 重建字体后重发 WM_SETFONT（日志/进度条） |
| P2-21 | P2 | metrics() 借 DC 不还原字体，drop_fonts 的 DeleteObject 因此失败 | 已修复（待验证） | Table::metrics 去掉手工 SelectObject（text_w 自己还原），drop_fonts 不再泄漏 HFONT |
| P2-22 | P2 | bs_result_build 失败无 else，has_pairs_ 仍为 true | 已修复（待验证） | bs_result_build 失败时 has_pairs_=false 并记 pair_note_，不再显示成「差异 0 行」 |
| P2-23 | P2 | 失败轮已落盘的 .partK 不入 parts 也不删；两次 MoveFileEx 共用一个 renamed | 已修复（待验证） | 逐表失败清掉本轮结果与两个 .partK；两次 MoveFileEx 分别记 ok_o/ok_n，日志不再指向已移走的路径 |
| P2-24 | P2 | 菜单 WM_PAINT 未 SetBkMode(TRANSPARENT) | 未开始 | — |
| P2-25 | P2 | 父窗口不转发滚轮，表格要在获得焦点后才收到 | 未开始 | — |
| P2-26 | P2 | GetOpenFileName 失败一律当取消，nMaxFile=520 而 manifest 宣称 longPathAware | 未开始 | — |
| P2-27 | P2 | AdjustWindowRectExForDpi 第三参是 bMenu，却传了 ex?TRUE:FALSE | 未开始 | — |
| P2-28 | P2 | 载入同步执行，起线程后仍在 UI 线程 WaitForSingleObject(INFINITE) | 未开始 | — |

### 9.2 P3（36 条，仅列问题与状态）

| 编号 | 问题 | 状态 |
|---|---|---|
| P3-01 | `std::wstring s(len, L'\0'); DragQueryFileW(..., &s[0], len+1)` 写入 `len+1` 个 wchar，越过了 `size()`（libstdc++ 恰好分配了终止符槽位，实际安全，形式上是 UB） | 未开始 |
| P3-02 | 多态类 `delete this` 且无虚析构（`-Wdelete-non-virtual-dtor`） | 未开始 |
| P3-03 | `parse_qty` 接受 `nan`/`inf`；`bs_pair.c:621-624` 输出 `"nan"` | 未开始 |
| P3-04 | `row_from_ref` 同样无溢出保护（影响 `mergeCells`） | 已修复 / 已验证（row_from_ref 限幅） |
| P3-05 | `put_utf8` 对 `0xD800-0xDFFF` 输出 CESU-8 非法序列；`&#0;` 会写入 NUL 截断字符串 | 未开始 |
| P3-06 | `sb_esc` 把控制字符直接丢弃、`\t\n\r` 换成空格 | 未开始 |
| P3-07 | 逐列涂色与 `bs_pair.c:340-356` 各自实现了一遍「列范围取并集」，两处上限（512）与归一化须手工同步 | 未开始 |
| P3-08 | `mark_push` 返回值全部忽略，OOM 时静默少涂 | 未开始 |
| P3-09 | 为了取一个部件名，把整个 workbook 重新 `bs_xlsx_open` 一遍（每次标注 2 次全量解析） | 未开始 |
| P3-10 | 替换部件按 `strcmp` 精确匹配；源包大小写不同（`xl/Worksheets/...`）时会追加同名不同大小写的条目 | 未开始 |
| P3-11 | `alt_path` 的 `%.*s_%d%s` 在 `stem` 超长时静默截断 | 未开始 |
| P3-12 | `bs_read_file` 在 `buf == NULL` 时泄漏整块缓冲（`-fanalyzer` 报出） | 未开始 |
| P3-13 | 字符串池 `strs_len` 用 `unsigned` 偏移，超 4 GB 溢出（理论上不可达） | 未开始 |
| P3-14 | 弹出菜单用 `SetCapture` 而不取焦点，`VK_ESC/UP/DOWN/RETURN` 分支永远收不到（Esc 关不掉下拉） | 未开始 |
| P3-15 | `delta / 120` 在高精度滚轮（delta < 120）下为 0，滚不动 | 未开始 |
| P3-16 | `fmt_size(...)` 算完 `(void)sz` 丢掉，死代码 | 未开始 |
| P3-17 | `write_all` 里的文件名拼接没做长度检查，超长路径会截断到 260/1024 | 未开始 |
| P3-18 | 大小写不敏感兜底用逐字符循环 + 递归 `./`，逻辑重复 | 未开始 |
| P3-19 | `inflate_dynamic` 不校验 code-length 树是否「完整」，依赖后续解码失败兜底 | 未开始 |
| P3-20 | 注释说「C3 只用到识别相关几项，其余留着给 C4/C5」，实际 C4–C6 已实现而这些字段仍未用 | 未开始 |
| P3-21 | 进度条子控件没有 `WS_VISIBLE`、全文件也没有 `ShowWindow(prog_)`，真正显示的是 `paint_actions` 自绘的那条——两套实现，一套是死代码 | 未开始 |
| P3-22 | `PBM_SETSTATE` 两个分支同为 `PBST_NORMAL`（`cond ? PBST_NORMAL : PBST_NORMAL`） | 未开始 |
| P3-23 | `hot_sheet` 只被读、全工程无赋值，下拉悬停态永远是 false | 未开始 |
| P3-24 | 注释称可返回 `"PerMonitorV2"`，`dpi.cpp:92-97` 只能返回 PerMonitor/System/Unaware | 未开始 |
| P3-25 | 注释说「找不到 id 要早点炸」，实际返回全 0 矩形，静默画到 (0,0) | 未开始 |
| P3-26 | `dump_table` 不转义 `\t`/`\n`，与 `xlsx_dump.cpp:10-20` 的 `put_escaped` 不一致，含换行的描述列会拆散探针协议 | 未开始 |
| P3-27 | 导出布局文档时 DPI 恒传 96，文案却说「按当前显示器 DPI」 | 未开始 |
| P3-28 | 帮助回退把「`CreateProcess` 返回成功」当「已打开」；失败兜底错误码 `e1 ? e1 : 1` 可能显示不存在的错误码 1 | 未开始 |
| P3-29 | exe 路径 ≥520 宽字符时 `dir` 为空，落到相对路径「用户使用手册.html」（依赖 CWD） | 未开始 |
| P3-30 | `WM_MOUSEMOVE` 无 `TrackMouseEvent`，鼠标直接移出窗口时 `hot_browse_` 残留高亮 | 未开始 |
| P3-31 | `widen()` 转换失败时不写 `buf[0]`（对比 `utf8.cpp:25-31` 先置空），会显示上一格文字 | 未开始 |
| P3-32 | `MSG` 未初始化、`GetMessageW` 返回 -1 未处理 | 未开始 |
| P3-33 | `file_safe` 把 `\ / : * ? " < > \|` 统一换成 `_`，`A/B` 与 `A:B` 会撞名，后写的覆盖先写的 | 未开始 |
| P3-34 | 注释说档位到 0.90 为止，实际 `{…, 0.90, 0.95}` 七档 | 未开始 |
| P3-35 | 逐表比对外层进度 `5+90k/total` 与内层 `20/60/85/100` 互相覆盖，进度条来回跳 | 未开始 |
| P3-36 | 先 `bs_result_close(res_)`，后 `make_table()->set_source(nullptr)`，中间 `table_.src_` 悬空（当前无消息泵，属顺序脆弱点） | 未开始 |

### 9.3 验证结果（独立构建目录 `%TEMP%\bs_fix\build2`，未触碰仓库内 build\ 与 dist\）

| 检查项 | 结果 |
|---|---|
| `check_sheet`（33 张表） | 33 张，0 失败（负样本白名单已收口） |
| `check_xlsx`（7 个文件，读取链逐格） | 7 个，0 失败 |
| `check_pair`（34 组） | 34 组，0 失败 |
| `check_write`（26 组 + 2 异常路径） | 0 失败（含诊断恒等式断言） |
| `check_mark`（8 组 + 4 专项） | 0 失败 |
| `check_ui`（23 项） | 23 项，0 失败（第 23 项不再条件性消失） |
| `check_fuzz`（18 例） | 18 例，0 崩溃 / 0 挂死 |
| `ctest` | 4/4 通过 |
| `repro\crash_bad_colref.xlsx` | 0xC0000005 → **exit 0，0.12 s** |
| `repro\hang_row_intmax.xlsx` | >20 s 未退出 → **exit 0，0.13 s** |
| `--ui-run` 主样本对（真实项目，未随仓库分发） | `ok=1`，配对 / 新增 / 删除 / 差异逐项与 oracle 一致，5 个产物 |
| 查重 / 差异「另存结果」 | 分别写出 1 份查重标注 / 3 份比对产物 |
| 竖向合并料号格 | 3 行全部建档（dup 1/3、2/3、3/3） |
| 超大合并区 `A1:XFD1048576` | 走护栏跳过，0.18 s 返回 |

### 9.4 改动明细

| 批次 | 文件 | 改动 |
|---|---|---|
| 1 | `src/core/bs_xlsx.h` | 新增 `BS_MAX_ROW` / `BS_MAX_COL` |
| 1 | `src/core/bs_xlsx.c` | `col_from_ref` 限 3 字母/16384、`row_from_ref` 限 1048576、`row_from_attr` 越界返回 -1 + `skip_row` |
| 1 | `src/core/bs_sheet.c` | `present_cols` 边界；`last_content_row()` 作扫描上界（`b.end` 不变） |
| 1 | `tools/check_fuzz.py` | 新增 5 个越界用例；超时计 `HANG` 失败 |
| 2 | `src/ui/paint.cpp` | `DT_NOPREFIX` |
| 2 | `src/ui/table.cpp` | 滚动条 `SIF_TRACKPOS` |
| 2 | `src/ui/app.cpp` / `app.h` | `WM_DESTROY` 撤拖放；`write_dups(dir)`；按钮门禁 `can_run()`；`act.stop` 移出命中表；日志「数据行」用总行数 |
| 2 | `src/core/bs_write.c` / `.h` | 所有权前置空；`built_rows` 字段与第 8 列 |
| 2 | `src/core/bs_sheet.c` / `.h` | `bs_sheet_data_rows()` |
| 2 | `src/core/bs_xlsx.c` | `cell_find()` + `expand_merges()`（含 4096/20 万护栏） |
| 2 | `gui/bomcore.py` | `_Grid` 铺 span 头（同护栏） |
| 2 | `gui/layout_spec.py` | `act.stop` 说明改为「占位」 |
| 2 | `tools/check_write.py` | 诊断第 4 列公式 + `diag_identity()` 独立断言 |
| 3 | `src/core/bs_pair.c` / `gui/bomcore.py` | 近似阶段 `FUZZY_MAX_PAIRS=5e7` / `FUZZY_MAX_CAND=20万` 护栏与降级 |
| 3 | `src/ui/app.cpp` | 日志 EDIT `EM_SETLIMITTEXT`；`apply_dpi` 重发 `WM_SETFONT`；`bs_result_build` 失败可见；逐表失败清理与 `ok_o/ok_n` 改名 |
| 3 | `src/ui/table.cpp` | `metrics()` 不再手工选字体（消除 HFONT 泄漏） |
| 3 | `src/core/bs_write.h` | 「整包 stored」注释改为按内容挑 method |
| 3 | `gui/layout_spec.py` + `docs/GUI控件位置对照图_v4.html` | 按钮文案统一为「另存结果…」，对照图重新生成 |
| 3 | `README.txt` + `docs/BOM对比工具_V4_设计与实施.md` | exe 体积口径统一；去掉写死的「168 PASS」 |
| 3 | `tools/pack.py` | 打包前对 README/手册 md,html/对照表统一断言版本号 |
| 3 | `tools/check_pack.py` | 缺 openpyxl 时「读回」由 SKIP 改为 FAIL |
| 3 | `tools/check_sheet.py` / `check_pair.py` / `check_write.py` | 「两边都拒」加负样本白名单，真实样本一律 FAIL |
| 3 | `tools/check_ui.py` | 第 23 项改为无条件计数（前提不成立即 FAIL） |

### 9.5 第三批的三个体系改动（对应 §5）

- **§5.1「两边都拒 = PASS」已收口**：`check_sheet` / `check_pair` / `check_write` 各自维护 `NEGATIVE` 白名单（`case_no_header`、`cover_first`、STRICT 重复键样本），白名单外两侧同时拒绝一律 FAIL。实测这三套脚本在收口后仍 0 失败。
- **§5.2 `check_pack` 的 SKIP 已改为 FAIL**：没有 openpyxl 时「读回」这一项不再打印 SKIP 跳过，而是计失败并说明验收不完整。
- **§5.3 版本单一来源扩到全部交付文档**：`pack.py` 现在对 `README.txt`、`用户使用手册.md/.html`、`GUI控件位置对照表.md` 逐个断言版本号，任一份没跟上就直接中止打包。

---

*本报告的每一条 P0/P1 都给出了「文件:行号 + 代码摘录」或「可执行复现步骤」。P2/P3 中标注来源为「第二路审查」的条目（§5、§3.5 部分）已由我复核原文；未复核的脚本级细节未写入本报告。修复状态以 §9 为准，随批次更新。*
