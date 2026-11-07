#ifndef BS_CFG_H
#define BS_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 运行配置。字段与文档 5.1 一致；C3 只用到其中识别相关的几项，其余留着给
   C4/C5 直接填，不中途改结构。
   注意 strike_scope 与 GUI 下拉同序：0 = 仅料号/位号/数量三格，1 = 整行任一格。 */
typedef struct bs_cfg {
    /* 输入：路径与页（UTF-8） */
    const char *old_path, *new_path;
    const char *old_sheets[2], *new_sheets[2];
    int old_n, new_n;                 /* 0 = 取第 1 页 */

    /* R3 料号 */
    int     seg1, seg2, seg3;         /* 默认 3,6,3（形态判定用） */
    int     strict_check;             /* 1 = 额外报严格档（仅报告） */
    double  min_hit_rate;             /* 默认 0.60 */
    int     sample_rows;              /* 默认 40（命中率取样行数） */
    int     case_sensitive;           /* 默认 0 */
    int     alias_exact;              /* 默认 1 */
    const char *pattern;              /* NULL = 内置 */

    /* R4 表头 */
    int header_scan_rows;             /* 默认 50；只用于文案/进度，不做硬截断 */
    int header_min_alias;             /* 默认 1 */
    int header_threshold;             /* 默认 5 */
    int block_mode;                   /* 0 = 单块，1 = 多块（默认） */
    int require_ref;                  /* 默认 0 */

    /* R2 删除线 */
    int strike_scope;                 /* 0 = 三格，1 = 整行（默认） */

    /* R2 双键 */
    int    dual_key;                  /* 默认 1 */
    int    ref_require_token;         /* 默认 1 */
    int    ref_fuzzy;                 /* 默认 1 */
    double ref_min_overlap;           /* 默认 0.60 */
    int    skip_label_rows;           /* 默认 1 */

    /* R5 重复 */
    int dup_policy;                   /* 0 = SEQ（默认）1 = STRICT 2 = CARTESIAN */

    /* R6 整行逐列：配对之后，除料号/位号/数量/删除线之外的列也比一遍，
       任一格不同即算「变更」，并把这些格涂黄（默认关，只在界面上勾选） */
    int    row_diff_all;

    /* 其他 */
    const char *out_dir;              /* NULL = 旧文件目录 */
    int overwrite;
    int dry_run;
    void (*progress)(void *ud, int phase, int done, int total);
    void *user;
} bs_cfg;

void bs_cfg_default(bs_cfg *c);

#ifdef __cplusplus
}
#endif

#endif /* BS_CFG_H */
