# BOMCheck V4.1.6 代码审查记录

本目录是 2026-11-07 对 `BOMCheck V4.1.6` 的代码审查与修复记录。
**审查阶段全程只读**（未改任何源文件、未重跑 `build_c.bat`）；审查结束后执行了两批修复，改动落在 `src/core`、`src/ui`、`gui`、`tools`，逐条记录见报告 §9。

## 修复状态（2026-11-07）

| 批次 | 内容 | 状态 |
|---|---|---|
| 第一批 | P0-01（列号溢出越界写）、P1-01（行号溢出死循环）、P3-04（`row_from_ref` 限幅） | ✅ 已修复并验证 |
| 第二批 | P1-02 合并单元格、P1-05 诊断数据行、P1-06 OOM 双释放、P1-07 查重另存、P1-08 滚动条、P1-09 `&` 被吞、P1-10 OLE 生命周期；P1-04 停止按钮只做了「不再假装可点」 | ✅ 已完成（P1-04 部分） |
| 第三批 | P1-03 阶段 C 规模护栏、P2-14～P2-23（日志/字体/GDI/失败可见/逐表残留/文档口径），以及 §5 的三项体系改动（负样本白名单、SKIP 计失败、全文档版本断言） | ✅ 已完成 |

> **状态更新（2026-11-07 11:10）**：三批修复已随 **V4.2.0** 重建并交付 ——
> `build\c\bin\BOMCheck_V4.2.0.exe`（706,560 B）与 `dist\BOMCheck_V4.2.0\` 的副本 md5 一致，
> `build_c.bat` 15 步 EXIT=0 / 173 PASS / 0 FAIL / ctest 4/4，干净目录验收 0 失败；
> 两份 repro 样本在交付版上分别 0.37 s / 0.40 s 正常退出（原为 0xC0000005 与永久卡死）。
> 审查时看的是修复前的 V4.1.6 二进制，此前的提示（「dist 里仍是修复前二进制」）已失效。

## 目录内容

| 文件 | 说明 |
|---|---|
| `BOMCheck_V4.1.6_代码审查报告.md` | 主报告：结论摘要、方法与覆盖范围、P0/P1 逐条详情（含代码摘录与复现命令）、P2/P3 清单、测试体系观察、逐模块结论、修复顺序、**§9 问题状态跟踪表** |
| `BOMCheck_V4.1.6_问题清单.csv` | 机器可读的问题清单（UTF-8 with BOM，Excel 可直接打开）。共 39 条：P0×1、P1×10、P2×28，含「状态」「修复记录」两列；P3 的 36 条以表格形式留在主报告 §4/§9.2 |
| `repro/control_clean.xlsx` | 对照样本：结构相同的正常 BOM（表头 + 2 行数据），用于证明问题样本才有异常 |
| `repro/crash_bad_colref.xlsx` | P0 样本：`<c r="ZZZZZZZ1">`，列号溢出成负数 → 越界写 → 退出码 `0xC0000005` |
| `repro/hang_row_intmax.xlsx` | P1 样本：`<row r="2147483647">`，行扫描 `r++` 溢出 → 不终止 |

## 三分钟版结论

1. 整体工程质量偏上：core 层零第三方依赖、模块边界清晰，`-Wall -Wextra` 下 core 只有 1 条告警；标注「在原包内改样式、其余部件按压缩字节搬运」的设计实现得相当克制。
2. **有一个 P0**：打开一份 1.5 KB 的构造 xlsx 会堆越界写并崩溃（非法列号 → `int` 溢出成负数 → 当作下标写进位图）。任何人发来的文件都能触发，不需要用户点任何按钮。
3. **有一个「永久卡死」**：另一份 1.5 KB 的构造 xlsx 让行号等于 `INT_MAX`，行扫描在 `INT_MAX + 1` 处有符号溢出，`-O2` 优化后循环不终止。
4. 界面层没有 P0，但有 4 个 P1：查重模式「另存结果」必失败、>65535 行滚动条拖不过去、`DrawTextW` 缺 `DT_NOPREFIX` 让 `&` 被吞、OLE 反初始化早于窗口销毁。
5. 口径层面最值得警惕的是 **「同源缺陷」**：诊断页「数据行」与「建档行」在 C 与 Python 两侧是同一个错误公式，对账永远绿；「跳过 N 行」这个核心语义完全没有进入对账。

## 复现

```powershell
$exe = 'D:\AI_Workspace\workbuddy_spa\BOMcheck\build\c\bin\BOMCheck_V4.1.6.exe'
$d   = 'D:\AI_Workspace\workbuddy_spa\BOMcheck\CodeReview\repro'

# 对照：约 0.1 s，exit 0
& $exe --drop-test "$d\control_clean.xlsx" "$d\control_clean.xlsx" --dump-table

# P0：exit 0xC0000005
& $exe --drop-test "$d\control_clean.xlsx" "$d\crash_bad_colref.xlsx" --dump-table

# P1：20 s 内不退出（记得结束进程）
& $exe --drop-test "$d\control_clean.xlsx" "$d\hang_row_intmax.xlsx" --dump-table
```

样本只含 `[Content_Types].xml` / `_rels/.rels` / `xl/workbook.xml` / `xl/_rels/workbook.xml.rels` / `xl/worksheets/sheet1.xml` 五个部件，无宏、无外部链接，可安全打开或删除。

## 审查边界

- 已读：`src/core` 15 个 `.c` + 12 个 `.h` 全文；`src/ui` 14 个 `.cpp` + 15 个 `.h` 全文（关键路径由主审逐行复核）。
- 静态：`gcc -Wall -Wextra -Wshadow -Wformat=2 -fanalyzer`，以及一次 guard-page 分配器实验（用于排除「超长 XML 标签名」这条怀疑）。
- 未做：`build_c.bat` 未执行（会写入 `build\`/`dist\`），因此「168 PASS / ctest 4/4」基线未经本次验证；未做真实 Excel/WPS 兼容性核对；未做多屏 DPI 实测。
