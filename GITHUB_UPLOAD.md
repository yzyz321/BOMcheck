# GitHub 上传清单（BOMCheck V4.2.0，2026-11-07）

> 结论：**上传 131 个文件、约 1.74 MB**，最大单文件 220 KB（控件位置对照图 html），无二进制大件，
> 不需要 Git LFS。构建产物（`build/` 6.3 MB、`dist/` 1.5 MB）不进仓库，发版走 Releases。

---

## 0 敏感数据：已脱敏（2026-11-07）

原本 `tests/data/` 里有 4 份真实项目 BOM（客户料号 / 位号 / 厂商）。**已全部移出仓库**，
换成一对脚本生成的虚构样本：

| 文件 | 内容 |
|---|---|
| `tests/data/demo_old.xlsx` | 旧版：主板 BOM + 电源板 BOM 两张表，29 个数据行 |
| `tests/data/demo_new.xlsx` | 新版：同结构，改数量 / 改位号 / 划删除线 / 清料号 / 增删若干料号 |
| `tools/gen_demo.py` | 生成器：料号 `900-1001A0-001`、位号 `R1/C22/TP1`、厂商 `VendorA…`、单位「示例电子（虚构）」，**连续两次生成逐字节相同**（zip 时间戳与 `docProps` 已压平） |

脱敏不是只删文件，连带改了四处：

1. **回归链**（`check_pair` / `check_write` / `check_mark` / `check_ui`）：样本引用全部换成
   `demo_old/demo_new`，覆盖的口径一条没少 —— 同文件自比、跨表、反向（增删互换）、
   位号兜底、位号近似（8 个位号差 1 个，0.875）、删除线、无料号行跳过。
2. **文档里的实测数字**（`docs/用户使用手册.md` §五/§九/§十一）：按演示样本重测后改写，
   不再出现原项目的型号与配对数。
3. **截图**：`docs/images/ui_main.png` 用演示样本重截（150% 缩放，2430×1620），
   界面上的文件路径、料号、位号、厂商全部是虚构值；旧的 V3 界面截图 `gui_150pct.png`
   带本机路径，已删除。
4. **审查记录**（`CodeReview/`）：只把样本标识抹成「主样本（未随仓库分发）」，
   结论与数字一条没动 —— 它仍是审计记录。

`CodeReview/repro/*.xlsx` 是构造的畸形包（仅 5 个 XML 部件，无业务数据），可以传。
`tests/data/cases/` 18 份构造边界样本本就是虚构的，可以传。

**要恢复真实样本做回归**：把文件放回 `tests/data/`、文件名对得上 `tools/*.py` 里的引用即可；
或者直接改 `tools/check_*.py` 里的样本名 —— 那些脚本只认相对路径。

## 0.5 许可（已定：MIT）

`LICENSE` 已补（MIT，`Copyright (c) 2026 BOMCheck Contributors`），
README 末尾「许可」一节指向它。发 Releases 时无需在附件里再放许可文本。

---

## 1 上传清单（按目录）

| 路径 | 文件数 | 体积 | 内容 |
|---|---:|---:|---|
| `src/core/` | 29 | 249 KB | 纯 C99 引擎：`bs_file` `bs_zip` `bs_inflate` `bs_xml` `bs_xlsx` `bs_sheet` `bs_pn` `bs_pair` `bs_write` `bs_mark` `bs_deflate` `bs_zipw` `bs_arena` + `bs_cfg.h` `bs_version.*` |
| `src/ui/` | 28 | 205 KB | Win32 C++17 界面：`main` `app` `paint` `layout` `layout_gen.h` `card` `table` `table_src` `drop` `dpi` `bench` `selfcheck` `xlsx_dump` `layout_doc` `utf8` |
| `src/res/` | 4 | 141 KB | `app.ico`（10 档图标）、`app.manifest.in`、`app.rc`、`bs_version_gen.h.in` |
| `tests/` | 22 | 147 KB | `test_core.c`（ctest 4 项）+ `data/` 21 份 xlsx（18 份 `cases/` 构造样本、2 份 `demo_old/new` 演示样本、1 份 `dup_sample`） |
| `tools/` | 22 | 221 KB | `build/` 辅助：`gen_layout_c` `gen_demo` `pack` `md2html` `make_layout_doc` `make_icon` `shot` `findexe` `samples` `gen_big` `gen_sheet_cases` + 11 个 `check_*.py` |
| `pyref/` | 3 | 57 KB | `bomcore.py`（对账 oracle）、`layout_spec.py`（布局单一数据源）、`README.md`（两者为什么还留着） |
| `docs/` | 8 | 633 KB | 设计文档、用户手册 md+html、流程图、控件位置对照表/对照图、1 张脱敏截图、`README.md` 索引 |
| `CodeReview/` | 6 | 91 KB | V4.1.6 审查报告、问题清单 CSV、3 份复现样本 |
| `cmake/` | 1 | 0.2 KB | `mingw-no-default-manifest.spec` |
| 根目录 | 8 | 32 KB | `CMakeLists.txt`（版本唯一来源）、`build_c.bat`、`README.md`（GitHub 首页）、`README.txt`（随包说明）、`LICENSE`（MIT）、`.gitignore`、`.gitattributes`、本文件 |
| **合计** | **131** | **1.74 MB** | |

## 2 不传的（已在 `.gitignore` 里）

| 路径 | 体积 | 为什么不传 |
|---|---:|---|
| `build/` | 6.3 MB | CMake + Ninja 增量目录，`.obj` / `.ninja` / 生成的版本头，换机跑一次 `build_c.bat` 全都有 |
| `dist/` | 1.5 MB | `pack.py` 产物（exe + zip）。二进制走 GitHub Releases，仓库只留 tag |
| `temp/` | 0 | 本地试验目录（样本、临时脚本、截图草稿） |
| `.workbuddy/` | 100 KB | 本机 AI 助手的工作记忆（个人笔记） |
| `__pycache__/`、`*.pyc` | — | Python 缓存 |
| `*.log` `*.tmp` `Thumbs.db` | — | 临时与系统垃圾 |

> 若确实想连二进制一起提交：删掉 `.gitignore` 里 `/dist/` 那行，并把 `dist/BOMCheck_V4.2.0/` 的
> 5 个文件一起 add。代价是每次发版仓库都多 1.5 MB，且 diff 不可读 —— 不推荐。

## 3 首次推送

```bash
cd D:/AI_Workspace/workbuddy_spa/BOMcheck

git init -b main
git add -A
git status --short | head -30        # 应只看到上表这些路径
git ls-files | wc -l                 # 期望 131

git commit -m "BOMCheck V4.2.0：C 重写版 BOM 对比工具（含代码审查修复）"

git remote add origin git@github.com:<账号>/<仓库>.git
git push -u origin main
```

推送前跑这三条自检（都应该没有输出）：

```bash
git status --short | grep -E "^A  (build|dist|temp|\.workbuddy)/"   # 误纳构建产物
git ls-files -z | xargs -0 file | grep -i "crlf" | grep -v "\.bat\|\.rc\|\.txt"  # 非预期 CRLF
git ls-files | xargs -I{} du -k {} 2>/dev/null | sort -rn | head -5  # 最大文件
```

## 4 发版（二进制走 Releases）

```bash
git tag -a v4.2.0 -m "BOMCheck V4.2.0（审查修复版）"
git push origin v4.2.0
```

然后在 GitHub → Releases → Draft a new release：选 `v4.2.0`，上传 `dist/BOMCheck_V4.2.0.zip`
（512 KB，含 exe + README.txt + 用户使用手册 md/html + 控件位置对照表），正文可摘自 `README.txt`
的「V4.2.0 修了什么」八条。

## 5 上传前最后一遍

- [ ] `build_c.bat` 跑通：15 步 EXIT=0、全 PASS、ctest 4/4（本机 2026-11-07 验证：**160 PASS / 0 FAIL**、ctest 4/4）
- [ ] `git ls-files | wc -l` = 131，且不含 `build/` `dist/` `temp/` `__pycache__`
- [ ] `build_c.bat` 仍是 **CRLF + 纯 ASCII**（`file build_c.bat` 应显示 `CRLF line terminators`；
      `.gitattributes` 已强制 `*.bat text eol=crlf`，但首次 add 前别用编辑器改它）
- [ ] 版本号单点：`CMakeLists.txt` 的 `4.2.0` = README/手册/对照表里的版本（`pack.py` 会逐个断言）
- [ ] 敏感数据：真实 BOM 已移出（`git ls-files tests/data` 里只剩 18 份构造样本 + 2 份演示样本），截图与文档无客户型号
- [ ] 换机器构建：`MINGW` / `PY` / `BOMCHECK_GCC` / `BOMCHECK_MINGW` / `BOMCHECK_PY` / `BOMCHECK_OBJDUMP`
      六个环境变量可覆盖默认的本机路径（默认值仍是 `D:/AICS/mingw64/bin`、`D:\miniconda3\python.exe`）

## 6 仓库里各目录的职责边界

```
src/        C 实现（唯一的生产代码）
pyref/      Python 参考实现（只服务构建与对账，不进交付包）
tests/      单测与回归样本
tools/      构建、打包、对账脚本
docs/       给人看的文档与图示
CodeReview/ 第三方审查记录（只追加，不改写）
```

判断一个文件该不该进仓库，用一条：**「换个人 clone 下来，不跑构建也能看出它是干什么的吗？」**
能（源码、文档、样本、脚本）→ 传；不能（`build/` 里的中间文件、`dist/` 里的二进制）→ 不传。
