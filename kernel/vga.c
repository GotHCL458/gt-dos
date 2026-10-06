/* ==========================================================================
 *  GT-DOS 文本控制台
 *  ---------------------------------------------------------------------------
 *  两种后端:
 *    1) 图形文本模式 (推荐): Bochs VBE 640x480x32 + 双缓冲, 字符直接由
 *       font.bin 字模软渲染 -> 汉字数量不受 VGA 256 槽限制, 无闪烁.
 *    2) 经典 VGA 文本模式 (回退): 0xB8000 显存 + 128..255 槽 LRU 重映射,
 *       仅在 VBE 不可用时使用.
 *  历史缓冲 hist_cp/hist_attr 按 Unicode 码点存储, 两后端共用.
 *  vga_putc 内置 UTF-8 解码.
 * ========================================================================== */
#include "gt.h"

static volatile u16 *const vga = VGA_MEM;

#define TXT_ROWS (VGA_ROWS - 1)
#define RIGHT_HALF 0xFFFEu
#define COLS_MAX 160                 /* 1280/8: 最大文本列数 */

static u16 hist_cp[HIST_LINES][COLS_MAX] __attribute__((section(".highbss")));
static u8  hist_attr[HIST_LINES][COLS_MAX] __attribute__((section(".highbss")));
static int hist_head;
static int hist_total;
static int cur_col;
static int view_off;
static u8  attr = 0x07;
static u8  def_attr = 0x07;
static bool cursor_vis = true;
static char scroll_msg[40];

static u16 status_buf_cp[COLS_MAX] __attribute__((section(".highbss")));
static u8   status_buf_attr[COLS_MAX] __attribute__((section(".highbss")));
static bool status_active = false;

/* 运行时文本网格: 图形模式按分辨率算, 文本回退模式固定 80x24 */
static int tcols(void) { return gfx_active() ? gfx_w() / 8 : VGA_COLS; }
static int trows(void) { return gfx_active() ? gfx_h() / 16 - 1 : TXT_ROWS; }

/* ---------------------------------------------------------- 汉字字库 */
#define GLYPH_BYTES 128              /* 16 行 x 8 字节 (4bit 灰度/像素) */
#define FONT_MAX 21504

static u8  font_buf[FONT_MAX * GLYPH_BYTES] __attribute__((section(".highbss")));
static u16 cpm_buf[FONT_MAX]       __attribute__((section(".highbss")));
static const u16 *cpm_tab;
static const u8  *font_tab;
static u32        cpm_count;
static bool       font_ready;

static bool is_wide(u32 cp)
{
    return (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

static const u8 *glyph_of(u16 cp)
{
    if (!font_ready)
        return 0;
    int lo = 0, hi = (int)cpm_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u16 c = cpm_tab[mid];
        if (c == cp)
            return font_tab + (u32)mid * GLYPH_BYTES;
        if (c < cp) lo = mid + 1;
        else        hi = mid - 1;
    }
    return 0;
}

/* 取任意码点的 32 字节字模 (gfx.c / GUI 用). 无字模返回 0. */
const u8 *vga_glyph(u32 cp)
{
    if (cp > 0xFFFF)
        return 0;
    return glyph_of((u16)cp);
}

/* 分块读盘 */
static bool read_sects(u32 lba, u32 count, u8 *dst)
{
    while (count > 0) {
        u32 n = count > 64 ? 64 : count;
        if (!ata_read(lba, n, dst))
            return false;
        lba += n;
        dst += n * 512u;
        count -= n;
    }
    return true;
}

/* 保护模式下用 ATA 读取引导器邮箱指定的 font/cpm 位置 */
void vga_load_font(void)
{
    u32 flba = *(volatile u32 *)MB_FONT_LBA;
    u32 fsec = *(volatile u32 *)MB_FONT_SECT;
    u32 clba = *(volatile u32 *)MB_CPM_LBA;
    u32 csec = *(volatile u32 *)MB_CPM_SECT;
    if (!flba || !clba || !fsec || !csec) {
        kprintf("[vga] font mailbox empty, no Chinese\n");
        return;
    }
    if (fsec * 512u > sizeof(font_buf) || csec * 512u > sizeof(cpm_buf)) {
        kprintf("[vga] font too big (%u/%u sect)\n", fsec, csec);
        return;
    }
    if (!read_sects(flba, fsec, font_buf) ||
        !read_sects(clba, csec, (u8 *)cpm_buf)) {
        kprintf("[vga] font ATA read failed\n");
        return;
    }
    cpm_count = (u32)(csec * 512u / 2u);
    font_tab = font_buf;
    cpm_tab = cpm_buf;
    font_ready = true;
    kprintf("[vga] font %u glyphs loaded\n", cpm_count);
}

/* ---------------------------------------------------------- VGA 16 色 */
static const u32 vga_pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

/* logical: 0 = 最旧行, hist_total-1 = 最新行 */
static int line_index(int logical)
{
    return ((hist_head - hist_total + 1 + logical) % HIST_LINES + HIST_LINES)
           % HIST_LINES;
}

/* -------------------------------------------------- 图形文本渲染后端 */
/* 光标块跟踪: 画光标前恢复其下字符, 移动光标后重画 */
static int cur_cx = -1;

static void gfx_put_cell(int cpos, u16 cp, u8 a)
{
    u32 fg = vga_pal[a & 0x0F];
    int x = cpos * 8, y = (trows() - 1) * 16;
    if (cp == RIGHT_HALF || cp == ' ') {
        gfx_fill(x, y, 16, 16, vga_pal[a >> 4]);
        return;
    }
    if (cp < 128) {
        gfx_fill(x, y, 8, 16, vga_pal[a >> 4]);
        gfx_char(x, y, cp, fg);
    } else if (is_wide(cp)) {
        gfx_fill(x, y, 16, 16, vga_pal[a >> 4]);
        gfx_char(x, y, cp, fg);
    } else {
        gfx_fill(x, y, 8, 16, vga_pal[a >> 4]);
        gfx_char(x, y, '?', fg);
    }
}

static void gfx_draw_cursor(bool on)
{
    if (!on) {
        if (cur_cx >= 0) {
            gfx_fill(cur_cx * 8, (trows() - 1) * 16, 8, 16,
                     vga_pal[def_attr >> 4]);
            gfx_put_cell(cur_cx, hist_cp[hist_head][cur_cx],
                         hist_attr[hist_head][cur_cx]);
            cur_cx = -1;
        }
        return;
    }
    if (view_off > 0 || !cursor_vis)
        return;
    cur_cx = cur_col;
    gfx_fill(cur_cx * 8, (trows() - 1) * 16, 8, 16,
             vga_pal[attr & 0x0F]);
}

static void render_gfx(void)
{
    int newest = hist_total - 1;
    int cols = tcols(), rows = trows();
    gfx_clear(vga_pal[def_attr >> 4]);

    for (int r = 0; r < rows; r++) {
        int logical = newest - view_off - (rows - 1 - r);
        if (logical < 0)
            continue;
        const u16 *srcp = hist_cp[line_index(logical)];
        const u8  *srca = hist_attr[line_index(logical)];
        int c = 0;
        while (c < cols) {
            u16 cp = srcp[c];
            if (cp == RIGHT_HALF) { c++; continue; }
            u32 fg = vga_pal[srca[c] & 0x0F];
            int x = c * 8, y = r * 16;
            if (cp < 128) {
                if (cp != ' ') gfx_char(x, y, cp, fg);
                c++;
            } else if (is_wide(cp)) {
                gfx_char(x, y, cp, fg);
                c += 2;
            } else {
                if (cp != ' ' && cp != 0) gfx_char(x, y, '?', fg);
                c++;
            }
        }
    }

    /* 回滚指示条 */
    if (view_off > 0 && scroll_msg[0]) {
        int len = (int)strlen(scroll_msg);
        if (len > cols - 2)
            len = cols - 2;
        gfx_text((cols - 1 - len) * 8, (rows - 1) * 16,
                 scroll_msg, vga_pal[C_YELLOW]);
    }

    /* 状态栏: 屏幕底部一行 */
    if (status_active) {
        int sy = gfx_h() - 16;
        gfx_fill(0, sy, gfx_w(), 16, vga_pal[C_LGRAY]);
        int c = 0;
        while (c < cols) {
            u16 cp = status_buf_cp[c];
            if (cp != ' ' && cp < 128)
                gfx_char(c * 8, sy, cp, vga_pal[C_BLACK]);
            c++;
        }
    }
    gfx_draw_cursor(true);
    gfx_flip();
}

/* 单字符追加到当前行 (图形模式局部刷新, 不整屏重绘) */
static void gfx_append_cell(int cpos, u16 cp, u8 a)
{
    int x = cpos * 8;
    int w = is_wide(cp) ? 16 : 8;
    if (cpos == cur_cx)
        gfx_draw_cursor(false);
    gfx_put_cell(cpos, cp, a);
    gfx_flip_rect(x, (trows() - 1) * 16, w + 16, 16);
}

/* -------------------------------------------------- 经典 VGA 文本后端 */
static void render_text(void)
{
    int newest = hist_total - 1;

    for (int r = 0; r < TXT_ROWS; r++) {
        int logical = newest - view_off - (TXT_ROWS - 1 - r);
        volatile u16 *dst = &vga[r * VGA_COLS];
        if (logical < 0) {
            u16 blank = (u16)((def_attr << 8) | ' ');
            for (int c = 0; c < VGA_COLS; c++)
                dst[c] = blank;
        } else {
            const u16 *srcp = hist_cp[line_index(logical)];
            const u8  *srca = hist_attr[line_index(logical)];
            for (int c = 0; c < VGA_COLS; c++) {
                u16 cp = srcp[c];
                if (cp == RIGHT_HALF)
                    continue;
                u8 a = srca[c];
                u8 slot = (cp < 128) ? (u8)cp : '?';
                dst[c] = (u16)((a << 8) | slot);
            }
        }
    }

    if (view_off > 0 && scroll_msg[0]) {
        int len = (int)strlen(scroll_msg);
        if (len > VGA_COLS - 2)
            len = VGA_COLS - 2;
        int start = VGA_COLS - 1 - len;
        volatile u16 *row = &vga[(TXT_ROWS - 1) * VGA_COLS];
        u16 a = (u16)((((C_BLACK << 4) | C_YELLOW) << 8));
        for (int i = 0; i < len; i++)
            row[start + i] = (u16)(a | (u8)scroll_msg[i]);
    }

    volatile u16 *srow = &vga[(VGA_ROWS - 1) * VGA_COLS];
    if (status_active) {
        for (int c = 0; c < VGA_COLS; c++) {
            u16 cp = status_buf_cp[c];
            u8 slot = (cp < 128) ? (u8)cp : '?';
            srow[c] = (u16)((status_buf_attr[c] << 8) | slot);
        }
    } else {
        u16 blank = (u16)((def_attr << 8) | ' ');
        for (int c = 0; c < VGA_COLS; c++)
            srow[c] = blank;
    }
}

static void render(void)
{
    if (gfx_active())
        render_gfx();
    else
        render_text();
}

/* ------------------------------------------------------------ UTF-8 解码 */
static const char *utf8_next(const char *p, u32 *out)
{
    u8 b = (u8)*p++;
    if (b < 0x80) { *out = b; return p; }
    u32 need, acc;
    if ((b & 0xE0) == 0xC0) { need = 1; acc = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { need = 2; acc = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { need = 3; acc = b & 0x07; }
    else { *out = '?'; return p; }
    for (u32 i = 0; i < need; i++) {
        u8 c = (u8)*p;
        if ((c & 0xC0) != 0x80) { *out = '?'; return p; }
        acc = (acc << 6) | (c & 0x3F);
        p++;
    }
    *out = acc;
    return p;
}

/* 统计 UTF-8 字符串的显示列数 (汉字算 2 列) */
int vga_utf8_cols(const char *s)
{
    int n = 0;
    while (*s) {
        u32 cp;
        s = utf8_next(s, &cp);
        n += is_wide(cp) ? 2 : 1;
    }
    return n;
}

/* 设置状态栏 (支持 UTF-8 中文) */
void vga_set_status(const char *left, const char *right)
{
    u8 a = (u8)((C_BLACK << 4) | C_LGRAY);
    int cols = tcols();
    for (int c = 0; c < cols; c++) {
        status_buf_cp[c] = ' ';
        status_buf_attr[c] = a;
    }

    int c = 1;
    if (left) {
        while (*left && c < cols - 1) {
            u32 cp;
            left = utf8_next(left, &cp);
            if (is_wide(cp)) { c++; continue; }
            status_buf_cp[c++] = (u16)cp;
            status_buf_attr[c - 1] = a;
        }
    }
    if (right) {
        int len = (int)strlen(right);
        int rc = cols - 1 - len;
        if (rc < c + 2) rc = c + 2;
        c = rc;
        while (*right && c < cols - 1) {
            u32 cp;
            right = utf8_next(right, &cp);
            if (is_wide(cp)) { c++; continue; }
            status_buf_cp[c++] = (u16)cp;
            status_buf_attr[c - 1] = a;
        }
    }
    status_active = true;
    render();
}

void vga_clear_status(void)
{
    status_active = false;
    render();
}

void vga_set_scroll_msg(const char *s)
{
    int i = 0;
    for (; s[i] && i < (int)sizeof(scroll_msg) - 1; i++)
        scroll_msg[i] = s[i];
    scroll_msg[i] = '\0';
    render();
}

void vga_puts(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

static void hw_cursor(void)
{
    if (gfx_active())
        return;                        /* 图形模式: 光标随 render 绘制 */
    u16 pos = (u16)((TXT_ROWS - 1) * VGA_COLS + cur_col);
    bool vis_ok = cursor_vis && view_off == 0;
    static u16 last_pos = 0xFFFF;
    static int last_vis = -1;
    if (!vis_ok) {
        if (last_vis != 0) {
            outb(0x3D4, 0x0A);
            outb(0x3D5, 0x20);
            last_vis = 0;
        }
        return;
    }
    if (last_vis != 1) {
        outb(0x3D4, 0x0A);
        outb(0x3D5, 0x00);
        last_vis = 1;
    }
    if (last_pos == pos)
        return;
    last_pos = pos;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

static void new_line(void)
{
    hist_head = (hist_head + 1) % HIST_LINES;
    if (hist_total < HIST_LINES)
        hist_total++;
    cur_col = 0;
    for (int c = 0; c < tcols(); c++) {
        hist_cp[hist_head][c] = ' ';
        hist_attr[hist_head][c] = attr;
    }
}

/* ------------------------------------------------------------------- 接口 */
void vga_init(void)
{
    hist_head = 0;
    hist_total = 1;
    cur_col = 0;
    view_off = 0;
    attr = 0x07;
    def_attr = 0x07;
    cursor_vis = true;

    for (int r = 0; r < HIST_LINES; r++)
        for (int c = 0; c < COLS_MAX; c++) {
            hist_cp[r][c] = ' ';
            hist_attr[r][c] = 0x07;
        }
    /* 图形文本后端在 kmain 里 ata_init 后由 vga_enter_gfx() 开启 */
    render();
    hw_cursor();
}

void vga_enter_gfx(void)
{
    if (gfx_init())
        render();
}

/* 强制重绘整个文本控制台 (GUI 退出返回文本时调用) */
void vga_refresh(void)
{
    render();
    hw_cursor();
}

/* 分辨率改变后重排文本网格 (清屏, 历史保留可回滚) */
void vga_relayout(void)
{
    cur_cx = -1;
    cur_col = 0;
    view_off = 0;
    hist_head = 0;
    hist_total = 1;
    for (int c = 0; c < COLS_MAX; c++) {
        hist_cp[0][c] = ' ';
        hist_attr[0][c] = def_attr;
    }
    render();
    hw_cursor();
}

void vga_set_color(u8 fg, u8 bg)
{
    attr = (u8)((bg << 4) | (fg & 0x0F));
}

u8 vga_get_fg(void) { return (u8)(attr & 0x0F); }
u8 vga_get_bg(void) { return (u8)(attr >> 4); }

void vga_set_attr_default(u8 a) { def_attr = a; }

void vga_get_cursor(int *row, int *col)
{
    if (row) *row = trows() - 1;
    if (col) *col = cur_col;
}

void vga_set_col(int col)
{
    int cols = tcols();
    if (col < 0) col = 0;
    if (col >= cols) col = cols - 1;
    cur_col = col;
    if (!gfx_active())
        hw_cursor();
    else
        render();
}

void vga_set_cursor_visible(bool on)
{
    cursor_vis = on;
    hw_cursor();
}

void vga_line_reset(void)
{
    for (int c = 0; c < tcols(); c++) {
        hist_cp[hist_head][c] = ' ';
        hist_attr[hist_head][c] = def_attr;
    }
    cur_col = 0;
}

void vga_erase_eol(void)
{
    for (int c = cur_col; c < tcols(); c++) {
        hist_cp[hist_head][c] = ' ';
        hist_attr[hist_head][c] = def_attr;
    }
}

/* 直接写入当前行 (UTF-8 解码, 行编辑器整行重绘用) */
void vga_write_raw(const char *s, int n)
{
    if (view_off > 0)
        view_off = 0;
    int cols = tcols();
    const char *p = s;
    const char *end = s + n;
    while (p < end) {
        u32 cp;
        p = utf8_next(p, &cp);
        if (is_wide(cp)) {
            if (cur_col + 1 >= cols)
                break;
            hist_cp[hist_head][cur_col] = (u16)cp;
            hist_attr[hist_head][cur_col] = attr;
            hist_cp[hist_head][cur_col + 1] = RIGHT_HALF;
            hist_attr[hist_head][cur_col + 1] = attr;
            cur_col += 2;
        } else {
            if (cur_col >= cols)
                break;
            hist_cp[hist_head][cur_col] = (cp < 128) ? (u16)cp : (u16)'?';
            hist_attr[hist_head][cur_col] = attr;
            cur_col++;
        }
    }
    render();
    hw_cursor();
}

void vga_clear(void)
{
    hist_head = 0;
    hist_total = 1;
    cur_col = 0;
    view_off = 0;
    for (int r = 0; r < HIST_LINES; r++)
        for (int c = 0; c < COLS_MAX; c++) {
            hist_cp[r][c] = ' ';
            hist_attr[r][c] = attr;
        }
    render();
    hw_cursor();
}

void vga_scroll_view(int delta)
{
    int max_off = hist_total - trows();
    if (max_off < 0) max_off = 0;
    view_off += delta;
    if (view_off < 0) view_off = 0;
    if (view_off > max_off) view_off = max_off;
    render();
    hw_cursor();
}

int vga_view_offset(void) { return view_off; }

int vga_view_max(void)
{
    int max_off = hist_total - trows();
    return max_off > 0 ? max_off : 0;
}

void vga_scroll_reset(void)
{
    if (view_off == 0)
        return;
    view_off = 0;
    render();
    hw_cursor();
}

/* 写一个码点到当前行 */
void vga_putc_cp(u32 cp)
{
    int cols = tcols();
    /* 快速路径: 普通可打印字符, 只写当前 cell, 不整屏刷新 */
    if (cp >= 32 && cp < 0x3000 && view_off == 0 && cur_col + 1 < cols) {
        hist_cp[hist_head][cur_col] = (u16)cp;
        hist_attr[hist_head][cur_col] = attr;
        if (gfx_active()) {
            gfx_append_cell(cur_col, (u16)cp, attr);
        } else {
            vga[(TXT_ROWS - 1) * VGA_COLS + cur_col] = (u16)((attr << 8) | cp);
        }
        cur_col++;
        hw_cursor();
        return;
    }
    /* 双宽汉字快速路径 (图形模式) */
    if (is_wide(cp) && view_off == 0 && cur_col + 1 < cols) {
        hist_cp[hist_head][cur_col] = (u16)cp;
        hist_attr[hist_head][cur_col] = attr;
        hist_cp[hist_head][cur_col + 1] = RIGHT_HALF;
        hist_attr[hist_head][cur_col + 1] = attr;
        if (gfx_active())
            gfx_append_cell(cur_col, (u16)cp, attr);
        cur_col += 2;
        hw_cursor();
        return;
    }

    if (view_off > 0)
        view_off = 0;

    if (cp == '\n') {
        new_line();
    } else if (cp == '\r') {
        cur_col = 0;
    } else if (cp == '\t') {
        int nc = (cur_col + 4) & ~3;
        if (nc >= cols)
            new_line();
        else
            cur_col = nc;
    } else if (cp == '\b') {
        if (cur_col > 0) {
            u16 old = hist_cp[hist_head][cur_col - 1];
            cur_col--;
            hist_cp[hist_head][cur_col] = ' ';
            hist_attr[hist_head][cur_col] = attr;
            if (old == RIGHT_HALF && cur_col > 0) {
                cur_col--;
                hist_cp[hist_head][cur_col] = ' ';
                hist_attr[hist_head][cur_col] = attr;
            }
        }
    } else if (cp < 32) {
        /* 其它控制字符忽略 */
    } else if (is_wide(cp)) {
        if (cur_col + 1 >= cols)
            new_line();
        hist_cp[hist_head][cur_col] = (u16)cp;
        hist_attr[hist_head][cur_col] = attr;
        hist_cp[hist_head][cur_col + 1] = RIGHT_HALF;
        hist_attr[hist_head][cur_col + 1] = attr;
        cur_col += 2;
    } else {
        hist_cp[hist_head][cur_col] = (cp < 128) ? (u16)cp : (u16)'?';
        hist_attr[hist_head][cur_col] = attr;
        cur_col++;
        if (cur_col >= cols)
            new_line();
    }

    render();
    hw_cursor();
}

/* UTF-8 解码状态 */
static u32 u8_need;
static u32 u8_acc;

void vga_putc(char c)
{
    u8 b = (u8)c;
    if (u8_need) {
        if ((b & 0xC0) == 0x80) {
            u8_acc = (u8_acc << 6) | (b & 0x3F);
            if (--u8_need == 0) {
                u32 cp = u8_acc;
                u8_acc = 0;
                vga_putc_cp(cp);
            }
            return;
        }
        u8_need = 0;
        u8_acc = 0;
    }
    if (b < 0x80) {
        vga_putc_cp(b);
    } else if ((b & 0xE0) == 0xC0) {
        u8_acc = b & 0x1F; u8_need = 1;
    } else if ((b & 0xF0) == 0xE0) {
        u8_acc = b & 0x0F; u8_need = 2;
    } else if ((b & 0xF8) == 0xF0) {
        u8_acc = b & 0x07; u8_need = 3;
    } else {
        vga_putc_cp('?');
    }
}
