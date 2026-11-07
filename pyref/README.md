# pyref —— Python 参考实现（V3 原型唯一留下来的活口）

这个目录只有两个文件，都不是"遗留代码"，而是 V4 的**质量基础设施**：C 实现要跟它们逐项对账。

| 文件 | 角色 | 谁在用 |
|---|---|---|
| `bomcore.py` | **参考实现 / oracle**：读取、识别、配对、12 列结果、写出的第二份实现，口径与 `src/core` 逐项一致 | `tools/check_xlsx.py` `check_pn.py` `check_sheet.py` `check_pair.py` `check_write.py` `check_mark.py` `check_ui.py` `check_pack.py` |
| `layout_spec.py` | **界面布局的单一数据源**：49 个控件的 id / 坐标 / 字号 / 说明（逻辑像素，96 DPI 基准） | `tools/gen_layout_c.py`（→ `src/ui/layout_gen.h`）、`tools/make_layout_doc.py`（→ 控件位置对照图） |

## 为什么留两份实现

C 与 Python 对同一批样本各跑一遍，结果逐格比 —— 语义上有歧义的地方（表头怎么认、位号怎么归并、
双键关掉时无料号行怎么处理）只要两边不一致就立刻暴露。这套对账在 V4 的代码审查里抓到过
"同一错误公式写在两边、对账永远绿"的问题，所以现在还有一条规矩：**两侧同时拒绝的样本不算通过**，
必须走白名单（见 `tools/check_sheet.py` 顶部的 `NEGATIVE`）。

## 改这里要注意

- 改 `bomcore.py` 的语义 → 必须同步改 `src/core` 的对应模块，否则 `build_c.bat` 第 7–10 步会红。
- 改 `layout_spec.py` → 跑一次 `build_c.bat`（第 0 步会重新生成 `src/ui/layout_gen.h`），
  再重新导出 `docs/GUI控件位置对照表.md`，否则文档与程序对不上（`check_layout.py` 会拦）。
- 这两个文件都**不进交付包**，目标机不需要 Python。
