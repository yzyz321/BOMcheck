# docs 索引

八份文档（含本索引）加一张截图，分三类：给**使用者**的、给**维护者**的、给**看坐标**的。改任何一份之前先看这张表。

## 一、给使用者的

| 文件 | 内容 | 怎么维护 |
|---|---|---|
| `用户使用手册.md` | 完整手册：安装与启动、操作步骤、12 列含义、四种颜色、输出文件命名、常见问题、命令行参数 | **改这份 md，再跑** `python tools/md2html.py "docs/用户使用手册.md" "docs/用户使用手册.html" --embed` 生成 html |
| `用户使用手册.html` | 上面那份的网页版，单文件、图片已内联，双击就能看。exe 的「使用手册」按钮打开的就是它 | 不手改；由 md 转换而来 |

## 二、给维护者的

| 文件 | 内容 | 怎么维护 |
|---|---|---|
| `BOM对比工具_V4_设计与实施.md` | 现状文档：产品定义、总体架构、目录结构、核心库与界面层设计、比对口径、构建与交付、质量体系、关键决策、性能、限制、**§13 代码审查与修复** | 手写。改了架构、口径、构建步骤就同步更新；版本号跟 `CMakeLists.txt` 走 |
| `BOM流程图_V4.html` | 八张 SVG 流程图：分层结构、比对主流程、单页数据流、表头识别链、选项与四阶段配对、帮助打开链、查重、布局与质量链。节点内标了涉及的文件名 | 手写 HTML，数据是页面里的 `const DIAGRAMS`；页面内可编辑 JSON、导出 SVG。改模块划分时同步改 |
| `../CodeReview/` | V4.1.6 的第三方审查报告、39 条问题清单 CSV、崩溃/挂死复现样本 | 只追加不改写：它是审计记录。修完在 §9 状态表里标状态 |

## 三、给"控件在哪"的

| 文件 | 内容 | 怎么维护 |
|---|---|---|
| `GUI控件位置对照表.md` | 49 个控件的 id / 类型 / 坐标 / 文案 / 说明（表格版，会打进交付包） | **不要手改**：跑 `BOMCheck --export-layout-doc docs/GUI控件位置对照表.md` 从程序自身布局表导出 |
| `GUI控件位置对照图_v4.html` | 同一份布局的视觉版：整体示意图（26 个编号）、实机截图、控件清单、悬停提示表、TSV 坐标 | **不要手改**：跑 `python tools/make_layout_doc.py` 重新生成 |
| `images/ui_main.png` | 主界面实机截图（1620×1080 逻辑像素、在 150% 缩放下抓取，实际 2430×1620），README 与对照图都用它。截图里的两份 BOM 是 `tests/data/demo_old.xlsx` / `demo_new.xlsx`（`tools/gen_demo.py` 生成的虚构数据），换样本后重截一张即可 | `python tools/shot.py <输出路径>`：先 `BOMCheck --drop-test tests/data/demo_old.xlsx tests/data/demo_new.xlsx --stay --visible`，再对着窗口抓客户区 |

## 坐标只有一个源头

`pyref/layout_spec.py` 定义 49 个控件的几何。三路消费，永不手抄：

```
pyref/layout_spec.py
  ├─ tools/gen_layout_c.py        → src/ui/layout_gen.h（程序摆控件与命中的唯一依据）
  ├─ BOMCheck --export-layout-doc → docs/GUI控件位置对照表.md（交付包里的表）
  └─ tools/make_layout_doc.py     → docs/GUI控件位置对照图_v4.html（视觉图）
```

改布局只改 `layout_spec.py`，然后重跑 `build_c.bat`（第 0 步重新生成头文件）并重新导出上面两份文档；
`tools/check_layout.py` 会在三种窗口尺寸下断言「绘制位置与命中区域一致、0 重叠、0 越界」。
