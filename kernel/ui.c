/* ==========================================================================
 *  GT-DOS 通用输入层 (shell / OOBE / 登录 共用)
 *  ---------------------------------------------------------------------------
 *  - PS/2 键盘与 COM1 串口双路输入, 串口 ANSI 转义解码成 KEY_* 虚拟键码
 *  - 带行编辑 (左右/Home/End/Del/插入/退格) 的整行读取
 *  - 命令历史 (上下翻阅), 密码掩码模式
 * ========================================================================== */
#include "gt.h"

#define UI_LINE_MAX 160
#define HIST_MAX    32
#define HIST_LEN    UI_LINE_MAX

/* ------------------------------------------------------------ 串口 ANSI */
static int serial_spin(void)
{
    for (int spin = 0; spin < 2000; spin++) {
        int c = serial_getchar();
        if (c >= 0)
            return c;
    }
    return -1;
}

static int serial_read_key(void)
{
    int c = serial_getchar();
    if (c < 0)
        return -1;
    if (c != 0x1B)
        return c;

    int b1 = serial_spin();
    if (b1 < 0)
        return 27;                    /* 单独的 ESC */
    if (b1 != '[' && b1 != 'O')
        return 27;

    int p[4] = { 0, 0, 0, 0 };
    int np = 0, cur = 0;
    int fin = -1;
    for (int guard = 0; guard < 12; guard++) {
        int b = serial_spin();
        if (b < 0)
            return 27;
        if (b >= '0' && b <= '9') {
            cur = cur * 10 + (b - '0');
            continue;
        }
        if (b == ';') {
            if (np < 4) p[np++] = cur;
            cur = 0;
            continue;
        }
        fin = b;
        break;
    }
    if (fin < 0)
        return 27;
    if (np < 4) p[np++] = cur;

    int mod = (np >= 2) ? p[1] : 1;   /* xterm 修饰符: 5=Ctrl 7=Ctrl+Alt */

    switch (fin) {
    case 'A': return (mod == 5) ? KEY_CUP   : KEY_UP;
    case 'B': return (mod == 5) ? KEY_CDOWN : KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'Z': return 27;
    }

    if (fin == '~') {
        switch (p[0]) {
        case 1: return (mod == 5) ? KEY_CUP   : KEY_HOME;
        case 2: return KEY_INS;
        case 3: return (mod == 7) ? KEY_REBOOT : KEY_DEL;  /* Ctrl+Alt+Del */
        case 4: return (mod == 5) ? KEY_CDOWN : KEY_END;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        default: return 27;
        }
    }
    return 27;
}

/* 统一按键读取 (阻塞). 返回 ASCII 或 KEY_* */
int ui_read_key(void)
{
    for (;;) {
        int v = keyboard_read();
        if (v >= 0)
            return v;
        v = serial_read_key();
        if (v >= 0)
            return v;
        cpu_relax();                  /* 空闲时派发软中断 (扫描码解码) */
    }
}

/* ---------------------------------------------------------- 彩色打印 */
void ui_err(const char *fmt, ...)
{
    va_list ap;
    gt_color(C_LRED);
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    gt_color_reset();
}

void ui_ok(const char *fmt, ...)
{
    va_list ap;
    gt_color(C_LGREEN);
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    gt_color_reset();
}

void ui_info(const char *fmt, ...)
{
    va_list ap;
    gt_color(C_LCYAN);
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    gt_color_reset();
}

/* ------------------------------------------------------------ 命令历史 */
static char hist[HIST_MAX][HIST_LEN];
static int  hist_count;

/* 滚动回调 (由 shell 注册): delta>0 上翻 */
static void (*scroll_cb)(int delta);
void ui_set_scroll_cb(void (*cb)(int delta)) { scroll_cb = cb; }

void ui_hist_add(const char *line)
{
    if (line[0] == '\0')
        return;
    if (hist_count > 0 && strcmp(hist[hist_count - 1], line) == 0)
        return;
    if (hist_count < HIST_MAX) {
        strncpy(hist[hist_count], line, HIST_LEN - 1);
        hist[hist_count][HIST_LEN - 1] = '\0';
        hist_count++;
    } else {
        for (int i = 1; i < HIST_MAX; i++)
            memcpy(hist[i - 1], hist[i], HIST_LEN);
        strncpy(hist[HIST_MAX - 1], line, HIST_LEN - 1);
        hist[HIST_MAX - 1][HIST_LEN - 1] = '\0';
    }
}

/* ------------------------------------------------------------ 行编辑器 */
static void ed_draw(const char *prompt, const char *buf, int len, int cur,
                    int mask)
{
    static char shown[UI_LINE_MAX];
    if (mask) {
        int n = len < (int)sizeof(shown) - 1 ? len : (int)sizeof(shown) - 1;
        for (int i = 0; i < n; i++)
            shown[i] = (char)mask;
        shown[n] = '\0';
        console_line_refresh(prompt, shown, n, cur, 0);
    } else {
        console_line_refresh(prompt, buf, len, cur, 0);
    }
}

/* 返回 0=回车提交, 1=Ctrl-C 放弃, -1=Ctrl-Alt-Del */
int ui_read_line(const char *prompt, char *buf, int max, int mask, bool history)
{
    int len = 0, cur = 0;
    int hist_at = hist_count;
    static char stash[UI_LINE_MAX];
    stash[0] = '\0';
    buf[0] = '\0';

    ed_draw(prompt, buf, len, cur, mask);

    for (;;) {
        int k = ui_read_key();

        if (k == KEY_REBOOT)
            return -1;

        if (k == '\r' || k == '\n') {
            console_line_end();
            buf[len] = '\0';
            return 0;
        }
        if (k == 3) {                 /* Ctrl-C */
            console_puts("^C\n");
            buf[0] = '\0';
            return 1;
        }
        if (k == KEY_LEFT) {
            if (cur > 0) { cur--; ed_draw(prompt, buf, len, cur, mask); }
            continue;
        }
        if (k == KEY_RIGHT) {
            if (cur < len) { cur++; ed_draw(prompt, buf, len, cur, mask); }
            continue;
        }
        if (k == KEY_HOME || k == 1) {
            cur = 0; ed_draw(prompt, buf, len, cur, mask); continue;
        }
        if (k == KEY_END || k == 5) {
            cur = len; ed_draw(prompt, buf, len, cur, mask); continue;
        }
        if (k == KEY_DEL) {
            if (cur < len) {
                for (int i = cur; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                buf[len] = '\0';
                ed_draw(prompt, buf, len, cur, mask);
            }
            continue;
        }
        if (k == '\b' || k == 127) {
            if (cur > 0) {
                for (int i = cur; i < len; i++)
                    buf[i - 1] = buf[i];
                len--;
                cur--;
                buf[len] = '\0';
                ed_draw(prompt, buf, len, cur, mask);
            }
            continue;
        }
        /* 滚动键: 交给 shell 注册的回调 (Ctrl+Up/Down, PgUp/PgDn) */
        if (scroll_cb) {
            if (k == KEY_CUP)  { scroll_cb(+3);  continue; }
            if (k == KEY_CDOWN){ scroll_cb(-3);  continue; }
            if (k == KEY_PGUP) { scroll_cb(+20); continue; }
            if (k == KEY_PGDN) { scroll_cb(-20); continue; }
        }
        if (k == 27) {                /* ESC: 清空当前行 */
            len = cur = 0;
            buf[0] = '\0';
            ed_draw(prompt, buf, len, cur, mask);
            continue;
        }

        if (history) {
            if (k == KEY_UP || k == 16) {
                if (hist_at == 0)
                    continue;
                if (hist_at == hist_count)
                    memcpy(stash, buf, sizeof(stash));
                hist_at--;
                len = (int)strlen(hist[hist_at]);
                if (len > max - 1) len = max - 1;
                memcpy(buf, hist[hist_at], len);
                buf[len] = '\0';
                cur = len;
                ed_draw(prompt, buf, len, cur, mask);
                continue;
            }
            if (k == KEY_DOWN || k == 14) {
                if (hist_at >= hist_count)
                    continue;
                hist_at++;
                const char *src = (hist_at == hist_count) ? stash
                                                          : hist[hist_at];
                len = (int)strlen(src);
                if (len > max - 1) len = max - 1;
                memcpy(buf, src, len);
                buf[len] = '\0';
                cur = len;
                ed_draw(prompt, buf, len, cur, mask);
                continue;
            }
        }

        if (k >= 32 && k <= 126 && len + 1 < max) {
            for (int i = len; i > cur; i--)
                buf[i] = buf[i - 1];
            buf[cur] = (char)k;
            len++;
            cur++;
            buf[len] = '\0';
            ed_draw(prompt, buf, len, cur, mask);
        }
    }
}