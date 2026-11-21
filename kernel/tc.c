/* TC: 系统内置 C 子集编译器.
   直接生成 x86-64 机器码, 输出 .gtp 平面程序 (由 RUN 装载到 0x600000).

   支持:
     * int / char / 指针变量, 全局与局部数组 (最多 4 维)
     * struct / union (具名定义, 字段数组, 嵌套结构, . 与 ->)
     * 函数 (最多 6 参数, 递归, 原型), if/else, while, for,
       switch/case/default (可穿透), break/continue/return
     * 完整整数表达式: + - * / % & | ^ ~ << >> 比较 && || !
     * 三目 ?:, 前置/后置 ++ --, 复合赋值 (= += -= *= /= %= &= |= ^= <<= >>=)
     * 字符串字面量, 全局常量初始化, 局部初始化, 数组/结构体初始化列表
     * SDK 内置函数 (SYSCALL 包装)

   布局: 代码 128KB @0x600000, 数据 128KB @0x620000
         (全局 64KB, 字符串池 64KB). 数值一律按 64 位整数处理. */
#include "gt.h"
/* 内置标准库源码 (tools/sdk/libc.c -> build/tc_lib.h, 由 build.py 生成) */
#include "tc_lib.h"

#define TC_CODE_MAX  0x20000
#define TC_DATA_GLOB 0x10000
#define TC_DATA_MAX  0x20000
#define TC_PROG_BASE 0x600000ULL
#define TC_DATA_BASE (TC_PROG_BASE + TC_CODE_MAX)
#define TC_GLOB_BASE TC_DATA_BASE
#define TC_STR_BASE  (TC_DATA_BASE + TC_DATA_GLOB)

/* 注意: 这些表在 .bss, 合计必须让 __bss_end 留在 0xA0000 (VGA 孔径) 以下.
   kernel.ld 末尾有链接期断言, 超限会直接构建失败. */
#define MAX_SYM   256
#define MAX_LOCAL 128
#define MAX_FCALL 256
#define MAX_NEST  16
#define MAX_ARGS  6
#define MAX_DIM   4
#define MAX_STRUCT 12
#define MAX_FIELDS 16
#define MAX_CASES 32
#define MAX_TYPEDEF 32
#define MAX_ENUM 64

enum {
    T_EOF = 0, T_ID = 256, T_NUM, T_STR, T_FNUM,
    T_EQ, T_NE, T_LE, T_GE, T_ANDAND, T_OROR, T_SHL, T_SHR,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_ANDEQ, T_OREQ, T_XOREQ, T_SHLEQ, T_SHREQ,
    T_INC, T_DEC, T_ARROW
};

/* ---------------------------------------------------------------- 类型 */
enum { CK_VOID, CK_INT, CK_CHAR, CK_STRUCT, CK_DOUBLE };

struct ctype {
    int kind;        /* CK_* */
    int ptr;         /* 指针层数 */
    int sid;         /* 结构体编号 (kind == CK_STRUCT) */
    int obj;         /* 非指针对象的字节数 */
    int func;        /* 1 = 函数指针 */
    int uns;         /* 1 = unsigned 整数 */
};

struct field {
    char name[32];
    struct ctype type;      /* 元素类型 (数组时) */
    int dims[MAX_DIM];
    int ndims;
    int nelem;
    int offset;
    int bit_off;            /* 位域: 在 8 字节存储单元中的起始位 */
    int bit_width;          /* 位域宽度; 0 = 普通字段 */
};

struct stype {
    char name[32];
    int size;
    int nf;
    bool is_union;
    bool complete;
    int bit_base;           /* 当前位域存储单元偏移, -1 = 无 */
    int bit_used;           /* 当前单元已用位数 */
    struct field f[MAX_FIELDS];
};

struct token {
    int t;
    i64 v;
    int line;
    char name[32];
    int slen;                 /* 字符串字面量字节数 (不含 NUL) */
    u32 soff;                 /* 字符串在池中的偏移 */
};

enum { SK_GLOBAL = 1, SK_LOCAL, SK_FUNC };

struct sym {
    char name[32];
    i64 addr;                 /* 全局地址 / 函数代码偏移 */
    int kind;
    int offset;               /* 局部变量 rbp 偏移 */
    int nparams;
    bool defined;
    struct ctype type;        /* 标量类型, 或数组元素类型 */
    int dims[MAX_DIM];
    int ndims;
    int nelem;
    int size;                 /* 变量总字节数 */
};

struct decl {
    char name[32];
    struct ctype type;        /* 元素类型 */
    int dims[MAX_DIM];
    int ndims;
    int nelem;                /* 0 = 未知 (初始化时确定) */
    bool incomplete;
};

struct fcall {
    u32 pos;
    char name[32];
};

struct loopctx {
    u32 breaks[16];
    int nbreak;
    u32 conts[16];
    int ncont;
    u32 cont;
    bool is_switch;
};

struct caselab {
    i64 val;
    u32 addr;
};

struct swctx {
    int tmp;
    int ncase;
    struct caselab cases[MAX_CASES];
    u32 def_addr;
    bool has_def;
};

/* ---------------------------------------------------------------- 状态 */
static u8  code[TC_CODE_MAX] __attribute__((section(".highbss")));
static u8  str_pool[TC_DATA_MAX - TC_DATA_GLOB] __attribute__((section(".highbss")));
static u8  out_buf[TC_CODE_MAX + TC_DATA_MAX] __attribute__((section(".highbss")));
static u8  gmem[TC_DATA_GLOB] __attribute__((section(".highbss")));

static const char *sp;
static int line_no = 1;
static struct token cur, nxt;

static u32 clen;
static u32 str_used;
static int gused;
static bool failed;
static char err_msg[96];

/* 优化开关 (tc_compile_opts 设置) */
static bool opt_fold, opt_strength, opt_dce, opt_loops;
static u32  last_code_len, last_data_len;

/* 大表放 .highbss, 避免把 .bss 推过 0xA0000 显存孔径 */
static struct sym syms[MAX_SYM] __attribute__((section(".highbss")));
static int nsyms;
static struct sym locs[MAX_LOCAL] __attribute__((section(".highbss")));
static int nloc;
static int lused;                  /* 局部变量已占用字节 */

static struct stype stypes[MAX_STRUCT] __attribute__((section(".highbss")));
static int nstypes;

static struct fcall fcs[MAX_FCALL] __attribute__((section(".highbss")));
static int nfc;
static struct loopctx loops[MAX_NEST] __attribute__((section(".highbss")));
static int nloop;
static struct swctx swstk[MAX_NEST] __attribute__((section(".highbss")));
static int nsw;

struct typedef_ent {
    char name[32];
    struct ctype type;
};
static struct typedef_ent typedefs[MAX_TYPEDEF] __attribute__((section(".highbss")));
static int ntypedefs;

struct enum_ent {
    char name[32];
    i64 val;
};
static struct enum_ent enums[MAX_ENUM] __attribute__((section(".highbss")));
static int nenums;

/* #include 处理: 最多 4 层, 每层 8KB; 标准库按包含的头文件按需编入 */
#define TC_INC_MAX 4
#define TC_INC_BUF 8192
#define TC_INC_SEEN 16
static char inc_buf[TC_INC_MAX][TC_INC_BUF] __attribute__((section(".highbss")));
static const char *inc_ret_sp[TC_INC_MAX];
static int inc_ret_line[TC_INC_MAX];
static char inc_ret_dir[TC_INC_MAX][96];
static int inc_depth;
static char inc_cur_dir[96];
static char inc_seen[TC_INC_SEEN][128];
static int nseen;
static bool want_lib_string, want_lib_stdlib, want_lib_ctype, want_lib_stdio;

/* goto / 标签 (函数作用域) */
#define MAX_LABEL 64
struct label_ent {
    char name[32];
    u32 addr;
    bool defined;
};
struct goto_ent {
    char name[32];
    u32 pos;
};
static struct label_ent labels[MAX_LABEL];
static int nlabels;
static struct goto_ent gotos[MAX_LABEL];
static int ngotos;

static u32 main_call_pos;
static u32 locals_imm_pos;

/* 上一次表达式结果 */
struct ety {
    struct ctype t;               /* 值类型 / 数组元素类型 */
    bool lval;                    /* rax 中保存的是地址 */
    bool is_arr;                  /* 仍是数组视图 (rax=首地址) */
    bool is_bit;                  /* 位域 lvalue: rax=所在存储单元地址 */
    bool is_const;                /* 编译期常量: 值在 cval, 尚未写入 rax */
    i64  cval;
    int bit_off;
    int bit_width;
    int dims[MAX_DIM];
    int ndims;
    int esize;
    int nelem;
};

static struct ety g_e;

static bool parse_ctype(struct ctype *t);

/* ------------------------------------------------------------------ 错误 */
static void fail(const char *msg)
{
    if (failed)
        return;
    failed = true;
    char num[16];
    itoa_base((u32)(cur.line > 0 ? cur.line : line_no), num, 10, false);
    int k = 0;
    const char *a = "line ";
    while (*a && k < 60) err_msg[k++] = *a++;
    const char *b = num;
    while (*b && k < 60) err_msg[k++] = *b++;
    if (k < 60) err_msg[k++] = ':';
    if (k < 60) err_msg[k++] = ' ';
    while (*msg && k < 90) err_msg[k++] = *msg++;
    err_msg[k] = 0;
}

const char *tc_error(void) { return err_msg; }

/* ------------------------------------------------------------------ 词法 */
static void add_str_byte(u8 b)
{
    if (str_used >= sizeof(str_pool)) {
        fail("string pool full");
        return;
    }
    str_pool[str_used++] = b;
}

/* ------------------------------------------------------ #include 支持 */
static void path_copy(char *d, const char *s, int cap)
{
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void dir_of(const char *path, char out[96])
{
    int last = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '\\' || path[i] == '/')
            last = i;
    int n = last < 0 ? 0 : last;
    if (n > 95) n = 95;
    memcpy(out, path, (u32)n);
    out[n] = 0;
}

static void join_path(char *out, int cap, const char *dir, const char *name)
{
    int k = 0;
    if (dir && dir[0]) {
        while (dir[k] && k < cap - 2) { out[k] = dir[k]; k++; }
        if (k > 0 && out[k - 1] != '\\' && out[k - 1] != '/')
            out[k++] = '\\';
    }
    int i = 0;
    while (name[i] && k < cap - 1) out[k++] = name[i++];
    out[k] = 0;
}

static void drive_path(char *out, int cap, const char *rel)
{
    int k = 0;
    out[k++] = fat_drive();
    out[k++] = ':';
    int i = 0;
    while (rel[i] && k < cap - 1) out[k++] = rel[i++];
    out[k] = 0;
}

static bool path_seen(const char *p)
{
    for (int i = 0; i < nseen; i++)
        if (!strcasecmp(inc_seen[i], p))
            return true;
    return false;
}

static void path_remember(const char *p)
{
    if (nseen >= TC_INC_SEEN)
        return;
    path_copy(inc_seen[nseen], p, 128);
    nseen++;
}

static void mark_module(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    if (!strcasecmp(b, "string.h")) want_lib_string = true;
    else if (!strcasecmp(b, "stdlib.h")) want_lib_stdlib = true;
    else if (!strcasecmp(b, "ctype.h")) want_lib_ctype = true;
    else if (!strcasecmp(b, "stdio.h")) want_lib_stdio = true;
}

/* 读入头文件并压栈; 找不到返回 false */
static bool do_include(const char *name, bool quoted)
{
    if (inc_depth >= TC_INC_MAX) {
        fail("include nested too deep");
        return true;
    }
    char cand[160];
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0) {
            if (!quoted)
                continue;
            join_path(cand, sizeof(cand), inc_cur_dir, name);
        } else if (pass == 1) {
            char rel[144];
            int k = 0;
            const char *pre = "\\SDK\\";
            while (*pre && k < 130) rel[k++] = *pre++;
            int i = 0;
            while (name[i] && k < 143) rel[k++] = name[i++];
            rel[k] = 0;
            drive_path(cand, sizeof(cand), rel);
        } else {
            char rel[144];
            int k = 0;
            const char *pre = "\\GT-DOS\\INCLUDE\\";
            while (*pre && k < 120) rel[k++] = *pre++;
            int i = 0;
            while (name[i] && k < 143) rel[k++] = name[i++];
            rel[k] = 0;
            drive_path(cand, sizeof(cand), rel);
        }
        if (path_seen(cand))
            return true;

        u32 got = 0;
        if (fat_read_file(cand, inc_buf[inc_depth], TC_INC_BUF - 1, &got) &&
            got > 0) {
            inc_buf[inc_depth][got] = 0;
            path_remember(cand);
            inc_ret_sp[inc_depth] = sp;
            inc_ret_line[inc_depth] = line_no;
            path_copy(inc_ret_dir[inc_depth], inc_cur_dir, 96);
            char nd[96];
            dir_of(cand, nd);
            path_copy(inc_cur_dir, nd, 96);
            sp = inc_buf[inc_depth];
            line_no = 1;
            inc_depth++;
            mark_module(name);
            return true;
        }
    }
    return false;
}

/* 取 "#include" 之后的名字: <name> 或 "name" */
static bool parse_inc_name(char *out, int cap, bool *quoted)
{
    while (*sp == ' ' || *sp == '\t')
        sp++;
    char close;
    if (*sp == '"') {
        *quoted = true;
        close = '"';
        sp++;
    } else if (*sp == '<') {
        *quoted = false;
        close = '>';
        sp++;
    } else {
        return false;
    }
    int k = 0;
    while (*sp && *sp != close && *sp != '\n' && k < cap - 1)
        out[k++] = *sp++;
    out[k] = 0;
    if (*sp == close)
        sp++;
    return k > 0;
}

/* --------------------------------------------------------- #define 宏 */
#define MAX_MACRO 32
#define MAC_PARAMS 6
#define MAC_BODY 200
#define MAC_POOL 4096
#define EXP_MAX 8
#define EXP_CAP 1024

struct macro {
    char name[32];
    bool func;
    int nparams;
    char param[MAC_PARAMS][16];
    const char *body;
};
static struct macro macros[MAX_MACRO] __attribute__((section(".highbss")));
static int nmacros;
static char mac_pool[MAC_POOL] __attribute__((section(".highbss")));
static int mac_used;
static char exp_buf[EXP_MAX][EXP_CAP] __attribute__((section(".highbss")));
static const char *exp_ret_sp[EXP_MAX];
static int exp_ret_line[EXP_MAX];
static int exp_depth;

static bool ident_char_(char c)
{
    return isalpha_((unsigned char)c) || isdigit_((unsigned char)c) || c == '_';
}

static struct macro *find_macro(const char *name)
{
    for (int i = 0; i < nmacros; i++)
        if (!strcmp(macros[i].name, name))
            return &macros[i];
    return 0;
}

static void trim_arg(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    int b = 0;
    while (s[b] == ' ' || s[b] == '\t')
        b++;
    if (b > 0) {
        int i = 0;
        while (s[b + i]) { s[i] = s[b + i]; i++; }
        s[i] = 0;
    }
}

/* 解析并登记 #define (sp 已指向 define 之后) */
static void parse_define(void)
{
    while (*sp == ' ' || *sp == '\t') sp++;
    char name[32];
    int n = 0;
    while (ident_char_(*sp) && n < 31)
        name[n++] = *sp++;
    name[n] = 0;
    if (!n) {
        while (*sp && *sp != '\n') sp++;
        return;
    }
    bool func = (*sp == '(');
    char params[MAC_PARAMS][16];
    int np = 0;
    if (func) {
        sp++;
        while (*sp && *sp != ')') {
            while (*sp == ' ' || *sp == '\t' || *sp == ',') sp++;
            if (*sp == ')') break;
            char pn[16];
            int k = 0;
            while (ident_char_(*sp) && k < 15)
                pn[k++] = *sp++;
            pn[k] = 0;
            if (k && np < MAC_PARAMS) {
                strncpy(params[np], pn, 15);
                params[np][15] = 0;
                np++;
            }
            while (*sp && *sp != ')' && *sp != ',') sp++;
        }
        if (*sp == ')') sp++;
    }
    while (*sp == ' ' || *sp == '\t') sp++;

    if (mac_used + MAC_BODY + 2 >= MAC_POOL) {
        fail("macro pool full");
        while (*sp && *sp != '\n') sp++;
        return;
    }
    int start = mac_used;
    int k = 0;
    while (*sp && *sp != '\n') {
        char c = *sp;
        if (c == '\\' && sp[1] == '\n') {
            sp += 2;
            line_no++;
            if (k < MAC_BODY) mac_pool[mac_used++] = ' ';
            continue;
        }
        if (c == '/' && sp[1] == '/') {
            while (*sp && *sp != '\n') sp++;
            break;
        }
        if (k < MAC_BODY) mac_pool[mac_used++] = c;
        sp++;
        k++;
    }
    while (mac_used > start &&
           (mac_pool[mac_used - 1] == ' ' || mac_pool[mac_used - 1] == '\t'))
        mac_used--;
    mac_pool[mac_used++] = 0;

    struct macro *m = find_macro(name);
    if (!m) {
        if (nmacros >= MAX_MACRO) {
            fail("too many macros");
            return;
        }
        m = &macros[nmacros++];
    }
    memset(m, 0, sizeof(*m));
    strncpy(m->name, name, 31);
    m->name[31] = 0;
    m->func = func;
    m->nparams = np;
    for (int i = 0; i < np; i++) {
        strncpy(m->param[i], params[i], 15);
        m->param[i][15] = 0;
    }
    m->body = &mac_pool[start];
}

/* 把宏展开文本写入 dst; 函数式宏会从 sp 解析实参. 返回 false 表示不展开. */
static bool macro_build(struct macro *m, char *dst, int cap)
{
    if (!m->func) {
        int k = 0;
        for (const char *p = m->body; *p && k < cap - 1; p++)
            dst[k++] = *p;
        dst[k] = 0;
        return true;
    }

    char *q = (char *)sp;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '(')
        return false;
    q++;

    char args[MAC_PARAMS][64];
    memset(args, 0, sizeof(args));
    int nargs = 0, k = 0, depth = 1;
    while (*q) {
        char c = *q;
        if (c == '"' || c == '\'') {
            char quote = c;
            if (k < 63) args[nargs][k++] = *q;
            q++;
            while (*q && *q != quote) {
                if (*q == '\\' && q[1]) {
                    if (k < 63) args[nargs][k++] = *q++;
                }
                if (k < 63) args[nargs][k++] = *q;
                q++;
            }
            if (*q == quote) {
                if (k < 63) args[nargs][k++] = *q;
                q++;
            }
            continue;
        }
        if (c == '(') depth++;
        if (c == ')') {
            depth--;
            if (depth == 0) { q++; break; }
        }
        if (c == ',' && depth == 1) {
            args[nargs][k] = 0;
            trim_arg(args[nargs]);
            nargs++;
            k = 0;
            if (nargs >= MAC_PARAMS)
                return false;
            q++;
            while (*q == ' ' || *q == '\t') q++;
            continue;
        }
        if (k < 63) args[nargs][k++] = c;
        q++;
    }
    args[nargs][k] = 0;
    trim_arg(args[nargs]);
    int na = nargs + 1;
    if (na == 1 && args[0][0] == 0)
        na = 0;
    if (na != m->nparams)
        return false;

    sp = q;
    int d = 0;
    bool paste = false;
    const char *p = m->body;
    while (*p && d < cap - 1) {
        char c = *p;
        /* ## 记号拼接: 去掉两侧空白 */
        if (c == '#' && p[1] == '#') {
            while (d > 0 && (dst[d - 1] == ' ' || dst[d - 1] == '\t'))
                d--;
            p += 2;
            paste = true;
            continue;
        }
        /* # 参数 字符串化 */
        if (c == '#' && m->func) {
            const char *r = p + 1;
            while (*r == ' ' || *r == '\t') r++;
            if (ident_char_(*r)) {
                char w[32];
                int wn = 0;
                while (ident_char_(*r) && wn < 31)
                    w[wn++] = *r++;
                w[wn] = 0;
                int pi = -1;
                for (int i = 0; i < m->nparams; i++)
                    if (!strcmp(m->param[i], w)) { pi = i; break; }
                if (pi >= 0) {
                    if (paste) {
                        while (d > 0 && dst[d - 1] == ' ')
                            d--;
                        paste = false;
                    }
                    if (d < cap - 1) dst[d++] = '"';
                    for (int i = 0; args[pi][i] && d < cap - 2; i++) {
                        char ch = args[pi][i];
                        if (ch == '"' || ch == '\\') {
                            if (d < cap - 2) dst[d++] = '\\';
                        }
                        dst[d++] = ch;
                    }
                    if (d < cap - 1) dst[d++] = '"';
                    p = r;
                    continue;
                }
            }
            dst[d++] = '#';
            p++;
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            if (paste) {
                while (d > 0 && dst[d - 1] == ' ') d--;
                paste = false;
            }
            dst[d++] = *p++;
            while (*p && *p != quote && d < cap - 1) {
                if (*p == '\\' && p[1]) {
                    dst[d++] = *p++;
                    if (d < cap - 1) dst[d++] = *p++;
                    continue;
                }
                dst[d++] = *p++;
            }
            if (*p == quote && d < cap - 1) dst[d++] = *p++;
            continue;
        }
        if (ident_char_(c)) {
            char w[32];
            int wn = 0;
            while (ident_char_(*p) && wn < 31)
                w[wn++] = *p++;
            w[wn] = 0;
            int pi = -1;
            for (int i = 0; i < m->nparams; i++) {
                if (!strcmp(m->param[i], w)) { pi = i; break; }
            }
            const char *src = (pi >= 0) ? args[pi] : w;
            if (paste) {
                while (d > 0 && dst[d - 1] == ' ') d--;
                paste = false;
            }
            for (int i = 0; src[i] && d < cap - 1; i++)
                dst[d++] = src[i];
            continue;
        }
        if (paste) {
            while (d > 0 && dst[d - 1] == ' ') d--;
            paste = false;
        }
        dst[d++] = *p++;
    }
    dst[d] = 0;
    return true;
}

/* --------------------------------------------- 十进制字面量 -> IEEE double */
static unsigned __int128 div128_u64(unsigned __int128 a, u64 d)
{
    unsigned __int128 q = 0;
    u64 rem = 0;
    for (int i = 127; i >= 0; i--) {
        rem = (u64)((rem << 1) | (u64)((a >> i) & 1));
        if (rem >= d) {
            rem = (u64)(rem - d);
            q |= (unsigned __int128)1 << i;
        }
    }
    return q;
}

static int hi_bit128(unsigned __int128 v)
{
    for (int i = 127; i >= 0; i--)
        if ((v >> i) != 0)
            return i;
    return -1;
}

/* 把 "1.5e3" 之类的十进制文本转为 IEEE-754 double 位模式 (纯整数运算) */
static u64 dec_to_double(const char *s)
{
    int i = 0, neg = 0;
    if (s[i] == '-') { neg = 1; i++; }
    else if (s[i] == '+') i++;

    unsigned __int128 m = 0;
    int fdig = 0, exp10 = 0;
    while (s[i] >= '0' && s[i] <= '9')
        m = m * 10 + (unsigned)(s[i++] - '0');
    if (s[i] == '.') {
        i++;
        while (s[i] >= '0' && s[i] <= '9') {
            m = m * 10 + (unsigned)(s[i++] - '0');
            fdig++;
        }
    }
    if (s[i] == 'e' || s[i] == 'E') {
        i++;
        int es = 0, en = 0;
        if (s[i] == '-') { es = 1; i++; }
        else if (s[i] == '+') i++;
        while (s[i] >= '0' && s[i] <= '9')
            en = en * 10 + (s[i++] - '0');
        exp10 = es ? -en : en;
    }
    exp10 -= fdig;
    u64 sign = neg ? (1ULL << 63) : 0;
    if (m == 0)
        return sign;

    const unsigned __int128 maxv = ~(unsigned __int128)0;
    int e2 = 0;
    if (exp10 >= 0) {
        for (int k = 0; k < exp10; k++) {
            if (m > maxv / 10)
                return sign | (0x7FFULL << 52);   /* 溢出 -> inf */
            m *= 10;
        }
    } else {
        m <<= 64;
        e2 = -64;
        for (int k = 0; k < -exp10; k++)
            m = div128_u64(m, 10);
    }

    if (m == 0)
        return sign;
    int p = hi_bit128(m);
    if (p > 52) {
        int sh = p - 52;
        m += (unsigned __int128)1 << (sh - 1);   /* 四舍五入 */
        p = hi_bit128(m);
        sh = p - 52;
        m >>= sh;
        e2 += sh;
    } else if (p < 52) {
        int sh = 52 - p;
        m <<= sh;
        e2 -= sh;
    }
    int ef = e2 + 1023 + 52;
    if (ef >= 0x7FF)
        return sign | (0x7FFULL << 52);
    if (ef <= 0)
        return sign;                              /* 下溢 -> 0 */
    u64 frac = (u64)m & ((1ULL << 52) - 1);
    return sign | ((u64)ef << 52) | frac;
}

/* 解析反斜杠转义 (sp 指向 '\\' 之后), 支持 \xNN 与八进制 */
static int scan_escape(void)
{
    char e = *sp++;
    if (e == 'x') {
        int v = 0, n = 0;
        for (;;) {
            char h = *sp;
            int d;
            if (h >= '0' && h <= '9') d = h - '0';
            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
            else break;
            v = v * 16 + d;
            sp++;
            if (++n >= 2) break;
        }
        return v;
    }
    if (e >= '0' && e <= '7') {
        int v = e - '0', n = 1;
        while (n < 3 && *sp >= '0' && *sp <= '7') {
            v = v * 8 + (*sp++ - '0');
            n++;
        }
        return v;
    }
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    default: return (u8)e;
    }
}

static struct token scan(void)
{
    struct token t;
    memset(&t, 0, sizeof(t));
    t.line = line_no;

    for (;;) {
        /* 宏展开文本结束: 回到上一层输入 */
        if (!*sp && exp_depth > 0) {
            exp_depth--;
            sp = exp_ret_sp[exp_depth];
            line_no = exp_ret_line[exp_depth];
            continue;
        }
        /* 头文件结束: 回到上一层源文件 */
        if (!*sp && inc_depth > 0) {
            inc_depth--;
            sp = inc_ret_sp[inc_depth];
            line_no = inc_ret_line[inc_depth];
            path_copy(inc_cur_dir, inc_ret_dir[inc_depth], 96);
            continue;
        }
        char c = *sp;
        if (!c) { t.t = T_EOF; return t; }
        if (c == ' ' || c == '\t' || c == '\r') { sp++; continue; }
        if (c == '\n') { line_no++; sp++; continue; }
        if (c == '/' && sp[1] == '/') {
            while (*sp && *sp != '\n') sp++;
            continue;
        }
        if (c == '/' && sp[1] == '*') {
            sp += 2;
            while (*sp && !(sp[0] == '*' && sp[1] == '/')) {
                if (*sp == '\n') line_no++;
                sp++;
            }
            if (*sp) sp += 2;
            continue;
        }
        if (c == '#') {
            sp++;
            while (*sp == ' ' || *sp == '\t')
                sp++;
            /* 仅处理 #include; 其余指令跳过整行 */
            if (sp[0] == 'i' && sp[1] == 'n' && sp[2] == 'c' &&
                sp[3] == 'l' && sp[4] == 'u' && sp[5] == 'd' &&
                sp[6] == 'e' &&
                !((sp[7] >= 'a' && sp[7] <= 'z') ||
                  (sp[7] >= 'A' && sp[7] <= 'Z') ||
                  (sp[7] >= '0' && sp[7] <= '9') || sp[7] == '_')) {
                sp += 7;
                char nm[120];
                bool quoted = false;
                if (!parse_inc_name(nm, sizeof(nm), &quoted) ||
                    !do_include(nm, quoted)) {
                    fail("include not found");
                }
                if (failed) { t.t = T_EOF; return t; }
                continue;
            }
            if (sp[0] == 'd' && sp[1] == 'e' && sp[2] == 'f' &&
                sp[3] == 'i' && sp[4] == 'n' && sp[5] == 'e' &&
                !ident_char_(sp[6])) {
                sp += 6;
                parse_define();
                if (failed) { t.t = T_EOF; return t; }
                continue;
            }
            /* 其它指令 (含 #if 系列) 整行跳过 */
            while (*sp && *sp != '\n') sp++;
            continue;
        }
        break;
    }

    char c = *sp;
    if (isalpha_((unsigned char)c) || c == '_') {
        int n = 0;
        while ((isalpha_((unsigned char)*sp) || isdigit_((unsigned char)*sp) ||
                *sp == '_') && n < 31)
            t.name[n++] = *sp++;
        t.name[n] = 0;
        struct macro *m = find_macro(t.name);
        if (m && exp_depth < EXP_MAX) {
            if (macro_build(m, exp_buf[exp_depth], EXP_CAP)) {
                exp_ret_sp[exp_depth] = sp;
                exp_ret_line[exp_depth] = line_no;
                sp = exp_buf[exp_depth];
                exp_depth++;
                return scan();
            }
        }
        t.t = T_ID;
        return t;
    }
    if (isdigit_((unsigned char)c) ||
        (c == '.' && sp[1] >= '0' && sp[1] <= '9')) {
        i64 v = 0;
        if (c == '0' && (sp[1] == 'x' || sp[1] == 'X')) {
            sp += 2;
            for (;;) {
                c = *sp;
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                v = v * 16 + d;
                sp++;
            }
            t.t = T_NUM;
            t.v = v;
            return t;
        }
        if (c == '0' && (sp[1] == 'b' || sp[1] == 'B')) {
            sp += 2;
            while (*sp == '0' || *sp == '1')
                v = v * 2 + (*sp++ - '0');
            t.t = T_NUM;
            t.v = v;
            return t;
        }

        /* 十进制: 收集文本, 判断是否为浮点字面量 */
        char txt[48];
        int tn = 0;
        bool isf = false;
        while (isdigit_(*sp) && tn < 40)
            txt[tn++] = *sp++;
        if (*sp == '.' && sp[1] >= '0' && sp[1] <= '9') {
            isf = true;
            txt[tn++] = '.';
            sp++;
            while (isdigit_(*sp) && tn < 40)
                txt[tn++] = *sp++;
        }
        if (*sp == 'e' || *sp == 'E') {
            const char *q = sp + 1;
            if (*q == '+' || *q == '-') q++;
            if (isdigit_((unsigned char)*q)) {
                isf = true;
                txt[tn++] = *sp++;
                if (*sp == '+' || *sp == '-')
                    txt[tn++] = *sp++;
                while (isdigit_(*sp) && tn < 46)
                    txt[tn++] = *sp++;
            }
        }
        txt[tn] = 0;
        if (isf) {
            if (*sp == 'f' || *sp == 'F')
                sp++;
            t.t = T_FNUM;
            t.v = (i64)dec_to_double(txt);
            return t;
        }
        for (int k = 0; k < tn; k++)
            v = v * 10 + (txt[k] - '0');
        t.t = T_NUM;
        t.v = v;
        return t;
    }
    if (c == '\'') {
        sp++;
        i64 v = 0;
        if (*sp == '\\') {
            sp++;
            v = scan_escape();
        } else {
            v = (u8)*sp++;
        }
        if (*sp == '\'') sp++;
        t.t = T_NUM;
        t.v = v;
        return t;
    }
    if (c == '"') {
        sp++;
        u32 start = str_used;
        int len = 0;
        while (*sp && *sp != '"') {
            char ch = *sp++;
            if (ch == '\\') {
                sp--;
                ch = (char)scan_escape();
            }
            add_str_byte((u8)ch);
            len++;
        }
        if (*sp == '"') sp++;
        add_str_byte(0);
        t.t = T_STR;
        t.v = (i64)(TC_STR_BASE + start);
        t.soff = start;
        t.slen = len;
        return t;
    }

    sp++;
    switch (c) {
    case '+':
        if (*sp == '+') { sp++; t.t = T_INC; }
        else if (*sp == '=') { sp++; t.t = T_PLUSEQ; }
        else t.t = '+';
        break;
    case '-':
        if (*sp == '-') { sp++; t.t = T_DEC; }
        else if (*sp == '>') { sp++; t.t = T_ARROW; }
        else if (*sp == '=') { sp++; t.t = T_MINUSEQ; }
        else t.t = '-';
        break;
    case '*':
        if (*sp == '=') { sp++; t.t = T_STAREQ; }
        else t.t = '*';
        break;
    case '/':
        if (*sp == '=') { sp++; t.t = T_SLASHEQ; }
        else t.t = '/';
        break;
    case '%':
        if (*sp == '=') { sp++; t.t = T_PERCENTEQ; }
        else t.t = '%';
        break;
    case '=':
        if (*sp == '=') { sp++; t.t = T_EQ; }
        else t.t = '=';
        break;
    case '!':
        if (*sp == '=') { sp++; t.t = T_NE; }
        else t.t = '!';
        break;
    case '<':
        if (*sp == '<' && sp[1] == '=') { sp += 2; t.t = T_SHLEQ; }
        else if (*sp == '=') { sp++; t.t = T_LE; }
        else if (*sp == '<') { sp++; t.t = T_SHL; }
        else t.t = '<';
        break;
    case '>':
        if (*sp == '>' && sp[1] == '=') { sp += 2; t.t = T_SHREQ; }
        else if (*sp == '=') { sp++; t.t = T_GE; }
        else if (*sp == '>') { sp++; t.t = T_SHR; }
        else t.t = '>';
        break;
    case '&':
        if (*sp == '&') { sp++; t.t = T_ANDAND; }
        else if (*sp == '=') { sp++; t.t = T_ANDEQ; }
        else t.t = '&';
        break;
    case '|':
        if (*sp == '|') { sp++; t.t = T_OROR; }
        else if (*sp == '=') { sp++; t.t = T_OREQ; }
        else t.t = '|';
        break;
    case '^':
        if (*sp == '=') { sp++; t.t = T_XOREQ; }
        else t.t = '^';
        break;
    default:
        t.t = (u8)c;
        break;
    }
    return t;
}

static void advance(void)
{
    cur = nxt;
    nxt = scan();
}

static bool accept(int t)
{
    if (cur.t == t) { advance(); return true; }
    return false;
}

static void expect(int t)
{
    if (!accept(t))
        fail("unexpected token");
}

static bool is_kw(const char *k)
{
    return cur.t == T_ID && strcmp(cur.name, k) == 0;
}

static bool accept_kw(const char *k)
{
    if (is_kw(k)) { advance(); return true; }
    return false;
}

/* ------------------------------------------------------------------ 出码 */
static void e8(u8 b)
{
    if (clen >= TC_CODE_MAX) { fail("program too large"); return; }
    code[clen++] = b;
}

static void e32(u32 v)
{
    e8((u8)v); e8((u8)(v >> 8)); e8((u8)(v >> 16)); e8((u8)(v >> 24));
}

static void e64(u64 v)
{
    e32((u32)v); e32((u32)(v >> 32));
}

static void mov_rax_imm(u64 v)
{
    e8(0x48); e8(0xB8); e64(v);
}

static void mov_rcx_imm(u64 v)
{
    e8(0x48); e8(0xB9); e64(v);
}

static void mov_r8_imm(u64 v)
{
    e8(0x49); e8(0xB8); e64(v);
}

static void push_rax(void) { e8(0x50); }
static void pop_rcx(void)  { e8(0x59); }

/* [rbp + disp] 的 ModRM; reg 为寄存器字段 */
static void modrm_rbp(int disp, u8 reg)
{
    if (disp >= -128 && disp <= 127) {
        e8((u8)(0x40 | (reg << 3) | 5));
        e8((u8)disp);
    } else {
        e8((u8)(0x80 | (reg << 3) | 5));
        e32((u32)disp);
    }
}

static void mov_rax_local(int disp)
{
    e8(0x48); e8(0x8B); modrm_rbp(disp, 0);
}

static void lea_rax_local(int disp)
{
    e8(0x48); e8(0x8D); modrm_rbp(disp, 0);
}

static void lea_rcx_local(int disp)
{
    e8(0x48); e8(0x8D); modrm_rbp(disp, 1);
}

static void lea_rdi_local(int disp)
{
    e8(0x48); e8(0x8D); modrm_rbp(disp, 7);
}

static void test_rax(void) { e8(0x48); e8(0x85); e8(0xC0); }

static void boolify(void)
{
    test_rax();
    e8(0x0F); e8(0x95); e8(0xC0);      /* setne al */
    e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0); /* movzx rax, al */
}

static u32 emit_jcc(u8 cc)
{
    e8(0x0F); e8(cc);
    u32 pos = clen;
    e32(0);
    return pos;
}

static u32 emit_jmp(void)
{
    e8(0xE9);
    u32 pos = clen;
    e32(0);
    return pos;
}

static void patch_rel(u32 pos, u32 target)
{
    i32 rel = (i32)(target - (pos + 4));
    u32 u = (u32)rel;
    code[pos] = (u8)u;
    code[pos + 1] = (u8)(u >> 8);
    code[pos + 2] = (u8)(u >> 16);
    code[pos + 3] = (u8)(u >> 24);
}

/* 用户函数实参: 左到右求值压栈后原地反转, 使 arg0 位于栈顶.
   这样形参固定为 [rbp+16+8*i], 变参从 [rbp+16+8*np] 开始连续. */
static void reverse_args(int n)
{
    for (int i = 0; i < n / 2; i++) {
        int a = i * 8, b = (n - 1 - i) * 8;
        e8(0x48); e8(0x8B); e8(0x44); e8(0x24); e8((u8)a);
        e8(0x48); e8(0x8B); e8(0x4C); e8(0x24); e8((u8)b);
        e8(0x48); e8(0x89); e8(0x4C); e8(0x24); e8((u8)a);
        e8(0x48); e8(0x89); e8(0x44); e8(0x24); e8((u8)b);
    }
}

static void add_rax_imm(i64 v)
{
    if (v >= 0) {
        e8(0x48); e8(0x05); e32((u32)v);           /* add rax, imm32 */
    } else {
        e8(0x48); e8(0x2D); e32((u32)(-v));        /* sub rax, imm32 */
    }
}

static void add_rsp_imm(int n)
{
    if (n <= 127) {
        e8(0x48); e8(0x83); e8(0xC4); e8((u8)n);
    } else {
        e8(0x48); e8(0x81); e8(0xC4); e32((u32)n);
    }
}

static void imul_rax_imm(int v)
{
    e8(0x48); e8(0x69); e8(0xC0); e32((u32)v);
}

static void imul_rcx_imm(int v)
{
    e8(0x48); e8(0x69); e8(0xC9); e32((u32)v);
}

static void mov_rax_rcx(void) { e8(0x48); e8(0x89); e8(0xC8); }
static void mov_rcx_rax(void) { e8(0x48); e8(0x89); e8(0xC1); }
static void mov_rdx_rax(void) { e8(0x48); e8(0x89); e8(0xC2); }
static void mov_rax_rdx(void) { e8(0x48); e8(0x89); e8(0xD0); }
static void mov_r10_rax(void) { e8(0x49); e8(0x89); e8(0xC2); }
static void mov_rax_r10(void) { e8(0x4C); e8(0x89); e8(0xD0); }
static void mov_rcx_rsp(void) { e8(0x48); e8(0x8B); e8(0x0C); e8(0x24); }

/* 以 rax 为地址读写对象 (char 用 1 字节, 其余 8 字节) */
static int value_bytes(const struct ctype *t)
{
    if (t->func)
        return 8;
    if (t->ptr > 0)
        return 8;
    if (t->kind == CK_CHAR)
        return 1;
    if (t->kind == CK_VOID)
        return 0;
    if (t->kind == CK_STRUCT)
        return stypes[t->sid].size;
    return 8;
}

static void load_at(const struct ctype *t)
{
    if (value_bytes(t) == 1) {
        e8(0x48); e8(0x0F); e8(0xB6); e8(0x00);    /* movzx rax, byte [rax] */
    } else {
        e8(0x48); e8(0x8B); e8(0x00);              /* mov rax, [rax] */
    }
}

static void store_at(const struct ctype *t)
{
    if (value_bytes(t) == 1) {
        e8(0x88); e8(0x01);                        /* mov [rcx], al */
    } else {
        e8(0x48); e8(0x89); e8(0x01);              /* mov [rcx], rax */
    }
}

/* 位域: rax 中的值向右移 off 位并按 width 掩码 (signed 时符号扩展) */
static void bit_extract(int off, int width, bool sign)
{
    if (off > 0) {
        e8(0x48); e8(0xC1); e8(0xE8); e8((u8)off);     /* shr rax, imm8 */
    }
    if (width < 64 && width > 0) {
        mov_rcx_imm((1ULL << width) - 1);
        e8(0x48); e8(0x21); e8(0xC8);                  /* and rax, rcx */
        if (sign) {
            int sh = 64 - width;
            e8(0x48); e8(0xC1); e8(0xE0); e8((u8)sh);  /* shl rax, imm8 */
            e8(0x48); e8(0xC1); e8(0xF8); e8((u8)sh);  /* sar rax, imm8 */
        }
    }
}

/* 位域读: 地址在 rax */
static void bit_load(int off, int width, bool sign)
{
    e8(0x48); e8(0x8B); e8(0x00);              /* mov rax, [rax] */
    bit_extract(off, width, sign);
}

/* 位域读改写: rax=新值, rcx=地址; 完成后 rax 仍为新值 */
static void bit_store(int off, int width)
{
    u64 m = (width >= 64 ? ~0ULL : ((1ULL << width) - 1)) << off;
    e8(0x49); e8(0x89); e8(0xC1);              /* mov r9, rax (保存新值) */
    e8(0x48); e8(0x89); e8(0xC2);              /* mov rdx, rax */
    if (off > 0) {
        e8(0x48); e8(0xC1); e8(0xE2); e8((u8)off); /* shl rdx, imm8 */
    }
    e8(0x48); e8(0x8B); e8(0x01);              /* mov rax, [rcx] */
    mov_r8_imm(~m);
    e8(0x4C); e8(0x21); e8(0xC0);              /* and rax, r8 */
    mov_r8_imm(m);
    e8(0x4C); e8(0x21); e8(0xC2);              /* and rdx, r8 */
    e8(0x48); e8(0x09); e8(0xD0);              /* or rax, rdx */
    e8(0x48); e8(0x89); e8(0x01);              /* mov [rcx], rax */
    e8(0x4C); e8(0x89); e8(0xC8);              /* mov rax, r9 */
}

static void zero_local(int disp, int size)
{
    if (size <= 0)
        return;
    lea_rdi_local(disp);
    e8(0x48); e8(0xC7); e8(0xC1); e32((u32)size);  /* mov rcx, imm32 */
    e8(0x31); e8(0xC0);                            /* xor eax, eax */
    e8(0xF3); e8(0xAA);                            /* rep stosb */
}

/* 二元运算: rax=右值, rcx=左值 -> rax=结果 */
static void apply_binop(int op)
{
    switch (op) {
    case '+': e8(0x48); e8(0x01); e8(0xC8); break;          /* add rax, rcx */
    case '-': e8(0x48); e8(0x29); e8(0xC1);                 /* sub rcx, rax */
              e8(0x48); e8(0x89); e8(0xC8); break;          /* mov rax, rcx */
    case '*': e8(0x48); e8(0x0F); e8(0xAF); e8(0xC1); break;/* imul rax, rcx */
    case '/':
    case '%':
        e8(0x48); e8(0x91);                                 /* xchg rax, rcx */
        e8(0x48); e8(0x99);                                 /* cqo */
        e8(0x48); e8(0xF7); e8(0xF9);                       /* idiv rcx */
        if (op == '%') { e8(0x48); e8(0x89); e8(0xD0); }    /* mov rax, rdx */
        break;
    case '&': e8(0x48); e8(0x21); e8(0xC8); break;
    case '|': e8(0x48); e8(0x09); e8(0xC8); break;
    case '^': e8(0x48); e8(0x31); e8(0xC8); break;
    case T_SHL:
        e8(0x48); e8(0x91);                                 /* xchg */
        e8(0x48); e8(0xD3); e8(0xE0); break;                /* shl rax, cl */
    case T_SHR:
        e8(0x48); e8(0x91);
        e8(0x48); e8(0xD3); e8(0xF8); break;                /* sar rax, cl */
    default: break;
    }
}

static void apply_cmp(int op)
{
    e8(0x48); e8(0x39); e8(0xC1);       /* cmp rcx, rax */
    u8 cc = 0x94;
    switch (op) {
    case T_EQ: cc = 0x94; break;
    case T_NE: cc = 0x95; break;
    case '<':  cc = 0x9C; break;
    case T_LE: cc = 0x9E; break;
    case '>':  cc = 0x9F; break;
    case T_GE: cc = 0x9D; break;
    default: break;
    }
    e8(0x0F); e8(cc); e8(0xC0);
    e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0);
}

/* 无符号除法/取模/右移 */
static void apply_binop_uns(int op)
{
    switch (op) {
    case '/':
    case '%':
        e8(0x48); e8(0x91);                 /* xchg rax, rcx */
        e8(0x31); e8(0xD2);                 /* xor edx, edx */
        e8(0x48); e8(0xF7); e8(0xF1);       /* div rcx */
        if (op == '%') { e8(0x48); e8(0x89); e8(0xD0); }
        break;
    case T_SHR:
        e8(0x48); e8(0x91);                 /* xchg */
        e8(0x48); e8(0xD3); e8(0xE8);       /* shr rax, cl */
        break;
    default:
        apply_binop(op);
        break;
    }
}

/* 无符号比较 (setb/setbe/seta/setae) */
static void apply_cmp_uns(int op)
{
    e8(0x48); e8(0x39); e8(0xC1);       /* cmp rcx, rax */
    u8 cc = 0x94;
    switch (op) {
    case T_EQ: cc = 0x94; break;
    case T_NE: cc = 0x95; break;
    case '<':  cc = 0x92; break;
    case T_LE: cc = 0x96; break;
    case '>':  cc = 0x97; break;
    case T_GE: cc = 0x93; break;
    default: break;
    }
    e8(0x0F); e8(cc); e8(0xC0);
    e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0);
}

/* ------------------------------------------------- 优化辅助 (O1..O4) */
static bool fits_i32(i64 v)
{
    return v >= -2147483648LL && v <= 2147483647LL;
}

static bool is_pow2(i64 v, int *sh)
{
    if (v <= 0)
        return false;
    if ((v & (v - 1)) != 0)
        return false;
    int s = 0;
    while ((1LL << s) < v)
        s++;
    *sh = s;
    return true;
}

static void and_rax_imm(i64 v) { e8(0x48); e8(0x25); e32((u32)v); }
static void or_rax_imm(i64 v)  { e8(0x48); e8(0x0D); e32((u32)v); }
static void xor_rax_imm(i64 v) { e8(0x48); e8(0x35); e32((u32)v); }

static void shift_rax_imm(bool left, bool uns, int k)
{
    e8(0x48); e8(0xC1);
    e8(left ? 0xE0 : (uns ? 0xE8 : 0xF8));
    e8((u8)k);
}

static void cmp_rax_imm(i64 v) { e8(0x48); e8(0x3D); e32((u32)v); }

static void setcc_al(u8 cc)
{
    e8(0x0F); e8(cc); e8(0xC0);
    e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0);
}

static u8 cmp_cc(int op, bool uns)
{
    switch (op) {
    case T_EQ: return 0x94;
    case T_NE: return 0x95;
    case '<':  return uns ? 0x92 : 0x9C;
    case T_LE: return uns ? 0x96 : 0x9E;
    case '>':  return uns ? 0x97 : 0x9F;
    case T_GE: return uns ? 0x93 : 0x9D;
    default:   return 0x94;
    }
}

/* 两个整数常量的二元折叠; 不支持或除零时返回 false */
static bool fold_bin(int op, i64 a, i64 b, bool uns, i64 *out)
{
    u64 ua = (u64)a, ub = (u64)b;
    switch (op) {
    case '+': *out = (i64)(ua + ub); return true;
    case '-': *out = (i64)(ua - ub); return true;
    case '*': *out = (i64)(ua * ub); return true;
    case '/':
        if (ub == 0) return false;
        *out = uns ? (i64)(ua / ub) : (i64)(a / b);
        return true;
    case '%':
        if (ub == 0) return false;
        *out = uns ? (i64)(ua % ub) : (i64)(a % b);
        return true;
    case '&': *out = (i64)(ua & ub); return true;
    case '|': *out = (i64)(ua | ub); return true;
    case '^': *out = (i64)(ua ^ ub); return true;
    case T_SHL: *out = (i64)(ua << (ub & 63)); return true;
    case T_SHR: *out = uns ? (i64)(ua >> (ub & 63)) : (i64)(a >> (ub & 63));
        return true;
    case T_EQ: *out = (a == b); return true;
    case T_NE: *out = (a != b); return true;
    case '<':  *out = uns ? (ua < ub) : (a < b); return true;
    case T_LE: *out = uns ? (ua <= ub) : (a <= b); return true;
    case '>':  *out = uns ? (ua > ub) : (a > b); return true;
    case T_GE: *out = uns ? (ua >= ub) : (a >= b); return true;
    default: return false;
    }
}

/* 立即数右操作数: 进入时 rax=lhs; 成功则结果在 rax 并填好 g_e */
static bool gen_imm_op(int op, i64 k, const struct ctype *lt,
                       const bool cmpop, bool uns)
{
    int sh;
    switch (op) {
    case '+':
        if (k != 0) {
            if (!fits_i32(k)) return false;
            add_rax_imm(k);
        }
        break;
    case '-':
        if (k != 0) {
            if (!fits_i32(k) || k == -2147483648LL) return false;
            add_rax_imm(-k);
        }
        break;
    case '*':
        if (k == 0) {
            e8(0x31); e8(0xC0);             /* xor eax, eax */
        } else if (k == 1) {
            ;
        } else if (opt_strength && is_pow2(k, &sh)) {
            shift_rax_imm(true, false, sh);
        } else {
            if (!fits_i32(k)) return false;
            imul_rax_imm((int)k);
        }
        break;
    case '&':
        if (k == -1) break;                 /* x & -1 = x */
        if (!fits_i32(k)) return false;
        and_rax_imm(k);
        break;
    case '|':
        if (k == 0) break;
        if (!fits_i32(k)) return false;
        or_rax_imm(k);
        break;
    case '^':
        if (k == 0) break;
        if (!fits_i32(k)) return false;
        xor_rax_imm(k);
        break;
    case T_SHL:
        if (k < 0 || k > 63) return false;
        shift_rax_imm(true, false, (int)k);
        break;
    case T_SHR:
        if (k < 0 || k > 63) return false;
        shift_rax_imm(false, lt->uns, (int)k);
        break;
    case '/':
        if (!(opt_strength && lt->uns && is_pow2(k, &sh)))
            return false;
        shift_rax_imm(false, true, sh);
        break;
    case '%':
        if (!(opt_strength && lt->uns && is_pow2(k, &sh)))
            return false;
        and_rax_imm(k - 1);
        break;
    default:
        if (cmpop) {
            if (!fits_i32(k)) return false;
            cmp_rax_imm(k);
            setcc_al(cmp_cc(op, uns));
            break;
        }
        return false;
    }
    memset(&g_e, 0, sizeof(g_e));
    g_e.t.kind = CK_INT;
    g_e.t.obj = 8;
    g_e.t.uns = cmpop ? 0 :
        ((op == '/' || op == '%' || op == T_SHR) ? (uns ? 1 : 0)
                                                 : (lt->uns ? 1 : 0));
    g_e.lval = false;
    return true;
}

/* ------------------------------------------------------------ 浮点 (SSE2) */
static bool is_dbl(const struct ctype *t)
{
    return t->kind == CK_DOUBLE && t->ptr == 0 && !t->func;
}

static void dbl_movq_rcx(void) { e8(0x66); e8(0x48); e8(0x0F); e8(0x6E); e8(0xC1); }
static void dbl_movq_rax(void) { e8(0x66); e8(0x48); e8(0x0F); e8(0x6E); e8(0xC8); }
static void dbl_movq_out(void) { e8(0x66); e8(0x48); e8(0x0F); e8(0x7E); e8(0xC0); }
static void dbl_cvtsi_rcx(void) { e8(0xF2); e8(0x48); e8(0x0F); e8(0x2A); e8(0xC1); }
static void dbl_cvtsi_rax(void) { e8(0xF2); e8(0x48); e8(0x0F); e8(0x2A); e8(0xC8); }
static void dbl_cvtsi_rax_in(void) { e8(0xF2); e8(0x48); e8(0x0F); e8(0x2A); e8(0xC0); }

/* 栈上操作数: rcx = 左, rax = 右 -> xmm0/xmm1 */
static void dbl_operands(bool lf, bool rf)
{
    if (lf) dbl_movq_rcx(); else dbl_cvtsi_rcx();
    if (rf) dbl_movq_rax(); else dbl_cvtsi_rax();
}

static void dbl_binop(int op)
{
    e8(0xF2); e8(0x0F);
    switch (op) {
    case '+': e8(0x58); break;              /* addsd */
    case '-': e8(0x5C); break;              /* subsd */
    case '*': e8(0x59); break;              /* mulsd */
    default:  e8(0x5E); break;              /* divsd */
    }
    e8(0xC1);                               /* xmm1 */
    dbl_movq_out();
}

static void dbl_cmp(int op)
{
    e8(0x66); e8(0x0F); e8(0x2E); e8(0xC1); /* ucomisd xmm0, xmm1 */
    u8 cc = 0x94;
    switch (op) {
    case T_EQ: cc = 0x94; break;            /* sete  */
    case T_NE: cc = 0x95; break;            /* setne */
    case '<':  cc = 0x92; break;            /* setb  */
    case T_LE: cc = 0x96; break;            /* setbe */
    case '>':  cc = 0x97; break;            /* seta  */
    case T_GE: cc = 0x93; break;            /* setae */
    default: break;
    }
    e8(0x0F); e8(cc); e8(0xC0);
    e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0);
}

/* rax 中的整数 -> rax 中的 double 位模式 */
static void dbl_from_int(void)
{
    dbl_cvtsi_rax_in();                     /* cvtsi2sd xmm0, rax */
    dbl_movq_out();                         /* movq rax, xmm0 */
}

/* rax 中的 double 位模式 -> rax 中的整数 (截断) */
static void dbl_to_int(void)
{
    e8(0x66); e8(0x48); e8(0x0F); e8(0x6E); e8(0xC0); /* movq xmm0, rax */
    e8(0xF2); e8(0x48); e8(0x0F); e8(0x2C); e8(0xC0); /* cvttsd2si rax, xmm0 */
}

/* rax += 1.0 / -= 1.0 (double) */
static void dbl_add_one(bool sub)
{
    e8(0x66); e8(0x48); e8(0x0F); e8(0x6E); e8(0xC0); /* movq xmm0, rax */
    mov_rax_imm(0x3FF0000000000000ULL);
    dbl_movq_rax();                                   /* movq xmm1, rax */
    e8(0xF2); e8(0x0F); e8(sub ? 0x5C : 0x58); e8(0xC1);
    dbl_movq_out();
}

/* ------------------------------------------------------------ 类型辅助 */
static int ptr_step(const struct ctype *t)
{
    if (t->ptr > 1)
        return 8;
    if (t->kind == CK_STRUCT) {
        int sz = stypes[t->sid].size;
        return sz > 0 ? sz : 1;
    }
    return t->obj > 0 ? t->obj : 1;
}

static int inc_step(const struct ctype *t)
{
    if (t->ptr > 0)
        return ptr_step(t);
    return 1;                       /* 整数/字符 ++ 每次加 1 */
}

static int align_up(int v, int a)
{
    return (v + a - 1) & ~(a - 1);
}

static int find_struct(const char *name)
{
    for (int i = 0; i < nstypes; i++)
        if (!strcmp(stypes[i].name, name))
            return i;
    return -1;
}

static int add_struct(const char *name, bool is_union)
{
    int sid = find_struct(name);
    if (sid >= 0)
        return sid;
    if (nstypes >= MAX_STRUCT) { fail("too many struct types"); return 0; }
    struct stype *st = &stypes[nstypes];
    memset(st, 0, sizeof(*st));
    strncpy(st->name, name, 31);
    st->is_union = is_union;
    return nstypes++;
}

static struct field *find_field(int sid, const char *name)
{
    if (sid < 0 || sid >= nstypes)
        return 0;
    struct stype *st = &stypes[sid];
    for (int i = 0; i < st->nf; i++)
        if (!strcmp(st->f[i].name, name))
            return &st->f[i];
    return 0;
}

/* ---------------------------------------------------------- typedef/enum */
static int find_typedef(const char *name)
{
    for (int i = 0; i < ntypedefs; i++)
        if (!strcmp(typedefs[i].name, name))
            return i;
    return -1;
}

static bool is_typedef_name(const char *name)
{
    return find_typedef(name) >= 0;
}

static bool find_enum(const char *name, i64 *out)
{
    for (int i = 0; i < nenums; i++) {
        if (!strcmp(enums[i].name, name)) {
            if (out) *out = enums[i].val;
            return true;
        }
    }
    return false;
}

static void add_enum(const char *name, i64 val)
{
    if (nenums >= MAX_ENUM) { fail("too many enum constants"); return; }
    strncpy(enums[nenums].name, name, 31);
    enums[nenums].name[31] = 0;
    enums[nenums].val = val;
    nenums++;
}

static void add_typedef(const char *name, struct ctype t)
{
    if (ntypedefs >= MAX_TYPEDEF) { fail("too many typedefs"); return; }
    strncpy(typedefs[ntypedefs].name, name, 31);
    typedefs[ntypedefs].name[31] = 0;
    typedefs[ntypedefs].type = t;
    ntypedefs++;
}

/* 当前 token 是否为一个类型起始 (供 sizeof / 强制转换区分括号表达式) */
static bool tok_is_type(const struct token *t)
{
    if (t->t != T_ID)
        return false;
    const char *n = t->name;
    return !strcmp(n, "int") || !strcmp(n, "char") || !strcmp(n, "void") ||
           !strcmp(n, "float") || !strcmp(n, "double") ||
           !strcmp(n, "struct") || !strcmp(n, "union") ||
           !strcmp(n, "const") || !strcmp(n, "unsigned") ||
           !strcmp(n, "signed") || !strcmp(n, "long") || !strcmp(n, "short") ||
           !strcmp(n, "static") || !strcmp(n, "volatile") ||
           !strcmp(n, "extern") || !strcmp(n, "inline") ||
           is_typedef_name(n);
}

/* ------------------------------------------------------------------ 符号 */
static struct sym *find_sym(const char *name)
{
    for (int i = 0; i < nsyms; i++)
        if (syms[i].kind != SK_LOCAL && !strcmp(syms[i].name, name))
            return &syms[i];
    return 0;
}

static struct sym *find_local(const char *name)
{
    for (int i = 0; i < nloc; i++)
        if (!strcmp(locs[i].name, name))
            return &locs[i];
    return 0;
}

static struct sym *add_func(const char *name)
{
    struct sym *s = find_sym(name);
    if (s)
        return s;
    if (nsyms >= MAX_SYM) { fail("too many symbols"); return 0; }
    s = &syms[nsyms++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 31);
    s->kind = SK_FUNC;
    return s;
}

static int alloc_local(int size)
{
    if (size <= 0)
        size = 8;
    lused += size;
    lused = align_up(lused, 8);
    if (lused > 0x10000) { fail("too many locals"); return 0; }
    return -lused;
}

static i64 alloc_global(int size)
{
    if (size <= 0)
        size = 8;
    gused = align_up(gused, 8);
    if (gused + size > TC_DATA_GLOB) {
        fail("global data too large");
        return TC_GLOB_BASE;
    }
    i64 a = TC_GLOB_BASE + gused;
    gused += size;
    return a;
}

static struct sym *new_local(const struct decl *d, int disp)
{
    if (nloc >= MAX_LOCAL) { fail("too many locals"); return 0; }
    struct sym *s = &locs[nloc++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, d->name, 31);
    s->kind = SK_LOCAL;
    s->offset = disp;
    s->type = d->type;
    s->ndims = d->ndims;
    for (int i = 0; i < d->ndims; i++)
        s->dims[i] = d->dims[i];
    s->nelem = d->nelem;
    s->size = d->ndims ? d->nelem * value_bytes(&d->type)
                       : value_bytes(&d->type);
    return s;
}

static struct sym *new_global(const struct decl *d)
{
    struct sym *s = find_sym(d->name);
    if (s && s->kind == SK_GLOBAL)
        return s;
    if (nsyms >= MAX_SYM) { fail("too many symbols"); return 0; }
    s = &syms[nsyms++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, d->name, 31);
    s->kind = SK_GLOBAL;
    s->type = d->type;
    s->ndims = d->ndims;
    for (int i = 0; i < d->ndims; i++)
        s->dims[i] = d->dims[i];
    s->nelem = d->nelem;
    s->size = d->ndims ? d->nelem * value_bytes(&d->type)
                       : value_bytes(&d->type);
    s->addr = alloc_global(s->size);
    return s;
}

static struct sym *lookup_var(const char *name)
{
    struct sym *s = find_local(name);
    if (s)
        return s;
    s = find_sym(name);
    if (s && s->kind == SK_GLOBAL)
        return s;
    return 0;
}

static void gen_sym_addr(struct sym *s)
{
    if (s->kind == SK_LOCAL)
        lea_rax_local(s->offset);
    else
        mov_rax_imm((u64)s->addr);
}

/* ------------------------------------------------------------ 常量表达式 */
static i64 c_cond(void);

static i64 c_unary(void)
{
    if (accept('-'))
        return -c_unary();
    if (accept('+'))
        return c_unary();
    if (accept('~'))
        return ~c_unary();
    if (accept('!'))
        return !c_unary();
    if (accept('(')) {
        i64 v = c_cond();
        expect(')');
        return v;
    }
    if (cur.t == T_NUM || cur.t == T_STR || cur.t == T_FNUM) {
        i64 v = cur.v;
        advance();
        return v;
    }
    if (cur.t == T_ID) {
        i64 v;
        if (find_enum(cur.name, &v)) {
            advance();
            return v;
        }
        struct sym *fs = find_sym(cur.name);
        if (fs && fs->kind == SK_FUNC) {
            advance();
            return (i64)(TC_PROG_BASE + (u64)fs->addr);
        }
    }
    fail("constant expected");
    return 0;
}

static i64 c_mul(void)
{
    i64 v = c_unary();
    for (;;) {
        int op = cur.t;
        if (op != '*' && op != '/' && op != '%')
            break;
        advance();
        i64 r = c_unary();
        if ((op == '/' || op == '%') && r == 0) { fail("divide by zero"); return 0; }
        if (op == '*') v *= r;
        else if (op == '/') v /= r;
        else v %= r;
    }
    return v;
}

static i64 c_add(void)
{
    i64 v = c_mul();
    for (;;) {
        int op = cur.t;
        if (op != '+' && op != '-')
            break;
        advance();
        i64 r = c_mul();
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

static i64 c_shift(void)
{
    i64 v = c_add();
    for (;;) {
        int op = cur.t;
        if (op != T_SHL && op != T_SHR)
            break;
        advance();
        i64 r = c_add();
        v = (op == T_SHL) ? (i64)((u64)v << (r & 63))
                          : (i64)((u64)v >> (r & 63));
    }
    return v;
}

static i64 c_rel(void)
{
    i64 v = c_shift();
    for (;;) {
        int op = cur.t;
        if (op != '<' && op != '>' && op != T_LE && op != T_GE)
            break;
        advance();
        i64 r = c_shift();
        if (op == '<') v = v < r;
        else if (op == '>') v = v > r;
        else if (op == T_LE) v = v <= r;
        else v = v >= r;
    }
    return v;
}

static i64 c_eq(void)
{
    i64 v = c_rel();
    for (;;) {
        int op = cur.t;
        if (op != T_EQ && op != T_NE)
            break;
        advance();
        i64 r = c_rel();
        v = (op == T_EQ) ? (v == r) : (v != r);
    }
    return v;
}

static i64 c_band(void)
{
    i64 v = c_eq();
    while (cur.t == '&') { advance(); v &= c_eq(); }
    return v;
}

static i64 c_bxor(void)
{
    i64 v = c_band();
    while (cur.t == '^') { advance(); v ^= c_band(); }
    return v;
}

static i64 c_bor(void)
{
    i64 v = c_bxor();
    while (cur.t == '|') { advance(); v |= c_bxor(); }
    return v;
}

static i64 c_land(void)
{
    i64 v = c_bor();
    while (cur.t == T_ANDAND) { advance(); v = (v && c_bor()) ? 1 : 0; }
    return v;
}

static i64 c_lor(void)
{
    i64 v = c_land();
    while (cur.t == T_OROR) { advance(); v = (v || c_land()) ? 1 : 0; }
    return v;
}

static i64 c_cond(void)
{
    i64 v = c_lor();
    if (accept('?')) {
        i64 a = c_cond();
        expect(':');
        i64 b = c_cond();
        return v ? a : b;
    }
    return v;
}

/* ------------------------------------------------------------ 表达式生成 */
static void gen_expr(int minp);
static void gen_unary(void);

static void mat_value(void)
{
    if (g_e.is_const) {
        mov_rax_imm((u64)g_e.cval);
        g_e.is_const = false;
        return;
    }
    if (g_e.is_arr) {
        /* 数组退化为指针: 首地址已在 rax */
        struct ctype p = g_e.t;
        p.ptr = 1;
        p.obj = (g_e.dims[0] > 0) ? (g_e.nelem / g_e.dims[0]) * g_e.esize
                                  : g_e.esize;
        g_e.t = p;
        g_e.is_arr = false;
        g_e.lval = false;
        return;
    }
    if (g_e.lval) {
        if (g_e.t.ptr == 0 && g_e.t.kind == CK_STRUCT && !g_e.t.func) {
            fail("struct value not supported (use a field)");
            return;
        }
        if (g_e.is_bit) {
            bit_load(g_e.bit_off, g_e.bit_width,
                     g_e.t.kind == CK_INT && !g_e.t.uns);
        } else {
            load_at(&g_e.t);
        }
        g_e.lval = false;
        g_e.is_bit = false;
    }
}

static void gen_value(int minp, struct ety *out)
{
    gen_expr(minp);
    mat_value();
    if (out)
        *out = g_e;
}

static int bin_prec(int t)
{
    switch (t) {
    case '*': case '/': case '%': return 11;
    case '+': case '-': return 10;
    case T_SHL: case T_SHR: return 9;
    case '<': case '>': case T_LE: case T_GE: return 8;
    case T_EQ: case T_NE: return 7;
    case '&': return 6;
    case '^': return 5;
    case '|': return 4;
    case T_ANDAND: return 3;
    case T_OROR: return 2;
    default: return -1;
    }
}

static bool is_assign_op(int t)
{
    return t == '=' || t == T_PLUSEQ || t == T_MINUSEQ || t == T_STAREQ ||
           t == T_SLASHEQ || t == T_PERCENTEQ || t == T_ANDEQ || t == T_OREQ ||
           t == T_XOREQ || t == T_SHLEQ || t == T_SHREQ;
}

static int assign_binop(int t)
{
    switch (t) {
    case T_PLUSEQ: return '+';
    case T_MINUSEQ: return '-';
    case T_STAREQ: return '*';
    case T_SLASHEQ: return '/';
    case T_PERCENTEQ: return '%';
    case T_ANDEQ: return '&';
    case T_OREQ: return '|';
    case T_XOREQ: return '^';
    case T_SHLEQ: return T_SHL;
    case T_SHREQ: return T_SHR;
    default: return 0;
    }
}

static void gen_call(const char *name);

/* 前缀/后缀 ++ -- */
static void gen_incdec(bool inc, bool prefix)
{
    int step;
    struct ctype t;
    advance();                      /* 吃掉 ++ / -- */
    gen_unary();
    if (g_e.is_arr) { fail("cannot modify array"); return; }
    if (!g_e.lval) { fail("++/-- requires lvalue"); return; }
    t = g_e.t;
    if (t.ptr == 0 && t.kind == CK_STRUCT && !t.func) {
        fail("++/-- on struct");
        return;
    }
    step = inc_step(&t);
    bool is_bit = g_e.is_bit;
    int bo = g_e.bit_off, bw = g_e.bit_width;

    push_rax();                     /* 地址 */
    if (is_bit)
        bit_load(bo, bw, t.kind == CK_INT && !t.uns);
    else
        load_at(&t);                /* rax = 旧值 */
    if (!prefix) {
        if (is_bit) mov_r10_rax();
        else        mov_rdx_rax();  /* 保存旧值 */
    }
    if (is_dbl(&t))
        dbl_add_one(!inc);
    else
        add_rax_imm(inc ? step : -step);
    mov_rcx_rsp();                  /* 地址 */
    if (is_bit)
        bit_store(bo, bw);
    else
        store_at(&t);
    add_rsp_imm(8);
    if (!prefix) {
        if (is_bit) mov_rax_r10();
        else        mov_rax_rdx();
    }
    g_e.lval = false;
    g_e.is_arr = false;
    g_e.is_bit = false;
    g_e.is_const = false;
    g_e.t = t;
}

/* 间接调用: g_e 中的函数指针值 (rax) 作为被调地址 */
static void gen_indirect_call(void)
{
    expect('(');
    if (g_e.lval) {
        if (g_e.is_bit)
            bit_load(g_e.bit_off, g_e.bit_width, false);
        else
            load_at(&g_e.t);
        g_e.lval = false;
        g_e.is_bit = false;
    }
    push_rax();                         /* 保存函数地址 */
    int nargs = 0;
    if (cur.t != ')') {
        for (;;) {
            gen_value(0, 0);
            push_rax();
            nargs++;
            if (nargs > MAX_ARGS) { fail("too many arguments"); return; }
            if (accept(','))
                continue;
            break;
        }
    }
    expect(')');
    reverse_args(nargs);
    int disp = nargs * 8;
    e8(0x48); e8(0x8B); e8(0x84); e8(0x24); e32((u32)disp); /* mov rax,[rsp+n] */
    e8(0xFF); e8(0xD0);                                     /* call rax */
    add_rsp_imm((nargs + 1) * 8);
    g_e.t.kind = CK_INT;
    g_e.t.ptr = 0;
    g_e.t.func = 0;
    g_e.t.obj = 8;
    g_e.lval = false;
    g_e.is_arr = false;
    g_e.is_bit = false;
}

static void gen_postfix(void)
{
    for (;;) {
        if (cur.t == '(') {
            gen_indirect_call();
            continue;
        }
        if (cur.t == '[') {
            advance();
            int step = 1;
            struct ctype elem = g_e.t;
            bool was_arr = g_e.is_arr;
            int adims[MAX_DIM] = { 0 };
            int andims = 0;
            int anelem = 0, aesize = 0;
            if (was_arr) {
                /* 下标表达式会覆盖 g_e, 先把数组视图完整保存 */
                andims = g_e.ndims;
                for (int i = 0; i < andims; i++)
                    adims[i] = g_e.dims[i];
                anelem = g_e.nelem;
                aesize = g_e.esize;
                int n0 = adims[0] > 0 ? adims[0] : 1;
                int rest = anelem / n0;
                step = rest * aesize;
                if (step <= 0)
                    step = aesize;
            } else {
                mat_value();
                if (g_e.t.ptr == 0) { fail("subscript requires pointer"); return; }
                step = ptr_step(&g_e.t);
                elem = g_e.t;
                elem.ptr--;
                if (elem.ptr == 0 && elem.kind == CK_STRUCT)
                    elem.obj = stypes[elem.sid].size;
            }
            push_rax();                 /* 基址 */
            gen_value(0, 0);            /* 下标 */
            if (step != 1)
                imul_rax_imm(step);
            pop_rcx();
            e8(0x48); e8(0x01); e8(0xC8);   /* add rax, rcx */
            expect(']');

            if (was_arr) {
                if (andims > 1) {
                    for (int i = 1; i < andims; i++)
                        adims[i - 1] = adims[i];
                    andims--;
                    adims[andims] = 0;
                    int n = 1;
                    for (int i = 0; i < andims; i++)
                        n *= adims[i];
                    g_e.ndims = andims;
                    for (int i = 0; i < andims; i++)
                        g_e.dims[i] = adims[i];
                    g_e.nelem = n;
                    g_e.esize = aesize;
                    g_e.t = elem;
                    g_e.is_arr = true;
                    g_e.lval = false;
                } else {
                    g_e.ndims = 0;
                    g_e.t = elem;
                    g_e.is_arr = false;
                    g_e.lval = true;
                }
            } else {
                g_e.t = elem;
                g_e.lval = true;
                g_e.is_arr = false;
            }
            continue;
        }
        if (cur.t == '.') {
            advance();
            if (!g_e.lval || g_e.is_arr || g_e.t.ptr != 0 ||
                g_e.t.kind != CK_STRUCT) {
                fail(". requires struct lvalue");
                return;
            }
            if (cur.t != T_ID) { fail("field name expected"); return; }
            struct field *fd = find_field(g_e.t.sid, cur.name);
            if (!fd) { fail("no such field"); return; }
            advance();
            add_rax_imm(fd->offset);
            g_e.is_bit = false;
            if (fd->bit_width > 0) {
                g_e.t = fd->type;
                g_e.lval = true;
                g_e.is_arr = false;
                g_e.is_bit = true;
                g_e.bit_off = fd->bit_off;
                g_e.bit_width = fd->bit_width;
            } else if (fd->ndims > 0) {
                g_e.is_arr = true;
                g_e.ndims = fd->ndims;
                for (int i = 0; i < fd->ndims; i++)
                    g_e.dims[i] = fd->dims[i];
                g_e.nelem = fd->nelem;
                g_e.esize = value_bytes(&fd->type);
                g_e.t = fd->type;
                g_e.lval = false;
            } else if (fd->type.kind == CK_STRUCT && fd->type.ptr == 0 &&
                       !fd->type.func) {
                g_e.t = fd->type;
                g_e.lval = true;
                g_e.is_arr = false;
            } else {
                g_e.t = fd->type;
                g_e.lval = true;
                g_e.is_arr = false;
            }
            continue;
        }
        if (cur.t == T_ARROW) {
            advance();
            mat_value();
            if (g_e.t.ptr != 1 || g_e.t.kind != CK_STRUCT) {
                fail("-> requires struct pointer");
                return;
            }
            if (cur.t != T_ID) { fail("field name expected"); return; }
            struct field *fd = find_field(g_e.t.sid, cur.name);
            if (!fd) { fail("no such field"); return; }
            advance();
            add_rax_imm(fd->offset);
            g_e.is_bit = false;
            if (fd->bit_width > 0) {
                g_e.t = fd->type;
                g_e.lval = true;
                g_e.is_arr = false;
                g_e.is_bit = true;
                g_e.bit_off = fd->bit_off;
                g_e.bit_width = fd->bit_width;
            } else if (fd->ndims > 0) {
                g_e.is_arr = true;
                g_e.ndims = fd->ndims;
                for (int i = 0; i < fd->ndims; i++)
                    g_e.dims[i] = fd->dims[i];
                g_e.nelem = fd->nelem;
                g_e.esize = value_bytes(&fd->type);
                g_e.t = fd->type;
                g_e.lval = false;
            } else {
                g_e.t = fd->type;
                g_e.lval = true;
                g_e.is_arr = false;
            }
            continue;
        }
        if (cur.t == T_INC || cur.t == T_DEC) {
            bool inc = (cur.t == T_INC);
            if (g_e.is_arr || !g_e.lval) { fail("++/-- requires lvalue"); return; }
            struct ctype t = g_e.t;
            if (t.ptr == 0 && t.kind == CK_STRUCT && !t.func) {
                fail("++/-- on struct");
                return;
            }
            bool is_bit = g_e.is_bit;
            int bo = g_e.bit_off, bw = g_e.bit_width;
            int step = inc_step(&t);
            push_rax();                 /* 地址 */
            mov_rcx_rsp();
            mov_rax_rcx();              /* rax = 地址 */
            if (is_bit)
                bit_load(bo, bw, t.kind == CK_INT && !t.uns);
            else
                load_at(&t);            /* rax = 旧值 */
            if (is_bit) mov_r10_rax();
            else        mov_rdx_rax();  /* 保存旧值 */
            if (is_dbl(&t))
                dbl_add_one(!inc);
            else
                add_rax_imm(inc ? step : -step);
            mov_rcx_rsp();
            if (is_bit)
                bit_store(bo, bw);
            else
                store_at(&t);
            add_rsp_imm(8);
            if (is_bit) mov_rax_r10();
            else        mov_rax_rdx();
            g_e.lval = false;
            g_e.is_arr = false;
            g_e.is_bit = false;
            g_e.is_const = false;
            g_e.t = t;
            advance();
            continue;
        }
        break;
    }
}

static void gen_primary(void)
{
    memset(&g_e, 0, sizeof(g_e));

    if (cur.t == T_NUM) {
        if (!opt_fold)
            mov_rax_imm((u64)cur.v);
        g_e.is_const = opt_fold;
        g_e.cval = cur.v;
        g_e.t.kind = CK_INT;
        g_e.t.obj = 8;
        advance();
        return;
    }
    if (cur.t == T_FNUM) {
        if (!opt_fold)
            mov_rax_imm((u64)cur.v);
        g_e.is_const = opt_fold;
        g_e.cval = cur.v;
        g_e.t.kind = CK_DOUBLE;
        g_e.t.obj = 8;
        advance();
        return;
    }
    if (cur.t == T_STR) {
        mov_rax_imm((u64)cur.v);
        g_e.t.kind = CK_CHAR;
        g_e.t.ptr = 1;
        g_e.t.obj = 1;
        advance();
        return;
    }
    if (cur.t == T_ID) {
        char name[32];
        strncpy(name, cur.name, 31);
        name[31] = 0;

        if (nxt.t == '(') {
            struct sym *vs = lookup_var(name);
            if (!(vs && vs->type.func)) {
                gen_call(name);         /* 直接调用 (可延迟解析) */
                return;
            }
            /* 函数指针变量: 按变量求值, 调用由后缀 '(' 处理 */
        }

        i64 ev;
        if (find_enum(name, &ev)) {
            if (!opt_fold)
                mov_rax_imm((u64)ev);
            g_e.is_const = opt_fold;
            g_e.cval = ev;
            g_e.t.kind = CK_INT;
            g_e.t.obj = 8;
            advance();
            return;
        }

        struct sym *fs = find_sym(name);
        if (fs && fs->kind == SK_FUNC) {
            /* 函数名作值: 取代码地址 */
            mov_rax_imm((u64)(TC_PROG_BASE + (u64)fs->addr));
            g_e.t.kind = CK_VOID;
            g_e.t.ptr = 0;
            g_e.t.func = 1;
            g_e.t.obj = 8;
            g_e.lval = false;
            g_e.is_arr = false;
            advance();
            return;
        }

        struct sym *s = lookup_var(name);
        if (!s) { fail("undefined variable"); return; }
        gen_sym_addr(s);
        if (s->ndims > 0) {
            g_e.is_arr = true;
            g_e.ndims = s->ndims;
            for (int i = 0; i < s->ndims; i++)
                g_e.dims[i] = s->dims[i];
            g_e.nelem = s->nelem;
            g_e.esize = value_bytes(&s->type);
            g_e.t = s->type;
            g_e.lval = false;
        } else {
            g_e.t = s->type;
            g_e.lval = true;
            g_e.is_arr = false;
        }
        advance();
        return;
    }
    if (accept('(')) {
        gen_expr(0);
        expect(')');
        return;
    }
    fail("bad expression");
}

static void gen_unary(void)
{
    /* sizeof(type) / sizeof 变量 (编译期常量) */
    if (is_kw("sizeof")) {
        advance();
        bool paren = accept('(');
        i64 szv = 0;
        if (paren && tok_is_type(&cur)) {
            struct ctype t;
            if (!parse_ctype(&t))
                return;
            expect(')');
            szv = value_bytes(&t);
        } else if (cur.t == T_ID &&
                   ((paren && nxt.t == ')') ||
                    (!paren && nxt.t != '[' && nxt.t != '.' &&
                     nxt.t != T_ARROW && nxt.t != '(' &&
                     bin_prec(nxt.t) < 0 && !is_assign_op(nxt.t)))) {
            int sz;
            int ti = find_typedef(cur.name);
            if (ti >= 0) {
                sz = value_bytes(&typedefs[ti].type);
                advance();
            } else {
                struct sym *s = lookup_var(cur.name);
                if (!s) { fail("sizeof unknown name"); return; }
                sz = s->size > 0 ? s->size : value_bytes(&s->type);
                advance();
            }
            if (paren)
                expect(')');
            szv = sz;
        } else {
            /* sizeof 表达式: 只取类型大小, 丢弃生成的代码 */
            u32 save = clen;
            if (paren) gen_expr(0);
            else       gen_unary();
            if (failed)
                return;
            int sz = g_e.is_arr ? g_e.nelem * g_e.esize
                                : value_bytes(&g_e.t);
            if (sz <= 0 && g_e.is_arr)
                sz = g_e.esize;
            clen = save;
            if (paren)
                expect(')');
            szv = sz;
        }
        if (opt_fold) {
            memset(&g_e, 0, sizeof(g_e));
            g_e.is_const = true;
            g_e.cval = szv;
        } else {
            mov_rax_imm((u64)szv);
            memset(&g_e, 0, sizeof(g_e));
        }
        g_e.t.kind = CK_INT;
        g_e.t.obj = 8;
        return;
    }

    /* 强制类型转换: (type)expr */
    if (cur.t == '(' && tok_is_type(&nxt)) {
        advance();
        struct ctype t;
        if (!parse_ctype(&t))
            return;
        expect(')');
        gen_unary();
        bool src_dbl = is_dbl(&g_e.t);
        if (opt_fold && g_e.is_const && !src_dbl && !is_dbl(&t)) {
            i64 v = g_e.cval;
            if (t.ptr == 0 && t.kind == CK_CHAR)
                v = (i64)(u8)v;
            memset(&g_e, 0, sizeof(g_e));
            g_e.is_const = true;
            g_e.cval = v;
            g_e.t = t;
            g_e.t.uns = 0;
            return;
        }
        mat_value();
        if (is_dbl(&t) && !src_dbl)
            dbl_from_int();
        else if (!is_dbl(&t) && src_dbl)
            dbl_to_int();
        if (t.ptr == 0 && t.kind == CK_CHAR) {
            e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0);   /* movzx rax, al */
        }
        g_e.t = t;
        g_e.lval = false;
        g_e.is_arr = false;
        return;
    }

    if (cur.t == T_INC) { gen_incdec(true, true); return; }
    if (cur.t == T_DEC) { gen_incdec(false, true); return; }
    if (accept('-')) {
        gen_unary();
        if (opt_fold && g_e.is_const) {
            if (is_dbl(&g_e.t))
                g_e.cval ^= (i64)0x8000000000000000ULL;
            else
                g_e.cval = (i64)(0 - (u64)g_e.cval);
            g_e.lval = false;
            return;
        }
        mat_value();
        if (is_dbl(&g_e.t)) {
            mov_rcx_imm(0x8000000000000000ULL);
            e8(0x48); e8(0x31); e8(0xC8);       /* xor rax, rcx (翻转符号位) */
            g_e.lval = false;
            return;
        }
        e8(0x48); e8(0xF7); e8(0xD8);       /* neg rax */
        g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.obj = 8;
        g_e.lval = false;
        return;
    }
    if (accept('!')) {
        gen_unary();
        if (opt_fold && g_e.is_const) {
            i64 v = g_e.cval ? 0 : 1;
            memset(&g_e, 0, sizeof(g_e));
            g_e.is_const = true;
            g_e.cval = v;
            g_e.t.kind = CK_INT;
            g_e.t.obj = 8;
            return;
        }
        mat_value();
        test_rax();
        e8(0x0F); e8(0x94); e8(0xC0);       /* sete al */
        e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0);
        g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.obj = 8;
        g_e.lval = false;
        return;
    }
    if (accept('~')) {
        gen_unary();
        if (opt_fold && g_e.is_const && !is_dbl(&g_e.t)) {
            g_e.cval = ~g_e.cval;
            g_e.t.kind = CK_INT;
            g_e.t.ptr = 0;
            g_e.t.obj = 8;
            g_e.lval = false;
            return;
        }
        mat_value();
        e8(0x48); e8(0xF7); e8(0xD0);       /* not rax */
        g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.obj = 8;
        g_e.lval = false;
        return;
    }
    if (accept('+')) {
        gen_unary();
        mat_value();
        return;
    }
    if (accept('&')) {
        gen_unary();
        if (g_e.t.func) {           /* &func == func */
            g_e.lval = false;
            g_e.is_arr = false;
            return;
        }
        if (g_e.is_arr) {
            struct ctype p = g_e.t;
            p.ptr = 1;
            p.obj = g_e.nelem * g_e.esize;
            g_e.t = p;
            g_e.is_arr = false;
            g_e.lval = false;
            return;
        }
        if (!g_e.lval) { fail("& requires lvalue"); return; }
        g_e.t.ptr++;
        g_e.lval = false;
        return;
    }
    if (accept('*')) {
        gen_unary();
        mat_value();
        if (g_e.t.func) {           /* *fp 即函数指示符 */
            g_e.lval = false;
            g_e.is_arr = false;
            return;
        }
        if (g_e.t.ptr == 0) { fail("* requires pointer"); return; }
        g_e.t.ptr--;
        if (g_e.t.ptr == 0 && g_e.t.kind == CK_STRUCT)
            g_e.t.obj = stypes[g_e.t.sid].size;
        g_e.lval = true;
        g_e.is_arr = false;
        return;
    }
    gen_primary();
    gen_postfix();
}

static void gen_expr(int minp)
{
    gen_unary();

    for (;;) {
        if (failed)
            return;

        /* 三目运算 */
        if (minp <= 1 && cur.t == '?') {
            advance();
            mat_value();
            test_rax();
            u32 jz = emit_jcc(0x84);
            struct ety mid;
            gen_value(0, &mid);
            expect(':');
            u32 jend = emit_jmp();
            patch_rel(jz, clen);
            struct ety rhs;
            gen_value(0, &rhs);
            patch_rel(jend, clen);
            g_e.t = (mid.t.ptr > 0) ? mid.t : rhs.t;
            g_e.lval = false;
            g_e.is_arr = false;
            g_e.is_const = false;
            continue;
        }

        /* 赋值 */
        if (minp == 0 && is_assign_op(cur.t)) {
            if (g_e.is_arr || !g_e.lval) {
                fail("assignment requires lvalue");
                return;
            }
            struct ctype lt = g_e.t;
            if (lt.ptr == 0 && lt.kind == CK_STRUCT && !lt.func) {
                fail("struct assignment not supported");
                return;
            }
            bool is_bit = g_e.is_bit;
            int bo = g_e.bit_off, bw = g_e.bit_width;
            int op = cur.t;
            advance();

            push_rax();                    /* 目标地址 */
            if (op != '=') {
                mov_rcx_rsp();
                mov_rax_rcx();
                if (is_bit)
                    bit_load(bo, bw, lt.kind == CK_INT && !lt.uns);
                else
                    load_at(&lt);           /* rax = 旧值 */
                push_rax();                /* 旧值 (左操作数) */
            }
            struct ety rop;
            gen_value(0, &rop);
            if (op != '=') {
                int bop = assign_binop(op);
                bool lf = is_dbl(&lt), rf = is_dbl(&rop.t);
                if (lf || rf) {
                    if (lt.ptr > 0 ||
                        (bop != '+' && bop != '-' && bop != '*' && bop != '/')) {
                        fail("invalid float operator");
                        return;
                    }
                    pop_rcx();             /* 旧值 */
                    dbl_operands(lf, rf);
                    dbl_binop(bop);
                    if (!lf)
                        dbl_to_int();
                } else {
                    if ((bop == '+' || bop == '-') && lt.ptr > 0)
                        imul_rax_imm(ptr_step(&lt));
                    pop_rcx();             /* 旧值 */
                    if (lt.uns) apply_binop_uns(bop);
                    else        apply_binop(bop);
                }
            } else {
                if (is_dbl(&lt) && !is_dbl(&rop.t))
                    dbl_from_int();
                else if (!is_dbl(&lt) && is_dbl(&rop.t))
                    dbl_to_int();
            }
            pop_rcx();                     /* 地址 */
            if (is_bit)
                bit_store(bo, bw);
            else
                store_at(&lt);
            g_e.t = lt;
            g_e.lval = false;
            g_e.is_arr = false;
            g_e.is_bit = false;
            g_e.is_const = false;
            continue;
        }

        int prec = bin_prec(cur.t);
        if (prec < 0 || prec < minp)
            break;

        int op = cur.t;
        bool logic = (op == T_ANDAND || op == T_OROR);
        bool cmpop = (op == T_EQ || op == T_NE || op == '<' || op == '>' ||
                      op == T_LE || op == T_GE);

        bool lc = opt_fold && g_e.is_const;
        i64 lcv = g_e.cval;
        struct ctype lt = g_e.t;
        if (!lc) {
            mat_value();
            lt = g_e.t;
            if (logic) boolify();
            push_rax();
        }

        advance();
        gen_expr(prec + 1);

        bool rc = opt_fold && g_e.is_const;
        i64 rcv = g_e.cval;
        struct ctype rt = g_e.t;
        if (!rc) {
            mat_value();
            rt = g_e.t;
            if (logic) boolify();
        }

        bool both_int = !is_dbl(&lt) && !is_dbl(&rt) &&
                        lt.ptr == 0 && rt.ptr == 0 &&
                        !lt.func && !rt.func;
        bool uns = (lt.uns || rt.uns) && lt.ptr == 0 && rt.ptr == 0;
        if (logic) {
            if (lc) lcv = lcv ? 1 : 0;
            if (rc) rcv = rcv ? 1 : 0;
        }

        /* O1: 两边都是整数常量 -> 直接折叠 */
        if (opt_fold && lc && rc && both_int) {
            i64 v = 0;
            bool ok = true;
            if (logic)
                v = (op == T_ANDAND) ? ((lcv && rcv) ? 1 : 0)
                                     : ((lcv || rcv) ? 1 : 0);
            else
                ok = fold_bin(op, lcv, rcv, uns, &v);
            if (ok) {
                memset(&g_e, 0, sizeof(g_e));
                g_e.is_const = true;
                g_e.cval = v;
                g_e.t.kind = CK_INT;
                g_e.t.obj = 8;
                g_e.t.uns = (cmpop || logic) ? 0 : (uns ? 1 : 0);
                continue;
            }
        }

        /* O1/O2: 常量右操作数 -> 立即数或强度削弱 */
        bool regs_ready = false;
        if (opt_fold && !lc && rc && both_int) {
            pop_rcx();                     /* lhs */
            mov_rax_rcx();
            if (gen_imm_op(op, rcv, &lt, cmpop, uns))
                continue;
            mov_rcx_rax();
            mov_rax_imm((u64)rcv);
            regs_ready = true;
        }

        /* 布置 rcx=lhs, rax=rhs */
        if (!regs_ready) {
            if (lc && rc) {
                mov_rcx_imm((u64)lcv);
                mov_rax_imm((u64)rcv);
            } else if (lc) {
                mov_rcx_imm((u64)lcv);
            } else if (rc) {
                pop_rcx();
                mov_rax_imm((u64)rcv);
            } else {
                pop_rcx();
            }
        }

        if (logic) {
            apply_binop(op == T_ANDAND ? '&' : '|');
            g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.obj = 8;
            g_e.lval = false;
            g_e.is_arr = false;
            g_e.is_const = false;
            g_e.is_bit = false;
            continue;
        }

        if (is_dbl(&lt) || is_dbl(&rt)) {
            if (op != '+' && op != '-' && op != '*' && op != '/' && !cmpop) {
                fail("invalid float operator");
                return;
            }
            dbl_operands(is_dbl(&lt), is_dbl(&rt));
            if (cmpop) {
                dbl_cmp(op);
                g_e.t.kind = CK_INT;
            } else {
                dbl_binop(op);
                g_e.t.kind = CK_DOUBLE;
            }
            g_e.t.ptr = 0;
            g_e.t.func = 0;
            g_e.t.uns = 0;
            g_e.t.obj = 8;
            g_e.lval = false;
            g_e.is_arr = false;
            g_e.is_bit = false;
            g_e.is_const = false;
            continue;
        }

        /* 指针算术: 按元素大小缩放整数侧 */
        if (op == '+' || op == '-') {
            if (lt.ptr > 0 && rt.ptr == 0) {
                int st = ptr_step(&lt);
                if (st != 1)
                    imul_rax_imm(st);
            } else if (op == '+' && lt.ptr == 0 && rt.ptr > 0) {
                int st = ptr_step(&rt);
                if (st != 1)
                    imul_rcx_imm(st);
            }
        }

        if (cmpop) {
            if (uns) apply_cmp_uns(op);
            else     apply_cmp(op);
            g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.obj = 8;
            g_e.t.uns = 0;
        } else if (op == '-' && lt.ptr > 0 && rt.ptr > 0) {
            /* 指针相减: 结果除以元素大小 */
            apply_binop('-');
            int st = ptr_step(&lt);
            if (st > 1) {
                mov_rcx_imm((u64)st);
                e8(0x48); e8(0x99);              /* cqo */
                e8(0x48); e8(0xF7); e8(0xF9);    /* idiv rcx */
            }
            g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.obj = 8;
        } else {
            if (uns) apply_binop_uns(op);
            else     apply_binop(op);
            if ((op == '+' || op == '-') && lt.ptr > 0 && rt.ptr == 0)
                g_e.t = lt;
            else if (op == '+' && lt.ptr == 0 && rt.ptr > 0)
                g_e.t = rt;
            else {
                g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.obj = 8;
                g_e.t.uns = uns ? 1 : 0;
            }
        }
        g_e.lval = false;
        g_e.is_arr = false;
        g_e.is_bit = false;
        g_e.is_const = false;
    }
}

static void gen_call(const char *name)
{
    advance();                          /* ID */
    advance();                          /* '(' */

    /* 变参内置: va_start(ap,last) / va_arg(ap,type) / va_end(ap) */
    if (!strcmp(name, "va_start")) {
        gen_unary();                    /* ap 左值 -> rax = &ap */
        push_rax();
        expect(',');
        gen_unary();                    /* last 左值 -> rax = &last */
        add_rax_imm(8);                 /* 第一个可变参数 */
        pop_rcx();
        e8(0x48); e8(0x89); e8(0x01);   /* mov [rcx], rax */
        expect(')');
        g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.func = 0; g_e.t.obj = 8;
        g_e.lval = false; g_e.is_arr = false; g_e.is_bit = false;
        return;
    }
    if (!strcmp(name, "va_arg")) {
        gen_unary();                    /* ap 左值 -> &ap */
        push_rax();
        expect(',');
        struct ctype vt;
        if (!parse_ctype(&vt))
            return;
        expect(')');
        pop_rcx();                      /* &ap */
        e8(0x48); e8(0x8B); e8(0x01);              /* mov rax,[rcx] */
        e8(0x48); e8(0x8D); e8(0x50); e8(0x08);    /* lea rdx,[rax+8] */
        e8(0x48); e8(0x89); e8(0x11);              /* mov [rcx],rdx */
        load_at(&vt);
        g_e.t = vt;
        g_e.lval = false; g_e.is_arr = false; g_e.is_bit = false;
        return;
    }
    if (!strcmp(name, "va_end")) {
        gen_value(0, 0);
        expect(')');
        g_e.t.kind = CK_INT; g_e.t.ptr = 0; g_e.t.func = 0; g_e.t.obj = 8;
        g_e.lval = false; g_e.is_arr = false; g_e.is_bit = false;
        return;
    }

    int nargs = 0;
    if (cur.t != ')') {
        for (;;) {
            gen_value(0, 0);
            push_rax();
            nargs++;
            if (nargs > MAX_ARGS) { fail("too many arguments"); return; }
            if (accept(','))
                continue;
            break;
        }
    }
    expect(')');

    /* printf: 参数已压栈, 把栈指针交给内核格式化 (SYS_PRINTF) */
    if (!strcmp(name, "printf")) {
        if (nargs < 1) { fail("printf needs a format"); return; }
        e8(0x48); e8(0x89); e8(0xE2);            /* mov rdx, rsp */
        e8(0x48); e8(0xC7); e8(0xC6);
        e32((u32)(nargs - 1));                   /* mov rsi, nargs-1 */
        int disp = (nargs - 1) * 8;
        if (disp <= 127) {
            e8(0x48); e8(0x8B); e8(0x7C); e8(0x24); e8((u8)disp);
        } else {
            e8(0x48); e8(0x8B); e8(0xBC); e8(0x24); e32((u32)disp);
        }
        mov_rax_imm((u64)SYS_PRINTF);
        e8(0xCD); e8(0x21);
        add_rsp_imm(nargs * 8);
        g_e.t.kind = CK_INT;
        g_e.t.ptr = 0;
        g_e.t.obj = 8;
        g_e.lval = false;
        g_e.is_arr = false;
        return;
    }

    struct { const char *n; int nr; int args; } bi[] = {
        { "putc", 1, 1 }, { "print", 11, 1 },
        { "print_int", 20, 1 }, { "getc", 2, 0 }, { "exit", 0, 1 },
        { "sleep", 16, 1 }, { "uptime", 7, 0 },
        { "open", 5, 2 }, { "read", 4, 3 }, { "write", 3, 3 },
        { "close", 6, 1 }, { "seek", 12, 3 },
        { "stat", 13, 2 }, { "statx", 19, 2 },
        { "reboot", 8, 0 }, { "shutdown", 9, 0 },
        { "netinfo", 21, 1 }, { "tcp_connect", 22, 2 },
        { "tcp_send", 23, 3 }, { "tcp_recv", 24, 3 },
        { "tcp_close", 25, 1 }, { "tcp_listen", 26, 1 },
        { "tcp_accept", 27, 1 }, { "dhcp", 28, 0 },
        { "dns", 29, 1 }, { "tftp_get", 30, 3 },
        { "http_get", 31, 3 },
        { "udp_open", 32, 1 }, { "udp_sendto", 33, 5 },
        { "udp_recvfrom", 34, 6 }, { "icmp_err", 35, 1 },
        { "udp_close", 36, 1 },
        { "cls", 38, 0 }, { "color", 39, 2 }, { "screen", 40, 1 },
        { "unlink", 41, 1 }, { "mkdir", 42, 1 }, { "rmdir", 43, 1 },
        { "getcwd", 44, 2 }, { "hostname", 45, 2 }, { "username", 46, 2 },
        { "isadmin", 47, 0 }, { "version", 48, 2 }, { "unixtime", 49, 0 },
        { "rich", 50, 3 },
        { NULL, 0, 0 }
    };
    int nr = -1;
    for (int i = 0; bi[i].n; i++) {
        if (!strcmp(bi[i].n, name)) {
            if (bi[i].args != nargs) { fail("wrong argument count"); return; }
            nr = bi[i].nr;
            break;
        }
    }
    if (!strcmp(name, "ip4")) {
        if (nargs != 4) { fail("ip4 needs 4 arguments"); return; }
        e8(0x5A); e8(0x59); e8(0x5E); e8(0x5F);   /* d,c,b,a */
        e8(0x48); e8(0x89); e8(0xF8);             /* mov rax,rdi */
        e8(0x48); e8(0xC1); e8(0xE0); e8(0x18);   /* shl rax,24 */
        e8(0x49); e8(0x89); e8(0xF0);             /* mov r8,rsi */
        e8(0x49); e8(0xC1); e8(0xE0); e8(0x10);   /* shl r8,16 */
        e8(0x4C); e8(0x09); e8(0xC0);             /* or rax,r8 */
        e8(0x49); e8(0x89); e8(0xC8);             /* mov r8,rcx */
        e8(0x49); e8(0xC1); e8(0xE0); e8(0x08);   /* shl r8,8 */
        e8(0x4C); e8(0x09); e8(0xC0);             /* or rax,r8 */
        e8(0x48); e8(0x09); e8(0xD0);             /* or rax,rdx */
    } else if (nr >= 0) {
        static const u8 regs[MAX_ARGS][2] = {
            { 0x5F, 0x00 }, { 0x5E, 0x00 }, { 0x5A, 0x00 },
            { 0x59, 0x00 }, { 0x41, 0x58 }, { 0x41, 0x59 }
        };
        for (int i = nargs - 1; i >= 0; i--) {
            if (regs[i][0] == 0x41) { e8(0x41); e8(regs[i][1]); }
            else e8(regs[i][0]);
        }
        mov_rax_imm((u64)nr);
        e8(0xCD); e8(0x21);
    } else {
        if (nfc >= MAX_FCALL) { fail("too many calls"); return; }
        reverse_args(nargs);            /* 用户函数: arg0 到栈顶 */
        e8(0xE8);
        fcs[nfc].pos = clen;
        e32(0);
        strncpy(fcs[nfc].name, name, 31);
        fcs[nfc].name[31] = 0;
        nfc++;
        if (nargs)
            add_rsp_imm(nargs * 8);
    }

    g_e.t.kind = CK_INT;
    g_e.t.ptr = 0;
    g_e.t.func = 0;
    g_e.t.obj = 8;
    g_e.lval = false;
    g_e.is_arr = false;
    g_e.is_bit = false;
}

/* ------------------------------------------------------------ 声明解析 */
static bool parse_ctype(struct ctype *t)
{
    memset(t, 0, sizeof(*t));

    bool plain_int = false;
    bool saw_uns = false;
    for (;;) {
        if (accept_kw("unsigned")) { plain_int = true; saw_uns = true; continue; }
        if (accept_kw("signed") || accept_kw("long") || accept_kw("short")) {
            plain_int = true;
            continue;
        }
        if (accept_kw("static") || accept_kw("const") || accept_kw("volatile") ||
            accept_kw("register") || accept_kw("extern") ||
            accept_kw("inline"))
            continue;
        break;
    }

    if (is_kw("struct") || is_kw("union")) {
        bool is_union = (cur.name[1] == 'n');
        advance();
        if (cur.t != T_ID) { fail("struct name expected"); return false; }
        char sname[32];
        strncpy(sname, cur.name, 31);
        sname[31] = 0;
        int sid = add_struct(sname, is_union);
        advance();
        t->kind = CK_STRUCT;
        t->sid = sid;
        t->obj = stypes[sid].complete ? stypes[sid].size : 0;
    } else if (accept_kw("int")) {
        t->kind = CK_INT;
        t->obj = 8;
    } else if (accept_kw("char")) {
        t->kind = CK_CHAR;
        t->obj = 1;
    } else if (accept_kw("void")) {
        t->kind = CK_VOID;
        t->obj = 0;
    } else if (cur.t == T_ID && find_typedef(cur.name) >= 0) {
        *t = typedefs[find_typedef(cur.name)].type;
        advance();
    } else if (is_kw("float") || is_kw("double")) {
        advance();
        t->kind = CK_DOUBLE;
        t->obj = 8;
    } else if (plain_int) {
        t->kind = CK_INT;           /* unsigned / long / short 单独出现 */
        t->obj = 8;
    } else {
        fail(cur.t == T_ID ? cur.name : "type expected");
        return false;
    }

    while (accept('*'))
        t->ptr++;
    if (saw_uns)
        t->uns = 1;
    return true;
}

static bool parse_declarator(struct ctype base, struct decl *d)
{
    memset(d, 0, sizeof(*d));
    d->type = base;
    d->nelem = 1;

    /* 函数指针: (*name)(参数列表) */
    if (cur.t == '(' && nxt.t == '*') {
        advance();                      /* ( */
        while (accept('*'))
            ;
        if (cur.t != T_ID) { fail("expected name"); return false; }
        strncpy(d->name, cur.name, 31);
        d->name[31] = 0;
        advance();
        while (accept('[')) {          /* 函数指针数组: (*ops[N])(...) */
            i64 n = 0;
            bool unsized = false;
            if (cur.t == ']') {
                unsized = true;
            } else {
                n = c_cond();
                if (n < 0) { fail("bad array size"); return false; }
            }
            expect(']');
            if (d->ndims >= MAX_DIM) { fail("too many dimensions"); return false; }
            d->dims[d->ndims++] = (int)n;
            if (unsized)
                d->incomplete = true;
            else if (n > 0)
                d->nelem *= (int)n;
        }
        expect(')');
        if (!accept('(')) { fail("expected parameter list"); return false; }
        if (!accept(')')) {
            int depth = 1;
            while (!failed && cur.t != T_EOF) {
                if (cur.t == '(') depth++;
                else if (cur.t == ')') {
                    depth--;
                    if (depth == 0) { advance(); break; }
                }
                advance();
            }
        }
        d->type.func = 1;
        d->type.ptr = 0;
        d->type.kind = CK_VOID;
        d->type.obj = 8;
        return true;
    }

    while (accept('*'))
        d->type.ptr++;

    if (cur.t != T_ID) { fail("expected name"); return false; }
    strncpy(d->name, cur.name, 31);
    d->name[31] = 0;
    advance();

    while (accept('[')) {
        i64 n = 0;
        bool unsized = false;
        if (cur.t == ']') {
            unsized = true;
        } else {
            n = c_cond();
            if (n < 0) { fail("bad array size"); return false; }
        }
        expect(']');
        if (d->ndims >= MAX_DIM) { fail("too many dimensions"); return false; }
        d->dims[d->ndims++] = (int)n;
        if (unsized) {
            d->incomplete = true;
        } else if (n > 0) {
            d->nelem *= (int)n;
        }
    }
    if (d->incomplete)
        d->nelem = 0;
    return true;
}

static bool decl_validate(const struct decl *d)
{
    if (d->type.kind == CK_VOID && d->type.ptr == 0 && !d->type.func) {
        fail("void variable");
        return false;
    }
    if (d->type.kind == CK_STRUCT && d->type.ptr == 0 &&
        !stypes[d->type.sid].complete) {
        fail("incomplete struct type");
        return false;
    }
    if (value_bytes(&d->type) <= 0) {
        fail("bad variable type");
        return false;
    }
    return true;
}

/* 初始化目标: 全局 (gaddr, 编译期写入) 或局部 (loff, 生成 store) */
static void init_put_scalar(struct ctype t, bool global, i64 gaddr, int loff)
{
    if (global) {
        i64 v = c_cond();
        u32 off = (u32)(gaddr - TC_GLOB_BASE);
        int n = value_bytes(&t);
        for (int i = 0; i < n; i++)
            if (off + (u32)i < sizeof(gmem))
                gmem[off + (u32)i] = (u8)(v >> (i * 8));
    } else {
        gen_value(0, 0);
        lea_rcx_local(loff);
        store_at(&t);
    }
}

static void init_put_byte(bool global, i64 gaddr, int loff, u8 v)
{
    if (global) {
        u32 off = (u32)(gaddr - TC_GLOB_BASE);
        if (off < sizeof(gmem))
            gmem[off] = v;
    } else {
        lea_rcx_local(loff);
        e8(0xC6); e8(0x01); e8(v);           /* mov byte [rcx], imm8 */
    }
}

static void init_object(struct ctype elem, int dims[], int ndims, int nelem,
                        bool global, i64 gaddr, int loff)
{
    int esize = value_bytes(&elem);

    if (ndims > 0) {
        int n0 = dims[0] > 0 ? dims[0] : nelem;
        int stride = esize;
        for (int i = 1; i < ndims; i++)
            stride *= dims[i];

        /* char 数组 = "字符串" */
        if (elem.kind == CK_CHAR && elem.ptr == 0 && ndims == 1 &&
            cur.t == T_STR) {
            int slen = cur.slen;
            u32 soff = cur.soff;
            advance();
            int n = n0 < slen + 1 ? n0 : slen + 1;
            for (int i = 0; i < n; i++) {
                u8 b = (i < slen) ? str_pool[soff + (u32)i] : 0;
                init_put_byte(global, gaddr + i, loff + i, b);
            }
            return;
        }

        /* 多维数组也允许扁平初始化 {1,2,3,4} */
        if (ndims > 1 && cur.t != '{') {
            int total = 1;
            for (int i = 0; i < ndims; i++)
                total *= dims[i];
            for (int i = 0; i < total; i++) {
                init_object(elem, 0, 0, 1, global,
                            gaddr + (i64)i * esize, loff + i * esize);
                if (!accept(','))
                    break;
            }
            return;
        }

        if (!accept('{')) { fail("initializer list expected"); return; }
        for (int i = 0; i < n0; i++) {
            if (cur.t == '}') break;
            init_object(elem, dims + 1, ndims - 1, n0, global,
                        gaddr + (i64)i * stride, loff + i * stride);
            if (!accept(','))
                break;
        }
        expect('}');
        return;
    }

    if (elem.kind == CK_STRUCT && elem.ptr == 0) {
        struct stype *st = &stypes[elem.sid];
        if (!accept('{')) { fail("initializer list expected"); return; }
        for (int i = 0; i < st->nf; i++) {
            if (cur.t == '}') break;
            struct field *fd = &st->f[i];
            init_object(fd->type, fd->dims, fd->ndims,
                        fd->nelem, global, gaddr + fd->offset,
                        loff + fd->offset);
            if (!accept(','))
                break;
        }
        expect('}');
        return;
    }

    init_put_scalar(elem, global, gaddr, loff);
}

/* ---------------------------------------------------------------- 语句 */
static void parse_stmt(void);

static void parse_local_decl(struct ctype base)
{
    for (;;) {
        struct decl d;
        if (!parse_declarator(base, &d))
            return;
        if (cur.t == '(') { fail("nested function"); return; }
        if (!decl_validate(&d))
            return;

        /* 未定长数组: 仅支持 char s[] = "..." */
        if (d.incomplete) {
            if (cur.t == '=' && nxt.t == T_STR) {
                d.nelem = nxt.slen + 1;
                d.dims[d.ndims - 1] = d.nelem;
                d.incomplete = false;
            } else {
                fail("array size unknown");
                return;
            }
        }

        int size = d.ndims ? d.nelem * value_bytes(&d.type)
                           : value_bytes(&d.type);
        if (size <= 0) { fail("bad variable size"); return; }
        int off = alloc_local(size);
        struct sym *s = new_local(&d, off);
        if (!s)
            return;

        if (accept('=')) {
            if (d.ndims || (d.type.kind == CK_STRUCT && d.type.ptr == 0)) {
                zero_local(off, size);
                init_object(d.type, d.dims, d.ndims,
                            d.ndims ? d.nelem : 1, false, 0, off);
            } else {
                gen_value(0, 0);
                lea_rcx_local(off);
                store_at(&d.type);
            }
        } else if (d.ndims || (d.type.kind == CK_STRUCT && d.type.ptr == 0)) {
            zero_local(off, size);
        } else {
            mov_rax_imm(0);
            lea_rcx_local(off);
            store_at(&d.type);
        }

        if (accept(','))
            continue;
        break;
    }
    expect(';');
}

/* typedef (已消费关键字) */
static void parse_typedef(void)
{
    struct ctype base;
    if (!parse_ctype(&base))
        return;
    for (;;) {
        struct ctype t = base;
        if (cur.t == '(' && nxt.t == '*') {
            /* 函数指针 typedef: typedef Ret (*name)(Args); */
            advance();
            while (accept('*'))
                ;
            if (cur.t != T_ID) { fail("typedef name expected"); return; }
            char n[32];
            strncpy(n, cur.name, 31);
            n[31] = 0;
            advance();
            expect(')');
            if (!accept('(')) { fail("expected parameter list"); return; }
            if (!accept(')')) {
                int depth = 1;
                while (!failed && cur.t != T_EOF) {
                    if (cur.t == '(') depth++;
                    else if (cur.t == ')') {
                        depth--;
                        if (depth == 0) { advance(); break; }
                    }
                    advance();
                }
            }
            t.func = 1;
            t.ptr = 0;
            t.kind = CK_VOID;
            t.obj = 8;
            add_typedef(n, t);
            if (accept(','))
                continue;
            break;
        }
        while (accept('*'))
            t.ptr++;
        if (cur.t != T_ID) { fail("typedef name expected"); return; }
        char n[32];
        strncpy(n, cur.name, 31);
        n[31] = 0;
        advance();
        add_typedef(n, t);
        if (accept(','))
            continue;
        break;
    }
    expect(';');
}

/* enum { A, B=5, C }; (已消费关键字, 标签可选) */
static void parse_enum(void)
{
    if (cur.t == T_ID)
        advance();
    if (!accept('{')) { fail("enum body expected"); return; }
    i64 v = 0;
    while (!failed && cur.t != '}' && cur.t != T_EOF) {
        if (cur.t != T_ID) { fail("enum name expected"); return; }
        char n[32];
        strncpy(n, cur.name, 31);
        n[31] = 0;
        advance();
        if (accept('='))
            v = c_cond();
        add_enum(n, v);
        v++;
        if (!accept(','))
            break;
    }
    expect('}');
    accept(';');
}

static struct label_ent *find_label(const char *name)
{
    for (int i = 0; i < nlabels; i++)
        if (!strcmp(labels[i].name, name))
            return &labels[i];
    return 0;
}

static void define_label(const char *name, u32 addr)
{
    struct label_ent *l = find_label(name);
    if (l) {
        if (l->defined) { fail("duplicate label"); return; }
        l->defined = true;
        l->addr = addr;
        return;
    }
    if (nlabels >= MAX_LABEL) { fail("too many labels"); return; }
    strncpy(labels[nlabels].name, name, 31);
    labels[nlabels].name[31] = 0;
    labels[nlabels].addr = addr;
    labels[nlabels].defined = true;
    nlabels++;
}

static void goto_label(const char *name)
{
    if (ngotos >= MAX_LABEL) { fail("too many gotos"); return; }
    u32 pos = emit_jmp();
    strncpy(gotos[ngotos].name, name, 31);
    gotos[ngotos].name[31] = 0;
    gotos[ngotos].pos = pos;
    ngotos++;
}

static void resolve_gotos(void)
{
    for (int i = 0; i < ngotos; i++) {
        struct label_ent *l = find_label(gotos[i].name);
        if (!l || !l->defined) {
            fail("undefined label");
            return;
        }
        patch_rel(gotos[i].pos, l->addr);
    }
}

static void parse_block(void)
{
    expect('{');
    while (!failed && cur.t != '}' && cur.t != T_EOF)
        parse_stmt();
    expect('}');
}

static bool is_type_start(void)
{
    if (cur.t == T_ID && is_typedef_name(cur.name))
        return true;
    return is_kw("int") || is_kw("char") || is_kw("void") ||
           is_kw("float") || is_kw("double") ||
           is_kw("struct") || is_kw("union") || is_kw("static") ||
           is_kw("const") || is_kw("unsigned") || is_kw("signed") ||
           is_kw("long") || is_kw("short") || is_kw("extern") ||
           is_kw("inline");
}

static void parse_switch(void)
{
    expect('(');
    gen_value(0, 0);
    expect(')');

    int tmp = alloc_local(8);
    struct ctype it = { CK_INT, 0, 0, 8, 0, 0 };
    lea_rcx_local(tmp);
    store_at(&it);

    u32 jdisp = emit_jmp();

    if (nloop >= MAX_NEST) { fail("nesting too deep"); return; }
    struct loopctx *lp = &loops[nloop++];
    lp->nbreak = 0;
    lp->ncont = 0;
    lp->cont = 0;
    lp->is_switch = true;

    if (nsw >= MAX_NEST) { fail("switch nesting too deep"); return; }
    struct swctx *sw = &swstk[nsw++];
    sw->tmp = tmp;
    sw->ncase = 0;
    sw->has_def = false;
    sw->def_addr = 0;

    parse_stmt();

    u32 jend = emit_jmp();
    u32 dispatch = clen;

    for (int i = 0; i < sw->ncase; i++) {
        mov_rax_local(tmp);
        mov_rcx_imm((u64)sw->cases[i].val);
        e8(0x48); e8(0x39); e8(0xC8);       /* cmp rax, rcx */
        u32 j = emit_jcc(0x84);             /* je */
        patch_rel(j, sw->cases[i].addr);
    }
    u32 jdef = emit_jmp();
    u32 end = clen;

    patch_rel(jdisp, dispatch);
    patch_rel(jend, end);
    patch_rel(jdef, sw->has_def ? sw->def_addr : end);
    for (int i = 0; i < lp->nbreak; i++)
        patch_rel(lp->breaks[i], end);

    nsw--;
    nloop--;
}

static void skip_parens(void)
{
    int depth = 0;
    while (cur.t != T_EOF) {
        if (cur.t == '(') depth++;
        else if (cur.t == ')') {
            depth--;
            if (depth == 0) { advance(); break; }
        }
        advance();
    }
}

/* 跳过一条语句 (不进行语义/代码生成), 用于常量分支/循环消除 */
static void skip_stmt(void)
{
    if (cur.t == T_EOF)
        return;
    if (cur.t == '{') {
        int depth = 0;
        while (cur.t != T_EOF) {
            if (cur.t == '{') depth++;
            else if (cur.t == '}') {
                depth--;
                if (depth == 0) { advance(); break; }
            }
            advance();
        }
        return;
    }
    if (accept_kw("if")) {
        if (cur.t == '(') skip_parens();
        skip_stmt();
        if (accept_kw("else"))
            skip_stmt();
        return;
    }
    if (is_kw("while") || is_kw("for") || is_kw("switch")) {
        advance();
        if (cur.t == '(') skip_parens();
        skip_stmt();
        return;
    }
    if (accept_kw("do")) {
        skip_stmt();
        if (accept_kw("while")) {
            if (cur.t == '(') skip_parens();
            accept(';');
        }
        return;
    }
    int depth = 0;
    while (cur.t != T_EOF) {
        if (cur.t == '(' || cur.t == '[') depth++;
        else if (cur.t == ')' || cur.t == ']') {
            if (depth > 0) depth--;
        } else if (cur.t == '}') {
            if (depth == 0) return;
            depth--;
        } else if (cur.t == ';' && depth == 0) {
            advance();
            return;
        }
        advance();
    }
}

static void parse_stmt(void)
{
    if (failed)
        return;

    if (cur.t == '{') {
        parse_block();
        return;
    }
    if (accept(';'))
        return;

    /* 标签: ID ':' (函数作用域, goto 可前向/后向跳转) */
    if (cur.t == T_ID && nxt.t == ':') {
        char n[32];
        strncpy(n, cur.name, 31);
        n[31] = 0;
        advance();
        advance();
        define_label(n, clen);
        return;
    }
    if (accept_kw("goto")) {
        if (cur.t != T_ID) { fail("label expected"); return; }
        char n[32];
        strncpy(n, cur.name, 31);
        n[31] = 0;
        advance();
        goto_label(n);
        expect(';');
        return;
    }

    if (accept_kw("typedef")) {
        parse_typedef();
        return;
    }
    if (is_kw("enum")) {
        advance();
        parse_enum();
        return;
    }

    if (is_type_start()) {
        struct ctype base;
        if (parse_ctype(&base))
            parse_local_decl(base);
        return;
    }
    if (accept_kw("if")) {
        expect('(');
        u32 csave = clen;
        gen_expr(0);
        bool cst = g_e.is_const;
        i64 cv = g_e.cval;
        if (!cst)
            mat_value();
        expect(')');
        if (opt_dce && cst) {
            clen = csave;                  /* 丢弃条件代码 */
            bool take = (cv != 0);
            if (take) parse_stmt(); else skip_stmt();
            if (accept_kw("else")) {
                if (take) skip_stmt(); else parse_stmt();
            }
            return;
        }
        if (cst)
            mov_rax_imm((u64)cv);
        test_rax();
        u32 jz = emit_jcc(0x84);
        parse_stmt();
        if (accept_kw("else")) {
            u32 jmp = emit_jmp();
            patch_rel(jz, clen);
            parse_stmt();
            patch_rel(jmp, clen);
        } else {
            patch_rel(jz, clen);
        }
        return;
    }
    if (accept_kw("while")) {
        expect('(');
        u32 csave = clen;
        gen_expr(0);
        bool cst = g_e.is_const;
        i64 cv = g_e.cval;
        if (!cst)
            mat_value();
        expect(')');
        if (opt_loops && cst) {
            clen = csave;
            if (cv == 0) {             /* while (0): 整段删除 */
                skip_stmt();
                return;
            }
            u32 start = clen;          /* while (1): 只保留回跳 */
            if (nloop >= MAX_NEST) { fail("loop nesting too deep"); return; }
            struct loopctx *lp = &loops[nloop++];
            lp->nbreak = 0;
            lp->ncont = 0;
            lp->cont = start;
            lp->is_switch = false;
            parse_stmt();
            u32 back = emit_jmp();
            patch_rel(back, start);
            for (int i = 0; i < lp->nbreak; i++)
                patch_rel(lp->breaks[i], clen);
            for (int i = 0; i < lp->ncont; i++)
                patch_rel(lp->conts[i], start);
            nloop--;
            return;
        }
        u32 start = csave;
        if (cst)
            mov_rax_imm((u64)cv);
        test_rax();
        u32 jz = emit_jcc(0x84);
        if (nloop >= MAX_NEST) { fail("loop nesting too deep"); return; }
        struct loopctx *lp = &loops[nloop++];
        lp->nbreak = 0;
        lp->ncont = 0;
        lp->cont = start;
        lp->is_switch = false;
        parse_stmt();
        u32 back = emit_jmp();
        patch_rel(back, start);
        patch_rel(jz, clen);
        for (int i = 0; i < lp->nbreak; i++)
            patch_rel(lp->breaks[i], clen);
        for (int i = 0; i < lp->ncont; i++)
            patch_rel(lp->conts[i], start);
        nloop--;
        return;
    }
    if (accept_kw("for")) {
        expect('(');
        if (!accept(';')) {
            if (is_type_start()) {
                struct ctype base;
                if (!parse_ctype(&base)) return;
                /* 声明自带分号 */
                parse_local_decl(base);
            } else {
                gen_value(0, 0);
                expect(';');
            }
        }
        u32 cond = clen;
        u32 jz = 0;
        bool has_cond = false;
        if (cur.t != ';') {
            gen_value(0, 0);
            test_rax();
            jz = emit_jcc(0x84);
            has_cond = true;
        }
        expect(';');
        u32 jbody = emit_jmp();
        u32 step = clen;
        if (cur.t != ')')
            gen_value(0, 0);
        expect(')');
        u32 back = emit_jmp();
        patch_rel(back, cond);
        patch_rel(jbody, clen);

        if (nloop >= MAX_NEST) { fail("loop nesting too deep"); return; }
        struct loopctx *lp = &loops[nloop++];
        lp->nbreak = 0;
        lp->ncont = 0;
        lp->cont = step;
        lp->is_switch = false;
        parse_stmt();
        u32 to_step = emit_jmp();
        patch_rel(to_step, step);
        for (int i = 0; i < lp->nbreak; i++)
            patch_rel(lp->breaks[i], clen);
        for (int i = 0; i < lp->ncont; i++)
            patch_rel(lp->conts[i], step);
        if (has_cond)
            patch_rel(jz, clen);
        nloop--;
        return;
    }
    if (accept_kw("do")) {
        u32 body = clen;
        if (nloop >= MAX_NEST) { fail("loop nesting too deep"); return; }
        struct loopctx *lp = &loops[nloop++];
        lp->nbreak = 0;
        lp->ncont = 0;
        lp->cont = 0;
        lp->is_switch = false;
        parse_stmt();
        u32 cond = clen;
        lp->cont = cond;
        if (!accept_kw("while")) { fail("expected while"); nloop--; return; }
        expect('(');
        gen_expr(0);
        bool cconst = opt_loops && g_e.is_const;
        i64 cval = g_e.cval;
        if (g_e.is_const && !cconst)
            mov_rax_imm((u64)cval);
        if (!g_e.is_const)
            mat_value();
        expect(')');
        if (cconst) {
            clen = cond;                   /* 丢弃常量条件代码 */
            if (cval != 0) {               /* do..while(1): 只保留回跳 */
                u32 back = emit_jmp();
                patch_rel(back, body);
            }
        } else {
            test_rax();
            u32 jnz = emit_jcc(0x85);      /* jne -> 循环体 */
            patch_rel(jnz, body);
        }
        expect(';');
        for (int i = 0; i < lp->nbreak; i++)
            patch_rel(lp->breaks[i], clen);
        for (int i = 0; i < lp->ncont; i++)
            patch_rel(lp->conts[i], (cconst && cval == 0) ? clen : cond);
        nloop--;
        return;
    }
    if (accept_kw("switch")) {
        parse_switch();
        return;
    }
    if (accept_kw("case")) {
        if (nsw == 0) { fail("case outside switch"); return; }
        struct swctx *sw = &swstk[nsw - 1];
        i64 v = c_cond();
        expect(':');
        if (sw->ncase >= MAX_CASES) { fail("too many cases"); return; }
        sw->cases[sw->ncase].val = v;
        sw->cases[sw->ncase].addr = clen;
        sw->ncase++;
        return;
    }
    if (accept_kw("default")) {
        if (nsw == 0) { fail("default outside switch"); return; }
        expect(':');
        swstk[nsw - 1].has_def = true;
        swstk[nsw - 1].def_addr = clen;
        return;
    }
    if (accept_kw("return")) {
        if (cur.t == ';') {
            mov_rax_imm(0);
        } else {
            gen_value(0, 0);
        }
        e8(0xC9);                       /* leave */
        e8(0xC3);                       /* ret */
        expect(';');
        return;
    }
    if (accept_kw("break")) {
        if (nloop == 0) { fail("break outside loop/switch"); return; }
        struct loopctx *lp = &loops[nloop - 1];
        if (lp->nbreak >= 16) { fail("too many breaks"); return; }
        lp->breaks[lp->nbreak++] = emit_jmp();
        expect(';');
        return;
    }
    if (accept_kw("continue")) {
        int ci = nloop - 1;
        while (ci >= 0 && loops[ci].is_switch)
            ci--;
        if (ci < 0) { fail("continue outside loop"); return; }
        struct loopctx *lp = &loops[ci];
        if (lp->ncont >= 16) { fail("too many continues"); return; }
        lp->conts[lp->ncont++] = emit_jmp();
        expect(';');
        return;
    }
    if (accept_kw("else")) {
        fail("else without if");
        return;
    }

    gen_value(0, 0);
    expect(';');
}

/* ------------------------------------------------------------ 结构体定义 */
static void parse_struct_body(int sid, bool is_union)
{
    struct stype *st = &stypes[sid];
    st->nf = 0;
    st->size = 0;
    st->complete = false;
    st->bit_base = -1;
    st->bit_used = 0;

    if (!accept('{')) { fail("expected struct body"); return; }

    while (!failed && cur.t != '}' && cur.t != T_EOF) {
        struct ctype base;
        if (!parse_ctype(&base))
            return;
        for (;;) {
            struct decl d;
            if (!parse_declarator(base, &d))
                return;
            if (d.incomplete) { fail("array size unknown"); return; }
            if (!decl_validate(&d))
                return;
            int bw = 0;
            if (accept(':')) {
                i64 w = c_cond();
                if (w <= 0 || w > 63) { fail("bad bit width"); return; }
                if (d.ndims) { fail("bitfield array"); return; }
                bw = (int)w;
            }
            if (st->nf >= MAX_FIELDS) { fail("too many fields"); return; }
            struct field *fd = &st->f[st->nf++];
            memset(fd, 0, sizeof(*fd));
            strncpy(fd->name, d.name, 31);
            fd->type = d.type;
            fd->ndims = d.ndims;
            for (int i = 0; i < d.ndims; i++)
                fd->dims[i] = d.dims[i];
            fd->nelem = d.ndims ? d.nelem : 1;
            fd->bit_width = bw;
            int fsize = d.ndims ? d.nelem * value_bytes(&d.type)
                                : value_bytes(&d.type);
            if (bw > 0) {
                if (is_union) {
                    fd->offset = 0;
                    fd->bit_off = 0;
                    if (st->size < 8)
                        st->size = 8;
                } else {
                    if (st->bit_base < 0 || st->bit_used + bw > 64) {
                        st->size = align_up(st->size, 8);
                        st->bit_base = st->size;
                        st->size += 8;
                        st->bit_used = 0;
                    }
                    fd->offset = st->bit_base;
                    fd->bit_off = st->bit_used;
                    st->bit_used += bw;
                }
            } else {
                if (is_union) {
                    fd->offset = 0;
                    if (fsize > st->size)
                        st->size = fsize;
                } else {
                    st->bit_base = -1;
                    st->bit_used = 0;
                    st->size = align_up(st->size, 8);
                    fd->offset = st->size;
                    st->size += fsize;
                }
            }
            if (accept(','))
                continue;
            break;
        }
        expect(';');
    }
    st->size = align_up(st->size, 8);
    st->is_union = is_union;
    st->complete = true;
    expect('}');
}

/* ------------------------------------------------------------ 顶层声明 */
static void parse_function(struct decl *d)
{
    struct sym *fs = add_func(d->name);
    if (!fs)
        return;

    char pnames[MAX_ARGS][32];
    struct ctype ptypes[MAX_ARGS];
    int np = 0;

    expect('(');
    if (!accept(')')) {
        if (is_kw("void") && nxt.t == ')') {
            advance();
            advance();
        } else {
            for (;;) {
                if (cur.t == '.') {          /* ... 可变参数原型 */
                    accept('.');
                    accept('.');
                    accept('.');
                    break;
                }
                struct ctype pt;
                if (!parse_ctype(&pt))
                    return;
                char pn[32];
                pn[0] = 0;
                if (cur.t == '(' && nxt.t == '*') {
                    /* 参数是函数指针: (*name)(...) */
                    advance();
                    while (accept('*'))
                        ;
                    if (cur.t == T_ID) {
                        strncpy(pn, cur.name, 31);
                        pn[31] = 0;
                        advance();
                    }
                    expect(')');
                    if (!accept('(')) { fail("expected parameter list"); return; }
                    if (!accept(')')) {
                        int depth = 1;
                        while (!failed && cur.t != T_EOF) {
                            if (cur.t == '(') depth++;
                            else if (cur.t == ')') {
                                depth--;
                                if (depth == 0) { advance(); break; }
                            }
                            advance();
                        }
                    }
                    pt.func = 1;
                    pt.ptr = 0;
                    pt.kind = CK_VOID;
                    pt.obj = 8;
                } else {
                    if (cur.t == T_ID) {
                        strncpy(pn, cur.name, 31);
                        pn[31] = 0;
                        advance();
                    }
                    while (accept('[')) {
                        int es = value_bytes(&pt);
                        if (cur.t != ']')
                            (void)c_cond();
                        expect(']');
                        pt.ptr = 1;
                        pt.obj = es > 0 ? es : 1;
                    }
                }
                if (np >= MAX_ARGS) { fail("too many parameters"); return; }
                strncpy(pnames[np], pn, 31);
                pnames[np][31] = 0;
                ptypes[np] = pt;
                np++;
                if (accept(','))
                    continue;
                break;
            }
            expect(')');
        }
    }
    fs->nparams = np;

    if (accept(';'))                    /* 原型 */
        return;
    if (cur.t != '{') { fail("expected function body"); return; }

    fs->addr = (i64)clen;
    fs->defined = true;

    nloc = 0;
    lused = 0;
    nlabels = 0;
    ngotos = 0;
    for (int i = 0; i < np; i++) {
        if (!pnames[i][0]) {
            pnames[i][0] = '_';
            pnames[i][1] = (char)('0' + i);
            pnames[i][2] = 0;
        }
        struct decl pd;
        memset(&pd, 0, sizeof(pd));
        strncpy(pd.name, pnames[i], 31);
        pd.type = ptypes[i];
        int disp = 16 + 8 * i;      /* 实参从右到左压栈, arg0 在 [rbp+16] */
        if (!new_local(&pd, disp))
            return;
    }

    e8(0x55);                           /* push rbp */
    e8(0x48); e8(0x89); e8(0xE5);       /* mov rbp, rsp */
    e8(0x48); e8(0x81); e8(0xEC);       /* sub rsp, imm32 */
    locals_imm_pos = clen;
    e32(0);

    parse_block();
    if (!failed)
        resolve_gotos();

    e8(0xC9);
    e8(0xC3);

    u32 frame = (u32)lused;
    code[locals_imm_pos] = (u8)frame;
    code[locals_imm_pos + 1] = (u8)(frame >> 8);
    code[locals_imm_pos + 2] = (u8)(frame >> 16);
    code[locals_imm_pos + 3] = (u8)(frame >> 24);
    nloc = 0;
    lused = 0;
}

static void parse_global_var(struct decl *d)
{
    if (!decl_validate(d))
        return;
    if (d->incomplete) {
        if (cur.t == '=' && nxt.t == T_STR) {
            d->nelem = nxt.slen + 1;
            d->dims[d->ndims - 1] = d->nelem;
            d->incomplete = false;
        } else {
            fail("array size unknown");
            return;
        }
    }
    struct sym *s = new_global(d);
    if (!s)
        return;
    if (accept('=')) {
        init_object(d->type, d->dims, d->ndims, d->ndims ? d->nelem : 1,
                    true, s->addr, 0);
    }
}

static void parse_top_decl(struct ctype base)
{
    struct decl d;
    if (!parse_declarator(base, &d))
        return;
    if (cur.t == '(') {
        parse_function(&d);
        return;
    }
    /* 全局变量 (可多个) */
    for (;;) {
        parse_global_var(&d);
        if (accept(',')) {
            if (!parse_declarator(base, &d))
                return;
            continue;
        }
        break;
    }
    expect(';');
}

static void parse_program(void)
{
    while (!failed && cur.t != T_EOF) {
        if (accept(';'))
            continue;

        if (accept_kw("typedef")) {
            parse_typedef();
            continue;
        }
        if (is_kw("enum")) {
            advance();
            parse_enum();
            continue;
        }

        if (is_kw("struct") || is_kw("union")) {
            bool is_union = (cur.name[1] == 'n');
            advance();
            if (cur.t != T_ID) { fail("struct name expected"); return; }
            char name[32];
            strncpy(name, cur.name, 31);
            name[31] = 0;
            advance();
            if (cur.t == '{') {
                int sid = add_struct(name, is_union);
                parse_struct_body(sid, is_union);
                if (accept(';'))
                    continue;
                /* struct X { ... } v; 形式 */
                struct ctype b2;
                memset(&b2, 0, sizeof(b2));
                b2.kind = CK_STRUCT;
                b2.sid = sid;
                b2.obj = stypes[sid].size;
                parse_top_decl(b2);
                continue;
            }
            int sid = add_struct(name, is_union);
            struct ctype base;
            memset(&base, 0, sizeof(base));
            base.kind = CK_STRUCT;
            base.sid = sid;
            base.obj = stypes[sid].complete ? stypes[sid].size : 1;
            parse_top_decl(base);
            continue;
        }

        struct ctype base;
        if (!parse_ctype(&base))
            return;
        parse_top_decl(base);
    }
}

/* 编译一个按需引入的标准库模块 (调用点稍后统一解析) */
static void compile_lib_module(const char *mod_src)
{
    if (failed || !mod_src || !mod_src[0])
        return;
    sp = mod_src;
    line_no = 1;
    inc_depth = 0;
    inc_cur_dir[0] = 0;
    cur = scan();
    nxt = scan();
    parse_program();
}

/* ------------------------------------------------------------------ 入口 */
void tc_opts_default(struct tc_opts *o)
{
    memset(o, 0, sizeof(*o));
    tc_opts_level(o, 0);
}

void tc_opts_level(struct tc_opts *o, int level)
{
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    o->level = level;
    o->fold = (level >= 1);
    o->strength = (level >= 2);
    o->dce = (level >= 3);
    o->loops = (level >= 4);
}

void tc_stats(u32 *code_bytes, u32 *data_bytes)
{
    if (code_bytes) *code_bytes = last_code_len;
    if (data_bytes) *data_bytes = last_data_len;
}

const u8 *tc_compile(const char *src, u32 len, u32 *out_len, const char *path)
{
    struct tc_opts o;
    tc_opts_default(&o);
    return tc_compile_opts(src, len, out_len, path, &o);
}

const u8 *tc_compile_opts(const char *src, u32 len, u32 *out_len,
                          const char *path, const struct tc_opts *opt)
{
    struct tc_opts dflt;
    if (!opt) {
        tc_opts_default(&dflt);
        opt = &dflt;
    }
    opt_fold = opt->fold;
    opt_strength = opt->strength;
    opt_dce = opt->dce;
    opt_loops = opt->loops;

    failed = false;
    err_msg[0] = 0;
    clen = 0;
    str_used = 0;
    gused = 0;
    nsyms = 0;
    nloc = 0;
    nfc = 0;
    nloop = 0;
    nsw = 0;
    nstypes = 0;
    ntypedefs = 0;
    nenums = 0;
    lused = 0;
    inc_depth = 0;
    nseen = 0;
    nmacros = 0;
    mac_used = 0;
    exp_depth = 0;
    want_lib_string = want_lib_stdlib = want_lib_ctype = want_lib_stdio = false;
    if (path && path[0])
        dir_of(path, inc_cur_dir);
    else
        inc_cur_dir[0] = 0;
    line_no = 1;
    sp = src;
    (void)len;

    memset(code, 0, sizeof(code));
    memset(gmem, 0, sizeof(gmem));
    memset(stypes, 0, sizeof(stypes));

    /* 入口: call main; exit(rax) */
    e8(0xE8);
    main_call_pos = clen;
    e32(0);
    e8(0x48); e8(0x89); e8(0xC7);       /* mov rdi, rax */
    mov_rax_imm(0);
    e8(0xCD); e8(0x21);

    /* 编译用户源码; #include 在扫描时按需展开 */
    sp = src;
    line_no = 1;
    cur = scan();
    nxt = scan();
    parse_program();

    /* 只有被 include 的头文件才把对应标准库模块编入程序 */
    if (want_lib_string)
        compile_lib_module(tc_lib_string);
    if (want_lib_stdlib)
        compile_lib_module(tc_lib_stdlib);
    if (want_lib_ctype)
        compile_lib_module(tc_lib_ctype);
    if (want_lib_stdio)
        compile_lib_module(tc_lib_stdio);

    /* 解析函数调用 */
    for (int i = 0; i < nfc && !failed; i++) {
        struct sym *s = find_sym(fcs[i].name);
        if (!s || s->kind != SK_FUNC || !s->defined) {
            fail("undefined function");
            break;
        }
        patch_rel(fcs[i].pos, (u32)s->addr);
    }
    if (!failed) {
        struct sym *m = find_sym("main");
        if (!m || m->kind != SK_FUNC || !m->defined) {
            fail("main() not found");
        } else {
            patch_rel(main_call_pos, (u32)m->addr);
        }
    }
    if (failed)
        return 0;

    if (out_len)
        *out_len = 0;

    /* 代码 + 数据区 */
    memset(out_buf, 0, sizeof(out_buf));
    memcpy(out_buf, code, clen);
    memcpy(out_buf + TC_CODE_MAX, gmem, (u32)gused);
    memcpy(out_buf + TC_CODE_MAX + TC_DATA_GLOB, str_pool, str_used);

    u32 dlen = (u32)gused;
    if (dlen < TC_DATA_GLOB + str_used)
        dlen = TC_DATA_GLOB + str_used;
    if (dlen > TC_DATA_MAX) {
        fail("data too large");
        return 0;
    }
    if (out_len)
        *out_len = TC_CODE_MAX + dlen;
    last_code_len = clen;
    last_data_len = dlen;
    return out_buf;
}
