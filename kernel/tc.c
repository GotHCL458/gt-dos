/* TC: 系统内置 C 子集编译器.
   直接生成 x86-64 机器码, 输出 .gtp 平面程序 (由 RUN 装载到 0x600000).
   支持: int/char/指针变量, 全局变量, 函数(最多 6 参数/递归), if/else,
   while/for/break/continue/return, 整数与位运算, 字符串字面量,
   以及 SDK 内置函数 (SYSCALL 包装).
   代码上限 64KB; 数据区固定映射在 0x610000 起. */
#include "gt.h"

#define TC_CODE_MAX  0x10000
#define TC_DATA_GLOB 0x4000
#define TC_DATA_MAX  0x10000
#define TC_PROG_BASE 0x600000ULL
#define TC_DATA_BASE (TC_PROG_BASE + TC_CODE_MAX)
#define TC_GLOB_BASE TC_DATA_BASE
#define TC_STR_BASE  (TC_DATA_BASE + TC_DATA_GLOB)

#define MAX_SYM   384
#define MAX_LOCAL 96
#define MAX_FCALL 256
#define MAX_NEST  24
#define MAX_ARGS  6

enum {
    T_EOF = 0, T_ID = 256, T_NUM, T_STR,
    T_EQ, T_NE, T_LE, T_GE, T_ANDAND, T_OROR, T_SHL, T_SHR,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ
};

struct token {
    int t;
    i64 v;
    int line;
    char name[32];
};

enum { SK_GLOBAL = 1, SK_LOCAL, SK_FUNC };

struct sym {
    char name[32];
    i64 addr;          /* 全局地址 / 函数代码偏移 */
    int kind;
    int offset;        /* 局部变量 rbp 偏移 */
    int nparams;
    bool defined;
};

struct fcall {
    u32 pos;
    char name[32];
};

struct loopctx {
    u32 breaks[32];
    int nbreak;
    u32 cont;                          /* continue 目标 */
};

static u8  code[TC_CODE_MAX] __attribute__((section(".highbss")));
static u8  str_pool[TC_DATA_MAX - TC_DATA_GLOB] __attribute__((section(".highbss")));
static u8  out_buf[TC_CODE_MAX + TC_DATA_MAX] __attribute__((section(".highbss")));

static const char *sp;
static int line_no = 1;
static struct token cur, nxt;

static u32 clen;
static u32 str_used;
static bool failed;
static char err_msg[96];

static struct sym syms[MAX_SYM];
static int nsyms;
static struct sym locs[MAX_LOCAL];
static int nloc;
static u64 ginit[MAX_SYM];
static int nglob;

static struct fcall fcs[MAX_FCALL];
static int nfc;
static struct loopctx loops[MAX_NEST];
static int nloop;

static u32 main_call_pos;
static u32 locals_imm_pos;
static int cur_nparams;

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

static struct token scan(void)
{
    struct token t;
    memset(&t, 0, sizeof(t));
    t.line = line_no;

    for (;;) {
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
        if (c == '#') {                     /* 预处理指令: 跳过整行 */
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
        t.t = T_ID;
        return t;
    }
    if (isdigit_((unsigned char)c)) {
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
        } else {
            while (isdigit_((unsigned char)*sp)) {
                v = v * 10 + (*sp - '0');
                sp++;
            }
        }
        t.t = T_NUM;
        t.v = v;
        return t;
    }
    if (c == '\'') {
        sp++;
        i64 v = 0;
        if (*sp == '\\') {
            sp++;
            char e = *sp++;
            switch (e) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case 'r': v = '\r'; break;
            case '0': v = 0; break;
            case '\\': v = '\\'; break;
            case '\'': v = '\''; break;
            case '"': v = '"'; break;
            default: v = (u8)e; break;
            }
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
        while (*sp && *sp != '"') {
            char ch = *sp++;
            if (ch == '\\') {
                char e = *sp++;
                switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '0': ch = 0; break;
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                default: ch = e; break;
                }
            }
            add_str_byte((u8)ch);
        }
        if (*sp == '"') sp++;
        add_str_byte(0);
        t.t = T_STR;
        t.v = (i64)(TC_STR_BASE + start);
        return t;
    }

    sp++;
    switch (c) {
    case '+':
        if (*sp == '=') { sp++; t.t = T_PLUSEQ; }
        else t.t = '+';
        break;
    case '-':
        if (*sp == '=') { sp++; t.t = T_MINUSEQ; }
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
        if (*sp == '=') { sp++; t.t = T_LE; }
        else if (*sp == '<') { sp++; t.t = T_SHL; }
        else t.t = '<';
        break;
    case '>':
        if (*sp == '=') { sp++; t.t = T_GE; }
        else if (*sp == '>') { sp++; t.t = T_SHR; }
        else t.t = '>';
        break;
    case '&':
        if (*sp == '&') { sp++; t.t = T_ANDAND; }
        else t.t = '&';
        break;
    case '|':
        if (*sp == '|') { sp++; t.t = T_OROR; }
        else t.t = '|';
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

static void mov_local_rax(int disp)
{
    e8(0x48); e8(0x89); modrm_rbp(disp, 0);
}

static void lea_rax_local(int disp)
{
    e8(0x48); e8(0x8D); modrm_rbp(disp, 0);
}

static void mov_rax_abs(u64 addr) { e8(0x48); e8(0xA1); e64(addr); }
static void mov_abs_rax(u64 addr) { e8(0x48); e8(0xA3); e64(addr); }

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

static struct sym *add_global(const char *name)
{
    struct sym *s = find_sym(name);
    if (s && s->kind == SK_GLOBAL)
        return s;
    if (nsyms >= MAX_SYM || nglob >= MAX_SYM) { fail("too many globals"); return 0; }
    s = &syms[nsyms++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 31);
    s->kind = SK_GLOBAL;
    s->addr = (i64)(TC_GLOB_BASE + (u64)nglob * 8);
    ginit[nglob] = 0;
    nglob++;
    return s;
}

static struct sym *add_local(const char *name, int disp)
{
    if (nloc >= MAX_LOCAL) { fail("too many locals"); return 0; }
    struct sym *s = &locs[nloc++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 31);
    s->kind = SK_LOCAL;
    s->offset = disp;
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

static void load_var(const char *name)
{
    struct sym *s = lookup_var(name);
    if (!s) { fail("undefined variable"); return; }
    if (s->kind == SK_LOCAL)
        mov_rax_local(s->offset);
    else
        mov_rax_abs((u64)s->addr);
}

static void store_var(const char *name)
{
    struct sym *s = lookup_var(name);
    if (!s) { fail("undefined variable"); return; }
    if (s->kind == SK_LOCAL)
        mov_local_rax(s->offset);
    else
        mov_abs_rax((u64)s->addr);
}

/* ------------------------------------------------------------------ 表达式 */
static void p_assign(void);
static void p_lor(void);

static void p_primary(void)
{
    if (cur.t == T_NUM) {
        mov_rax_imm((u64)cur.v);
        advance();
        return;
    }
    if (cur.t == T_STR) {
        mov_rax_imm((u64)cur.v);
        advance();
        return;
    }
    if (cur.t == T_ID) {
        char name[32];
        strncpy(name, cur.name, 31);
        name[31] = 0;

        if (nxt.t == '(') {
            advance();                  /* ID */
            advance();                  /* ( */
            int nargs = 0;
            if (cur.t != ')') {
                for (;;) {
                    p_assign();
                    push_rax();
                    nargs++;
                    if (nargs > MAX_ARGS) { fail("too many arguments"); return; }
                    if (accept(','))
                        continue;
                    break;
                }
            }
            expect(')');

            /* SDK 内置函数 -> INT 0x21 */
            struct { const char *n; int nr; int args; } bi[] = {
                { "putc", 1, 1 }, { "print", 11, 1 }, { "puts", 11, 1 },
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
                return;
            }
            if (nr >= 0) {
                /* 参数按 rdi,rsi,rdx,rcx,r8,r9 反向弹出 */
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
                return;
            }

            /* 用户函数: 参数按左到右压栈, 由被调者按声明顺序取 */
            if (nfc >= MAX_FCALL) { fail("too many calls"); return; }
            e8(0xE8);
            fcs[nfc].pos = clen;        /* rel32 字段位置 */
            e32(0);
            strncpy(fcs[nfc].name, name, 31);
            fcs[nfc].name[31] = 0;
            nfc++;
            if (nargs)
                { e8(0x48); e8(0x81); e8(0xC4); e32((u32)nargs * 8); }
            return;
        }

        /* 变量读取 */
        load_var(name);
        advance();
        return;
    }
    if (accept('(')) {
        p_assign();
        expect(')');
        return;
    }
    fail("bad expression");
}

static void p_unary(void)
{
    if (accept('-')) {
        p_unary();
        e8(0x48); e8(0xF7); e8(0xD8);       /* neg rax */
        return;
    }
    if (accept('!')) {
        p_unary();
        test_rax();
        e8(0x0F); e8(0x94); e8(0xC0);       /* sete al */
        e8(0x48); e8(0x0F); e8(0xB6); e8(0xC0);
        return;
    }
    if (accept('~')) {
        p_unary();
        e8(0x48); e8(0xF7); e8(0xD0);       /* not rax */
        return;
    }
    if (accept('&')) {
        if (cur.t != T_ID) { fail("& requires variable"); return; }
        struct sym *s = lookup_var(cur.name);
        if (!s) { fail("undefined variable"); return; }
        if (s->kind == SK_LOCAL)
            lea_rax_local(s->offset);
        else
            mov_rax_imm((u64)s->addr);
        advance();
        return;
    }
    if (accept('*')) {
        p_unary();
        e8(0x48); e8(0x8B); e8(0x00);       /* mov rax, [rax] */
        return;
    }
    if (accept('+'))
        { p_unary(); return; }
    p_primary();
}

static void p_mul(void)
{
    p_unary();
    for (;;) {
        int op = cur.t;
        if (op != '*' && op != '/' && op != '%')
            break;
        advance();
        push_rax();
        p_unary();
        pop_rcx();
        apply_binop(op);
    }
}

static void p_add(void)
{
    p_mul();
    for (;;) {
        int op = cur.t;
        if (op != '+' && op != '-')
            break;
        advance();
        push_rax();
        p_mul();
        pop_rcx();
        apply_binop(op);
    }
}

static void p_shift(void)
{
    p_add();
    for (;;) {
        int op = cur.t;
        if (op != T_SHL && op != T_SHR)
            break;
        advance();
        push_rax();
        p_add();
        pop_rcx();
        apply_binop(op);
    }
}

static void p_rel(void)
{
    p_shift();
    for (;;) {
        int op = cur.t;
        if (op != '<' && op != '>' && op != T_LE && op != T_GE)
            break;
        advance();
        push_rax();
        p_shift();
        pop_rcx();
        apply_cmp(op);
    }
}

static void p_eq(void)
{
    p_rel();
    for (;;) {
        int op = cur.t;
        if (op != T_EQ && op != T_NE)
            break;
        advance();
        push_rax();
        p_rel();
        pop_rcx();
        apply_cmp(op);
    }
}

static void p_band(void)
{
    p_eq();
    while (cur.t == '&') {
        advance();
        push_rax();
        p_eq();
        pop_rcx();
        apply_binop('&');
    }
}

static void p_bxor(void)
{
    p_band();
    while (cur.t == '^') {
        advance();
        push_rax();
        p_band();
        pop_rcx();
        apply_binop('^');
    }
}

static void p_bor(void)
{
    p_bxor();
    while (cur.t == '|') {
        advance();
        push_rax();
        p_bxor();
        pop_rcx();
        apply_binop('|');
    }
}

static void p_land(void)
{
    p_bor();
    while (cur.t == T_ANDAND) {
        advance();
        boolify();
        push_rax();
        p_bor();
        boolify();
        pop_rcx();
        apply_binop('&');
    }
}

static void p_lor(void)
{
    p_land();
    while (cur.t == T_OROR) {
        advance();
        boolify();
        push_rax();
        p_land();
        boolify();
        pop_rcx();
        apply_binop('|');
    }
}

static void p_assign(void)
{
    if (cur.t == T_ID && nxt.t == '=') {
        char name[32];
        strncpy(name, cur.name, 31);
        name[31] = 0;
        advance();
        advance();
        p_assign();
        store_var(name);
        return;
    }
    if (cur.t == T_ID &&
        (nxt.t == T_PLUSEQ || nxt.t == T_MINUSEQ || nxt.t == T_STAREQ ||
         nxt.t == T_SLASHEQ || nxt.t == T_PERCENTEQ)) {
        char name[32];
        strncpy(name, cur.name, 31);
        name[31] = 0;
        int op = nxt.t;
        advance();
        advance();
        load_var(name);
        push_rax();
        p_assign();
        pop_rcx();
        if (op == T_PLUSEQ) apply_binop('+');
        else if (op == T_MINUSEQ) apply_binop('-');
        else if (op == T_STAREQ) apply_binop('*');
        else if (op == T_SLASHEQ) apply_binop('/');
        else apply_binop('%');
        store_var(name);
        return;
    }
    p_lor();
}

/* ------------------------------------------------------------------ 语句 */
static void parse_stmt(void);

static void parse_local_decl(void)
{
    for (;;) {
        while (accept('*'))
            ;
        if (cur.t != T_ID) { fail("expected variable name"); return; }
        char name[32];
        strncpy(name, cur.name, 31);
        name[31] = 0;
        advance();
        struct sym *s = add_local(name, -8 * (nloc + 1));
        if (!s)
            return;
        if (accept('=')) {
            p_assign();
        } else {
            mov_rax_imm(0);
        }
        mov_local_rax(s->offset);
        if (accept(','))
            continue;
        break;
    }
    expect(';');
}

static void parse_block(void)
{
    expect('{');
    while (!failed && cur.t != '}' && cur.t != T_EOF)
        parse_stmt();
    expect('}');
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

    if (is_kw("int") || is_kw("char")) {
        advance();
        parse_local_decl();
        return;
    }
    if (accept_kw("if")) {
        expect('(');
        p_assign();
        expect(')');
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
        u32 start = clen;
        p_assign();
        expect(')');
        test_rax();
        u32 jz = emit_jcc(0x84);
        if (nloop >= MAX_NEST) { fail("loop nesting too deep"); return; }
        struct loopctx *lp = &loops[nloop++];
        lp->nbreak = 0;
        lp->cont = start;
        parse_stmt();
        u32 back = emit_jmp();
        patch_rel(back, start);
        patch_rel(jz, clen);
        for (int i = 0; i < lp->nbreak; i++)
            patch_rel(lp->breaks[i], clen);
        nloop--;
        return;
    }
    if (accept_kw("for")) {
        expect('(');
        if (!accept(';')) {
            if (is_kw("int") || is_kw("char")) {
                advance();
                while (accept('*')) ;
                parse_local_decl();
            } else {
                p_assign();
                expect(';');
            }
        }
        u32 cond = clen;
        u32 jz = 0;
        bool has_cond = false;
        if (cur.t != ';') {
            p_assign();
            test_rax();
            jz = emit_jcc(0x84);
            has_cond = true;
        }
        expect(';');
        u32 jbody = emit_jmp();         /* 跳过 step 进入循环体 */
        u32 step = clen;
        if (cur.t != ')') {
            p_assign();
        }
        expect(')');
        u32 back = emit_jmp();
        patch_rel(back, cond);
        patch_rel(jbody, clen);

        if (nloop >= MAX_NEST) { fail("loop nesting too deep"); return; }
        struct loopctx *lp = &loops[nloop++];
        lp->nbreak = 0;
        lp->cont = step;
        parse_stmt();
        u32 to_step = emit_jmp();
        patch_rel(to_step, step);
        for (int i = 0; i < lp->nbreak; i++)
            patch_rel(lp->breaks[i], clen);
        if (has_cond)
            patch_rel(jz, clen);
        nloop--;
        return;
    }
    if (accept_kw("return")) {
        if (cur.t == ';') {
            mov_rax_imm(0);
        } else {
            p_assign();
        }
        e8(0xC9);                       /* leave */
        e8(0xC3);                       /* ret */
        expect(';');
        return;
    }
    if (accept_kw("break")) {
        if (nloop == 0) { fail("break outside loop"); return; }
        struct loopctx *lp = &loops[nloop - 1];
        if (lp->nbreak >= 32) { fail("too many breaks"); return; }
        lp->breaks[lp->nbreak++] = emit_jmp();
        expect(';');
        return;
    }
    if (accept_kw("continue")) {
        if (nloop == 0) { fail("continue outside loop"); return; }
        u32 j = emit_jmp();
        patch_rel(j, loops[nloop - 1].cont);
        expect(';');
        return;
    }
    if (accept_kw("else")) {
        fail("else without if");
        return;
    }

    p_assign();
    expect(';');
}

/* ------------------------------------------------------------------ 顶层 */
static u64 const_expr(void)
{
    bool neg = false;
    if (accept('-'))
        neg = true;
    else
        accept('+');
    i64 v = 0;
    if (cur.t == T_NUM || cur.t == T_STR) {
        v = cur.v;
        advance();
    } else {
        fail("constant expected");
        return 0;
    }
    return neg ? (u64)(-v) : (u64)v;
}

static void parse_globals(void)
{
    for (;;) {
        while (accept('*'))
            ;
        if (cur.t != T_ID) { fail("expected name"); return; }
        char name[32];
        strncpy(name, cur.name, 31);
        name[31] = 0;
        advance();
        struct sym *s = add_global(name);
        if (!s)
            return;
        if (accept('=')) {
            int idx = (int)((s->addr - (i64)TC_GLOB_BASE) / 8);
            ginit[idx] = const_expr();
        }
        if (accept(','))
            continue;
        break;
    }
    expect(';');
}

static void parse_params(char names[][32], int *np)
{
    expect('(');
    if (accept(')'))
        return;
    if (is_kw("void") && nxt.t == ')') {
        advance();
        advance();
        return;
    }
    for (;;) {
        if (is_kw("int") || is_kw("char") || is_kw("void"))
            advance();
        else { fail("parameter type expected"); return; }
        while (accept('*'))
            ;
        if (cur.t != T_ID) { fail("parameter name expected"); return; }
        if (*np >= MAX_ARGS) { fail("too many parameters"); return; }
        strncpy(names[*np], cur.name, 31);
        names[*np][31] = 0;
        (*np)++;
        advance();
        if (accept(','))
            continue;
        break;
    }
    expect(')');
}

static void parse_function(void)
{
    char name[32];
    strncpy(name, cur.name, 31);
    name[31] = 0;
    advance();

    char pnames[MAX_ARGS][32];
    int np = 0;
    parse_params(pnames, &np);
    if (failed)
        return;

    struct sym *fs = add_func(name);
    if (!fs)
        return;
    fs->addr = (i64)clen;
    fs->nparams = np;
    fs->defined = true;

    if (accept(';'))                    /* 原型 */
        return;
    if (cur.t != '{') { fail("expected function body"); return; }

    nloc = 0;
    cur_nparams = np;
    for (int i = 0; i < np; i++) {
        int disp = 16 + 8 * (np - 1 - i);
        if (!add_local(pnames[i], disp))
            return;
    }

    e8(0x55);                           /* push rbp */
    e8(0x48); e8(0x89); e8(0xE5);       /* mov rbp, rsp */
    e8(0x48); e8(0x81); e8(0xEC);       /* sub rsp, imm32 */
    locals_imm_pos = clen;
    e32(0);
    u32 frame_patch = locals_imm_pos;

    parse_block();

    e8(0xC9);
    e8(0xC3);

    u32 frame = (u32)nloc * 8;
    code[frame_patch] = (u8)frame;
    code[frame_patch + 1] = (u8)(frame >> 8);
    code[frame_patch + 2] = (u8)(frame >> 16);
    code[frame_patch + 3] = (u8)(frame >> 24);
    (void)cur_nparams;
    nloc = 0;
}

static void parse_program(void)
{
    while (!failed && cur.t != T_EOF) {
        if (is_kw("int") || is_kw("char") || is_kw("void") ||
            is_kw("static")) {
            if (is_kw("static"))
                advance();
            advance();                  /* 类型 */
            while (accept('*'))
                ;
            if (cur.t != T_ID) { fail("expected name"); return; }
            if (nxt.t == '(')
                parse_function();
            else
                parse_globals();
        } else if (accept(';')) {
            /* 空声明 */
        } else {
            fail("expected declaration");
            return;
        }
    }
}

/* ------------------------------------------------------------------ 入口 */
const u8 *tc_compile(const char *src, u32 len, u32 *out_len)
{
    failed = false;
    err_msg[0] = 0;
    clen = 0;
    str_used = 0;
    nsyms = 0;
    nloc = 0;
    nglob = 0;
    nfc = 0;
    nloop = 0;
    line_no = 1;
    sp = src;
    (void)len;

    cur = scan();
    nxt = scan();

    /* 入口: call main; exit(rax) */
    e8(0xE8);
    main_call_pos = clen;
    e32(0);
    e8(0x48); e8(0x89); e8(0xC7);       /* mov rdi, rax */
    mov_rax_imm(0);
    e8(0xCD); e8(0x21);

    parse_program();

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
    for (int i = 0; i < nglob; i++) {
        u64 v = ginit[i];
        u8 *p = out_buf + TC_CODE_MAX + (u32)i * 8;
        for (int k = 0; k < 8; k++)
            p[k] = (u8)(v >> (k * 8));
    }
    memcpy(out_buf + TC_CODE_MAX + TC_DATA_GLOB, str_pool, str_used);

    u32 dlen = (u32)nglob * 8;
    if (dlen < TC_DATA_GLOB + str_used)
        dlen = TC_DATA_GLOB + str_used;
    if (dlen > TC_DATA_MAX) {
        fail("data too large");
        return 0;
    }
    if (out_len)
        *out_len = TC_CODE_MAX + dlen;
    return out_buf;
}
