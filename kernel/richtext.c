/* 富文本渲染 (Markdown 子集 + 少量内联标签).
   输出走 console_putc / gt_color / gt_bold, 因此 VGA 与串口都得到一致的
   颜色与样式; 适合 TYPE/RICH/CAT 显示文档、说明与表格. */
#include "gt.h"

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

/* 内联样式: **粗体**  *斜体*  `代码`  [文字](链接)
   以及 <b> <i> <c=red> </c> 等简单标签 */
static void rich_inline(const char *s, int n)
{
    bool bold = false, code = false, italic = false;
    for (int i = 0; i < n;) {
        if (s[i] == '\\' && i + 1 < n) {
            console_putc(s[i + 1]);
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            code = !code;
            gt_bold(false);
            gt_color(code ? C_YELLOW : C_LGRAY);
            i++;
            continue;
        }
        if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') {
            bold = !bold;
            gt_bold(bold);
            gt_color(bold ? C_WHITE : (italic ? C_LCYAN : C_LGRAY));
            i += 2;
            continue;
        }
        if (s[i] == '*') {
            italic = !italic;
            gt_bold(bold);
            gt_color(italic ? C_LCYAN : C_LGRAY);
            i++;
            continue;
        }
        if (s[i] == '[') {
            int j = i + 1;
            while (j < n && s[j] != ']')
                j++;
            if (j + 1 < n && s[j] == ']' && s[j + 1] == '(') {
                int k = j + 2;
                while (k < n && s[k] != ')')
                    k++;
                if (k < n) {
                    rich_inline(s + i + 1, j - i - 1);
                    gt_color(C_DGRAY);
                    console_putc(' ');
                    console_putc('(');
                    for (int q = j + 2; q < k; q++)
                        console_putc(s[q]);
                    console_putc(')');
                    gt_color(code ? C_YELLOW : (italic ? C_LCYAN : C_LGRAY));
                    i = k + 1;
                    continue;
                }
            }
        }
        if (s[i] == '<') {
            if (n - i >= 3 && s[i + 1] == 'b' && s[i + 2] == '>') {
                bold = true; gt_bold(true); gt_color(C_WHITE); i += 3; continue;
            }
            if (n - i >= 4 && s[i + 1] == '/' && s[i + 2] == 'b' && s[i + 3] == '>') {
                bold = false; gt_bold(false); gt_color(C_LGRAY); i += 4; continue;
            }
            if (n - i >= 3 && s[i + 1] == 'i' && s[i + 2] == '>') {
                italic = true; gt_color(C_LCYAN); i += 3; continue;
            }
            if (n - i >= 4 && s[i + 1] == '/' && s[i + 2] == 'i' && s[i + 3] == '>') {
                italic = false; gt_color(C_LGRAY); i += 4; continue;
            }
            if (n - i >= 5 && s[i + 1] == 'c' && s[i + 2] == '=') {
                int j = i + 3;
                while (j < n && s[j] != '>')
                    j++;
                if (j < n) {
                    gt_color(color_by_name(s + i + 3, j - i - 3));
                    i = j + 1;
                    continue;
                }
            }
            if (n - i >= 4 && s[i + 1] == '/' && s[i + 2] == 'c' && s[i + 3] == '>') {
                gt_color(code ? C_YELLOW : C_LGRAY);
                i += 4;
                continue;
            }
        }
        console_putc(s[i++]);
    }
    gt_bold(false);
}

static bool only_chars(const char *s, int n, const char *set)
{
    if (n == 0)
        return false;
    for (int i = 0; i < n; i++) {
        bool ok = false;
        for (const char *p = set; *p; p++)
            if (s[i] == *p) { ok = true; break; }
        if (!ok)
            return false;
    }
    return true;
}

static void render_table_row(const char *s, int n)
{
    gt_color(C_LCYAN);
    console_putc('|');
    int cell = 0;
    int start = 1;
    for (int i = 1; i <= n; i++) {
        if (i == n || s[i] == '|') {
            int a = start, b = i;
            while (a < b && s[a] == ' ')
                a++;
            while (b > a && s[b - 1] == ' ')
                b--;
            gt_color((cell & 1) ? C_LGRAY : C_YELLOW);
            for (int q = a; q < b; q++)
                console_putc(s[q]);
            gt_color(C_LCYAN);
            if (i < n)
                console_putc('|');
            cell++;
            start = i + 1;
        }
    }
    console_putc('\n');
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
        gt_bold(false);
        gt_color_reset();
    }

    bool code = false;
    u32 p = 0;
    while (p < len) {
        u32 q = p;
        while (q < len && text[q] != '\n')
            q++;
        int n = (int)(q - p);
        while (n > 0 && text[p + n - 1] == '\r')
            n--;
        const char *s = text + p;

        if (n >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
            code = !code;
            gt_color(C_DGRAY);
            put_repeat('-', cols);
            console_putc('\n');
            gt_color_reset();
            p = q + 1;
            continue;
        }
        if (code) {
            gt_color(C_LGREEN);
            console_puts("  ");
            for (int i = 0; i < n; i++)
                console_putc(s[i]);
            console_putc('\n');
            gt_color_reset();
            p = q + 1;
            continue;
        }

        if (n == 0) {
            console_putc('\n');
            p = q + 1;
            continue;
        }

        /* 标题 */
        int h = 0;
        while (h < n && s[h] == '#' && h < 6)
            h++;
        if (h > 0 && h < n && s[h] == ' ') {
            gt_bold(true);
            gt_color(h == 1 ? C_WHITE : (h == 2 ? C_YELLOW : C_LCYAN));
            console_puts(h == 1 ? "* " : (h == 2 ? "- " : "  "));
            rich_inline(s + h + 1, n - h - 1);
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }

        /* 分隔线 */
        if (n >= 3 && only_chars(s, n, "-=*")) {
            gt_color(C_DGRAY);
            put_repeat('-', cols);
            console_putc('\n');
            gt_color_reset();
            p = q + 1;
            continue;
        }

        /* 引用 */
        if (s[0] == '>') {
            gt_color(C_LCYAN);
            console_puts("  | ");
            gt_color(C_LGRAY);
            rich_inline(s + (n > 1 && s[1] == ' ' ? 2 : 1), n - (n > 1 && s[1] == ' ' ? 2 : 1));
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }

        /* 列表 */
        if (n >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') {
            gt_color(C_YELLOW);
            console_puts("  * ");
            gt_color(C_LGRAY);
            rich_inline(s + 2, n - 2);
            console_putc('\n');
            rich_reset();
            p = q + 1;
            continue;
        }

        /* 表格 */
        if (s[0] == '|') {
            if (only_chars(s, n, "|-: ")) {
                gt_color(C_DGRAY);
                for (int i = 0; i < n; i++)
                    console_putc('-');
                console_putc('\n');
                gt_color_reset();
            } else {
                render_table_row(s, n);
                gt_color_reset();
            }
            p = q + 1;
            continue;
        }

        rich_inline(s, n);
        console_putc('\n');
        rich_reset();
        p = q + 1;
    }
    rich_reset();
}
