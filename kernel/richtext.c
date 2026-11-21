/* 富文本渲染 (Markdown 子集 + 内联标签).
   输出走 console_putc / gt_color / gt_bold, 因此 VGA 与串口都得到一致的
   颜色与样式; 适合 RICH/CAT 显示文档、说明与表格.

   支持:
     * ATX 标题 (#..######) 与 Setext 标题 (=== / ---)
     * 无序/有序/任务列表, 2 空格缩进嵌套
     * 多级引用 (> / >>), 分隔线 (--- *** ___)
     * 围栏代码块 ```lang (C 系语言做简单高亮) 与 4 空格缩进代码
     * 表格: 自动列宽 + :---: / ---: 对齐 + 表头
     * 行内: **粗体** __粗体__ *斜体* _斜体_ ***粗斜***
             ~~删除线~~ `代码` [文字](链接) ![alt](src)
             <b> <i> <c=red> </c> <br> */
#include "gt.h"

#define RICH_MAX_COLS 10

struct rstyle {
    bool bold, italic, code, strike;
    int  ovr;                        /* 覆盖色 (<c=...>), -1 = 无 */
};

struct tcell {
    const char *p;
    int n;
};

static bool is_ws_(char c) { return c == ' ' || c == '\t'; }

static int cols_of_console(void)
{
    if (gfx_active()) {
        int w = gfx_cell_w();
        if (w > 0)
            return gfx_w() / w;
    }
    return VGA_COLS;
}

static void put_repeat(char c, int n)
{
    while (n-- > 0)
        console_putc(c);
}

static void rich_reset(void)
{
    gt_bold(false);
    gt_color_reset();
    gt_color(C_LGRAY);
}

static u8 color_by_name(const char *s, int n)
{
    static const struct { const char *n; u8 c; } tab[] = {
        { "black", C_BLACK }, { "red", C_LRED }, { "green", C_LGREEN },
        { "blue", C_LBLUE }, { "yellow", C_YELLOW }, { "cyan", C_LCYAN },
        { "magenta", C_LMAGENTA }, { "white", C_WHITE }, { "gray", C_DGRAY },
        { "grey", C_DGRAY }, { "lgray", C_LGRAY }
    };
    for (int i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++) {
        int k = 0;
        while (k < n && tab[i].n[k] && s[k] == tab[i].n[k])
            k++;
        if (k == n && tab[i].n[k] == 0)
            return tab[i].c;
    }
    return C_LGRAY;
}

/* ---------------------------------------------------------- UTF-8 宽度 */
static int utf8_n(u8 c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static u32 utf8_cp(const u8 *p, int n)
{
    if (n <= 0) return 0;
    if (n == 1) return p[0];
    u32 v = p[0] & ((1u << (7 - n)) - 1);
    for (int i = 1; i < n; i++)
        v = (v << 6) | (p[i] & 0x3F);
    return v;
}

static int cp_wide(u32 cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) ||
           (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

/* 字符串显示宽度 (CJK 记 2 列) */
static int disp_len_(const char *s, int n)
{
    int w = 0, i = 0;
    const u8 *p = (const u8 *)s;
    while (i < n) {
        int k = utf8_n(p[i]);
        if (i + k > n) k = 1;
        w += cp_wide(utf8_cp(p + i, k)) ? 2 : 1;
        i += k;
    }
    return w;
}

/* ------------------------------------------------------------ 行内样式 */
static void style_apply(const struct rstyle *st)
{
    u8 c;
    if (st->code) c = C_YELLOW;
    else if (st->strike) c = C_DGRAY;
    else if (st->bold) c = C_WHITE;
    else if (st->italic) c = C_LCYAN;
    else c = C_LGRAY;
    if (st->ovr >= 0)
        c = (u8)st->ovr;
    gt_bold(st->bold || st->code);
    gt_color(c);
}

static bool tag_is(const char *t, int n, const char *w)
{
    int i = 0;
    while (i < n && w[i] && t[i] == w[i])
        i++;
    return i == n && w[i] == 0;
}

static void rich_inline(const char *s, int n)
{
    struct rstyle st;
    memset(&st, 0, sizeof(st));
    st.ovr = -1;
    style_apply(&st);

    for (int i = 0; i < n;) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            console_putc(s[i + 1]);
            i += 2;
            continue;
        }
        /* 行内代码: 原样输出 */
        if (c == '`') {
            int j = i + 1;
            while (j < n && s[j] != '`')
                j++;
            gt_bold(false);
            gt_color(C_YELLOW);
            for (int q = i + 1; q < j; q++)
                console_putc(s[q]);
            style_apply(&st);
            i = (j < n) ? j + 1 : n;
            continue;
        }
        if (c == '*' || c == '_') {
            char d = c;
            int run = 1;
            while (i + run < n && s[i + run] == d && run < 3)
                run++;
            if (run == 3) {
                st.bold = !st.bold;
                st.italic = !st.italic;
            } else if (run == 2) {
                st.bold = !st.bold;
            } else {
                st.italic = !st.italic;
            }
            i += run;
            style_apply(&st);
            continue;
        }
        if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            st.strike = !st.strike;
            i += 2;
            style_apply(&st);
            continue;
        }
        /* 图片 ![alt](src) */
        if (c == '!' && i + 1 < n && s[i + 1] == '[') {
            int j = i + 2;
            while (j < n && s[j] != ']')
                j++;
            if (j < n) {
                int k = j + 1, close = -1;
                if (k < n && s[k] == '(') {
                    close = k + 1;
                    while (close < n && s[close] != ')')
                        close++;
                    if (close >= n) close = -1;
                }
                gt_bold(false);
                gt_color(C_LMAGENTA);
                console_puts("[img: ");
                for (int q = i + 2; q < j; q++)
                    console_putc(s[q]);
                console_putc(']');
                style_apply(&st);
                i = (close >= 0) ? close + 1 : j + 1;
                continue;
            }
        }
        /* 链接 [文字](地址) */
        if (c == '[') {
            int j = i + 1;
            while (j < n && s[j] != ']')
                j++;
            if (j < n && j + 1 < n && s[j + 1] == '(') {
                int k = j + 2;
                while (k < n && s[k] != ')')
                    k++;
                if (k < n) {
                    gt_bold(true);
                    gt_color(C_LBLUE);
                    for (int q = i + 1; q < j; q++)
                        console_putc(s[q]);
                    gt_bold(false);
                    gt_color(C_DGRAY);
                    console_puts(" (");
                    for (int q = j + 2; q < k; q++)
                        console_putc(s[q]);
                    console_putc(')');
                    style_apply(&st);
                    i = k + 1;
                    continue;
                }
            }
        }
        /* 内联 HTML 风格标签 */
        if (c == '<') {
            int e = -1;
            for (int q = i + 1; q < n && q < i + 24; q++) {
                if (s[q] == '>') { e = q; break; }
            }
            if (e > i) {
                const char *t = s + i + 1;
                int tl = e - i - 1;
                bool handled = true;
                if (tag_is(t, tl, "b")) st.bold = true;
                else if (tag_is(t, tl, "/b")) st.bold = false;
                else if (tag_is(t, tl, "i")) st.italic = true;
                else if (tag_is(t, tl, "/i")) st.italic = false;
                else if (tl >= 2 && t[0] == 'c' && t[1] == '=')
                    st.ovr = color_by_name(t + 2, tl - 2);
                else if (tag_is(t, tl, "/c")) st.ovr = -1;
                else if (tag_is(t, tl, "br")) console_putc('\n');
                else handled = false;
                if (handled) {
                    i = e + 1;
                    style_apply(&st);
                    continue;
                }
            }
        }
        console_putc(c);
        i++;
    }
    rich_reset();
}

/* ------------------------------------------------------------ 代码高亮 */
static bool code_kw(const char *w, int n)
{
    static const char *kw[] = {
        "int", "char", "void", "double", "float", "struct", "union",
        "if", "else", "while", "for", "do", "switch", "case", "default",
        "break", "continue", "return", "goto", "typedef", "enum", "sizeof",
        "const", "unsigned", "signed", "static", "extern", "long", "short",
        "inline", "include", "define", NULL
    };
    for (int i = 0; kw[i]; i++) {
        int k = 0;
        while (k < n && kw[i][k] && w[k] == kw[i][k])
            k++;
        if (k == n && kw[i][k] == 0)
            return true;
    }
    return false;
}

static bool lang_clike(const char *lang)
{
    if (!lang[0]) return true;
    return !strcasecmp(lang, "c") || !strcasecmp(lang, "h") ||
           !strcasecmp(lang, "cc") || !strcasecmp(lang, "cpp") ||
           !strcasecmp(lang, "java") || !strcasecmp(lang, "js") ||
           !strcasecmp(lang, "rust") || !strcasecmp(lang, "go");
}

static void code_out(const char *s, int n, const char *lang)
{
    bool clike = lang_clike(lang);
    gt_bold(false);
    gt_color(C_LGREEN);
    for (int i = 0; i < n;) {
        char c = s[i];
        if (clike && c == '/' && i + 1 < n && s[i + 1] == '/') {
            gt_color(C_DGRAY);
            for (; i < n; i++)
                console_putc(s[i]);
            break;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            gt_color(C_LGREEN);
            console_putc(c);
            i++;
            while (i < n) {
                console_putc(s[i]);
                if (s[i] == '\\' && i + 1 < n) {
                    i++;
                    console_putc(s[i]);
                } else if (s[i] == q) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (c >= '0' && c <= '9') {
            gt_color(C_YELLOW);
            while (i < n &&
                   ((s[i] >= '0' && s[i] <= '9') ||
                    (s[i] >= 'a' && s[i] <= 'f') ||
                    (s[i] >= 'A' && s[i] <= 'F') ||
                    s[i] == 'x' || s[i] == 'X' || s[i] == '.')) {
                console_putc(s[i++]);
            }
            continue;
        }
        if (isalpha_((u8)c) || c == '_') {
            int j = i, wn = 0;
            char w[24];
            while (j < n && (isalpha_((u8)s[j]) || isdigit_((u8)s[j]) ||
                             s[j] == '_')) {
                if (wn < 23) w[wn++] = s[j];
                j++;
            }
            w[wn] = 0;
            gt_color((clike && code_kw(w, wn)) ? C_LCYAN : C_LGREEN);
            for (int q = i; q < j; q++)
                console_putc(s[q]);
            i = j;
            continue;
        }
        gt_color(C_LGREEN);
        console_putc(c);
        i++;
    }
    rich_reset();
}

/* -------------------------------------------------------------- 表格 */
static int split_cells(const char *s, int n, struct tcell *out, int max)
{
    int k = 0, start = 0, i;
    if (n > 0 && s[0] == '|')
        start = 1;
    i = start;
    while (i <= n) {
        if (i == n || s[i] == '|') {
            int a = start, b = i;
            while (a < b && is_ws_(s[a])) a++;
            while (b > a && is_ws_(s[b - 1])) b--;
            if (k < max) {
                out[k].p = s + a;
                out[k].n = b - a;
                k++;
            }
            start = i + 1;
        }
        i++;
    }
    if (k > 0 && out[k - 1].n == 0 && n > 0 && s[n - 1] == '|')
        k--;
    return k;
}

static bool sep_cells(const struct tcell *cs, int nc)
{
    if (nc == 0) return false;
    bool dash = false;
    for (int c = 0; c < nc; c++) {
        for (int i = 0; i < cs[c].n; i++) {
            char ch = cs[c].p[i];
            if (ch == '-') dash = true;
            else if (ch != ':' && ch != ' ')
                return false;
        }
    }
    return dash;
}

static int align_of(const struct tcell *c)
{
    if (c->n == 0) return 0;
    bool l = c->p[0] == ':';
    bool r = c->p[c->n - 1] == ':';
    if (l && r) return 2;             /* 居中 */
    if (r) return 1;                  /* 右对齐 */
    return 0;
}

static void table_border(const int *w, int nc, char fill)
{
    gt_color(C_LCYAN);
    console_putc('+');
    for (int c = 0; c < nc; c++) {
        put_repeat(fill, w[c] + 2);
        console_putc('+');
    }
    console_putc('\n');
    rich_reset();
}

static void table_row(const struct tcell *cs, int nc, const int *w,
                      const int *al, bool header)
{
    gt_color(C_LCYAN);
    console_putc('|');
    for (int c = 0; c < nc; c++) {
        int len = disp_len_(cs[c].p, cs[c].n);
        int pad = w[c] - len;
        if (pad < 0) pad = 0;
        int l = 0, r = pad;
        if (al[c] == 2) { l = pad / 2; r = pad - l; }
        else if (al[c] == 1) { l = pad; r = 0; }
        console_putc(' ');
        for (int k = 0; k < l; k++)
            console_putc(' ');
        gt_bold(header);
        if (header) gt_color(C_WHITE);
        rich_inline(cs[c].p, cs[c].n);
        gt_bold(false);
        for (int k = 0; k < r; k++)
            console_putc(' ');
        gt_color(C_LCYAN);
        console_putc(' ');
        console_putc('|');
    }
    console_putc('\n');
    rich_reset();
}

/* 渲染从 p 开始的一组表格行, 返回下一行偏移 */
static u32 render_table(const char *text, u32 len, u32 p, int console_cols)
{
    int w[RICH_MAX_COLS];
    int al[RICH_MAX_COLS];
    int ncol = 0;
    for (int c = 0; c < RICH_MAX_COLS; c++) {
        w[c] = 1;
        al[c] = 0;
    }

    u32 pe = p, rp = p;
    while (rp < len) {
        u32 q = rp;
        while (q < len && text[q] != '\n')
            q++;
        int n = (int)(q - rp);
        while (n > 0 && text[rp + n - 1] == '\r')
            n--;
        const char *s = text + rp;
        if (n == 0 || s[0] != '|')
            break;
        struct tcell cs[RICH_MAX_COLS];
        int nc = split_cells(s, n, cs, RICH_MAX_COLS);
        if (nc > ncol)
            ncol = nc;
        for (int c = 0; c < nc; c++) {
            int cw = disp_len_(cs[c].p, cs[c].n);
            if (cw > w[c]) w[c] = cw;
        }
        if (sep_cells(cs, nc)) {
            for (int c = 0; c < nc; c++)
                al[c] = align_of(&cs[c]);
        }
        pe = (q < len) ? q + 1 : len;
        rp = pe;
    }
    if (ncol == 0) {
        console_putc('\n');
        return pe;
    }

    /* 控制总宽, 超宽时压缩最宽列 */
    for (;;) {
        int total = 1;
        for (int c = 0; c < ncol; c++)
            total += w[c] + 3;
        if (total <= console_cols)
            break;
        int mi = 0;
        for (int c = 1; c < ncol; c++)
            if (w[c] > w[mi]) mi = c;
        if (w[mi] <= 3)
            break;
        w[mi]--;
    }

    table_border(w, ncol, '-');
    rp = p;
    int ri = 0;
    while (rp < pe) {
        u32 q = rp;
        while (q < len && text[q] != '\n')
            q++;
        int n = (int)(q - rp);
        while (n > 0 && text[rp + n - 1] == '\r')
            n--;
        const char *s = text + rp;
        struct tcell cs[RICH_MAX_COLS];
        int nc = split_cells(s, n, cs, RICH_MAX_COLS);
        if (sep_cells(cs, nc)) {
            table_border(w, ncol, '=');
        } else {
            table_row(cs, nc, w, al, ri == 0);
            ri++;
        }
        rp = (q < len) ? q + 1 : len;
    }
    table_border(w, ncol, '-');
    return pe;
}

/* -------------------------------------------------------------- 行分类 */
static bool all_same(const char *s, int n, char c)
{
    if (n < 3)
        return false;
    for (int i = 0; i < n; i++)
        if (s[i] != c)
            return false;
    return true;
}

static bool is_rule(const char *s, int n)
{
    char c = s[0];
    if (c != '-' && c != '*' && c != '_' && c != '=')
        return false;
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == c) count++;
        else if (!is_ws_(s[i])) return false;
    }
    return count >= 3;
}

void rich_render(const char *text, u32 len, const char *title)
{
    int cols = cols_of_console();
    if (cols < 20)
        cols = 20;

    if (title && title[0]) {
        gt_bold(true);
        gt_color(C_LCYAN);
        console_puts("== ");
        console_puts(title);
        console_putc(' ');
        put_repeat('=', cols > (int)strlen(title) + 6
                          ? cols - (int)strlen(title) - 6 : 3);
        console_putc('\n');
        rich_reset();
    }

    bool code = false;
    char lang[16];
    lang[0] = 0;
    u32 p = 0;
    while (p < len) {
        u32 q = p;
        while (q < len && text[q] != '\n')
            q++;
        int n = (int)(q - p);
        while (n > 0 && text[p + n - 1] == '\r')
            n--;
        const char *s = text + p;

        /* 围栏代码块 */
        if (n >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
            if (!code) {
                code = true;
                int k = 3, ln = 0;
                while (k < n && is_ws_(s[k])) k++;
                while (k + ln < n && !is_ws_(s[k + ln]) && ln < 15) {
                    lang[ln] = s[k + ln];
                    ln++;
                }
                lang[ln] = 0;
                gt_color(C_DGRAY);
                put_repeat('-', cols);
                console_putc('\n');
                if (ln) {
                    gt_color(C_LCYAN);
                    console_puts("  [");
                    console_puts(lang);
                    console_puts("]\n");
                }
            } else {
                code = false;
                gt_color(C_DGRAY);
                put_repeat('-', cols);
                console_putc('\n');
            }
            rich_reset();
            p = q + 1;
            continue;
        }
        if (code) {
            gt_color(C_LGREEN);
            console_puts("  ");
            code_out(s, n, lang);
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }
        if (n == 0) {
            console_putc('\n');
            p = q + 1;
            continue;
        }

        /* 表格 */
        if (s[0] == '|') {
            p = render_table(text, len, p, cols);
            continue;
        }

        /* ATX 标题 */
        int h = 0;
        while (h < n && h < 6 && s[h] == '#')
            h++;
        if (h > 0 && (h == n || s[h] == ' ' || s[h] == '\t')) {
            int a = h;
            while (a < n && is_ws_(s[a]))
                a++;
            int b = n;
            while (b > a && (is_ws_(s[b - 1]) || s[b - 1] == '#'))
                b--;
            u8 col = h == 1 ? C_WHITE :
                     h == 2 ? C_YELLOW :
                     h == 3 ? C_LCYAN :
                     h == 4 ? C_LGREEN :
                     h == 5 ? C_LMAGENTA : C_DGRAY;
            gt_bold(true);
            gt_color(col);
            for (int k = 0; k < h; k++)
                console_putc('#');
            console_putc(' ');
            rich_inline(s + a, b - a);
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }

        /* Setext 标题: 下一行全 = 或 - */
        {
            u32 q2 = q + 1;
            u32 e2 = q2;
            while (e2 < len && text[e2] != '\n')
                e2++;
            int n2 = (int)(e2 - q2);
            const char *s2 = text + q2;
            if (n2 >= 3 && (all_same(s2, n2, '=') || all_same(s2, n2, '-'))) {
                bool h1 = (s2[0] == '=');
                gt_bold(true);
                gt_color(h1 ? C_WHITE : C_YELLOW);
                rich_inline(s, n);
                console_putc('\n');
                rich_reset();
                p = e2 + 1;
                continue;
            }
        }

        /* 分隔线 */
        if (is_rule(s, n)) {
            gt_color(C_DGRAY);
            put_repeat('-', cols);
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }

        /* 引用 (支持 >>) */
        if (s[0] == '>') {
            int depth = 0, i = 0;
            while (i < n && s[i] == '>') {
                depth++;
                i++;
                if (i < n && s[i] == ' ')
                    i++;
            }
            gt_color(C_LCYAN);
            for (int d = 0; d < depth; d++)
                console_puts("| ");
            gt_color(C_LGRAY);
            rich_inline(s + i, n - i);
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }

        /* 4 空格缩进代码 */
        if (n >= 5 && s[0] == ' ' && s[1] == ' ' && s[2] == ' ' &&
            s[3] == ' ' && s[4] != ' ') {
            gt_color(C_LGREEN);
            console_puts("  ");
            code_out(s + 4, n - 4, "");
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }

        /* 列表 / 任务列表 */
        {
            int ind = 0;
            while (ind < n && s[ind] == ' ')
                ind++;
            int level = ind / 2;
            if (level > 3) level = 3;
            if (ind < n) {
                char mk = s[ind];
                bool ul = (mk == '-' || mk == '*' || mk == '+') &&
                          ind + 1 < n && s[ind + 1] == ' ';
                int num = 0, ni = ind;
                while (ni < n && s[ni] >= '0' && s[ni] <= '9') {
                    num = num * 10 + (s[ni] - '0');
                    ni++;
                }
                bool ol = ni > ind && ni + 1 < n &&
                          (s[ni] == '.' || s[ni] == ')') && s[ni + 1] == ' ';
                if (ul || ol) {
                    int a = ul ? ind + 2 : ni + 2;
                    bool task = false, done = false;
                    if (a + 2 < n && s[a] == '[' && s[a + 2] == ']' &&
                        (s[a + 1] == ' ' || s[a + 1] == 'x' ||
                         s[a + 1] == 'X')) {
                        task = true;
                        done = (s[a + 1] == 'x' || s[a + 1] == 'X');
                        a += 3;
                        while (a < n && s[a] == ' ')
                            a++;
                    }
                    for (int k = 0; k < level; k++)
                        console_puts("  ");
                    if (ul) {
                        gt_color(C_YELLOW);
                        console_puts("* ");
                    } else {
                        char nb[12];
                        itoa_base((u32)num, nb, 10, false);
                        gt_color(C_YELLOW);
                        console_puts(nb);
                        console_puts(". ");
                    }
                    if (task) {
                        gt_color(done ? C_LGREEN : C_DGRAY);
                        console_puts(done ? "[x] " : "[ ] ");
                    }
                    gt_color(C_LGRAY);
                    rich_inline(s + a, n - a);
                    console_putc('\n');
                    rich_reset();
                    p = q + 1;
                    continue;
                }
            }
        }

        rich_inline(s, n);
        console_putc('\n');
        rich_reset();
        p = q + 1;
    }
    rich_reset();
}
