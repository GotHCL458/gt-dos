/* ASM: 内置 x86-64 汇编器.
   把单文件汇编源码 (NASM 风格) 直接编码为 .gtp 平面程序:
   代码 128KB @0x600000, 数据 @0x620000, 与 TC 的输出格式一致, 可直接 RUN.
   入口即代码偏移 0; 程序用 INT 0x21 (rax=调用号) 与内核交互, 退出:

       xor edi, edi          ; 退出码
       xor eax, eax          ; SYS_EXIT
       int 0x21

   支持:
     * .text / .data 段 (也可写 section .text / .data)
     * 标签、equ 常量、db/dw/dd/dq、times
     * 64/32 位整数寄存器 (rax..r15 / eax..r15d)
     * mov, lea, add/sub/and/or/xor/cmp/test, inc/dec, neg/not,
       imul, div/idiv, shl/shr/sar, push/pop, call/ret, jmp/jcc,
       int, nop, hlt, leave, cqo/cdq
     * 内存操作数: [reg], [reg+disp], [reg+index*scale+disp], [label],
       [label+disp], 绝对地址 [disp]
   注释: ';' 或 '//' 到行尾. 标签区分大小写, 指令/寄存器不区分. */
#include "gt.h"

#define AS_CODE_MAX  0x20000            /* 代码窗口 128KB */
#define AS_DATA_MAX  0x10000            /* 数据区 (输出到 0x620000 起) */
#define AS_CODE_BASE 0x600000ULL
#define AS_DATA_BASE 0x620000ULL

#define AS_MAX_LABEL 256
#define AS_MAX_CONST 256
#define AS_MAX_FIX   1024

struct as_label {
    char name[32];
    u32  off;
    int  sec;                           /* 0=text 1=data */
};

struct as_const {
    char name[32];
    i64  val;
};

struct as_fix {
    u32  pos;                           /* 输出缓冲中的字节位置 */
    int  kind;                          /* 0=rel32 1=abs64 2=abs32 */
    i64  add;
    char name[32];
};

static u8  as_out[AS_CODE_MAX + AS_DATA_MAX] __attribute__((section(".highbss")));
static u32 as_clen, as_dlen;
static int as_sec;                      /* 当前段 */

static struct as_label as_lab[AS_MAX_LABEL];
static int as_nlab;
static struct as_const as_con[AS_MAX_CONST];
static int as_ncon;
static struct as_fix as_fix[AS_MAX_FIX];
static int as_nfix;

static int  as_line;
static bool as_failed;
static char as_err[96];

static void as_fail(const char *msg)
{
    if (as_failed)
        return;
    as_failed = true;
    char num[16];
    itoa_base((u32)(as_line > 0 ? as_line : 1), num, 10, false);
    int k = 0;
    const char *a = "line ";
    while (*a && k < 60) as_err[k++] = *a++;
    const char *b = num;
    while (*b && k < 60) as_err[k++] = *b++;
    if (k < 60) as_err[k++] = ':';
    if (k < 60) as_err[k++] = ' ';
    while (*msg && k < 90) as_err[k++] = *msg++;
    as_err[k] = 0;
}

static void as_fail2(const char *msg, const char *arg)
{
    if (as_failed)
        return;
    char tmp[64];
    int k = 0;
    while (*msg && k < 40) tmp[k++] = *msg++;
    while (*arg && k < 60) tmp[k++] = *arg++;
    tmp[k] = 0;
    as_fail(tmp);
}

const char *asm_error(void) { return as_err; }

/* ---------------------------------------------------------------- 输出 */
static void ae8(u8 b)
{
    if (as_sec == 0) {
        if (as_clen >= AS_CODE_MAX) { as_fail("text too large"); return; }
        as_out[as_clen++] = b;
    } else {
        if (as_dlen >= AS_DATA_MAX) { as_fail("data too large"); return; }
        as_out[AS_CODE_MAX + as_dlen++] = b;
    }
}

static void ae16(u16 v)
{
    ae8((u8)v); ae8((u8)(v >> 8));
}

static void ae32(u32 v)
{
    ae8((u8)v); ae8((u8)(v >> 8)); ae8((u8)(v >> 16)); ae8((u8)(v >> 24));
}

static void ae64(u64 v)
{
    ae32((u32)v); ae32((u32)(v >> 32));
}

static void aw32(u32 pos, u32 v)
{
    as_out[pos] = (u8)v;
    as_out[pos + 1] = (u8)(v >> 8);
    as_out[pos + 2] = (u8)(v >> 16);
    as_out[pos + 3] = (u8)(v >> 24);
}

static void aw64(u32 pos, u64 v)
{
    aw32(pos, (u32)v);
    aw32(pos + 4, (u32)(v >> 32));
}

/* REX: w=64 位, r/x/b 为寄存器编号的高位 */
static void arex(int w, int r, int x, int b)
{
    u8 v = 0x40 | (w ? 8 : 0) |
           (u8)(((r >> 3) & 1) << 2) | (u8)(((x >> 3) & 1) << 1) |
           (u8)((b >> 3) & 1);
    if (v != 0x40)
        ae8(v);
}

static void add_fix(int kind, u32 pos, i64 add, const char *name)
{
    if (as_nfix >= AS_MAX_FIX) { as_fail("too many relocations"); return; }
    struct as_fix *f = &as_fix[as_nfix++];
    f->pos = pos;
    f->kind = kind;
    f->add = add;
    strncpy(f->name, name, 31);
    f->name[31] = 0;
}

/* ---------------------------------------------------------------- 词法 */
struct acur {
    const char *p;
    const char *end;
};

static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_id0(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '.';
}
static bool is_idc(int c)
{
    return is_id0(c) || is_digit(c) || c == '$';
}

static void skip_ws(struct acur *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t'))
        c->p++;
}

static bool read_id(struct acur *c, char *out, int cap)
{
    skip_ws(c);
    if (c->p >= c->end || !is_id0(*c->p))
        return false;
    int k = 0;
    while (c->p < c->end && is_idc(*c->p)) {
        if (k < cap - 1)
            out[k++] = *c->p;
        c->p++;
    }
    out[k] = 0;
    return k > 0;
}

static bool read_num(struct acur *c, i64 *out)
{
    skip_ws(c);
    if (c->p >= c->end)
        return false;
    if (*c->p == '\'') {
        c->p++;
        if (c->p >= c->end) { as_fail("bad char literal"); return false; }
        i64 v;
        if (*c->p == '\\') {
            c->p++;
            if (c->p >= c->end) { as_fail("bad escape"); return false; }
            char e = *c->p++;
            switch (e) {
            case 'n': v = 10; break;
            case 'r': v = 13; break;
            case 't': v = 9; break;
            case '0': v = 0; break;
            case '\\': v = '\\'; break;
            case '\'': v = '\''; break;
            case '"': v = '"'; break;
            case 'x': {
                v = 0;
                for (int i = 0; i < 2 && c->p < c->end; i++) {
                    int ch = *c->p, d;
                    if (ch >= '0' && ch <= '9') d = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                    else break;
                    v = v * 16 + d;
                    c->p++;
                }
                break;
            }
            default: v = e; break;
            }
        } else {
            v = (u8)*c->p++;
        }
        if (c->p < c->end && *c->p == '\'')
            c->p++;
        *out = v;
        return true;
    }
    if (!is_digit((u8)*c->p))
        return false;
    u64 v = 0;
    int base = 10;
    if (c->p + 1 < c->end && c->p[0] == '0' &&
        (c->p[1] == 'x' || c->p[1] == 'X')) {
        base = 16;
        c->p += 2;
    } else if (c->p + 1 < c->end && c->p[0] == '0' &&
               (c->p[1] == 'b' || c->p[1] == 'B')) {
        base = 2;
        c->p += 2;
    } else if (c->p + 1 < c->end && c->p[0] == '0' &&
               (c->p[1] == 'o' || c->p[1] == 'O')) {
        base = 8;
        c->p += 2;
    }
    bool any = false;
    while (c->p < c->end) {
        int ch = (u8)*c->p, d = -1;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        if (d < 0 || d >= base)
            break;
        v = v * (u64)base + (u64)d;
        c->p++;
        any = true;
    }
    if (!any) { as_fail("bad number"); return false; }
    *out = (i64)v;
    return true;
}

/* 字符串字面量 -> 字节; 返回 false 表示不是字符串 */
static bool read_str(struct acur *c, u8 *buf, int cap, int *len)
{
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"')
        return false;
    c->p++;
    int k = 0;
    while (c->p < c->end && *c->p != '"') {
        int ch = (u8)*c->p++;
        if (ch == '\\' && c->p < c->end) {
            char e = *c->p++;
            switch (e) {
            case 'n': ch = 10; break;
            case 'r': ch = 13; break;
            case 't': ch = 9; break;
            case '0': ch = 0; break;
            case '\\': ch = '\\'; break;
            case '"': ch = '"'; break;
            default: ch = e; break;
            }
        }
        if (k < cap)
            buf[k++] = (u8)ch;
    }
    if (c->p < c->end && *c->p == '"')
        c->p++;
    *len = k;
    return true;
}

static const char *const as_reg64[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};
static const char *const as_reg32[16] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
};

struct as_reg { int num; int bits; };

static bool reg_lookup(const char *w, struct as_reg *r)
{
    for (int i = 0; i < 16; i++) {
        if (!strcasecmp(w, as_reg64[i])) {
            r->num = i;
            r->bits = 64;
            return true;
        }
        if (!strcasecmp(w, as_reg32[i])) {
            r->num = i;
            r->bits = 32;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- 表达式 */
struct as_val {
    int  is_sym;
    char name[32];
    i64  add;
};

static bool parse_val_expr(struct acur *c, struct as_val *v);

static struct as_const *find_const(const char *name)
{
    for (int i = 0; i < as_ncon; i++)
        if (!strcmp(as_con[i].name, name))
            return &as_con[i];
    return 0;
}

static bool parse_primary(struct acur *c, struct as_val *v)
{
    skip_ws(c);
    v->is_sym = 0;
    v->add = 0;
    v->name[0] = 0;
    if (c->p >= c->end) {
        as_fail("expression expected");
        return false;
    }
    if (*c->p == '(') {
        c->p++;
        if (!parse_val_expr(c, v))
            return false;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ')') { as_fail("expected )"); return false; }
        c->p++;
        return true;
    }
    i64 num;
    if (read_num(c, &num)) {
        v->add = num;
        return true;
    }
    char w[32];
    if (read_id(c, w, sizeof(w))) {
        struct as_const *k = find_const(w);
        if (k) {
            v->add = k->val;
        } else {
            v->is_sym = 1;
            strncpy(v->name, w, 31);
            v->name[31] = 0;
        }
        return true;
    }
    as_fail("bad expression");
    return false;
}

static bool parse_term(struct acur *c, struct as_val *v)
{
    skip_ws(c);
    if (c->p < c->end && *c->p == '-') {
        c->p++;
        if (!parse_term(c, v))
            return false;
        if (v->is_sym) { as_fail("cannot negate a label"); return false; }
        v->add = -v->add;
        return true;
    }
    if (c->p < c->end && *c->p == '+') {
        c->p++;
        return parse_term(c, v);
    }
    if (c->p < c->end && *c->p == '~') {
        c->p++;
        if (!parse_term(c, v))
            return false;
        if (v->is_sym) { as_fail("cannot invert a label"); return false; }
        v->add = ~v->add;
        return true;
    }
    return parse_primary(c, v);
}

static bool parse_val_expr(struct acur *c, struct as_val *v)
{
    if (!parse_term(c, v))
        return false;
    for (;;) {
        skip_ws(c);
        if (c->p >= c->end || (*c->p != '+' && *c->p != '-'))
            break;
        int sign = (*c->p == '-') ? -1 : 1;
        c->p++;
        struct as_val w;
        if (!parse_term(c, &w))
            return false;
        if (v->is_sym && w.is_sym) { as_fail("two labels in one expression"); return false; }
        if (w.is_sym) {
            v->is_sym = 1;
            strncpy(v->name, w.name, 31);
            v->name[31] = 0;
            v->add = (i64)sign * w.add;
        } else {
            v->add += (i64)sign * w.add;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- 操作数 */
enum { ASO_NONE, ASO_REG, ASO_IMM, ASO_MEM };

struct as_mem {
    int base;                           /* -1 = 无 */
    int index;                          /* -1 = 无 */
    int scale;
    int has_disp;
    struct as_val disp;
};

struct as_op {
    int kind;
    struct as_reg reg;
    struct as_val imm;
    struct as_mem mem;
};

static bool parse_reg_peek(struct acur *c, struct as_reg *r)
{
    const char *save = c->p;
    char w[32];
    if (read_id(c, w, sizeof(w)) && reg_lookup(w, r))
        return true;
    c->p = save;
    return false;
}

static bool parse_mem(struct acur *c, struct as_mem *m)
{
    m->base = -1;
    m->index = -1;
    m->scale = 1;
    m->has_disp = 0;
    m->disp.is_sym = 0;
    m->disp.add = 0;
    m->disp.name[0] = 0;
    bool first = true;
    for (;;) {
        skip_ws(c);
        if (c->p >= c->end) { as_fail("unterminated ["); return false; }
        if (*c->p == ']')
            break;
        int sign = 1;
        if (*c->p == '+' || *c->p == '-') {
            sign = (*c->p == '-') ? -1 : 1;
            c->p++;
            skip_ws(c);
        } else if (!first) {
            as_fail("expected + or - in memory operand");
            return false;
        }
        struct as_reg r;
        if (parse_reg_peek(c, &r)) {
            skip_ws(c);
            int sc = 1;
            if (c->p < c->end && *c->p == '*') {
                c->p++;
                i64 sv;
                if (!read_num(c, &sv) || sv <= 0) {
                    as_fail("bad scale");
                    return false;
                }
                sc = (int)sv;
                if (sc != 1 && sc != 2 && sc != 4 && sc != 8) {
                    as_fail("scale must be 1/2/4/8");
                    return false;
                }
            }
            if (sc != 1 || (sign < 0)) {
                if (sc == 1 && sign < 0) { as_fail("cannot negate register"); return false; }
                if (m->index >= 0) { as_fail("two index registers"); return false; }
                if ((r.num & 7) == 4) { as_fail("rsp/r12 cannot index"); return false; }
                m->index = r.num;
                m->scale = sc;
            } else if (m->base < 0) {
                m->base = r.num;
            } else if (m->index < 0) {
                if ((r.num & 7) == 4) { as_fail("rsp/r12 cannot index"); return false; }
                m->index = r.num;
                m->scale = 1;
            } else {
                as_fail("too many registers in memory operand");
                return false;
            }
        } else {
            struct as_val v;
            if (!parse_term(c, &v))
                return false;
            if (sign < 0) {
                if (v.is_sym) { as_fail("cannot negate a label"); return false; }
                v.add = -v.add;
            }
            if (m->has_disp) {
                if (m->disp.is_sym && v.is_sym) { as_fail("two labels in address"); return false; }
                if (v.is_sym) {
                    m->disp.is_sym = 1;
                    strncpy(m->disp.name, v.name, 31);
                    m->disp.name[31] = 0;
                    m->disp.add = v.add;
                } else {
                    m->disp.add += v.add;
                }
            } else {
                m->disp = v;
                m->has_disp = 1;
            }
        }
        first = false;
    }
    if (c->p < c->end && *c->p == ']')
        c->p++;
    return true;
}

static bool parse_operand(struct acur *c, struct as_op *o)
{
    memset(o, 0, sizeof(*o));
    skip_ws(c);
    if (c->p >= c->end)
        return false;
    if (*c->p == '[') {
        c->p++;
        if (!parse_mem(c, &o->mem))
            return false;
        o->kind = ASO_MEM;
        return true;
    }
    if (parse_reg_peek(c, &o->reg)) {
        o->kind = ASO_REG;
        return true;
    }
    if (!parse_val_expr(c, &o->imm))
        return false;
    o->kind = ASO_IMM;
    return true;
}

/* ---------------------------------------------------------------- 编码 */
static void emit_imm32(const struct as_val *v)
{
    if (v->is_sym) {
        add_fix(2, as_clen, v->add, v->name);
        ae32(0);
    } else {
        ae32((u32)v->add);
    }
}

static void emit_imm64(const struct as_val *v)
{
    if (v->is_sym) {
        add_fix(1, as_clen, v->add, v->name);
        ae64(0);
    } else {
        ae64((u64)v->add);
    }
}

/* rax 侧: 输出 ModRM/SIB/disp (REX 已由调用者发出) */
static void emit_rm_mem(const struct as_mem *m, int reg_field)
{
    int base = m->base, index = m->index;
    if (base < 0 && index < 0) {
        ae8((u8)(0x04 | ((reg_field & 7) << 3)));      /* mod=00 rm=100 */
        ae8(0x25);                                     /* SIB: [disp32] */
        if (m->has_disp && m->disp.is_sym) {
            add_fix(2, as_clen, m->disp.add, m->disp.name);
            ae32(0);
        } else {
            ae32(m->has_disp ? (u32)m->disp.add : 0);
        }
        return;
    }
    if (index >= 0 || (base >= 0 && (base & 7) == 4)) {
        ae8((u8)(0x80 | ((reg_field & 7) << 3) | 0x04)); /* mod=10 rm=100 */
        int sc = (m->scale == 2) ? 1 : (m->scale == 4) ? 2
               : (m->scale == 8) ? 3 : 0;
        int ix = (index >= 0) ? index : 4;
        int bs = (base >= 0) ? base : 5;
        ae8((u8)((sc << 6) | ((ix & 7) << 3) | (bs & 7)));
    } else {
        ae8((u8)(0x80 | ((reg_field & 7) << 3) | (base & 7)));
    }
    if (m->has_disp && m->disp.is_sym) {
        add_fix(2, as_clen, m->disp.add, m->disp.name);
        ae32(0);
    } else {
        ae32(m->has_disp ? (u32)m->disp.add : 0);
    }
}

/* 内存目标 + 寄存器源: 发出 REX + opcode + ModRM */
static void emit_mem_reg(int opcode, int w, int src, const struct as_mem *m)
{
    int ix = (m->index >= 0) ? m->index : 0;
    int bx = (m->base >= 0) ? m->base : 0;
    arex(w, src, ix, bx);
    ae8((u8)opcode);
    emit_rm_mem(m, src);
}

static void emit_alu_rr(int opcode, int w, int src, int dst)
{
    arex(w, src, 0, dst);
    ae8((u8)opcode);
    ae8((u8)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_alu_group_imm(int group, int w, const struct as_op *dst,
                               const struct as_val *imm, int opcode32)
{
    int small = opcode32 != 0xC7 && !imm->is_sym &&
                imm->add >= -128 && imm->add <= 127;
    if (dst->kind == ASO_REG) {
        arex(w, 0, 0, dst->reg.num);
        if (small) {
            ae8(0x83);
            ae8((u8)(0xC0 | (group << 3) | (dst->reg.num & 7)));
            ae8((u8)imm->add);
        } else {
            ae8((u8)opcode32);
            ae8((u8)(0xC0 | (group << 3) | (dst->reg.num & 7)));
            emit_imm32(imm);
        }
    } else {
        int ix = (dst->mem.index >= 0) ? dst->mem.index : 0;
        int bx = (dst->mem.base >= 0) ? dst->mem.base : 0;
        arex(w, 0, ix, bx);
        if (small) {
            ae8(0x83);
            emit_rm_mem(&dst->mem, group);
            ae8((u8)imm->add);
        } else {
            ae8((u8)opcode32);
            emit_rm_mem(&dst->mem, group);
            emit_imm32(imm);
        }
    }
}

static void emit_jcc(int cc, const struct as_val *t)
{
    if (!t->is_sym) { as_fail("branch target must be a label"); return; }
    ae8(0x0F);
    ae8((u8)cc);
    add_fix(0, as_clen, t->add, t->name);
    ae32(0);
}

static void emit_rel_branch(int opcode, const struct as_val *t)
{
    if (!t->is_sym) { as_fail("branch target must be a label"); return; }
    ae8((u8)opcode);
    add_fix(0, as_clen, t->add, t->name);
    ae32(0);
}

static int jcc_code(const char *m)
{
    static const struct { const char *n; int cc; } tab[] = {
        { "jo", 0x80 }, { "jno", 0x81 }, { "jb", 0x82 }, { "jc", 0x82 },
        { "jae", 0x83 }, { "jnb", 0x83 }, { "je", 0x84 }, { "jz", 0x84 },
        { "jne", 0x85 }, { "jnz", 0x85 }, { "jbe", 0x86 }, { "jna", 0x86 },
        { "ja", 0x87 }, { "jnbe", 0x87 }, { "js", 0x88 }, { "jns", 0x89 },
        { "jp", 0x8A }, { "jpe", 0x8A }, { "jnp", 0x8B }, { "jpo", 0x8B },
        { "jl", 0x8C }, { "jnge", 0x8C }, { "jge", 0x8D }, { "jnl", 0x8D },
        { "jle", 0x8E }, { "jng", 0x8E }, { "jg", 0x8F }, { "jnle", 0x8F },
        { 0, 0 }
    };
    for (int i = 0; tab[i].n; i++)
        if (!strcasecmp(m, tab[i].n))
            return tab[i].cc;
    return -1;
}

static int alu_group(const char *m)
{
    if (!strcasecmp(m, "add")) return 0;
    if (!strcasecmp(m, "or"))  return 1;
    if (!strcasecmp(m, "and")) return 4;
    if (!strcasecmp(m, "sub")) return 5;
    if (!strcasecmp(m, "xor")) return 6;
    if (!strcasecmp(m, "cmp")) return 7;
    return -1;
}

static int alu_opcode(const char *m)
{
    if (!strcasecmp(m, "add")) return 0x01;
    if (!strcasecmp(m, "or"))  return 0x09;
    if (!strcasecmp(m, "and")) return 0x21;
    if (!strcasecmp(m, "sub")) return 0x29;
    if (!strcasecmp(m, "xor")) return 0x31;
    if (!strcasecmp(m, "cmp")) return 0x39;
    return -1;
}

/* ---------------------------------------------------------------- 指令 */
static void emit_insn(const char *mn, struct as_op *o, int n)
{
    if (as_failed)
        return;

    /* jcc */
    int cc = jcc_code(mn);
    if (cc >= 0) {
        if (n != 1) { as_fail("jcc needs one operand"); return; }
        if (o[0].kind == ASO_REG) {
            arex(0, 0, 0, o[0].reg.num);
            ae8(0xFF);
            ae8((u8)(0xC0 | (4 << 3) | (o[0].reg.num & 7)));
        } else if (o[0].kind == ASO_IMM) {
            emit_jcc(cc, &o[0].imm);
        } else {
            as_fail("bad jcc operand");
        }
        return;
    }

    int ag = alu_group(mn);
    if (ag >= 0) {
        if (n != 2) { as_fail("two operands expected"); return; }
        int w = (o[0].kind == ASO_REG) ? (o[0].reg.bits == 64) : 1;
        int opcode = alu_opcode(mn);
        if (o[0].kind == ASO_REG && o[1].kind == ASO_REG) {
            if (o[0].reg.bits != o[1].reg.bits) { as_fail("operand size mismatch"); return; }
            emit_alu_rr(opcode, w, o[1].reg.num, o[0].reg.num);
        } else if (o[0].kind == ASO_REG && o[1].kind == ASO_MEM) {
            int ix = (o[1].mem.index >= 0) ? o[1].mem.index : 0;
            int bx = (o[1].mem.base >= 0) ? o[1].mem.base : 0;
            arex(w, o[0].reg.num, ix, bx);
            ae8((u8)opcode);
            emit_rm_mem(&o[1].mem, o[0].reg.num);
        } else if (o[0].kind == ASO_MEM && o[1].kind == ASO_REG) {
            emit_mem_reg(opcode, o[1].reg.bits == 64, o[1].reg.num, &o[0].mem);
        } else if (o[0].kind == ASO_REG && o[1].kind == ASO_IMM) {
            emit_alu_group_imm(ag, w, &o[0], &o[1].imm, 0x81);
        } else if (o[0].kind == ASO_MEM && o[1].kind == ASO_IMM) {
            emit_alu_group_imm(ag, 1, &o[0], &o[1].imm, 0x81);
        } else {
            as_fail("bad operands");
        }
        return;
    }

    if (!strcasecmp(mn, "mov")) {
        if (n != 2) { as_fail("two operands expected"); return; }
        if (o[0].kind == ASO_REG && o[1].kind == ASO_REG) {
            if (o[0].reg.bits != o[1].reg.bits) { as_fail("operand size mismatch"); return; }
            emit_alu_rr(0x89, o[0].reg.bits == 64, o[1].reg.num, o[0].reg.num);
        } else if (o[0].kind == ASO_REG && o[1].kind == ASO_MEM) {
            int w = (o[0].reg.bits == 64);
            int ix = (o[1].mem.index >= 0) ? o[1].mem.index : 0;
            int bx = (o[1].mem.base >= 0) ? o[1].mem.base : 0;
            arex(w, o[0].reg.num, ix, bx);
            ae8(0x8B);
            emit_rm_mem(&o[1].mem, o[0].reg.num);
        } else if (o[0].kind == ASO_MEM && o[1].kind == ASO_REG) {
            emit_mem_reg(0x89, o[1].reg.bits == 64, o[1].reg.num, &o[0].mem);
        } else if (o[0].kind == ASO_REG && o[1].kind == ASO_IMM) {
            int w = (o[0].reg.bits == 64);
            int rd = o[0].reg.num;
            if (w && (o[1].imm.is_sym ||
                      o[1].imm.add < -2147483648LL ||
                      o[1].imm.add > 2147483647LL)) {
                arex(1, 0, 0, rd);
                ae8((u8)(0xB8 + (rd & 7)));
                emit_imm64(&o[1].imm);
            } else if (w) {
                arex(1, 0, 0, rd);
                ae8(0xC7);
                ae8((u8)(0xC0 | (rd & 7)));
                emit_imm32(&o[1].imm);
            } else {
                arex(0, 0, 0, rd);
                ae8((u8)(0xB8 + (rd & 7)));
                emit_imm32(&o[1].imm);
            }
        } else if (o[0].kind == ASO_MEM && o[1].kind == ASO_IMM) {
            emit_alu_group_imm(0, 1, &o[0], &o[1].imm, 0xC7);
        } else {
            as_fail("bad operands");
        }
        return;
    }
    if (!strcasecmp(mn, "lea")) {
        if (n != 2 || o[0].kind != ASO_REG || o[1].kind != ASO_MEM) {
            as_fail("lea needs reg, [mem]");
            return;
        }
        int ix = (o[1].mem.index >= 0) ? o[1].mem.index : 0;
        int bx = (o[1].mem.base >= 0) ? o[1].mem.base : 0;
        arex(o[0].reg.bits == 64, o[0].reg.num, ix, bx);
        ae8(0x8D);
        emit_rm_mem(&o[1].mem, o[0].reg.num);
        return;
    }
    if (!strcasecmp(mn, "test")) {
        if (n != 2) { as_fail("two operands expected"); return; }
        if (o[0].kind == ASO_REG && o[1].kind == ASO_REG) {
            if (o[0].reg.bits != o[1].reg.bits) { as_fail("operand size mismatch"); return; }
            emit_alu_rr(0x85, o[0].reg.bits == 64, o[1].reg.num, o[0].reg.num);
        } else if (o[0].kind == ASO_REG && o[1].kind == ASO_IMM) {
            int w = (o[0].reg.bits == 64);
            arex(w, 0, 0, o[0].reg.num);
            ae8(0xF7);
            ae8((u8)(0xC0 | (0 << 3) | (o[0].reg.num & 7)));
            emit_imm32(&o[1].imm);
        } else {
            as_fail("bad test operands");
        }
        return;
    }
    if (!strcasecmp(mn, "inc") || !strcasecmp(mn, "dec") ||
        !strcasecmp(mn, "neg") || !strcasecmp(mn, "not") ||
        !strcasecmp(mn, "div") || !strcasecmp(mn, "idiv")) {
        if (n != 1 || (o[0].kind != ASO_REG && o[0].kind != ASO_MEM)) {
            as_fail("one register/memory operand expected");
            return;
        }
        int w = (o[0].kind == ASO_REG) ? (o[0].reg.bits == 64) : 1;
        int group;
        if (!strcasecmp(mn, "inc")) group = 0;
        else if (!strcasecmp(mn, "dec")) group = 1;
        else if (!strcasecmp(mn, "neg")) group = 3;
        else if (!strcasecmp(mn, "not")) group = 2;
        else if (!strcasecmp(mn, "div")) group = 6;
        else group = 7;
        int opcode = (!strcasecmp(mn, "inc") || !strcasecmp(mn, "dec")) ? 0xFF : 0xF7;
        if (o[0].kind == ASO_REG) {
            arex(w, 0, 0, o[0].reg.num);
            ae8((u8)opcode);
            ae8((u8)(0xC0 | (group << 3) | (o[0].reg.num & 7)));
        } else {
            int ix = (o[0].mem.index >= 0) ? o[0].mem.index : 0;
            int bx = (o[0].mem.base >= 0) ? o[0].mem.base : 0;
            arex(w, 0, ix, bx);
            ae8((u8)opcode);
            emit_rm_mem(&o[0].mem, group);
        }
        return;
    }
    if (!strcasecmp(mn, "imul")) {
        if (n != 2 || o[0].kind != ASO_REG) {
            as_fail("imul needs reg, reg/imm");
            return;
        }
        int w = (o[0].reg.bits == 64);
        if (o[1].kind == ASO_REG) {
            if (o[1].reg.bits != o[0].reg.bits) { as_fail("operand size mismatch"); return; }
            arex(w, o[0].reg.num, 0, o[1].reg.num);
            ae8(0x0F); ae8(0xAF);
            ae8((u8)(0xC0 | ((o[0].reg.num & 7) << 3) | (o[1].reg.num & 7)));
        } else if (o[1].kind == ASO_IMM) {
            if (!o[1].imm.is_sym && o[1].imm.add >= -128 && o[1].imm.add <= 127) {
                arex(w, o[0].reg.num, 0, o[0].reg.num);
                ae8(0x6B);
                ae8((u8)(0xC0 | ((o[0].reg.num & 7) << 3) | (o[0].reg.num & 7)));
                ae8((u8)o[1].imm.add);
            } else {
                arex(w, o[0].reg.num, 0, o[0].reg.num);
                ae8(0x69);
                ae8((u8)(0xC0 | ((o[0].reg.num & 7) << 3) | (o[0].reg.num & 7)));
                emit_imm32(&o[1].imm);
            }
        } else {
            as_fail("bad imul operands");
        }
        return;
    }
    if (!strcasecmp(mn, "shl") || !strcasecmp(mn, "sal") ||
        !strcasecmp(mn, "shr") || !strcasecmp(mn, "sar")) {
        if (n != 2 || o[0].kind != ASO_REG || o[1].kind != ASO_IMM) {
            as_fail("shift needs reg, imm");
            return;
        }
        int group = !strcasecmp(mn, "shl") || !strcasecmp(mn, "sal") ? 4
                  : !strcasecmp(mn, "shr") ? 5 : 7;
        int w = (o[0].reg.bits == 64);
        arex(w, 0, 0, o[0].reg.num);
        ae8(0xC1);
        ae8((u8)(0xC0 | (group << 3) | (o[0].reg.num & 7)));
        ae8((u8)o[1].imm.add);
        return;
    }
    if (!strcasecmp(mn, "push") || !strcasecmp(mn, "pop")) {
        if (n != 1 || o[0].kind != ASO_REG) { as_fail("push/pop need a register"); return; }
        if (o[0].reg.bits != 64) { as_fail("push/pop need a 64-bit register"); return; }
        arex(0, 0, 0, o[0].reg.num);
        ae8((u8)((!strcasecmp(mn, "push") ? 0x50 : 0x58) + (o[0].reg.num & 7)));
        return;
    }
    if (!strcasecmp(mn, "call") || !strcasecmp(mn, "jmp")) {
        if (n != 1) { as_fail("one operand expected"); return; }
        int is_call = !strcasecmp(mn, "call");
        if (o[0].kind == ASO_IMM) {
            emit_rel_branch(is_call ? 0xE8 : 0xE9, &o[0].imm);
        } else if (o[0].kind == ASO_REG) {
            arex(0, 0, 0, o[0].reg.num);
            ae8(0xFF);
            ae8((u8)(0xC0 | ((is_call ? 2 : 4) << 3) | (o[0].reg.num & 7)));
        } else {
            as_fail("bad branch operand");
        }
        return;
    }
    if (!strcasecmp(mn, "ret")) { ae8(0xC3); return; }
    if (!strcasecmp(mn, "leave")) { ae8(0xC9); return; }
    if (!strcasecmp(mn, "nop")) { ae8(0x90); return; }
    if (!strcasecmp(mn, "hlt")) { ae8(0xF4); return; }
    if (!strcasecmp(mn, "cqo")) { ae8(0x48); ae8(0x99); return; }
    if (!strcasecmp(mn, "cdq")) { ae8(0x99); return; }
    if (!strcasecmp(mn, "int")) {
        if (n != 1 || o[0].kind != ASO_IMM || o[0].imm.is_sym) {
            as_fail("int needs an immediate");
            return;
        }
        ae8(0xCD);
        ae8((u8)o[0].imm.add);
        return;
    }
    if (!strcasecmp(mn, "xchg")) {
        if (n != 2 || o[0].kind != ASO_REG || o[1].kind != ASO_REG) {
            as_fail("xchg needs two registers");
            return;
        }
        if (o[0].reg.bits != o[1].reg.bits) { as_fail("operand size mismatch"); return; }
        emit_alu_rr(0x87, o[0].reg.bits == 64, o[1].reg.num, o[0].reg.num);
        return;
    }
    as_fail2("unknown instruction: ", mn);
}

/* ---------------------------------------------------------------- 数据 */
static u32 as_out_pos(void)
{
    return (as_sec == 0) ? as_clen : (AS_CODE_MAX + as_dlen);
}

static void emit_data(struct acur *c, int size)
{
    for (;;) {
        skip_ws(c);
        if (c->p >= c->end)
            return;
        bool done = false;
        if (size == 1) {
            u8 buf[512];
            int len = 0;
            if (read_str(c, buf, (int)sizeof(buf), &len)) {
                for (int i = 0; i < len; i++)
                    ae8(buf[i]);
                done = true;
            }
        }
        if (!done) {
            struct as_val v;
            if (!parse_val_expr(c, &v))
                return;
            if (size == 1) {
                if (v.is_sym) { as_fail("byte cannot be a label"); return; }
                ae8((u8)v.add);
            } else if (size == 2) {
                if (v.is_sym) { as_fail("word cannot be a label"); return; }
                ae16((u16)v.add);
            } else if (size == 4) {
                if (v.is_sym) {
                    add_fix(2, as_out_pos(), v.add, v.name);
                    ae32(0);
                } else {
                    ae32((u32)v.add);
                }
            } else {
                if (v.is_sym) {
                    add_fix(1, as_out_pos(), v.add, v.name);
                    ae64(0);
                } else {
                    ae64((u64)v.add);
                }
            }
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        return;
    }
}

/* ---------------------------------------------------------------- 标签 */
static struct as_label *find_label(const char *name)
{
    for (int i = 0; i < as_nlab; i++)
        if (!strcmp(as_lab[i].name, name))
            return &as_lab[i];
    return 0;
}

static void define_label(const char *name)
{
    if (find_label(name)) { as_fail2("duplicate label: ", name); return; }
    if (as_nlab >= AS_MAX_LABEL) { as_fail("too many labels"); return; }
    struct as_label *l = &as_lab[as_nlab++];
    strncpy(l->name, name, 31);
    l->name[31] = 0;
    l->sec = as_sec;
    l->off = (as_sec == 0) ? as_clen : as_dlen;
}

static void define_const(const char *name, i64 v)
{
    if (find_const(name)) { as_fail2("duplicate constant: ", name); return; }
    if (as_ncon >= AS_MAX_CONST) { as_fail("too many constants"); return; }
    struct as_const *k = &as_con[as_ncon++];
    strncpy(k->name, name, 31);
    k->name[31] = 0;
    k->val = v;
}

/* ---------------------------------------------------------------- 语句 */
static void as_statement(struct acur *c);

static void section_directive(struct acur *c, const char *w)
{
    /* w 可能是 ".text" / ".data" / ".section" / "section" */
    if (!strcasecmp(w, ".text")) { as_sec = 0; return; }
    if (!strcasecmp(w, ".data")) { as_sec = 1; return; }
    if (!strcasecmp(w, ".section") || !strcasecmp(w, "section")) {
        char n[32];
        if (!read_id(c, n, sizeof(n))) { as_fail("section name expected"); return; }
        if (!strcasecmp(n, ".text") || !strcasecmp(n, "text")) as_sec = 0;
        else if (!strcasecmp(n, ".data") || !strcasecmp(n, "data")) as_sec = 1;
        else if (!strcasecmp(n, ".rodata") || !strcasecmp(n, "rodata")) as_sec = 1;
        else as_fail2("unknown section: ", n);
        return;
    }
    if (!strcasecmp(w, ".globl") || !strcasecmp(w, "global")) {
        char n[32];
        read_id(c, n, sizeof(n));
        return;
    }
    if (!strcasecmp(w, ".byte")) { emit_data(c, 1); return; }
    if (!strcasecmp(w, ".word")) { emit_data(c, 2); return; }
    if (!strcasecmp(w, ".long")) { emit_data(c, 4); return; }
    if (!strcasecmp(w, ".quad")) { emit_data(c, 8); return; }
    as_fail2("unknown directive: ", w);
}

static void as_statement(struct acur *c)
{
    skip_ws(c);
    if (c->p >= c->end || as_failed)
        return;

    if (*c->p == '.') {
        char w[32];
        if (!read_id(c, w, sizeof(w))) { c->p++; return; }
        section_directive(c, w);
        return;
    }

    char w[32];
    if (!read_id(c, w, sizeof(w))) {
        as_fail("label or instruction expected");
        return;
    }

    skip_ws(c);
    if (c->p < c->end && *c->p == ':') {
        c->p++;
        define_label(w);
        as_statement(c);
        return;
    }
    {
        /* 常量定义: name equ expr */
        const char *save = c->p;
        char w2[32];
        if (read_id(c, w2, sizeof(w2)) && !strcasecmp(w2, "equ")) {
            struct as_val v;
            if (!parse_val_expr(c, &v))
                return;
            if (v.is_sym) { as_fail("equ value must be constant"); return; }
            define_const(w, v.add);
            return;
        }
        c->p = save;
    }

    if (!strcasecmp(w, "section") || !strcasecmp(w, ".section")) {
        section_directive(c, w);
        return;
    }
    if (!strcasecmp(w, "db") || !strcasecmp(w, "byte")) {
        emit_data(c, 1);
        return;
    }
    if (!strcasecmp(w, "dw") || !strcasecmp(w, "word")) {
        emit_data(c, 2);
        return;
    }
    if (!strcasecmp(w, "dd") || !strcasecmp(w, "long")) {
        emit_data(c, 4);
        return;
    }
    if (!strcasecmp(w, "dq") || !strcasecmp(w, "quad")) {
        emit_data(c, 8);
        return;
    }
    if (!strcasecmp(w, "times")) {
        struct as_val cnt;
        if (!parse_val_expr(c, &cnt) || cnt.is_sym) {
            as_fail("times needs a constant count");
            return;
        }
        skip_ws(c);
        const char *sub = c->p;
        char d[32];
        struct acur tmp;
        tmp.p = sub;
        tmp.end = c->end;
        if (!read_id(&tmp, d, sizeof(d))) { as_fail("times needs a directive"); return; }
        int size = 0;
        if (!strcasecmp(d, "db") || !strcasecmp(d, "byte")) size = 1;
        else if (!strcasecmp(d, "dw") || !strcasecmp(d, "word")) size = 2;
        else if (!strcasecmp(d, "dd") || !strcasecmp(d, "long")) size = 4;
        else if (!strcasecmp(d, "dq") || !strcasecmp(d, "quad")) size = 8;
        if (!size) { as_fail("times supports db/dw/dd/dq"); return; }
        for (i64 i = 0; i < cnt.add && !as_failed; i++) {
            struct acur t2;
            t2.p = sub;
            t2.end = c->end;
            read_id(&t2, d, sizeof(d));
            emit_data(&t2, size);
        }
        return;
    }

    /* 普通指令: 解析逗号分隔的操作数 */
    struct as_op ops[3];
    int n = 0;
    skip_ws(c);
    if (c->p < c->end) {
        for (;;) {
            if (n >= 3) { as_fail("too many operands"); return; }
            if (!parse_operand(c, &ops[n]))
                return;
            n++;
            skip_ws(c);
            if (c->p < c->end && *c->p == ',') { c->p++; continue; }
            break;
        }
    }
    emit_insn(w, ops, n);
}

/* ---------------------------------------------------------------- 重定位 */
static bool resolve_fixups(void)
{
    for (int i = 0; i < as_nfix && !as_failed; i++) {
        struct as_fix *f = &as_fix[i];
        struct as_label *l = find_label(f->name);
        if (!l) { as_fail2("undefined label: ", f->name); return false; }
        u64 addr = (l->sec == 0 ? AS_CODE_BASE : AS_DATA_BASE) + l->off;
        if (f->kind == 0) {
            if (l->sec != 0) { as_fail2("not a text label: ", f->name); return false; }
            i64 rel = (i64)l->off + f->add - (i64)(f->pos + 4);
            aw32(f->pos, (u32)rel);
        } else if (f->kind == 1) {
            aw64(f->pos, addr + (u64)f->add);
        } else {
            i64 v = (i64)addr + f->add;
            if (v < -2147483648LL || v > 2147483647LL) {
                as_fail2("address out of 32-bit range: ", f->name);
                return false;
            }
            aw32(f->pos, (u32)v);
        }
    }
    return !as_failed;
}

/* ---------------------------------------------------------------- 入口 */
const u8 *asm_assemble(const char *src, u32 len, u32 *out_len)
{
    as_clen = 0;
    as_dlen = 0;
    as_sec = 0;
    as_nlab = 0;
    as_ncon = 0;
    as_nfix = 0;
    as_line = 0;
    as_failed = false;
    as_err[0] = 0;

    const char *p = src;
    const char *limit = src + len;
    while (p < limit && !as_failed) {
        const char *ls = p, *le = p;
        while (le < limit && *le != '\n')
            le++;
        p = (le < limit) ? le + 1 : limit;
        as_line++;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ' ||
                           le[-1] == '\t'))
            le--;
        /* 去注释 (字符串内的 ; 与 // 不算) */
        const char *ce = le;
        bool q = false;
        for (const char *q2 = ls; q2 < le; q2++) {
            char ch = *q2;
            if (q) {
                if (ch == '\\' && q2 + 1 < le) q2++;
                else if (ch == '"') q = false;
            } else if (ch == '"') {
                q = true;
            } else if (ch == ';') {
                ce = q2;
                break;
            } else if (ch == '/' && q2 + 1 < le && q2[1] == '/') {
                ce = q2;
                break;
            }
        }
        while (ce > ls && (ce[-1] == ' ' || ce[-1] == '\t'))
            ce--;
        struct acur c;
        c.p = ls;
        c.end = ce;
        as_statement(&c);
    }

    if (as_failed)
        return 0;
    if (as_clen == 0) {
        as_fail("empty program");
        return 0;
    }
    if (!resolve_fixups())
        return 0;
    if (as_failed)
        return 0;

    if (out_len)
        *out_len = AS_CODE_MAX + as_dlen;
    return as_out;
}
