/* ==========================================================================
 *  GT-DOS 控制台: VGA + 串口双路输出 / ANSI 彩色 / 简化 printf
 *  ---------------------------------------------------------------------------
 *  console_putc 是一个 ANSI 状态机:
 *    - 串口路径: ESC 序列原样透传, 由宿主终端渲染颜色
 *    - VGA  路径: 解析 SGR (ESC[...m), 转换成 16 色属性
 *  两条路径共享同一套颜色状态, 因此屏幕与终端看到的颜色一致。
 * ========================================================================== */
#include "gt.h"

static bool ser_on = true;

/* ---------------------------------------------------------- 颜色状态机 */
enum { ST_TEXT = 0, ST_ESC, ST_CSI, ST_CSI_PRIV };

static int  st;
static int  csi_n;                       /* 已收集参数个数 */
static int  csi_p[8];
static bool csi_has;                     /* 当前参数是否已有数字 */

static u8  cur_fg = C_LGRAY;
static u8  cur_bg = C_BLACK;
static bool cur_bold = false;

static u8 compose(void)
{
    u8 fg = cur_fg;
    if (cur_bold && fg <= C_DGRAY)
        fg = (u8)(fg + 8);
    return (u8)((cur_bg << 4) | (fg & 0x0F));
}

static void apply_attr(void)
{
    u8 a = compose();
    vga_set_color((u8)(a & 0x0F), (u8)(a >> 4));
    vga_set_attr_default(a);
}

/* 颜色函数统一走 console_putc: 串口原样透传 ANSI, VGA 解析成 16 色属性,
 * 两条路径共享同一状态机, 因此屏幕与终端颜色始终一致。 */
static void emit_sgr(const char *seq)
{
    console_puts("\x1b[");
    console_puts(seq);
    console_putc('m');
}

void gt_color(u8 fg)
{
    char s[8];
    int v = (fg & 0x07);
    if (fg >= 8) { s[0] = '9'; s[1] = (char)('0' + v); s[2] = 0; }
    else         { s[0] = '3'; s[1] = (char)('0' + v); s[2] = 0; }
    emit_sgr(s);
}

void gt_color2(u8 fg, u8 bg)
{
    char s[12];
    int f = fg & 0x07, b = bg & 0x07;
    int k = 0;
    if (fg >= 8) { s[k++] = '9'; s[k++] = (char)('0' + f); }
    else         { s[k++] = '3'; s[k++] = (char)('0' + f); }
    s[k++] = ';';
    if (bg >= 8) { s[k++] = '1'; s[k++] = '0'; s[k++] = (char)('0' + b); }
    else         { s[k++] = '4'; s[k++] = (char)('0' + b); }
    s[k] = 0;
    emit_sgr(s);
}

void gt_bold(bool on)
{
    emit_sgr(on ? "1" : "22");
}

void gt_color_reset(void)
{
    emit_sgr("0");
}

/* ANSI SGR 参数 -> VGA 16 色 */
static void sgr_apply(int *p, int n)
{
    if (n == 0) {
        p = (int[]){ 0 };
        n = 1;
    }
    for (int i = 0; i < n; i++) {
        int v = p[i];
        if (v == 0) {
            cur_fg = C_LGRAY;
            cur_bg = C_BLACK;
            cur_bold = false;
        } else if (v == 1) {
            cur_bold = true;
        } else if (v == 22) {
            cur_bold = false;
        } else if (v >= 30 && v <= 37) {
            cur_fg = (u8)(v - 30);
        } else if (v == 39) {
            cur_fg = C_LGRAY;
        } else if (v >= 40 && v <= 47) {
            cur_bg = (u8)(v - 40);
        } else if (v == 49) {
            cur_bg = C_BLACK;
        } else if (v >= 90 && v <= 97) {
            cur_bold = true;
            cur_fg = (u8)(v - 90);
        }
    }
    apply_attr();
}

/* ------------------------------------------------------------- 基础输出 */
void console_init(void)
{
    serial_init();
    vga_init();
    ser_on = true;
    st = ST_TEXT;
    csi_n = 0;
    cur_fg = C_LGRAY;
    cur_bg = C_BLACK;
    cur_bold = false;
    apply_attr();
}

void console_set_serial(bool on) { ser_on = on; }
bool console_serial_enabled(void) { return ser_on; }

/* 串口透传一个字节 (含 ESC) */
static void ser_putc(char c)
{
    if (ser_on)
        serial_putc(c);
}

void console_putc(char c)
{
    /* ---- 串口: 原样透传 ---- */
    ser_putc(c);

    /* ---- VGA: ANSI 解析 ---- */
    switch (st) {
    case ST_TEXT:
        if (c == 0x1B) {
            st = ST_ESC;
            return;
        }
        vga_putc(c);
        return;

    case ST_ESC:
        if (c == '[') {
            st = ST_CSI;
            csi_n = 0;
            csi_has = false;
        } else {
            st = ST_TEXT;
            vga_putc(c);
        }
        return;

    case ST_CSI:
        if (c == '?') {              /* 私有序列 (如 ESC[?25l): 吞到终止字节 */
            st = ST_CSI_PRIV;
            return;
        }
        if (c >= '0' && c <= '9') {
            if (!csi_has && csi_n < 8) {
                csi_p[csi_n++] = 0;
                csi_has = true;
            }
            if (csi_n > 0) {
                int idx = csi_n - 1;
                int nv = csi_p[idx] * 10 + (c - '0');
                csi_p[idx] = (nv > 99999) ? 99999 : nv;
            }
            return;
        }
        if (c == ';') {
            csi_has = false;
            if (csi_n < 8)
                csi_p[csi_n++] = 0;   /* 空参数保持 0 */
            return;
        }
        /* 终止字节 */
        if (c == 'm')
            sgr_apply(csi_p, csi_n);
        else if (c == 'J')
            vga_clear();
        else if (c == 'K')
            vga_erase_eol();
        st = ST_TEXT;
        return;

    case ST_CSI_PRIV:
        /* 私有序列: 吞掉数字/分号, 遇到字母终止 */
        if ((c >= '0' && c <= '9') || c == ';' || c == '<' || c == '>')
            return;
        st = ST_TEXT;
        return;

    default:
        st = ST_TEXT;
        return;
    }
}

void console_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        console_putc(s[i]);
}

void console_puts(const char *s)
{
    while (*s)
        console_putc(*s++);
}

void console_clear(void) { vga_clear(); }

/* --------------------------------------------------------- 行编辑器原语 */
/* 把 prompt+buf 重绘到当前行, 光标停在 prompt_len+cur; 串口用 ANSI 控制序列 */
void console_line_refresh(const char *prompt, const char *buf, int len,
                          int cur, int prev_len)
{
    int plen = (int)strlen(prompt);
    int pcols = vga_utf8_cols(prompt);     /* 提示符可能是 UTF-8 中文 */

    if (ser_on) {
        /* 回到行首 + 清除到行尾, 然后重打整行 */
        serial_putc('\r');
        serial_write("\x1b[K", 3);
        serial_write("\x1b[92m", 5);         /* 提示符: 亮绿 */
        serial_write(prompt, (size_t)plen);
        serial_write("\x1b[0m\x1b[37m", 9);  /* 输入: 白 */
        serial_write(buf, (size_t)len);
        int back = len - cur;
        if (back > 0) {
            char mv[16];
            int k = 0;
            mv[k++] = 0x1B;
            mv[k++] = '[';
            char num[8];
            itoa_base((u32)back, num, 10, false);
            for (char *q = num; *q; q++)
                mv[k++] = *q;
            mv[k++] = 'D';
            serial_write(mv, (size_t)k);
        }
    }

    vga_line_reset();
    vga_set_color(C_LGREEN, C_BLACK);        /* 提示符: 亮绿 */
    vga_write_raw(prompt, plen);
    vga_set_color(C_WHITE, C_BLACK);         /* 输入: 白 */
    vga_write_raw(buf, len);
    /* 恢复控制台逻辑颜色, 供后续普通输出使用 */
    vga_set_color((u8)(compose() & 0x0F), (u8)(compose() >> 4));
    vga_set_col(pcols + cur);
    (void)prev_len;
}

void console_line_end(void)
{
    if (ser_on)
        serial_write("\r\n", 2);
    vga_putc('\n');
}

/* --------------------------------------------------------------- printf */
static int fmt_unsigned(char *buf, u32 v, int base, bool upper)
{
    static const char *lo = "0123456789abcdef";
    static const char *up = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    char tmp[32];
    int n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v) {
            tmp[n++] = digits[v % (u32)base];
            v /= (u32)base;
        }
    }
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return n;
}

static void pad_out(int written, int width, char pad)
{
    while (written < width) {
        console_putc(pad);
        written++;
    }
}

int kvprintf(const char *fmt, va_list ap)
{
    int count = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            console_putc(*p);
            count++;
            continue;
        }
        p++;
        if (*p == '\0')
            break;

        bool left = false;
        char pad = ' ';
        int  width = 0;

        if (*p == '-') { left = true; p++; }
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        char cbuf[64];
        int  n = 0;

        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (left) {
                console_write(s, (size_t)len);
                pad_out(len, width, ' ');
            } else {
                pad_out(len, width, pad);
                console_write(s, (size_t)len);
            }
            count += (len > width ? len : width);
            continue;
        }
        case 'c':
            console_putc((char)va_arg(ap, int));
            count++;
            continue;
        case 'd':
        case 'i': {
            i32 v = va_arg(ap, i32);
            if (v < 0) {
                console_putc('-');
                count++;
                n = fmt_unsigned(cbuf, (u32)(-(i64)v), 10, false);
            } else {
                n = fmt_unsigned(cbuf, (u32)v, 10, false);
            }
            break;
        }
        case 'u':
            n = fmt_unsigned(cbuf, va_arg(ap, u32), 10, false);
            break;
        case 'x':
            n = fmt_unsigned(cbuf, va_arg(ap, u32), 16, false);
            break;
        case 'X':
            n = fmt_unsigned(cbuf, va_arg(ap, u32), 16, true);
            break;
        case 'p': {
            u64 v = (u64)va_arg(ap, void *);
            static const char *hexd = "0123456789abcdef";
            cbuf[0] = '0';
            cbuf[1] = 'x';
            for (int i = 0; i < 16; i++)
                cbuf[2 + i] = hexd[(v >> (60 - 4 * i)) & 0xF];
            n = 18;
            break;
        }
        case '%':
            console_putc('%');
            count++;
            continue;
        default:
            console_putc('%');
            console_putc(*p);
            count += 2;
            continue;
        }

        if (left) {
            console_write(cbuf, (size_t)n);
            pad_out(n, width, ' ');
        } else {
            pad_out(n, width, pad);
            console_write(cbuf, (size_t)n);
        }
        count += (n > width ? n : width);
    }
    return count;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}