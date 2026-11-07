/* core 层单测。纯 C99，不链接 Win32 —— 出错时直接 printf + 退出码 1。

   六种用法：
     bstest                 跑内建用例（ctest 用这个）
     bstest --dump <file>   对账模式：文件每行 "<pn>\t<ref>"（值按 \\ \n \r \t 转义），
                            逐行输出 "<norm>\t<pn>\t<w>\t<s>\t<sig>\t<ok>\t<bad>\t<key>"
                            给 tools/check_pn.py 和 Python oracle 比。
     bstest --sheet ...     识别层对账，见 sheet_dump
     bstest --pair ...      配对与 12 列对账，见 pair_dump
     bstest --write ...     写出 xlsx 给 tools/check_write.py 读回比对
     bstest --mark ...      标注回写给 tools/check_mark.py 读回比对 */
#include "bs_deflate.h"
#include "bs_inflate.h"
#include "bs_mark.h"
#include "bs_pair.h"
#include "bs_pn.h"
#include "bs_sheet.h"
#include "bs_write.h"
#include "bs_xlsx.h"
#include "bs_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

static void fail(const char *group, int idx, const char *what,
                 const char *got, const char *want)
{
    g_fail++;
    printf("FAIL  %s[%d] %s\n        got : [%s]\n        want: [%s]\n",
           group, idx, what, got ? got : "(null)", want ? want : "(null)");
}

static void ok(void)
{
    g_pass++;
}

/* ---------------- norm ---------------- */
typedef struct {
    const char *in, *want;
} str_case;

static const str_case NORM[] = {
    { NULL,                 "" },
    { "",                   "" },
    { "Part Number",        "partnumber" },
    { " Part  Number ",     "partnumber" },
    { "\xEF\xBC\xB0\xEF\xBC\xA1\xEF\xBC\xB2\xEF\xBC\xB4"          /* ＰＡＲＴ */
      "\xE3\x80\x80"                                              /* 全角空格 */
      "\xEF\xBC\xAE\xEF\xBC\xB5\xEF\xBC\xAD\xEF\xBC\xA2\xEF\xBC\xA5\xEF\xBC\xB2",
      "partnumber" },                                             /* ＮＵＭＢＥＲ */
    { "\xE6\x96\x99\xE5\x8F\xB7",        "\xE6\x96\x99\xE5\x8F\xB7" },  /* 料号 → 料号 */
    { "\xE6\x95\xB0\xE9\x87\x8F\n",      "\xE6\x95\xB0\xE9\x87\x8F" },  /* 数量\n */
    { "REF\tDesignator",   "refdesignator" },
    { "A\xC2\xA0" "B",     "ab" },      /* NBSP */
    { "Qty\xE3\x80\x80",   "qty" },     /* 全角空格结尾 */
    { "\xE2\x80\x8B",      "\xE2\x80\x8B" },  /* 零宽空格不是空白，保留 */
    { "Caf\xC3\xA9",       "caf\xC3\xA9" },   /* 2 字节 UTF-8（é） */
    { "\xF0\x9F\x98\x80x", "\xF0\x9F\x98\x80x" },  /* 4 字节 UTF-8（emoji） */
    /* 下面三组是非法 UTF-8：C 侧原样保留字节，Python 侧会抛异常 —— 这是
       bs_pn.h 里写明的第 3 条口径差异，只在本文件里验，不进对账脚本。 */
    { "\xF8\x80",          "\xF8\x80" },      /* 非法首字节 */
    { "\xE4\xB8",          "\xE4\xB8" },      /* 截断的 3 字节序列 */
    { "\xE4\xFFzz",        "\xE4\xFFzz" },    /* 续字节不合法 */
    { "a\xE2\x80\x83" "b", "ab" },            /* U+2003 EM SPACE */
    { "a\xE1\x9A\x80" "b", "ab" },            /* U+1680 OGHAM SPACE */
};

/* ---------------- 料号 ---------------- */
typedef struct {
    const char *in, *want;
    int w, s;
} pn_case;

static const pn_case PN[] = {
    { NULL,                     "",                  0, 0 },
    { "",                       "",                  0, 0 },
    { "   ",                    "",                  0, 0 },
    { "111-000005-C50",         "111-000005-C50",    1, 1 },
    { "111-000005-c50",         "111-000005-C50",    1, 1 },
    { " 111-000005-C50 ",       "111-000005-C50",    1, 1 },
    { "111 - 000005 - C50",     "111-000005-C50",    1, 1 },
    { "111\xE2\x80\x93" "000005\xE2\x80\x93" "C50",    /* en dash */
      "111-000005-C50",         1, 1 },
    { "111\xEF\xBC\x8D" "000005\xEF\xBC\x8D" "C50",    /* 全角连字符 */
      "111-000005-C50",         1, 1 },
    { "111-000005-C5",          "111-000005-C5",     0, 0 },
    { "111-00005-C50",          "111-00005-C50",     0, 0 },
    { "ABC-123456-DEF",         "ABC-123456-DEF",    1, 0 },
    { "111-000005-C50.0",       "111-000005-C50.0",  0, 0 },  /* 前缀不是纯数字 */
    { "123456.0",               "123456",            0, 0 },
    { "0.0",                    "0",                 0, 0 },
    { ".0",                     ".0",                0, 0 },  /* 前缀为空，Python 也不剥 */
    { "\xEF\xBC\x91\xEF\xBC\x91\xEF\xBC\x91-000005-C50",      /* 全角 111 */
      "\xEF\xBC\x91\xEF\xBC\x91\xEF\xBC\x91-000005-C50", 0, 1 },
    { "111-000005-\xEF\xBC\xA3" "50",                          /* 全角 Ｃ */
      "111-000005-\xEF\xBC\xA3" "50",                    0, 0 },
    { "\xEF\xBD\x81\xEF\xBD\x82\xEF\xBD\x83-123456-def",        /* 全角 ａｂｃ */
      "\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3-123456-DEF", 0, 0 },
    { "A\xFF" "B",              "A\xFF" "B",          0, 0 },  /* 非法字节原样保留 */
    { "111-000005X" "C50",      "111-000005X" "C50",  0, 0 },  /* 14 字符但下标 10 不是 '-' */
    { "PN-001",                 "PN-001",            0, 0 },
};

/* ---------------- 位号 ---------------- */
typedef struct {
    const char *in, *want;
    int ok, bad;
} ref_case;

static const ref_case REF[] = {
    { NULL,                 "",              0, 0 },
    { "",                   "",              0, 0 },
    { "   ",                "",              0, 0 },
    { "R1,R2,R10",          "R1|R10|R2",     3, 0 },
    { "r2 r1",              "R1|R2",         2, 0 },
    { "R1,R1,R2",           "R1|R2",         3, 0 },
    { "R1\xE3\x80\x81R2\xEF\xBC\x9BR3",  "R1|R2|R3",  3, 0 },   /* 、； */
    { "R1/R2\\R3|R4;R5",    "R1|R2|R3|R4|R5", 5, 0 },
    { "R1 X R2",            "R1|R2",         2, 1 },
    { "R123456",            "R123456",       1, 0 },   /* 5 位数字 + 1 位后缀 */
    { "ABCD1",              "",              0, 1 },
    { "R",                  "",              0, 1 },
    { "C1,C2\nC3",          "C1|C2|C3",      3, 0 },   /* 单元格里换行 */
    { "U1,U2,U10,U3",       "U1|U10|U2|U3",  4, 0 },
    { "L1, L2 ,, L3",       "L1|L2|L3",      3, 0 },   /* 连续分隔符不产生空 token */
    { "R1\xFF" "R2",        "",              0, 1 },   /* 非法字节进 token，形态校验不过 */
    { "R1,\xE4\xB8",        "R1",            1, 1 },   /* 截断序列也算一个坏 token */
};

/* ---------------- 键 ---------------- */
typedef struct {
    const char *pn, *ref, *want;
    char kind;
} key_case;

static const key_case KEY[] = {
    { "111-000005-C50", "R1",      "P:111-000005-C50", 'P' },
    { "",               "R2,R1",   "R:R1|R2",         'R' },
    { "   ",            "R1",      "R:R1",            'R' },
    { "not a pn",       "",        "P:NOTAPN",        'P' },  /* 料号非空就用，不管形态 */
    { "",               "X",       "",                0 },
    { "",               "",        "",                0 },
    { NULL,             NULL,      "",                0 },
};

static void run_norm(void)
{
    char out[BS_PN_MAX];
    size_t i;

    for (i = 0; i < sizeof(NORM) / sizeof(NORM[0]); i++) {
        int n = bs_norm_str(NORM[i].in, out, sizeof(out));
        if (n < 0 || strcmp(out, NORM[i].want)) {
            fail("norm", (int)i, NORM[i].in ? NORM[i].in : "NULL", out, NORM[i].want);
        } else {
            ok();
        }
    }
    /* 缓冲不够时必须截断并返回 -1，且仍有 '\0' */
    {
        char small[4];
        int n = bs_norm_str("Part Number", small, sizeof(small));
        if (n != -1 || strcmp(small, "par") || small[3] != 0)
            fail("norm", -1, "截断", small, "par");
        else
            ok();
        if (bs_norm_str("x", NULL, 0) != -1)
            fail("norm", -1, "cap=0", "", "-1");
        else
            ok();
    }
}

static void run_pn(void)
{
    char out[BS_PN_MAX];
    size_t i;

    for (i = 0; i < sizeof(PN) / sizeof(PN[0]); i++) {
        int w = -1, s = -1, n;
        n = bs_pn_str(PN[i].in, out, sizeof(out), &w, &s);
        if (n < 0 || strcmp(out, PN[i].want) || w != PN[i].w || s != PN[i].s) {
            char buf[512];
            sprintf(buf, "[%s] w=%d s=%d", out, w, s);
            fail("pn", (int)i, PN[i].in ? PN[i].in : "NULL", buf,
                 /* 期望值也拼一下 */
                 PN[i].want);
            printf("        期望 w=%d s=%d\n", PN[i].w, PN[i].s);
        } else {
            ok();
        }
    }
    /* 形态指针同样可以不给 */
    {
        char out[64];
        if (bs_pn_str("111-000005-C50", out, sizeof(out), NULL, NULL) >= 0 &&
            strcmp(out, "111-000005-C50") == 0)
            ok();
        else
            fail("pn", -1, "NULL 形态指针", out, "111-000005-C50");
    }
}

static void run_ref(void)
{
    char out[1024];
    size_t i;

    for (i = 0; i < sizeof(REF) / sizeof(REF[0]); i++) {
        int a = -1, b = -1, n;
        n = bs_ref_sig_str(REF[i].in, out, sizeof(out), &a, &b);
        if (n < 0 || strcmp(out, REF[i].want) || a != REF[i].ok || b != REF[i].bad) {
            char buf[600];
            sprintf(buf, "[%s] ok=%d bad=%d", out, a, b);
            fail("ref", (int)i, REF[i].in ? REF[i].in : "NULL", buf, REF[i].want);
            printf("        期望 ok=%d bad=%d\n", REF[i].ok, REF[i].bad);
        } else {
            ok();
        }
    }
    /* 位号多到装不下：截断时必须返回 -1，不能越界 */
    {
        char big[64], many[4096];
        size_t k;
        many[0] = 0;
        for (k = 0; k < 500; k++) {
            char t[16];
            sprintf(t, "%sR%u", k ? "," : "", (unsigned)k + 1);
            strcat(many, t);
        }
        if (bs_ref_sig_str(many, big, sizeof(big), NULL, NULL) == -1)
            ok();
        else
            fail("ref", -1, "超长必须截断", big, "(返回 -1)");
    }
    /* 计数指针可以不要（C3 里只关心签名时会这么用） */
    {
        char out[64];
        if (bs_ref_sig_str("R1,R2", out, sizeof(out), NULL, NULL) >= 0 &&
            strcmp(out, "R1|R2") == 0)
            ok();
        else
            fail("ref", -2, "NULL 计数指针", out, "R1|R2");
    }
}

static void run_key(void)
{
    char out[BS_PN_MAX];
    size_t i;

    for (i = 0; i < sizeof(KEY) / sizeof(KEY[0]); i++) {
        char kind = bs_make_key(KEY[i].pn, KEY[i].ref, out, sizeof(out));
        if (kind != KEY[i].kind || strcmp(out, KEY[i].want)) {
            char buf[300];
            sprintf(buf, "[%s] kind=%c", out, kind ? kind : '0');
            fail("key", (int)i, KEY[i].pn ? KEY[i].pn : "NULL", buf, KEY[i].want);
        } else {
            ok();
        }
    }
    /* 只要 kind、不要字符串 */
    if (bs_make_key("111-000005-C50", "R1", NULL, 0) == 'P')
        ok();
    else
        fail("key", -1, "out=NULL", "-", "P");
    /* 给了缓冲但 cap=0：同样不写 */
    {
        char tiny[4] = "abc";
        if (bs_make_key("111-000005-C50", "R1", tiny, 0) == 'P' &&
            strcmp(tiny, "abc") == 0)
            ok();
        else
            fail("key", -2, "cap=0 不该写", tiny, "abc");
    }
}

/* 复用同一份 bs_keys 反复调用：第二次必须完全覆盖第一次的结果，
   不能残留上一行的料号 / 位号（这两组缓冲是复用的）。 */
/* 标注模块里不依赖文件的几条：颜色表与入参校验 */
static void run_mark(void)
{
    bs_err e;

    if (bs_mark_color_count() != 4 ||
        strcmp(bs_mark_color_hex(BS_MARK_ADD), "C6EFCE") ||
        strcmp(bs_mark_color_hex(BS_MARK_DEL), "FFC7CE") ||
        strcmp(bs_mark_color_hex(BS_MARK_CHG), "FFEB9C") ||
        strcmp(bs_mark_color_hex(BS_MARK_DUP), "D9D2E9") ||
        bs_mark_color_hex(4)[0] || bs_mark_color_hex(-1)[0]) {
        fail("mark", 1, "四色常量", bs_mark_color_hex(0), "C6EFCE/FFC7CE/FFEB9C/D9D2E9");
    } else {
        ok();
    }

    e.msg[0] = 0;
    if (bs_mark_workbook(NULL, NULL, "x", NULL, 0, BS_SIDE_OLD, "y", NULL, 0, &e) == 0 ||
        !e.msg[0]) {
        fail("mark", 2, "空指针应当拒绝", e.msg, "非空错误");
    } else {
        ok();
    }

    e.msg[0] = 0;
    if (bs_mark_dups(NULL, "x", "y", NULL, 0, &e) == 0 || !e.msg[0]) {
        fail("mark", 3, "空 sheet 应当拒绝", e.msg, "非空错误");
    } else {
        ok();
    }
}

static void run_reuse(void)
{
    bs_keys k;

    bs_keys_init(&k);
    bs_keys_make(&k, "111-000005-C50", "R1,R2");
    if (strcmp(k.pn, "111-000005-C50") || strcmp(k.ref, "R1|R2") ||
        strcmp(k.key, "P:111-000005-C50") || k.kind != 'P' || k.n_ok != 2) {
        fail("reuse", 1, "第一次", k.key ? k.key : "(null)", "P:111-000005-C50");
    } else {
        ok();
    }
    bs_keys_make(&k, "", "R9");
    if (strcmp(k.pn, "") || strcmp(k.ref, "R9") || strcmp(k.key, "R:R9") ||
        k.kind != 'R' || k.n_ok != 1 || k.n_bad != 0) {
        fail("reuse", 2, "第二次（料号必须清空）", k.pn ? k.pn : "(null)", "");
    } else {
        ok();
    }
    bs_keys_make(&k, "", "");
    if (k.pn[0] || k.ref[0] || k.key[0] || k.kind != 0) {
        fail("reuse", 3, "第三次（全空）", k.key ? k.key : "(null)", "");
    } else {
        ok();
    }
    bs_keys_free(&k);
    if (k.pn || k.ref || k.key || k.kind) {
        fail("reuse", 4, "free 之后", "non-null", "null");
    } else {
        ok();
    }
}

/* ---------------- 对账模式 ---------------- */
static void esc(const char *s, char *out, size_t cap)
{
    size_t o = 0;

    if (!s) s = "";
    for (; *s && o + 2 < cap; s++) {
        switch (*s) {
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:   out[o++] = *s; break;
        }
    }
    out[o] = 0;
}

static void unesc(const char *s, char *out, size_t cap)
{
    size_t o = 0;

    for (; *s && o + 1 < cap; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            default:  out[o++] = *s;   break;
            }
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

static int dump(const char *path)
{
    FILE *f = fopen(path, "rb");
    char line[8192];
    int lineno = 0;

    if (!f) {
        fprintf(stderr, "打不开 %s\n", path);
        return 2;
    }
    while (fgets(line, sizeof(line), f)) {
        char raw_pn[2048], raw_ref[2048];
        char nm[1024], pn[1024], ref[4096], key[6144];
        char e_nm[2048], e_pn[2048], e_ref[8192], e_key[12288];
        char *tab, *nl;
        int w = 0, s = 0, a = 0, b = 0;

        lineno++;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        nl = strchr(line, '\r');
        if (nl) *nl = 0;

        tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            unesc(tab + 1, raw_ref, sizeof(raw_ref));
        } else {
            raw_ref[0] = 0;
        }
        unesc(line, raw_pn, sizeof(raw_pn));

        bs_norm_str(raw_pn, nm, sizeof(nm));
        bs_pn_str(raw_pn, pn, sizeof(pn), &w, &s);
        bs_ref_sig_str(raw_ref, ref, sizeof(ref), &a, &b);
        bs_make_key(raw_pn, raw_ref, key, sizeof(key));

        esc(nm, e_nm, sizeof(e_nm));
        esc(pn, e_pn, sizeof(e_pn));
        esc(ref, e_ref, sizeof(e_ref));
        esc(key, e_key, sizeof(e_key));
        printf("%s\t%s\t%d\t%d\t%s\t%d\t%d\t%s\n",
               e_nm, e_pn, w, s, e_ref, a, b, e_key);
    }
    fclose(f);
    return 0;
}

/* ---------------- 识别层对账模式 ----------------
   输出格式固定为若干行，供 tools/check_sheet.py 与 Python 逐行比：
     blocks <n>
     block <i> hr=.. start=.. end=.. pn=<列名|-> ref=.. qty=..
     cols pn=.. ref=.. qty=..
     hit <%.6f>
     labels <n>
     unkeyable <n>
     rows <n>
     row <r> <block> <c1> <c2> <kind> <pn> <ref> <sig> <key> <qty|-> <strike> <dup_i> <dup_n> */
static void col_name_of(int c, char *out, size_t cap)
{
    char tmp[8];
    int i = 0, n = 0;

    out[0] = 0;
    if (c <= 0) { out[0] = '-'; out[1] = 0; return; }
    while (c && n < 7) {
        tmp[n++] = (char)('A' + (c - 1) % 26);
        c = (c - 1) / 26;
    }
    if ((size_t)n + 1 > cap) n = (int)cap - 1;
    while (n > 0) out[i++] = tmp[--n];
    out[i] = 0;
}

static int sheet_dump(const char *path, int idx, const char *scope)
{
    bs_err e;
    bs_cfg cfg;
    bs_sheet *s = NULL;
    int i;

    /* 不给 scope 就传 NULL，顺带走一遍「内部取默认配置」那条路 */
    bs_cfg_default(&cfg);
    if (scope && strcmp(scope, "key") == 0) cfg.strike_scope = 0;  /* 只算三格 */

    if (bs_sheet_open(path, idx, scope ? &cfg : NULL, &s, &e) != 0) {
        printf("ERR %s\n", e.msg);
        return 1;
    }
    printf("blocks %d\n", bs_sheet_block_count(s));
    for (i = 0; i < bs_sheet_block_count(s); i++) {
        const bs_block *b = bs_sheet_block(s, i);
        char a[8], r[8], q[8];
        col_name_of(b->col_pn, a, sizeof(a));
        col_name_of(b->col_ref, r, sizeof(r));
        col_name_of(b->col_qty, q, sizeof(q));
        printf("block %d hr=%d start=%d end=%d pn=%s ref=%s qty=%s\n",
               i, b->hr, b->start, b->end, a, r, q);
    }
    printf("cols pn=%s ref=%s qty=%s\n",
           bs_sheet_col_name(s, 'P'), bs_sheet_col_name(s, 'R'),
           bs_sheet_col_name(s, 'Q'));
    printf("hit %.6f\n", bs_sheet_hit_rate(s));
    printf("labels %d\n", bs_sheet_labels(s));
    printf("unkeyable %d\n", bs_sheet_unkeyable(s));
    printf("rows %d\n", bs_sheet_row_count(s));

    for (i = 0; i < bs_sheet_row_count(s); i++) {
        const bs_row *r = bs_sheet_row(s, i);
        char e_pn[1024], e_ref[4096], e_sig[4096], e_key[8192], qty[64];
        esc(r->pn, e_pn, sizeof(e_pn));
        esc(r->ref, e_ref, sizeof(e_ref));
        esc(r->ref_sig, e_sig, sizeof(e_sig));
        esc(r->key, e_key, sizeof(e_key));
        if (r->has_qty) sprintf(qty, "%.6f", r->qty);
        else strcpy(qty, "-");
        printf("row %d %d %d %d %c %s %s %s %s %s %d %d %d\n",
               r->row, r->block, r->c1, r->c2, r->kind ? r->kind : '0',
               e_pn, e_ref, e_sig, e_key, qty,
               (int)r->strike, r->dup_i, r->dup_n);
    }
    bs_sheet_close(s);
    return 0;
}

/* ---------------- 配对与 12 列对账 ----------------

   bstest --pair <old.xlsx> <oidx> <new.xlsx> <nidx> [all|key] [dup_policy]

   输出：
     stat pairs=.. pn=.. ref=.. fuzzy=.. pnchg=.. add=.. del=.. chg=.. cart=..
     cols <n>
     row <12 列，\t 分隔，值按 \\ \n \r \t 转义>
   前四行与 bomcore.pair 的 stats、后两段与 build_result 的返回逐格比。 */
static void put_esc(const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:   putchar((unsigned char)*s); break;
        }
    }
}

static int pair_dump(const char *opath, int oidx, const char *npath, int nidx,
                     const char *scope, const char *dup)
{
    bs_err e;
    bs_cfg cfg;
    bs_sheet *so = NULL, *sn = NULL;
    bs_pair *p = NULL;
    bs_result *res = NULL;
    bs_pairstat st;
    size_t np = 0, i;
    int k;

    bs_cfg_default(&cfg);
    if (scope && strcmp(scope, "key") == 0) cfg.strike_scope = 0;
    if (dup) cfg.dup_policy = atoi(dup);

    if (bs_sheet_open(opath, oidx, &cfg, &so, &e) != 0) {
        printf("ERR old: %s\n", e.msg);
        return 1;
    }
    if (bs_sheet_open(npath, nidx, &cfg, &sn, &e) != 0) {
        printf("ERR new: %s\n", e.msg);
        bs_sheet_close(so);
        return 1;
    }
    if (bs_pair_rows(so, sn, (scope && strcmp(scope, "def") == 0) ? NULL : &cfg,
                     &p, &np, &st, &e) != 0) {
        printf("ERR %s\n", e.msg);
        bs_sheet_close(so);
        bs_sheet_close(sn);
        return 1;
    }
    printf("stat pairs=%d pn=%d ref=%d fuzzy=%d pnchg=%d add=%d del=%d chg=%d cart=%d\n",
           st.n_paired, st.n_pn, st.n_ref, st.n_fuzzy, st.n_pnchg,
           st.n_added, st.n_deleted, st.n_changed, st.cartesian_fallback);
    if (bs_result_build(so, sn, p, np, &res, &e) != 0) {
        printf("ERR %s\n", e.msg);
        bs_pair_free(p);
        bs_sheet_close(so);
        bs_sheet_close(sn);
        return 1;
    }
    printf("cols %d\n", (int)bs_result_count(res));
    for (i = 0; i < bs_result_count(res); i++) {
        const bs_resrow *r = bs_result_row(res, i);
        printf("row");
        for (k = 0; k < BS_RES_COLS; k++) {
            putchar('\t');
            put_esc(r->col[k]);
        }
        putchar('\n');
    }
    bs_result_close(res);
    bs_pair_free(p);
    bs_sheet_close(so);
    bs_sheet_close(sn);
    return 0;
}

/* 写出对账：跑完整链路并写 xlsx，路径与行数打给 tools/check_write.py */
static int write_dump(const char *opath, int oidx, const char *npath, int nidx,
                      const char *out, const char *scope)
{
    bs_err e;
    bs_cfg cfg;
    bs_sheet *so = NULL, *sn = NULL;
    bs_pair *p = NULL;
    bs_result *res = NULL;
    bs_pairstat st;
    bs_diag dg[2];
    size_t np = 0;
    char real[1024];
    int rc = 1;

    bs_cfg_default(&cfg);
    if (scope && strcmp(scope, "key") == 0) cfg.strike_scope = 0;

    if (bs_sheet_open(opath, oidx, &cfg, &so, &e) != 0) {
        printf("ERR old: %s\n", e.msg);
        return 1;
    }
    if (bs_sheet_open(npath, nidx, &cfg, &sn, &e) != 0) {
        printf("ERR new: %s\n", e.msg);
        bs_sheet_close(so);
        return 1;
    }
    if (bs_pair_rows(so, sn, &cfg, &p, &np, &st, &e) != 0 ||
        bs_result_build(so, sn, p, np, &res, &e) != 0) {
        printf("ERR %s\n", e.msg);
        goto done;
    }

    bs_diag_fill(&dg[0], so, opath);
    bs_diag_fill(&dg[1], sn, npath);
    if (bs_write_result(out, res, dg, 2, real, sizeof(real), &e) != 0) {
        printf("ERR write: %s\n", e.msg);
        goto done;
    }
    printf("WROTE %s\n", real);
    printf("rows %lu\n", (unsigned long)bs_result_count(res));
    printf("diag %d\n", 2);
    rc = 0;

done:
    bs_result_close(res);
    bs_pair_free(p);
    bs_sheet_close(so);
    bs_sheet_close(sn);
    return rc;
}

/* 标注对账：把两侧的标注文件写出来，路径打给 tools/check_mark.py

   bstest --mark <old.xlsx> <oidx> <new.xlsx> <nidx> <outdir> [all|key]
   bstest --markdups <xlsx> <idx> <out.xlsx> */

/* 数一数刚写出的文件里有几条批注 —— 用 bs_zip 读回自己写的部件，
   等于从「写入」到「读回」走了一遍闭环。没有批注部件算 0 条；
   zip 打不开返回 -1。 */
static int count_comments(const char *path)
{
    bs_zip *z = NULL;
    char err[160];
    int i, cnt = 0;

    if (bs_zip_open(path, &z, err, sizeof err) != 0) return -1;
    for (i = 0; i < bs_zip_count(z); i++) {
        const char *name = bs_zip_entry_name(z, i);
        unsigned char *b = NULL;
        size_t n = 0;

        if (!strstr(name, "comments")) continue;
        if (bs_zip_read(z, name, &b, &n, err, sizeof err) != 0) continue;
        cnt = 0;
        {
            const char *p = (const char *)b;
            while ((p = strstr(p, "<comment ")) != NULL) { cnt++; p += 9; }
        }
        free(b);
        break;
    }
    bs_zip_close(z);
    return cnt;
}

static int mark_dump(const char *opath, int oidx, const char *npath, int nidx,
                     const char *outdir, const char *scope)
{
    bs_err e;
    bs_cfg cfg;
    bs_sheet *so = NULL, *sn = NULL;
    bs_pair *p = NULL;
    bs_pairstat st;
    size_t np = 0;
    char dst[1024], real[1024];
    int rc = 1;

    bs_cfg_default(&cfg);
    if (scope && strcmp(scope, "key") == 0) cfg.strike_scope = 0;

    if (bs_sheet_open(opath, oidx, &cfg, &so, &e) != 0) {
        printf("ERR old: %s\n", e.msg);
        return 1;
    }
    if (bs_sheet_open(npath, nidx, &cfg, &sn, &e) != 0) {
        printf("ERR new: %s\n", e.msg);
        bs_sheet_close(so);
        return 1;
    }
    if (bs_pair_rows(so, sn, &cfg, &p, &np, &st, &e) != 0) {
        printf("ERR %s\n", e.msg);
        goto done;
    }

    snprintf(dst, sizeof dst, "%s/%s_旧.xlsx", outdir, "mark");
    if (bs_mark_workbook(so, sn, opath, p, np, BS_SIDE_OLD, dst, real, sizeof real, &e) != 0) {
        printf("ERR mark old: %s\n", e.msg);
        goto done;
    }
    printf("MARK old %s\n", real);
    printf("NOTES old %d\n", count_comments(real));

    snprintf(dst, sizeof dst, "%s/%s_新.xlsx", outdir, "mark");
    if (bs_mark_workbook(sn, so, npath, p, np, BS_SIDE_NEW, dst, real, sizeof real, &e) != 0) {
        printf("ERR mark new: %s\n", e.msg);
        goto done;
    }
    printf("MARK new %s\n", real);
    printf("NOTES new %d\n", count_comments(real));
    rc = 0;

done:
    bs_pair_free(p);
    bs_sheet_close(so);
    bs_sheet_close(sn);
    return rc;
}

static int markdups_dump(const char *path, int idx, const char *out)
{
    bs_err e;
    bs_cfg cfg;
    bs_sheet *s = NULL;
    char real[1024];

    bs_cfg_default(&cfg);
    if (bs_sheet_open(path, idx, &cfg, &s, &e) != 0) {
        printf("ERR %s\n", e.msg);
        return 1;
    }
    if (bs_mark_dups(s, path, out, real, sizeof real, &e) != 0) {
        printf("ERR mark dups: %s\n", e.msg);
        bs_sheet_close(s);
        return 1;
    }
    printf("MARK dup %s\n", real);
    printf("NOTES dup %d\n", count_comments(real));
    bs_sheet_close(s);
    return 0;
}

/* ---------------- deflate：压缩后必须能被自家 inflate 原样解回来 ----------
   这条回环比「压得小」重要得多：写坏一个 zip 会让 Excel 直接说文件损坏，
   而用户手上那份产物里可能有几万个单元格。 */
static int g_df_idx;

static void deflate_case(const char *what, const unsigned char *src, size_t n)
{
    unsigned char *c = NULL, *back = NULL;
    size_t cn = 0, bn = 0;
    char err[128] = "";
    const int df = bs_deflate_raw(src, n, &c, &cn);
    const int inf = df == 0 ? bs_inflate_raw(c, cn, &back, &bn, 0, err, sizeof err) : -1;
    char got[160];

    g_df_idx++;
    if (df != 0 || inf != 0 || bn != n || (n && memcmp(src, back, n) != 0)) {
        snprintf(got, sizeof got, "df=%d inf=%d n=%lu/%lu %s", df, inf,
                 (unsigned long)bn, (unsigned long)n, err);
        fail("deflate", g_df_idx, what, got, "回环一致");
    } else {
        ok();
    }
    free(c);
    free(back);
}

static void run_deflate(void)
{
    size_t i, n;
    unsigned char *buf;
    unsigned seed = 12345u;

    deflate_case("单字节", (const unsigned char *)"a", 1);
    deflate_case("短串", (const unsigned char *)"abcabcabc", 9);

    /* 全同字节：LZ77 最理想的情形 */
    n = 70000;
    buf = (unsigned char *)malloc(n);
    if (buf) {
        memset(buf, 'x', n);
        deflate_case("7 万同字节", buf, n);
        free(buf);
    }

    /* 伪随机：基本无法匹配，专门压 deflate 的字面量路径 */
    n = 40000;
    buf = (unsigned char *)malloc(n);
    if (buf) {
        for (i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = (unsigned char)(seed >> 16);
        }
        deflate_case("4 万伪随机", buf, n);
        free(buf);
    }

    /* 像 xml 一样的重复结构：真实场景 */
    {
        const char *unit = "<c r=\"A1\" t=\"inlineStr\"><is><t>100-033300-014</t></is></c>";
        const size_t un = strlen(unit);
        n = un * 3000;
        buf = (unsigned char *)malloc(n);
        if (buf) {
            for (i = 0; i < 3000; i++) memcpy(buf + i * un, unit, un);
            deflate_case("xml 片段 x3000", buf, n);
            free(buf);
        }
    }

    /* 0..255 循环：覆盖所有字节值，顺便验证二进制数据不被打断 */
    n = 256 * 40;
    buf = (unsigned char *)malloc(n);
    if (buf) {
        for (i = 0; i < n; i++) buf[i] = (unsigned char)(i & 0xFF);
        deflate_case("0..255 循环", buf, n);
        free(buf);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--dump") == 0) return dump(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--sheet") == 0)
        return sheet_dump(argv[2], argc >= 4 ? atoi(argv[3]) : 0,
                          argc >= 5 ? argv[4] : NULL);
    if (argc >= 6 && strcmp(argv[1], "--pair") == 0)
        return pair_dump(argv[2], atoi(argv[3]), argv[4], atoi(argv[5]),
                         argc >= 7 ? argv[6] : NULL, argc >= 8 ? argv[7] : NULL);
    if (argc >= 7 && strcmp(argv[1], "--write") == 0)
        return write_dump(argv[2], atoi(argv[3]), argv[4], atoi(argv[5]),
                          argv[6], argc >= 8 ? argv[7] : NULL);
    if (argc >= 7 && strcmp(argv[1], "--mark") == 0)
        return mark_dump(argv[2], atoi(argv[3]), argv[4], atoi(argv[5]),
                         argv[6], argc >= 8 ? argv[7] : NULL);
    if (argc >= 5 && strcmp(argv[1], "--markdups") == 0)
        return markdups_dump(argv[2], atoi(argv[3]), argv[4]);

    run_norm();
    run_pn();
    run_ref();
    run_key();
    run_reuse();
    run_mark();
    run_deflate();

    printf("%d 项通过，%d 项失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
