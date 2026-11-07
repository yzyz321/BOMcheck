# BOM 对比工具 V4 —— 设计与实施

**版本 V4.2.0** ｜ 2026-11-07 ｜ 本文档取代《BOM对比工具_V3_设计与实施.md》（V3 原型已退役、不随仓库分发）

V3 时代（Python 原型）的历史演进记录随 V3 文档归档。本文只描述 **C/C++ 重写之后**的
现状：当前目录结构即规范，Python 只剩两个角色——回归对账的 oracle 和布局单一数据源。

---

## 1 产品定义

把两份 xlsx 格式的电子料 BOM 放进程序，自动按料号 / 位号配对，输出差异结果表、
带颜色标注的旧表和新表（Excel 批注另附说明），帮硬件工程师快速核对两个版本的差异。

**做什么**

- 单页比对、逐表比对（同名工作表各出一组）、同一个文件自己比自己 = 料号查重
- 三份产物：`<旧>_vs_<新>_差异结果.xlsx`、`<旧>_差异标注_旧.xlsx`、`<新>_差异标注_新.xlsx`；查重出 1 份
- 12 列差异结果（料号、键类型、变更项、两侧位号/数量/删除线、重复序号、配对来源）
- 标注保留源文件全部格式（合并格、列宽、底色、样式），只加底色和批注

**不做什么（刻意不做）**

- 不支持 CSV / xls（先转 xlsx）；不联网、不写注册表、只在输出目录写文件
- 不做 Excel 没有的语义（公式格按缓存值读，不重算）
- 不做原地修改：永不覆盖用户源文件，产物全部另存

**形态**：Windows x64 单 exe（约 690 KB，`check_exe` 报的体积），静态链接，零第三方运行库，双击即用。

---

## 2 总体架构

两层，依赖方向单一向下：

```
┌─────────────────────────────────────────────────┐
│ src/ui   BOMCheck_V<版本>.exe（Win32 C++，自绘） │
│   窗口/绘制/DPI/拖放 · 探针 · 命令行解析          │
│            │ 只调 core 的公开接口                 │
├──────────▼───────────────────────────────────────┤
│ src/core  bscore.lib（纯 C，零依赖，可独立单测）  │
│   读取 → 识别 → 配对 → 差异 → 写出 → 标注        │
└──────────────────────────────────────────────────┘
```

- `bscore` 不知道窗口的存在；`bstest.exe`（`tests/test_core.c`）链它做 ctest
- `pyref/bomcore.py` 是同一口径的 **Python 参考实现（oracle）**，与 C 逐项对账（见 §8）
- 无 zlib / libzip / minizip / openpyxl / 第三方任何库：zip 读写、deflate、XML 都是自研

---

## 3 目录结构（现状即规范）

```
BOMcheck/
├── CMakeLists.txt          版本号唯一来源：project(BOMCheck VERSION x.y.z)
├── build_c.bat             一键构建 + 全量对账（15 步，§7）
├── src/
│   ├── core/               纯 C，一个模块一对 .c/.h
│   │   ├── bs_file.*       文件读入内存（bs_file_read）
│   │   ├── bs_inflate.*    deflate 解压（自研，含 stored/fixed/dynamic 三种块）
│   │   ├── bs_zip.*        zip 中央目录解析（读 xlsx 包）
│   │   ├── bs_xml.*        流式 XML 解析（够 xlsx 用：元素/属性/文本）
│   │   ├── bs_xlsx.*       xlsx 组装：sheet 名单、单元格网格（含合并格）
│   │   ├── bs_sheet.*      识别层：表头行、料号/位号/数量列、数据块（bs_sheet_from_grid）
│   │   ├── bs_pn.*         料号归一化 + 位号集合（Jaccard）、bs_norm_str 全角/空白归一
│   │   ├── bs_pair.*       四阶段配对 + 12 列结果（§4.3、§6）
│   │   ├── bs_write.*      差异表 / 查重表 写出（bs_zipw + bs_deflate）
│   │   ├── bs_mark.*       标注回写：保留原样式，涂色 + 批注（10 万格级）
│   │   ├── bs_arena.*      区块内存池（整批分配、整体释放）
│   │   ├── bs_deflate.*    deflate 压缩（自研，写 xlsx 包用）
│   │   ├── bs_zipw.*       zip 写出（store/deflate、时间戳、CRC32）
│   │   ├── bs_cfg.h        运行配置结构 + bs_cfg_default（引擎默认值）
│   │   └── bs_version.*    版本字符串
│   ├── ui/                 Win32 C++
│   │   ├── main.cpp        入口：命令行解析、探针分发（§5.4）
│   │   ├── app.*           状态机 + 消息处理 + 事件（配对/写出/查重/选项/帮助）
│   │   ├── layout_gen.h    49 个控件几何常量 —— 由 gen_layout_c.py 生成，勿手改
│   │   ├── layout.*        逻辑像素 → 物理像素、按窗口宽度解析
│   │   ├── paint.*         双缓冲绘制骨架
│   │   ├── card.*          ①② 文件卡片（表信息、位号/料号列摘要）
│   │   ├── table.*         结果表格自绘（冻结列、行色、页签切换）
│   │   ├── table_src.*     表格数据源（12 列结果 / 查重表 / 诊断）
│   │   ├── drop.*          OLE 拖放（.xlsx → 卡片）
│   │   ├── dpi.*           Per-Monitor V2（窗口移动/跨屏重算）
│   │   ├── bench.*         --bench 纯计算基准
│   │   ├── selfcheck.*     --self-check 资源/manifest/DPI 自证
│   │   ├── xlsx_dump.*     --xlsx-dump 读取链探针
│   │   ├── layout_doc.*    --export-layout-doc 生成控件位置对照表
│   │   └── utf8.*          UTF-8 ↔ UTF-16
│   └── res/                app.ico（make_icon.py 生成）、manifest、.rc
├── tests/
│   ├── test_core.c         ctest 用的核心库单测（version/dpi/self/core 4 项）
│   └── data/               回归样本（demo_old/demo_new 脱敏演示样本、构造边界样本）
├── pyref/
│   ├── bomcore.py          Python oracle（§8.1），与 C 逐项对账
│   └── layout_spec.py      布局单一数据源（§5.3）
├── tools/                  构建/验收脚本（§7、§8）
├── docs/                   用户使用手册（md + html）、设计与图示、控件位置对照表/对照图、图片
├── dist/                   pack.py 产物（目录版 + zip），只留当前版本；不进版本库
├── CodeReview/             V4.1.6 代码审查报告、问题清单 CSV、崩溃/挂死复现样本（§13）
├── temp/                   本地试验材料（不入包、不进库）
├── .gitignore              不进版本库的东西（build/ dist/ temp/ 缓存与产物）
├── .gitattributes          行尾与二进制归属（bat 强制 CRLF，xlsx/ico 不走转换）
├── GITHUB_UPLOAD.md        上传清单：传什么、不传什么、怎么推、发版怎么挂附件
└── README.md               GitHub 首页（给人看的项目说明）；README.txt 是随包给用户的说明
```

**已退役**：V3 的 Python GUI（`app.py`、`win32.py`）、PyInstaller 打包（`build_exe.py`）、
V3 版布局像素图生成器，以及 V3 的设计文档、老流程图、V3.2.0 发布包 —— 项目收尾时整体清理出仓库，
V4 不再依赖它们。`pyref/bomcore.py`（oracle）与 `pyref/layout_spec.py`（布局数据源）
是唯一保留的 Python，且只服务构建与对账（见 `pyref/README.md`）。

---

## 4 核心库设计

### 4.1 数据模型

`bs_grid`（bs_xlsx）是原始网格：`bs_cell{row, col, len}` 指向共享字符串池；
合并单元格以 span 记录，取值走 span 头。`bs_sheet`（bs_sheet）是识别后的视图：
表头行号、料号/位号/数量列、数据行区间（`bs_row{row, c1, c2}`）、命中率和诊断信息。
取格文本统一走 `bs_sheet_text_at(sheet, row, col)`（合并格取 span 头、多段取最后非空值），
页面显示与逐列比较同源，不会出现「看得到的和比的不一样」。

内存：除 12 列结果的字符串外全部走 `bs_arena`，一次 `bs_arena_free` 释放整棵；
10 万行 × 2 份的读取在常驻内存 200 MB 内。

### 4.2 读取链

`bs_file_read → bs_zip_open → bs_xml_parse → bs_xlsx`：

- 只解析 `[Content_Types].xml`、`workbook.xml`、`_rels`、`sharedStrings.xml`、`sheetN.xml`
- 数值/共享字符串/内联串/布尔四种格取缓存值；日期不转义（按原文本比）
- 自研 inflate 覆盖 stored / fixed / dynamic 全部块型；坏包在 fuzz 里挨个试过（§8.3）

### 4.3 识别层（bs_sheet）

1. **表头行**：前 50 行里找「列名命中别名表 + 当行存在料号格式单元格」的双证据行；
   别名表与 oracle 同值（`ALIAS_PN/REF/QTY`，含 partnumber、料号、物料编码、位号、designator…），
   匹配前先 `bs_norm_str`（全角→半角、大写、压空白）
2. **列角色**：别名精确命中定料号/位号/数量列；位号列认不出时按「位号形态命中率 ≥ 50%」
   猜（`guess_ref_column`），**与双键开关无关**——识别是识别，配对是配对（10.21 的教训）
3. **数据块**：block_mode=1 时允许多块（表中间夹说明行）；每块独立给数据行区间
4. 识别结果进诊断页（`--dump-table` 的 诊断 sheet）：表头行、各列名、块数、命中率，
   给用户看「程序是怎么理解的」

### 4.4 键函数（bs_pn）

- 料号归一：去空白、大写；形态判定用 `seg1/seg2/seg3`（默认 3,6,3）+ 分隔符族
- 位号集合：`R1,R2,C3` → token 集合；Jaccard = 交集 ÷ 并集，作阶段 C 的相似度
- `bs_norm_str` 是全项目的文本归一原语：表头识别、逐列比较共用

### 4.5 配对引擎（bs_pair，§6 详列口径）

四阶段 A/B/C/D 产出 `bs_pair{oi, ni, act, kt, strike_dir, chg}`；CHG 行保留
`chg==0` 的配对（标注阶段要用来定位行），生成 12 列时再过滤。
统计 `bs_pairstat`：料号/位号/近似/料号变更四类配对数、增删数、**跳过数**（双键关闭时
无料号行整行跳过，不进阶段 D）、变更行数。

### 4.6 写出与标注

- `bs_write`：差异表 / 查重表（openpyxl 风格但纯 C，deflate 自研，10 万行约 380 KB）
- `bs_mark`：读源包 → 按行/格涂底色 → 追加批注 → 重写包。**逐格保留原样式**：
  先复制源 cell 的 `s=` 索引再叠加，回归里「N 格样式保持」断言盯的就是这个；
  源表已有批注时整体跳过批注写入，只涂色（避免把别人的批注搞坏）

---

## 5 界面层设计

### 5.1 无框架 Win32 自绘

一个 `WS_OVERLAPPEDWINDOW` 窗口，`WM_PAINT` 画进内存位图再 `BitBlt`；
没有子窗口控件，所有「控件」是矩形 + 命中测试（`hit_action` / `hit_options`），
`layout_rect(id)` 统一取几何。好处：DPI、冻结列、页签这些自绘更可控；代价是
命中与绘制必须同一份几何——所以有 §5.3。

### 5.2 DPI

PerMonitorV2 声明（manifest），窗口移动/跨屏重算 `ui_scale`；逻辑坐标（96 基准）
统一经 `ui.sc()` 缩放。探针 `--probe-dpi` 自证 awareness / client 尺寸 / dragdrop。

### 5.3 布局：单一数据源

```
pyref/layout_spec.py（49 控件：id/位置/字号/note/tip，逻辑像素）
   ├─ tools/gen_layout_c.py     →  src/ui/layout_gen.h（几何常量，编译期固化）
   ├─ exe --export-layout-doc   →  docs/GUI控件位置对照表.md（交付包里那份，逐行坐标）
   └─ tools/make_layout_doc.py  →  docs/GUI控件位置对照图_v4.html（视觉图 + 截图 + TSV）
tools/check_layout.py  拿 layout_gen.h 的三组采样（1080×720 / 980×620 / 1400×900）
                       断言：绘制与命中一致 · 0 重叠 · 0 越界
```

改布局只改 spec 一处；「文档上的坐标」和「实际控件位置」永不走样。视觉图里的版本号
也从 `CMakeLists.txt` 读，不在脚本里写死。

### 5.4 探针体系（自测的地基）

| 探针 | 用途 |
|---|---|
| `--drop-test <旧> <新>` | 把两份样本灌进真实拖放链路，之后可接 `--dump-table` / `--ui-run` |
| `--dump-table` / `--table-dump` | 吐表格内容 + 行/格底色 + 冻结列宽 + 横向偏移 |
| `--table-scroll` | 滚到最右再读回，验证冻结列仍在 |
| `--ui-run <旧> <新> <目录>` | 「开始比对/查重」按钮那条完整链路（含日志），产物落目录 |
| `--write-all <目录>` / `--write-result <路径>` | 写出动作 |
| `--click opt.dual,opt.thr` | 走真实命中分支点控件（选项联动、帮助按钮） |
| `--help-probe` | 打印帮助落点与存在性 |
| `--dual-key/--min-overlap/--row-diff-all/--all-sheets/--pick-sheet/--out-dir` | 选项注入 |
| `--dump-layout` | 布局表（id\tx\ty\tw\th），check_layout / help_pos 用 |
| `--xlsx-dump` | 读取链中间产物 |
| `--bench <行数>` / `--serial` | 性能基准 |
| `--self-check` | 资源（图标组）/manifest/DPI 自证 |
| `--probe-dpi` / `--drag-over` / `--open-menu` / `--tab-log` / `--stay` / `--visible` / `--client-size` | 截图与专项 |

探针让 GUI 的关键行为全部可以在脚本里逐项断言——`check_ui` 的 23 项就是这么搭起来的。

---

## 6 比对口径（业务规则，与 oracle 逐项一致）

### 6.1 四阶段配对

| 阶段 | 规则 |
|---|---|
| A 料号键 | 归一化后相等的行按组配对，组内按行序一对一（min(旧数, 新数) 对） |
| B 位号键 | 剩余行里位号集合完全相等者配对（受 `dual_key` 控制） |
| C 位号近似 | 剩余行里 Jaccard ≥ `ref_min_overlap` 且**至少一侧无料号**；按（相似度降序, 旧行号, 新行号）贪心严格一对一；配上的若两侧料号都存在且不同 → 键类型「位号(料号变更)」 |
| D 收尾 | 仍未配对 → 新增 / 删除 |
| 跳过 | **双键关闭时无料号的行（kind='R'）整行跳过**并计入 `n_skipped`，不进阶段 D——否则同一份文件比自己也会成对冒出增删（10.21 的教训） |

### 6.2 默认值与选项

| 项 | 引擎默认（`bs_cfg_default`） | 界面默认 | 说明 |
|---|---|---|---|
| 双键（位号兜底） | 1 | **关** | 关 = 无料号行跳过；界面构造函数另设，批量/回归走引擎值 |
| 近似阈值 `ref_min_overlap` | 0.60 | **0.90** | 只对阶段 C 生效；界面点按循环 0.40…0.95 七档，滚轮 ±0.05，夹 0.30–0.95；双键关时置灰 |
| 整行逐列比较 `row_diff_all` | 0 | 关 | 配对成立后除料号/位号/数量/删除线外逐格比（`bs_norm_str` 归一），任一格不同置 `BS_CHG_CELL`，变更项写「其他列」，**只涂真正不同的格** |
| 重复料号 `dup_policy` | SEQ | 固定 | 顺序配对；STRICT / CARTESIAN 保留字段 |
| 删除线范围 `strike_scope` | 1（整行） | 固定 | 实测删除线常打在数量/位号/描述列 |
| 双键开时阈值语义 | — | — | 位号完全一致 = Jaccard 1.0；0.95 下 19 个位号以上的长行才可能漏放一个 |

### 6.3 变更项与 12 列

`BS_CHG_REF/QTY/PN/STRIKE/CELL` 位掩码 → 「变更项」逗号串；12 列 =
`Part Number | 键类型 | 差异类型 | 变更项 | 旧_多出位号 | 新_多出位号 |
旧_Quantity | 新_Quantity | 旧_删除线 | 新_删除线 | 重复序号 | 配对来源`。

### 6.4 查重（同一个文件）

新旧同路径 → 不比差异，按料号分组列重复：8 列
`Part Number | 重复次数 | 查重结果 | 位号 | 数量 | 删除线 | 行号 | 工作表`，
产物 `<文件名>_查重标注.xlsx`（9 条批注的样本在 check_write 里盯）。

### 6.5 帮助按钮

打开 exe 同目录 `用户使用手册.html`；链路：可读性预检（CreateFileW，把「权限问题」和
「关联问题」分开报）→ `ShellExecuteExW` → `rundll32 url.dll,FileProtocolHandler` →
注册表 ProgId 自起进程。缺失时弹四段速查（双键 / 阈值 / 逐列比较 / 同文件查重）。

---

## 7 构建与交付

### 7.1 工具链

MinGW-w64（默认 `D:/AICS/mingw64`，可用环境变量 `MINGW` 覆盖；gcc 路径一律正斜杠写进 CMake）
+ Ninja + CMake。工具脚本里的 gcc / objdump / python 默认值也都能用
`BOMCHECK_GCC` / `BOMCHECK_MINGW` / `BOMCHECK_OBJDUMP` / `BOMCHECK_PY` / `PY` 覆盖，换台机器不必改源码。
版本号唯一来源 `CMakeLists.txt` 的 `project(BOMCheck VERSION x.y.z)`，派生
`BS_VERSION` 编译定义、`bs_version_gen.h`（.rc）与产物名 `BOMCheck_V<版本>.exe`；
任何脚本不得写死版本号（统一 `tools/findexe.py::find_exe()`）。改版本必须重跑
configure（Ninja 的 target 名在配置期确定）。

### 7.2 build_c.bat —— 15 步

```
[0]  gen_layout_c.py 生成布局头
[1]  cmake configure        [8]  配对/12列 vs oracle（check_pair 34 组）
[2]  ninja build            [9]  写出 vs oracle（check_write 26 组）
[3]  exe 自检（check_exe）  [10] 标注 vs oracle（check_mark 8 组+4 专项）
[4]  布局表（check_layout） [11] 模糊/极端输入 18 例不崩（check_fuzz）
[5]  读取链 vs oracle       [12] 界面接线 23 项（check_ui）
[6]  pn/键函数 vs oracle    [13] pack.py 打包 + 干净目录验收（check_pack）
[7]  表头识别 vs oracle     [14] ctest 4 项
```

全绿基线以 `build_c.bat` 最后一次输出为准（各脚本自报 PASS 数，别在文档里写死总数——
样本增减、负样本白名单都会让它漂），另加 `ctest 4/4` 与干净目录 0 失败，全程约 40 秒。

硬约束（踩过的坑，写死在脚本注释里）：bat 只写 ASCII 且 CRLF；gcc 路径正斜杠；
工具脚本不做大批量 rmtree（安全策略抛 `SystemExit(1)`，`ignore_errors` 兜不住）；
`PY` 默认取 `D:\miniconda3\python.exe`（PATH 首个 python 常缺 openpyxl），可用环境变量覆盖。

### 7.3 交付包

`tools/pack.py` → `dist/BOMCheck_V<版本>/`：exe + README.txt + GUI控件位置对照表.md +
用户使用手册.md/.html + 同名 zip（5 条目全 deflate）。`check_pack.py` 用「空目录 +
净化 PATH/TEMP」模拟干净机器：版本自证、三份产物、读回核对、源文件 sha256 不变。
手册源是 `docs/用户使用手册.md`，`tools/md2html.py --embed` 出单文件 html（图片内联）。
图标：`tools/make_icon.py` → `src/res/app.ico`（16…256 十档；小尺寸重绘放大镜，
check_exe 断言恰好 1 组图标资源）。

---

## 8 质量体系

### 8.1 Oracle 对账（核心思想）

C 实现与 `pyref/bomcore.py` 是同一份口径的两份实现：**语义在 Python 里先写清楚，
C 照着做，对账脚本拿同一批样本逐格比两者输出**。改口径必须两侧同改，漏一侧当轮
对账就红。12 列的空串位置、数量文本格式（整数不带小数点、-0.0 归 0）、删除线方向
这类容易含糊的细节，全靠对账钉死。

### 8.2 对账清单（tools/check_*.py）

| 脚本 | 盯什么 |
|---|---|
| check_sheet (33 张) | 读取 + 识别（表头行/列角色/块/命中率） |
| check_pn (384 用例) | 料号归一、形态判定、位号集合与 Jaccard |
| check_pair (34 组) | 四阶段配对 + 12 列（含空串位置、跳过语义） |
| check_write (26 组) | 差异表 / 查重表 写出与读回 |
| check_mark (8 组 + 4 专项) | 标注：涂色格数、样式保持、批注条数、目标被占/已有 rels/源表自带批注 |
| check_xlsx | 读取链细粒度 |
| check_ui (23 项) | 表格映射、冻结列、页签切换、写出产物、选项联动、逐列比较、帮助（落点/位置/弹框内容）、查重表 |
| check_layout | 49 控件 × 三尺寸：0 不一致 / 0 重叠 / 0 越界 |
| check_exe | 自检输出：图标 1 组 16-256 齐全、DPI 声明、无意外 DLL 依赖 |
| check_fuzz (18 例) | 空 zip / 假 PK 头 / 截断包 / 20 万字符格 / 单行 1200 位号…不崩、能拒绝 |
| ctest | version / dpi / self / core |

### 8.3 原则

- 每修一个 bug，先补一条会红的用例再修（10.20–10.22 每节都有对应回归）
- 界面类需求断言**数据层可见的量**（布局表、日志行、弹框文本），不靠肉眼
- 构建链必须一键可重复：任何人 clone 后跑 `build_c.bat` 得到同一份绿

---

## 9 关键设计决策

| 决策 | 理由 | 代价与对策 |
|---|---|---|
| 自研 zip/inflate/deflate/XML，零第三方 | 单 exe 静态链接、不装运行库、离线；xlsx 用的子集很小 | 要自己扛坏包——fuzz 18 例 + 拒绝路径全覆盖 |
| 保留 Python oracle 而不是删掉 | C 与 Python 双实现互证，语义歧义无处藏身 | 两份代码要同步改；用对账脚本强制 |
| 布局用 Python spec 生成 C 表 | 几何、文档、校验三者同源，改一处 | 多一步生成；gen_layout_c 纳入构建第 0 步 |
| 界面默认值与引擎默认值分开（`App` 构造 vs `bs_cfg_default`） | 产品口径（双键默认关）不该污染批量/回归口径 | 容易忘同步——check_ui 的 options 用例盯界面默认 |
| 无料号行在双键关闭时「跳过」而非「增删」 | 同文件自比不能冒差异；口径与 V2 用户习惯一致 | 状态栏/日志必须写明跳过数，否则像丢行 |
| 探针驱动 GUI 测试 | 自绘界面没有控件句柄可查，文本层断言最稳 | 探针本身也是代码；纳入 check_ui 反向验证 |
| 只保留 oracle 这一份 Python | 双实现互证是质量体系的地基，其余原型代码没有维护价值 | 退役代码移出仓库；`pyref/` 只服务构建与对账 |

---

## 10 性能

同一台机器（10 万行 × 2 份）：

| 场景 | 耗时 |
|---|---|
| 读取 + 识别 + 配对 + 写差异表 | ≈ 0.65 s |
| 再加新旧两份标注（共 3 个文件） | ≈ 2.4 s |
| 5 千行常规 BOM | < 0.1 s |

差异表 10 万行约 380 KB（自研 deflate；不压缩要 3.5 MB）。`--bench` 可复测。

---

## 11 已知限制与待办

- 整行逐列比较只做文字相等判断（不做数值/单位归一）
- CARTESIAN 重复策略未实现全组合（`cartesian_fallback` 置位，仅回归对照）
- 日期按原文本比，不做日期语义
- **比对与写盘跑在 UI 线程**：没有取消、没有进度（「停止」按钮只是占位），大文件期间窗口不响应（§13.3）
- 读包侧没做 zip-bomb 上限、不校验 CRC32（本机生成的文件无碍，收外部文件时是风险面）
- 待办：命令行批量模式、位号自然排序（R2 < R10）、位号重复查重、深色模式、
  Excel 365 / WPS 三端批注显示验证；审查遗留的 P2-01～P2-05（死配置字段）与 P2-06～P2-13（并发/内存）见 §13.3

---

## 12 历史衔接

V3 是 Python 原型（V3.x），C 重写后它的代码、设计文档、老流程图与最后一个发布包都已
退役出仓库 —— 项目收尾时统一清理，V4 不依赖它们。需要回看历史时，V3 的设计文档记录到
V4.1.6 为止的变更（含 C 版 10.16–10.22 的逐次改动），该文件已不在本仓库内。

仍在仓库里的只有两份 Python（`pyref/`）：`bomcore.py` 是逐项对账用的参考实现，
`layout_spec.py` 是界面布局的单一数据源。它们不是遗留，是质量体系的一部分
（为什么留、改的时候要注意什么，见 `pyref/README.md`）。

配套图示：`docs/BOM流程图_V4.html` —— 八张 SVG 流程图（分层模块、比对主流程、
单页数据流、表头识别链、选项与四阶段配对、帮助打开链、查重模式、布局与质量链），
节点内标注涉及文件名；页面内嵌 JSON 数据，可离线编辑、导出 SVG。

---

## 13 V4.2.0 —— 代码审查与修复

对 V4.1.6 做了一次全量代码审查（`src/core` 15 个 `.c` + 12 个 `.h`、`src/ui` 14 个
`.cpp` + 15 个 `.h` 全文，加 `gcc -Wall -Wextra -Wshadow -Wformat=2 -fanalyzer`），
报告与复现样本在 `CodeReview/`（主报告、39 条 P0/P1/P2 的 CSV 清单、3 份构造 xlsx）。
审查阶段全程只读；随后分三批修复，V4.2.0 是这三批修复后的第一个交付版本。

### 13.1 修的两类「真问题」

**① 崩溃与死循环（P0/P1，任何人发来的文件都能触发）**

| 症状 | 根因 | 修法 |
|---|---|---|
| 打开 1.5 KB 构造 xlsx 即 `0xC0000005` | `<c r="ZZZZZZZ1">` 的列号按 26 进制回绕成负数，当作 `present_cols` 下标越界写 | `BS_MAX_ROW`/`BS_MAX_COL` 限幅：`col_from_ref` 只认 3 字母/16384 列，`row_from_ref` 限 1048576；越界行整行跳过 |
| 打开另一份构造 xlsx 永久卡死 | `<row r="2147483647">` 让扫描上界等于 `INT_MAX`，`r++` 有符号溢出，`-O2` 把循环条件优化成恒真 | 行号超 1048576 忽略；扫描上界改取「最后一个有内容的行」`last_content_row()` |
| 双键近似阶段可能跑不完 | 阶段 C 是完整笛卡尔积，候选表无上限 | `FUZZY_MAX_PAIRS=5e7` / `FUZZY_MAX_CAND=20 万` 护栏，超限降级给阶段 D；Python oracle 同值 |
| 写结果时 OOM 双释放 | `bs_zipw_add` 失败已 `free(data)`，调用方 `oom` 分支再 free 一次 | 交出所有权前先把 `s1/s2` 置空 |

**② 口径与可用性（P1）**

- **合并单元格从来没被用上**（与文档相反）：新增 `expand_merges()`，把合并区按 span 头铺满，
  纵向合并的料号格现在 3 行全部建档；Python `_Grid` 同口径，`check_sheet` 24 张表仍 0 失败。
- **诊断页「数据行」与「建档行」是同一个错误公式**（C 与 Python 两侧同源，对账永远绿）：
  新增 `bs_sheet_data_rows()` = 建档 + 无键 + 分组标题，诊断第 4/8 列分开，`check_write`
  增加独立恒等式断言。同一页的「数据行」与「建档行」不再恒等：有分组标题、无料号行时
  数据行会比建档行多出几行（演示样本主板页 29 / 28），两者相等只代表这页很干净。
- **查重模式「另存结果」必失败**：拆出 `write_dups(dir)`，查重只写一份查重标注。
- **>65535 行的滚动条拖不过去**：`SB_THUMBTRACK` 改用 `GetScrollInfo(SIF_TRACKPOS)` 的 32 位位置。
- **`&` 被当加速键吞掉**：`paint.cpp` 唯一的 `DrawTextW` 出口统一加 `DT_NOPREFIX`。
- **OLE 生命周期**：`WM_DESTROY` 里先 `drop_revoke()` 再 `PostQuitMessage`。
- 另有 10 条 P2（日志 32767 字符上限、DPI 变更后重发 `WM_SETFONT`、HFONT 泄漏、
  `bs_result_build` 失败显示成「差异 0 行」、逐表失败残留 `.partK` 等）。

### 13.2 顺带收口的三个体系漏洞

1. **「两边都拒 = PASS」**：`check_sheet`/`check_pair`/`check_write` 里「C 和 Python 都拒绝」
   曾被一律当通过，于是同源缺陷永远绿。现在各自维护 `NEGATIVE` 白名单，白名单外
   两侧同时拒绝一律 FAIL。
2. **`check_pack` 的 SKIP**：缺 openpyxl 时「读回」原本打印 SKIP 跳过，等于验收不完整也算通过；
   改为计失败。
3. **版本单一来源扩到全部交付文档**：`pack.py` 对 `README.txt`、`用户使用手册.md/.html`、
   `GUI控件位置对照表.md` 逐个断言版本号，任一份没跟上就中止打包。

### 13.3 状态

| 项 | 结果 |
|---|---|
| P0 | 1 条，已修复（repro 样本 exit 0 / 0.12 s） |
| P1 | 10 条：9 条已修复（含 3 条未在真实样本上实测的路径），1 条部分（`act.stop` 只做到「不再假装可取消」） |
| P2 | 28 条：10 条已修，18 条留待办 |
| P3 | 36 条：1 条已修（`row_from_ref` 限幅），其余为形式缺陷，未动 |
| 回归 | `check_sheet` 33 / `check_xlsx` 7 / `check_pair` 34 / `check_write` 26+2 / `check_mark` 12 / `check_ui` 23 / `check_fuzz` 18 例 / `ctest` 4，全 0 失败 |

遗留的主要是三类：**界面线程（P1-04）**——比对与写盘仍同步跑在 UI 线程，取消与进度未实现；
**内存与并发（P2-06～P2-10）**——`g_o/g_n` 全局行数组不可重入、并行载入的 `double` 计数非原子、
解压无上限且不校验 CRC32；**死配置（P2-01～P2-05）**——`header_scan_rows` 等 7 个字段从未被读，
文案却按它们写。
