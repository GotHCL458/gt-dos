/* ==========================================================================
 *  GT-DOS TUI 框架
 *  ---------------------------------------------------------------------------
 *  - 启动画面 (splash): ASCII logo + 进度条, VGA 彩色 / 串口纯文本
 *  - 对话框边框: 双线框 (box-drawing 字符), 串口退化为分隔线
 *  - 底部状态栏: 用户@主机 盘符:目录 | 时钟
 * ========================================================================== */
#include "gt.h"

/* 串口是否处于"纯文本终端"模式 (TUI 装饰只画到 VGA) */
static bool serial_plain(void) { return console_serial_enabled(); }

static void ser_line(const char *s)
{
    if (serial_plain()) {
        serial_puts(s);
        serial_putc('\n');
    }
}

/* ---------------------------------------------------------------- 状态栏 */
void tui_status_refresh(void)
{
    char left[80], right[40];
    int k = 0;
    const char *u = user_name();
    while (*u && k < 60) left[k++] = *u++;
    left[k++] = '@';
    const char *h = gcfg.hostname;
    while (*h && k < 60) left[k++] = *h++;
    left[k] = '\0';

    int y, mo, d, hh, mm, ss;
    rtc_read(&y, &mo, &d, &hh, &mm, &ss);
    k = 0;
    const char *fmt = "%04d-%02d-%02d %02d:%02d";
    /* 手写格式化, 避免依赖 printf 到栈缓冲 */
    char tmp[8];
    #define PUTN(v, w) do { itoa_base((u32)(v), tmp, 10, false); \
        int l = (int)strlen(tmp); for (int z = l; z < (w); z++) right[k++] = '0'; \
        for (char *q = tmp; *q; q++) right[k++] = *q; } while (0)
    (void)fmt;
    PUTN(y, 4); right[k++] = '-';
    PUTN(mo, 2); right[k++] = '-';
    PUTN(d, 2); right[k++] = ' ';
    PUTN(hh, 2); right[k++] = ':';
    PUTN(mm, 2);
    right[k] = '\0';

    vga_set_status(left, right);
}

void tui_clock_update(void)
{
    tui_status_refresh();
}

/* ---------------------------------------------------------------- 边框 */
/* ASCII 边框: 不占用 VGA 文本模式 128..255 汉字槽池, VGA 与串口显示一致,
 * 不会出现问号. 用 '=' 横线 + '+' 角 + '|' 竖线模拟双线感. */
void tui_frame_open(const char *title)
{
    char line[84];
    int i;

    /* 顶边 */
    line[0] = '+';
    for (i = 1; i < 79; i++)
        line[i] = '=';
    line[79] = '\0';
    vga_set_color(C_LCYAN, C_BLACK);
    vga_puts(line);
    vga_putc('\n');

    /* 标题行: | 标题(左对齐) + 填充 | */
    if (title && title[0]) {
        int tl = (int)strlen(title);
        if (tl > 76) tl = 76;
        for (i = 0; i < 80; i++) line[i] = ' ';
        line[0] = '|';
        line[79] = '|';
        memcpy(line + 2, title, tl);
        line[80] = '\0';
        vga_set_color(C_YELLOW, C_BLACK);
        vga_puts(line);
        vga_putc('\n');
    }

    /* 串口: 纯文本分隔线 */
    ser_line("+==============================================="
             "===============================+");
    if (title && title[0])
        ser_line(title);

    gt_color(C_LGRAY);
}

void tui_frame_close(void)
{
    char line[84];
    int i;
    line[0] = '+';
    for (i = 1; i < 79; i++)
        line[i] = '=';
    line[79] = '\0';
    vga_set_color(C_LCYAN, C_BLACK);
    vga_puts(line);
    vga_putc('\n');
    gt_color_reset();
}

/* ---------------------------------------------------------------- 启动画面 */
void tui_splash(void)
{
    console_clear();
    vga_clear_status();

    vga_set_color(C_LCYAN, C_BLACK);
    const char *art[] = {
        "  ________ _______  _____      ____   ___  ____",
        " |  ____  |  _   ||  _  \\    / ___| / _ \\/ ___|",
        " | |__    | |_| || |_| |   | |  _ | | | \\__ \\",
        " |  __|   |  _   ||  _  /    | |_| || |_| |___) |",
        " |_|      |_| |_||_| \\_\\     \\____| \\___/|____/",
        0
    };
    for (int i = 0; art[i]; i++) {
        vga_puts(art[i]);
        vga_putc('\n');
    }
    vga_set_color(C_LMAGENTA, C_BLACK);
    vga_puts(L("splash.sub"));
    gt_color_reset();

    /* 串口纯文本版 */
    if (serial_plain()) {
        serial_puts("\n  GT-DOS v0.6  --  a tiny DOS-like OS (NASM + C)\n\n");
    }

    /* 进度条 */
    static const char *step_keys[] = { "splash.s1", "splash.s2", "splash.s3", 0 };
    for (int s = 0; step_keys[s]; s++) {
        const char *label = L(step_keys[s]);
        char bar[80];
        int k = 0;
        int cols = 0;
        bar[k++] = '\r';
        bar[k++] = ' ';
        cols += 1;
        const char *p = label;
        while (*p) {
            if ((*p & 0xC0) != 0x80) cols++;
            if (cols >= 22) break;
            bar[k++] = *p++;
        }
        while (cols < 23) { bar[k++] = ' '; cols++; }
        bar[k++] = '[';
        for (int j = 0; j < 20; j++) {
            int done = (j * 3) <= (s * 20 + 19);
            bar[k++] = done ? '#' : '.';
        }
        bar[k++] = ']';
        bar[k] = '\0';
        vga_set_color(C_LGREEN, C_BLACK);
        vga_puts(bar);
        gt_color_reset();
        if (serial_plain())
            serial_puts(bar);
        timer_sleep_ms(220);
    }
    vga_puts("\n");
    if (serial_plain())
        serial_puts("\n");
    timer_sleep_ms(300);
    console_clear();
}

void tui_puts(const char *s)
{
    console_puts(s);
}