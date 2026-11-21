/* VIM: 全屏模态编辑器.
   模式: NORMAL / INSERT / COMMAND(:) / SEARCH(/).
   常用操作: h j k l 方向键 Home End PgUp PgDn 0 $ gg G
             i I a A o O x X dd yy p u / n :w :q :q! :wq
   文件缓冲直接使用 PROG_BASE (程序未运行时空闲), 支持 256KB.
   退出后调用 vga_refresh() 恢复文本控制台. */
#include "gt.h"

#define ED_MAX   (256u * 1024u)
#define ED_BUF   ((char *)PROG_BASE)
#define ED_UNDO  ((char *)(PROG_BASE + 0x40000u))

#define MD_NORMAL 0
#define MD_INSERT 1
#define MD_CMD    2
#define MD_SEARCH 3

#define RGB(r, g, b) ((u32)(((r) & 0xFF) << 16) | (u32)(((g) & 0xFF) << 8) | \
                      (u32)((b) & 0xFF))

static const u32 COL_BG = RGB(16, 20, 24);
static const u32 COL_FG = RGB(216, 222, 233);
static const u32 COL_DIM = RGB(107, 114, 128);
static const u32 COL_ACC = RGB(79, 195, 247);
static const u32 COL_STBG = RGB(30, 58, 95);
static const u32 COL_STFG = RGB(227, 242, 253);

static char ed_path[160];
static u32  ed_len;
static u32  ed_undo_len;
static bool ed_undo_ok;
static bool ed_mod;
static u32  ed_cur;
static int  ed_top, ed_xoff;
static int  ed_mode;
static char ed_cmd[128];
static int  ed_cmdlen;
static char ed_msg[96];
static char ed_search[64];
static char ed_yank[2048];
static int  ed_yanklen;
static int  ed_rows, ed_cols, ed_gutter;
static bool ed_nums = true;
static int  ed_nlines;
static int  ed_cw, ed_ch;
static bool ed_quit;
static bool ed_pending_d, ed_pending_g, ed_pending_y;

static u8 *buf(void) { return (u8 *)ED_BUF; }

static void str_set(char *d, const char *s, int cap)
{
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int app(char *d, int k, const char *s, int cap)
{
    while (*s && k < cap - 1) d[k++] = *s++;
    d[k] = 0;
    return k;
}

static int appn(char *d, int k, int v, int cap)
{
    char b[24];
    itoa_base((u32)v, b, 10, false);
    return app(d, k, b, cap);
}

/* ------------------------------------------------------------- 文本模型 */
static void recount(void)
{
    ed_nlines = 1;
    for (u32 i = 0; i < ed_len; i++)
        if (buf()[i] == '\n')
            ed_nlines++;
}

static u32 line_start_of(int ln)
{
    u32 p = 0;
    while (ln > 0 && p < ed_len) {
        if (buf()[p] == '\n')
            ln--;
        p++;
    }
    return p;
}

static int line_index_of(u32 off)
{
    int ln = 0;
    for (u32 i = 0; i < off && i < ed_len; i++)
        if (buf()[i] == '\n')
            ln++;
    return ln;
}

static int line_len(u32 start)
{
    u32 p = start;
    while (p < ed_len && buf()[p] != '\n')
        p++;
    return (int)(p - start);
}

static int utf8_len(u8 c)
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
    return (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

static int disp_width(u32 start, u32 off)
{
    int col = 0;
    u32 p = start;
    while (p < off && p < ed_len) {
        u8 c = buf()[p];
        if (c == '\t') {
            col = (col + 4) & ~3;
            p++;
        } else {
            int n = utf8_len(c);
            col += cp_wide(utf8_cp(buf() + p, n)) ? 2 : 1;
            p += (u32)n;
        }
    }
    return col;
}

/* 返回显示列 >= target 的字节位置 (不超过行尾) */
static u32 col_to_off(u32 start, int target)
{
    int col = 0;
    u32 p = start;
    while (p < ed_len && buf()[p] != '\n' && col < target) {
        u8 c = buf()[p];
        if (c == '\t') {
            int nx = (col + 4) & ~3;
            if (nx > target) break;
            col = nx;
            p++;
        } else {
            int n = utf8_len(c);
            int w = cp_wide(utf8_cp(buf() + p, n)) ? 2 : 1;
            if (col + w > target) break;
            col += w;
            p += (u32)n;
        }
    }
    return p;
}

static u32 prev_char(u32 off, u32 start)
{
    if (off <= start) return start;
    off--;
    while (off > start && (buf()[off] & 0xC0) == 0x80)
        off--;
    return off;
}

static u32 next_char(u32 off, u32 end)
{
    if (off >= end) return end;
    u8 c = buf()[off];
    int n = utf8_len(c);
    u32 p = off + (u32)n;
    if (p > end) p = end;
    return p;
}

/* ------------------------------------------------------------- 查找/快照 */
static bool snapshot(void)
{
    if (ed_len > ED_MAX) return false;
    memcpy(ED_UNDO, ED_BUF, ed_len);
    ed_undo_len = ed_len;
    ed_undo_ok = true;
    return true;
}

static void clamp_cur(void)
{
    if (ed_cur > ed_len) ed_cur = ed_len;
}

static void insert_bytes(const char *s, int n)
{
    if (n <= 0 || ed_len + (u32)n > ED_MAX) {
        str_set(ed_msg, "buffer full", sizeof(ed_msg));
        return;
    }
    memmove(buf() + ed_cur + n, buf() + ed_cur, ed_len - ed_cur);
    memcpy(buf() + ed_cur, s, (u32)n);
    ed_len += (u32)n;
    ed_cur += (u32)n;
    recount();
    ed_mod = true;
}

static void delete_range(u32 at, u32 n)
{
    if (at >= ed_len || n == 0) return;
    if (at + n > ed_len) n = ed_len - at;
    memmove(buf() + at, buf() + at + n, ed_len - at - n);
    ed_len -= n;
    if (ed_cur > at + n) ed_cur -= n;
    else if (ed_cur > at) ed_cur = at;
    clamp_cur();
    recount();
    ed_mod = true;
}

static u32 find_next(const char *pat, u32 from)
{
    int n = (int)strlen(pat);
    if (n == 0 || ed_len < (u32)n) return (u32)-1;
    for (u32 i = from; i + (u32)n <= ed_len; i++)
        if (memcmp(buf() + i, pat, (u32)n) == 0)
            return i;
    return (u32)-1;
}

static void do_search(bool repeat)
{
    const char *pat = ed_search;
    if (!pat[0]) { str_set(ed_msg, "no previous search", sizeof(ed_msg)); return; }
    u32 from = ed_cur + 1;
    u32 hit = find_next(pat, from);
    if (hit == (u32)-1 && repeat)
        hit = find_next(pat, 0);
    if (hit == (u32)-1) {
        str_set(ed_msg, "pattern not found", sizeof(ed_msg));
        return;
    }
    ed_cur = hit;
}

/* --------------------------------------------------------------- 绘制 */
static int text_cols(void)
{
    int c = ed_cols - ed_gutter;
    return c > 4 ? c : 4;
}

static void calc_gutter(void)
{
    if (!ed_nums) {
        ed_gutter = 0;
        return;
    }
    int d = 1;
    int n = ed_nlines;
    while (n >= 10) { d++; n /= 10; }
    ed_gutter = d + 2;
}

static int build_row(u32 start, int skip, char *out, int cap)
{
    u32 p = start;
    int col = 0, k = 0;
    while (p < ed_len && buf()[p] != '\n' && col < skip) {
        u8 c = buf()[p];
        if (c == '\t') {
            col = (col + 4) & ~3;
            p++;
        } else {
            int n = utf8_len(c);
            col += cp_wide(utf8_cp(buf() + p, n)) ? 2 : 1;
            p += (u32)n;
        }
    }
    int maxc = text_cols() + skip;
    while (p < ed_len && buf()[p] != '\n' && col < maxc && k < cap - 5) {
        u8 c = buf()[p];
        if (c == '\t') {
            int nx = (col + 4) & ~3;
            while (col < nx && k < cap - 5) { out[k++] = ' '; col++; }
            p++;
        } else {
            int n = utf8_len(c);
            if (col + (cp_wide(utf8_cp(buf() + p, n)) ? 2 : 1) > maxc)
                break;
            for (int i = 0; i < n && k < cap - 5; i++)
                out[k++] = (char)buf()[p + (u32)i];
            col += cp_wide(utf8_cp(buf() + p, n)) ? 2 : 1;
            p += (u32)n;
        }
    }
    out[k] = 0;
    return k;
}

static void draw_status(void)
{
    int sy = ed_rows * ed_ch;
    int sh = gfx_h() - sy;
    if (sh <= 0) return;
    gfx_fill(0, sy, gfx_w(), sh, COL_STBG);

    char s[200];
    int cap = ed_cols + 1;
    if (cap > (int)sizeof(s)) cap = (int)sizeof(s);
    int k = 0;
    const char *mode = ed_mode == MD_INSERT ? "INSERT" :
                       ed_mode == MD_CMD ? "COMMAND" :
                       ed_mode == MD_SEARCH ? "SEARCH" : "NORMAL";
    k = app(s, k, " ", cap);
    k = app(s, k, mode, cap);
    k = app(s, k, "  ", cap);
    k = app(s, k, ed_path, cap);

    int ln = line_index_of(ed_cur) + 1;
    u32 st = line_start_of(ln - 1);
    int col = disp_width(st, ed_cur) + 1;
    k = app(s, k, "  L ", cap);
    k = appn(s, k, ln, cap);
    k = app(s, k, "/", cap);
    k = appn(s, k, ed_nlines, cap);
    k = app(s, k, " C ", cap);
    k = appn(s, k, col, cap);
    if (ed_mod)
        k = app(s, k, " [+]", cap);

    if (ed_mode == MD_CMD) {
        k = app(s, k, "   :", cap);
        k = app(s, k, ed_cmd, cap);
    } else if (ed_mode == MD_SEARCH) {
        k = app(s, k, "   /", cap);
        k = app(s, k, ed_cmd, cap);
    } else if (ed_msg[0]) {
        k = app(s, k, "   ", cap);
        k = app(s, k, ed_msg, cap);
    }
    gfx_text(0, sy, s, COL_STFG);
    gfx_flip_rect(0, sy, gfx_w(), sh);
}

static void draw_cursor(int row, u32 start)
{
    if (row < 0 || row >= ed_rows)
        return;
    int col = disp_width(start, ed_cur) - ed_xoff;
    if (col < 0 || col >= text_cols())
        return;
    int x = (ed_gutter + col) * ed_cw;
    int y = row * ed_ch;
    int w = ed_cw;
    u32 p = ed_cur;
    if (p < ed_len && buf()[p] != '\n' && buf()[p] != '\t') {
        int n = utf8_len(buf()[p]);
        if (cp_wide(utf8_cp(buf() + p, n)))
            w = ed_cw * 2;
    }
    gfx_fill(x, y, w, ed_ch, COL_ACC);
    if (p < ed_len && buf()[p] != '\n' && buf()[p] != '\t') {
        char ch[8];
        int n = utf8_len(buf()[p]);
        if (n > 7) n = 7;
        for (int i = 0; i < n; i++)
            ch[i] = (char)buf()[p + (u32)i];
        ch[n] = 0;
        gfx_text(x, y, ch, COL_BG);
    }
}

static void draw_row(int r)
{
    int y = r * ed_ch;
    gfx_fill(0, y, gfx_w(), ed_ch, COL_BG);
    int ln = ed_top + r;
    if (ln >= ed_nlines)
        return;
    u32 start = line_start_of(ln);

    if (ed_nums) {
        char num[16];
        itoa_base((u32)(ln + 1), num, 10, false);
        int nl = (int)strlen(num);
        int x = (ed_gutter - 2 - nl) > 0 ? (ed_gutter - 2 - nl) * ed_cw : 0;
        gfx_text(x, y, num, COL_DIM);
    }

    char tmp[1200];
    build_row(start, ed_xoff, tmp, sizeof(tmp));
    gfx_text(ed_gutter * ed_cw, y, tmp, COL_FG);

    if (ln == line_index_of(ed_cur))
        draw_cursor(r, start);
}

static void draw_all(void)
{
    calc_gutter();
    gfx_clear(COL_BG);
    for (int r = 0; r < ed_rows; r++)
        draw_row(r);
    gfx_flip();
    draw_status();
}

static void redraw_rows(int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b >= ed_rows) b = ed_rows - 1;
    if (a > b) return;
    for (int r = a; r <= b; r++)
        draw_row(r);
    gfx_flip_rect(0, a * ed_ch, gfx_w(), (b - a + 1) * ed_ch);
}

static void adjust_view(void)
{
    int ln = line_index_of(ed_cur);
    if (ed_top > ln)
        ed_top = ln;
    if (ln >= ed_top + ed_rows)
        ed_top = ln - ed_rows + 1;
    if (ed_top < 0) ed_top = 0;

    u32 st = line_start_of(ln);
    int col = disp_width(st, ed_cur);
    if (col < ed_xoff)
        ed_xoff = col;
    if (col >= ed_xoff + text_cols())
        ed_xoff = col - text_cols() + 1;
    if (ed_xoff < 0) ed_xoff = 0;
}

/* --------------------------------------------------------------- 文件 */
static bool save_file(void)
{
    if (!fat_create(ed_path, (const u8 *)ED_BUF, ed_len)) {
        str_set(ed_msg, "write failed", sizeof(ed_msg));
        return false;
    }
    ed_mod = false;
    str_set(ed_msg, "written", sizeof(ed_msg));
    return true;
}

static int load_file(const char *abs)
{
    ed_len = 0;
    struct fat_dirent d;
    if (fat_find(abs, &d)) {
        if (d.is_dir) return -3;
        if (d.size > ED_MAX) return -2;
        u32 got = 0;
        if (!fat_read_file(abs, ED_BUF, ED_MAX, &got)) return -3;
        ed_len = got;
    }
    /* 统一换行: 去掉 CR */
    u32 w = 0;
    for (u32 i = 0; i < ed_len; i++)
        if (buf()[i] != '\r')
            buf()[w++] = buf()[i];
    ed_len = w;
    recount();
    return 0;
}

/* --------------------------------------------------------------- 命令 */
static void do_command(void)
{
    char c[128];
    int k = 0;
    while (k < 127 && ed_cmd[k]) { c[k] = ed_cmd[k]; k++; }
    c[k] = 0;
    while (k > 0 && c[k - 1] == ' ') c[--k] = 0;
    char *p = c;
    while (*p == ' ') p++;

    if (!strcmp(p, "w") || !strcmp(p, "w!")) {
        save_file();
    } else if (!strcmp(p, "q")) {
        if (ed_mod) str_set(ed_msg, "no write since last change (:q!)", sizeof(ed_msg));
        else ed_quit = true;
    } else if (!strcmp(p, "q!")) {
        ed_quit = true;
    } else if (!strcmp(p, "wq") || !strcmp(p, "x") || !strcmp(p, "wq!")) {
        if (save_file()) ed_quit = true;
    } else if (!strcmp(p, "set nu")) {
        ed_nums = true;
    } else if (!strcmp(p, "set nonu")) {
        ed_nums = false;
    } else if (*p >= '0' && *p <= '9') {
        int ln = 0;
        while (*p >= '0' && *p <= '9') { ln = ln * 10 + (*p - '0'); p++; }
        if (ln < 1) ln = 1;
        if (ln > ed_nlines) ln = ed_nlines;
        ed_cur = line_start_of(ln - 1);
    } else if (!*p) {
        /* 空命令 */
    } else {
        str_set(ed_msg, "unknown command", sizeof(ed_msg));
    }
}

/* ------------------------------------------------------------ 编辑动作 */
static void move_vertical(int delta)
{
    int ln = line_index_of(ed_cur);
    int col = disp_width(line_start_of(ln), ed_cur);
    int target = ln + delta;
    if (target < 0) target = 0;
    if (target >= ed_nlines) target = ed_nlines - 1;
    u32 ts = line_start_of(target);
    ed_cur = col_to_off(ts, col);
}

static void delete_line(void)
{
    int ln = line_index_of(ed_cur);
    u32 a = line_start_of(ln);
    u32 b = (ln + 1 < ed_nlines) ? line_start_of(ln + 1) : ed_len;
    if (b <= a && a >= ed_len) return;
    delete_range(a, b - a);
    ed_cur = a;
    clamp_cur();
}

static void yank_line(void)
{
    int ln = line_index_of(ed_cur);
    u32 a = line_start_of(ln);
    int n = line_len(a);
    if (n > (int)sizeof(ed_yank) - 2) n = (int)sizeof(ed_yank) - 2;
    memcpy(ed_yank, buf() + a, (u32)n);
    ed_yank[n] = '\n';
    ed_yanklen = n + 1;
}

static void edit_delete_char(void)
{
    if (ed_cur >= ed_len) return;
    u32 n = 1;
    if (buf()[ed_cur] == '\n')
        n = 1;
    else
        n = (u32)utf8_len(buf()[ed_cur]);
    snapshot();
    delete_range(ed_cur, n);
}

static void enter_insert(int where)
{
    snapshot();
    int ln = line_index_of(ed_cur);
    u32 st = line_start_of(ln);
    if (where == 1) {                 /* a */
        if (ed_cur < ed_len && buf()[ed_cur] != '\n')
            ed_cur = next_char(ed_cur, st + (u32)line_len(st));
    } else if (where == 2) {          /* I */
        ed_cur = st;
    } else if (where == 3) {          /* A */
        ed_cur = st + (u32)line_len(st);
    } else if (where == 4 || where == 5) { /* o / O */
        ed_cur = (where == 4) ? st + (u32)line_len(st) : st;
        insert_bytes("\n", 1);
        if (where == 5)
            ed_cur = st;
    }
    ed_mode = MD_INSERT;
    ed_msg[0] = 0;
}

/* --------------------------------------------------------------- 主循环 */
static void normal_key(int k, bool *full)
{
    int ln = line_index_of(ed_cur);
    u32 st = line_start_of(ln);
    u32 en = st + (u32)line_len(st);

    if (ed_pending_y) {
        ed_pending_y = false;
        if (k == 'y') { yank_line(); str_set(ed_msg, "yanked", sizeof(ed_msg)); }
        return;
    }
    if (ed_pending_d) {
        ed_pending_d = false;
        if (k == 'd') { snapshot(); delete_line(); *full = true; }
        return;
    }
    if (ed_pending_g) {
        ed_pending_g = false;
        if (k == 'g') { ed_cur = 0; ed_top = 0; *full = true; }
        return;
    }

    switch (k) {
    case 'h': case KEY_LEFT:
        ed_cur = prev_char(ed_cur, st);
        break;
    case 'l': case KEY_RIGHT:
        if (ed_cur < en)
            ed_cur = next_char(ed_cur, en);
        break;
    case 'j': case KEY_DOWN:
        move_vertical(1);
        break;
    case 'k': case KEY_UP:
        move_vertical(-1);
        break;
    case '0': case KEY_HOME:
        ed_cur = st;
        break;
    case '$': case KEY_END:
        ed_cur = en;
        break;
    case KEY_PGUP:
        for (int i = 0; i < ed_rows; i++) move_vertical(-1);
        break;
    case KEY_PGDN:
        for (int i = 0; i < ed_rows; i++) move_vertical(1);
        break;
    case 'g':
        ed_pending_g = true;
        break;
    case 'G':
        ed_cur = ed_len;
        break;
    case 'i':
        enter_insert(0);
        break;
    case 'a':
        enter_insert(1);
        break;
    case 'I':
        enter_insert(2);
        break;
    case 'A':
        enter_insert(3);
        break;
    case 'o':
        enter_insert(4);
        *full = true;
        break;
    case 'O':
        enter_insert(5);
        *full = true;
        break;
    case 'x':
        edit_delete_char();
        *full = true;
        break;
    case 'X':
        if (ed_cur > 0) {
            ed_cur = prev_char(ed_cur, st);
            edit_delete_char();
            *full = true;
        }
        break;
    case 'd':
        ed_pending_d = true;
        break;
    case 'y':
        ed_pending_y = true;
        break;
    case 'p':
        if (ed_yanklen > 0) {
            snapshot();
            insert_bytes(ed_yank, ed_yanklen);
            *full = true;
        }
        break;
    case 'u':
        if (ed_undo_ok) {
            memcpy(ED_BUF, ED_UNDO, ed_undo_len);
            ed_len = ed_undo_len;
            ed_undo_ok = false;
            clamp_cur();
            recount();
            ed_mod = true;
            *full = true;
            str_set(ed_msg, "undo", sizeof(ed_msg));
        }
        break;
    case '/':
        ed_mode = MD_SEARCH;
        ed_cmdlen = 0;
        ed_cmd[0] = 0;
        ed_msg[0] = 0;
        break;
    case 'n':
        do_search(true);
        *full = true;
        break;
    case ':':
        ed_mode = MD_CMD;
        ed_cmdlen = 0;
        ed_cmd[0] = 0;
        ed_msg[0] = 0;
        break;
    case 27:
        ed_pending_d = ed_pending_g = ed_pending_y = false;
        break;
    default:
        break;
    }
}

static void insert_key(int k, bool *full)
{
    if (k == 27 || k == 3) {          /* ESC / Ctrl-C */
        ed_mode = MD_NORMAL;
        if (ed_cur > 0 && (ed_cur == ed_len || buf()[ed_cur] != '\n'))
            ed_cur = prev_char(ed_cur, line_start_of(line_index_of(ed_cur)));
        return;
    }
    if (k == '\r' || k == '\n') {
        insert_bytes("\n", 1);
        *full = true;
        return;
    }
    if (k == 8 || k == 127) {
        if (ed_cur > 0) {
            u32 st = line_start_of(line_index_of(ed_cur));
            u32 p = prev_char(ed_cur, st);
            delete_range(p, ed_cur - p);
            *full = true;
        }
        return;
    }
    if (k == KEY_DEL) {
        if (ed_cur < ed_len) {
            u32 n = (buf()[ed_cur] == '\n') ? 1 : (u32)utf8_len(buf()[ed_cur]);
            delete_range(ed_cur, n);
            *full = true;
        }
        return;
    }
    if (k == KEY_LEFT) { ed_cur = prev_char(ed_cur, 0); return; }
    if (k == KEY_RIGHT) { if (ed_cur < ed_len) ed_cur = next_char(ed_cur, ed_len); return; }
    if (k == KEY_UP) { move_vertical(-1); return; }
    if (k == KEY_DOWN) { move_vertical(1); return; }
    if (k == KEY_HOME) { ed_cur = line_start_of(line_index_of(ed_cur)); return; }
    if (k == KEY_END) {
        int ln = line_index_of(ed_cur);
        ed_cur = line_start_of(ln) + (u32)line_len(line_start_of(ln));
        return;
    }
    if (k == 19) {                    /* Ctrl-S 保存 */
        save_file();
        return;
    }
    if (k == '\t') {
        insert_bytes("\t", 1);
        *full = true;
        return;
    }
    if (k >= 32 && k < 256) {
        char c = (char)k;
        insert_bytes(&c, 1);
        *full = true;
    }
}

static void prompt_key(int k, bool *full)
{
    if (k == 27) {
        ed_mode = MD_NORMAL;
        ed_cmd[0] = 0;
        ed_cmdlen = 0;
        return;
    }
    if (k == '\r' || k == '\n') {
        int was_search = (ed_mode == MD_SEARCH);
        ed_cmd[ed_cmdlen] = 0;
        ed_mode = MD_NORMAL;
        if (was_search) {
            str_set(ed_search, ed_cmd, sizeof(ed_search));
            do_search(false);
            *full = true;
        } else {
            do_command();
            *full = true;
        }
        ed_cmdlen = 0;
        ed_cmd[0] = 0;
        return;
    }
    if (k == 8 || k == 127) {
        if (ed_cmdlen > 0)
            ed_cmd[--ed_cmdlen] = 0;
        return;
    }
    if (k >= 32 && k < 127 && ed_cmdlen < (int)sizeof(ed_cmd) - 1) {
        ed_cmd[ed_cmdlen++] = (char)k;
        ed_cmd[ed_cmdlen] = 0;
    }
}

static void input_loop(void)
{
    while (!ed_quit) {
        int k = ui_read_key();
        if (k == KEY_REBOOT)
            continue;
        int old_top = ed_top;
        int old_nlines = ed_nlines;
        int old_row = line_index_of(ed_cur) - ed_top;
        bool full = false;

        ed_msg[0] = 0;
        if (ed_mode == MD_NORMAL)
            normal_key(k, &full);
        else if (ed_mode == MD_INSERT)
            insert_key(k, &full);
        else
            prompt_key(k, &full);

        clamp_cur();
        adjust_view();

        if (ed_top != old_top || full || ed_nlines != old_nlines)
            draw_all();
        else {
            int new_row = line_index_of(ed_cur) - ed_top;
            redraw_rows(old_row, new_row);
            draw_status();
        }
    }
}

/* --------------------------------------------------------------- 入口 */
int vim_run(const char *abs)
{
    if (!gfx_active())
        return -1;
    if (!abs || !abs[0])
        return -1;

    str_set(ed_path, abs, sizeof(ed_path));
    ed_cur = 0;
    ed_top = 0;
    ed_xoff = 0;
    ed_mode = MD_NORMAL;
    ed_cmdlen = 0;
    ed_cmd[0] = 0;
    ed_msg[0] = 0;
    ed_search[0] = 0;
    ed_yanklen = 0;
    ed_mod = false;
    ed_quit = false;
    ed_pending_d = ed_pending_g = ed_pending_y = false;
    ed_undo_ok = false;
    ed_nums = true;

    int rc = load_file(abs);
    if (rc < 0)
        return rc;

    ed_cw = gfx_cell_w();
    ed_ch = gfx_cell_h();
    if (ed_cw <= 0 || ed_ch <= 0)
        return -1;
    ed_cols = gfx_w() / ed_cw;
    ed_rows = gfx_h() / ed_ch - 1;
    if (ed_rows < 2) ed_rows = 2;
    if (ed_cols < 20) return -1;

    calc_gutter();
    draw_all();
    input_loop();
    return 0;
}
