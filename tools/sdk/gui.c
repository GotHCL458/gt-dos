/* GT-DOS TC 标准库: gui.c (仅在 #include <gui.h> 后编入程序)
   在底层 gfx_* 系统调用之上提供面板/按钮/复选框/单选/进度条等组合控件. */
#include <gui.h>

static int gt_len(char *s)
{
    int n = 0;
    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

/* 面板: 背景 + 边框 + 标题栏 */
int gt_panel(int x, int y, int w, int h, char *title)
{
    struct gt_gui_info gi;
    gfx_info(&gi);
    gfx_fill(x, y, w, h, GT_PANEL);
    gfx_frame(x, y, w, h, 0x9aa8c0);
    gfx_fill(x + 2, y + 2, w - 4, gi.ch + 8, GT_TITLE);
    if (title)
        gfx_text(x + 8, y + 6, title, GT_TEXT);
    return 0;
}

/* 按钮: hot 为真时用高亮配色 */
int gt_button(int x, int y, int w, int h, char *label, int hot)
{
    struct gt_gui_info gi;
    gfx_info(&gi);
    gfx_fill(x, y, w, h, hot ? 0x3f6fae : GT_BTN);
    gfx_frame(x, y, w, h, hot ? GT_ACCENT : 0xbfd0e8);
    if (label) {
        int tw = gt_len(label) * gi.cw;
        gfx_text(x + (w - tw) / 2, y + (h - gi.ch) / 2, label, GT_TEXT);
    }
    return 0;
}

/* 复选框 */
int gt_checkbox(int x, int y, char *label, int on)
{
    struct gt_gui_info gi;
    int box;
    gfx_info(&gi);
    box = gi.ch - 4;
    gfx_fill(x, y + 2, box, box, 0x0e1622);
    gfx_frame(x, y + 2, box, box, 0xbfd0e8);
    if (on)
        gfx_fill(x + box / 3, y + 2 + box / 3, box / 3, box / 3, GT_ACCENT);
    if (label)
        gfx_text(x + box + 6, y, label, on ? GT_TEXT : GT_MUTED);
    return 0;
}

/* 单选按钮 */
int gt_radio(int x, int y, char *label, int on)
{
    struct gt_gui_info gi;
    int box;
    gfx_info(&gi);
    box = gi.ch - 4;
    gfx_fill(x, y + 2, box, box, 0x0e1622);
    gfx_frame(x, y + 2, box, box, 0xbfd0e8);
    if (on)
        gfx_fill(x + box / 4, y + 2 + box / 4, box / 2, box / 2, GT_ACCENT);
    if (label)
        gfx_text(x + box + 6, y, label, on ? GT_TEXT : GT_MUTED);
    return 0;
}

/* 进度条 */
int gt_progress(int x, int y, int w, int h, int pct)
{
    gfx_bar(x, y, w, h, pct, 0x4f9fdf);
    return 0;
}

/* 命中测试: 事件坐标在矩形内且左键按下 */
int gt_hit(int x, int y, int w, int h, struct gt_gui_event *ev)
{
    if (!ev)
        return 0;
    if (!(ev->buttons & 1))
        return 0;
    return ev->x >= x && ev->x < x + w && ev->y >= y && ev->y < y + h;
}
