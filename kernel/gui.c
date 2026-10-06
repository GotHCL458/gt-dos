/* ==========================================================================
 *  GT-DOS 图形桌面 (真桌面 + 多窗口管理)
 *  ---------------------------------------------------------------------------
 *  - 渲染: 全部走 gfx.c 双缓冲层 (先画后台缓冲, 每帧一次 gfx_flip) -> 无闪烁
 *  - 桌面: 渐变壁纸 + 桌面图标 (单击打开窗口) + 顶部菜单栏 + 底部任务栏
 *  - 窗口: 最多 6 个, z-order 层叠; 标题栏拖拽移动 / 关闭按钮 / 点击置顶 /
 *          任务栏按钮最小化与还原
 *  - 控件: 按钮 / 复选框 / 进度条 (自研, 命中测试分发)
 *  - 鼠标: 指针位置直接取 mouse_x()/mouse_y() (绝对坐标, 唯一数据源),
 *          点击/滚轮走事件环 mouse_pop_event() -> 修复移动与点击冲突
 *  - 键盘: ESC 退出桌面
 *  退出: 点任务栏 "退出" 或按 ESC, 返回文本控制台.
 * ========================================================================== */
#include "gt.h"

#define MENUBAR_H 22
#define TASKBAR_H 30
#define TITLE_H   22
#define MAX_WIN   6

/* 运行时分辨率 (进入桌面时快照) */
static int SCR_W, SCR_H;

static bool gui_on;

bool gui_active(void) { return gui_on; }

/* ------------------------------------------------------------ 颜色 (RGB) */
#define COL_WHITE   0xFFFFFFu
#define COL_BLACK   0x000000u
#define COL_WIN     0xD4D0C8u   /* 窗口底 (经典灰) */
#define COL_FACE    0xC0C0C0u
#define COL_HI      0xEDEDEDu
#define COL_DK      0x808080u
#define COL_TITLE   0x0A246Au   /* 活动标题深蓝 */
#define COL_TITLE2  0x3A6EA5u   /* 非活动标题 */
#define COL_SEL     0x0A5ACD
#define COL_TEXT    0x101010u
#define COL_TASK    0x2A4A7A

/* ---------------------------------------------------------- 窗口类型 */
enum { WK_DEMO, WK_ABOUT, WK_SYS };

struct win {
    int  kind;
    bool open;        /* 存在 (未关闭) */
    bool min;         /* 最小化 */
    int  x, y, w, h;
    const char *title_key;
    int  counter;     /* demo: 按钮计数 */
    int  chk;         /* demo: 复选框 */
    int  scroll;      /* about/sys: 内容滚动 */
};

static struct win wins[MAX_WIN];
static int zorder[MAX_WIN];        /* 前->后 的窗口索引 */
static int top = -1;               /* 顶层 (活动) 窗口索引, -1=桌面 */

static void win_init(void)
{
    for (int i = 0; i < MAX_WIN; i++) {
        wins[i].open = false;
        wins[i].min = false;
        wins[i].counter = 0;
        wins[i].chk = 0;
        wins[i].scroll = 0;
        zorder[i] = i;
    }
    top = -1;
}

/* 打开 (或激活) 指定类型窗口 */
static void win_open(int kind)
{
    int idx = -1;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].open && wins[i].kind == kind) { idx = i; break; }
    if (idx < 0) {
        for (int i = 0; i < MAX_WIN; i++)
            if (!wins[i].open) { idx = i; break; }
        if (idx < 0) return;                 /* 满 */
        struct win *w = &wins[idx];
        w->open = true; w->min = false; w->kind = kind;
        w->counter = 0; w->chk = 0; w->scroll = 0;
        w->title_key = kind == WK_DEMO ? "gui.demo"
                     : kind == WK_ABOUT ? "gui.about" : "gui.sys";
        int n = 0;
        for (int i = 0; i < MAX_WIN; i++) if (wins[i].open) n++;
        w->x = 40 + (n - 1) * 26;
        w->y = 50 + (n - 1) * 24;
        w->w = kind == WK_DEMO ? 300 : 320;
        w->h = kind == WK_DEMO ? 200 : 240;
    }
    wins[idx].min = false;
    top = idx;
    /* 提到 z-order 最前 */
    for (int i = 0; i < MAX_WIN; i++)
        if (zorder[i] == idx) {
            for (int j = i; j > 0; j--) zorder[j] = zorder[j - 1];
            zorder[0] = idx;
            break;
        }
}

static void win_raise(int idx)
{
    top = idx;
    for (int i = 0; i < MAX_WIN; i++)
        if (zorder[i] == idx) {
            for (int j = i; j > 0; j--) zorder[j] = zorder[j - 1];
            zorder[0] = idx;
            break;
        }
}

static void win_close(int idx)
{
    wins[idx].open = false;
    if (top == idx) top = -1;
}

static bool in_rect(int p_, int q, int x, int y, int w, int h)
{
    return p_ >= x && p_ < x + w && q >= y && q < y + h;
}

/* ------------------------------------------------------ 桌面图标 */
struct icon { int kind; int y; const char *label_key; };
static const struct icon icons[] = {
    { WK_DEMO,  48, "gui.demo" },
    { WK_ABOUT, 128, "gui.about" },
    { WK_SYS,   208, "gui.sys" },
};
#define NICON ((int)(sizeof(icons) / sizeof(icons[0])))
#define ICON_X() (SCR_W - 60)        /* 贴右边缘, 不与级联窗口重叠 */

/* ---------------------------------------------------------- 绘制小工具 */
static void draw_icon_glyph(int x, int y, int kind)
{
    /* 32x32 简易图标 */
    u32 c1 = kind == WK_DEMO ? 0x3A6EA5u : kind == WK_ABOUT ? 0xB8860Bu : 0x2E8B57u;
    gfx_fill(x, y, 32, 32, c1);
    gfx_bevel(x, y, 32, 32, COL_HI, COL_DK);
    gfx_fill(x + 6, y + 6, 20, 4, COL_WHITE);
    gfx_fill(x + 6, y + 14, 20, 3, COL_WHITE);
    gfx_fill(x + 6, y + 21, 14, 3, COL_WHITE);
}

static void draw_desktop(void)
{
    /* 垂直渐变壁纸 */
    for (int y = 0; y < SCR_H; y++) {
        u32 t = (u32)y * 255 / (SCR_H - 1);
        u32 r = 0x10 + t * 0x20 / 255;
        u32 g = 0x30 + t * 0x40 / 255;
        u32 b = 0x60 + t * 0x60 / 255;
        u32 col = (r << 16) | (g << 8) | b;
        gfx_fill(0, y, SCR_W, 1, col);
    }
    /* 图标 */
    for (int i = 0; i < NICON; i++) {
        int ix = ICON_X();
        draw_icon_glyph(ix, icons[i].y, icons[i].kind);
        const char *lab = L(icons[i].label_key);
        int tw = gfx_text_w(lab);
        int tx = ix + 16 - tw / 2;
        if (tx < 0) tx = 0;
        /* 文字阴影提升可读性 */
        gfx_text(tx + 1, icons[i].y + 34 + 1, lab, 0x000000u);
        gfx_text(tx, icons[i].y + 34, lab, COL_WHITE);
    }
}

static void draw_menubar(void)
{
    gfx_fill(0, 0, SCR_W, MENUBAR_H, COL_TASK);
    gfx_bevel(0, 0, SCR_W, MENUBAR_H, COL_HI, COL_DK);
    gfx_text(8, 4, L("gui.welcome"), COL_WHITE);
    /* 右侧用户名 */
    const char *u = user_name();
    int uw = gfx_text_w(u) + 8;
    gfx_text(SCR_W - uw, 4, u, COL_WHITE);
}

/* ---------------------------------------------------------- 窗口内容 */
static void draw_button(int x, int y, int w, int h, const char *lab, bool hot)
{
    gfx_fill(x, y, w, h, hot ? COL_HI : COL_FACE);
    gfx_bevel(x, y, w, h, COL_WHITE, COL_DK);
    int tw = gfx_text_w(lab);
    gfx_text(x + (w - tw) / 2, y + (h - 16) / 2, lab, COL_TEXT);
}

/* demo 窗口控件的屏幕矩形 (供命中测试) */
static void demo_rects(struct win *w, int r[4][4])
{
    r[0][0] = w->x + 20; r[0][1] = w->y + 60; r[0][2] = 120; r[0][3] = 30; /* btn */
    r[1][0] = w->x + 20; r[1][1] = w->y + 105; r[1][2] = 180; r[1][3] = 22; /* chk */
    r[2][0] = w->x + 20; r[2][1] = w->y + 145; r[2][2] = 240; r[2][3] = 16; /* bar */
    r[3][0] = w->x + 180; r[3][1] = w->y + 60; r[3][2] = 100; r[3][3] = 30; /* exit */
}

static void draw_window(struct win *w, int mx, int my)
{
    bool active = (top == (int)(w - wins));
    /* 阴影 */
    gfx_fill(w->x + 4, w->y + 4, w->w, w->h, 0x00101010u);
    /* 主体 */
    gfx_fill(w->x, w->y, w->w, w->h, COL_WIN);
    gfx_bevel(w->x, w->y, w->w, w->h, COL_WHITE, COL_DK);
    /* 标题栏 */
    gfx_fill(w->x + 2, w->y + 2, w->w - 4, TITLE_H,
             active ? COL_TITLE : COL_TITLE2);
    gfx_text(w->x + 8, w->y + 5, L(w->title_key), COL_WHITE);
    /* 关闭按钮 */
    int cx = w->x + w->w - 20, cy = w->y + 5;
    bool hotc = in_rect(mx, my, cx, cy - 2, 16, 16);
    gfx_fill(cx, cy - 2, 16, 16, hotc ? 0xC04040u : 0xE0E0E0u);
    gfx_bevel(cx, cy - 2, 16, 16, COL_WHITE, COL_DK);
    gfx_fill(cx + 4, cy + 2, 8, 2, COL_BLACK);
    gfx_fill(cx + 7, cy - 1, 2, 8, COL_BLACK);

    int content_y = w->y + TITLE_H + 4;

    if (w->kind == WK_DEMO) {
        int r[4][4];
        demo_rects(w, r);
        char buf[48];
        const char *pre = L("gui.clicks");
        int n = 0;
        while (*pre && n < 30) buf[n++] = *pre++;
        itoa_base((u32)w->counter, buf + n, 10, false);
        while (buf[n]) n++;
        buf[n] = '\0';
        gfx_text(w->x + 20, content_y, buf, COL_TEXT);
        /* 按钮 */
        draw_button(r[0][0], r[0][1], r[0][2], r[0][3], L("gui.press"),
                    in_rect(mx, my, r[0][0], r[0][1], r[0][2], r[0][3]));
        /* 复选框 */
        int bx = r[1][0], by = r[1][1];
        gfx_fill(bx, by + 3, 16, 16, COL_WHITE);
        gfx_frame(bx, by + 3, 16, 16, COL_DK);
        if (w->chk) {
            for (int i = 0; i < 6; i++) gfx_fill(bx + 3 + i, by + 11 - i / 2, 1, 1, COL_TITLE);
            for (int i = 0; i < 9; i++) gfx_fill(bx + 7 + i, by + 6 + i, 1, 1, COL_TITLE);
        }
        gfx_text(bx + 24, by + 3, L("gui.chk"), COL_TEXT);
        /* 进度条 */
        int px = r[2][0], py = r[2][1];
        gfx_fill(px, py, r[2][2], r[2][3], COL_WHITE);
        gfx_frame(px, py, r[2][2], r[2][3], COL_DK);
        int fw = (r[2][2] - 4) * (w->counter > 10 ? 10 : w->counter) / 10;
        gfx_fill(px + 2, py + 2, fw, r[2][3] - 4, COL_SEL);
        /* 退出窗口按钮 */
        draw_button(r[3][0], r[3][1], r[3][2], r[3][3], L("gui.close"),
                    in_rect(mx, my, r[3][0], r[3][1], r[3][2], r[3][3]));
    } else {
        /* about / sys: 文本内容 */
        const char *lines[12];
        int nl = 0;
        char l0[64], l1[64], l2[64];
        if (w->kind == WK_ABOUT) {
            lines[nl++] = L("gui.about_t");
            lines[nl++] = L("gui.about_v");
            lines[nl++] = L("gui.about_d");
            lines[nl++] = L("gui.about_f");
        } else {
            lines[nl++] = L("gui.sys_t");
            u32 mb = (u32)(*(volatile u16 *)0x413);   /* 常规内存 KB */
            l0[0] = 0;
            int k = 0; const char *p = L("gui.sys_mem");
            while (*p && k < 40) l0[k++] = *p++;
            itoa_base(mb, l0 + k, 10, false);
            while (l0[k]) k++;
            l0[k++] = ' '; l0[k++] = 'K'; l0[k++] = 'B'; l0[k] = 0;
            lines[nl++] = l0;
            int up = timer_uptime_ms() / 1000;
            int k2 = 0; const char *p2 = L("gui.sys_up");
            while (*p2 && k2 < 40) l1[k2++] = *p2++;
            itoa_base((u32)(up / 60), l1 + k2, 10, false);
            while (l1[k2]) k2++;
            l1[k2++] = ':';
            itoa_base((u32)(up % 60), l1 + k2, 10, false);
            while (l1[k2]) k2++;
            if (l1[k2 - 2] < '0' || l1[k2 - 2] > '9') { /* 补零 */ }
            lines[nl++] = l1;
            int k3 = 0; const char *p3 = L("gui.sys_drv");
            while (*p3 && k3 < 40) l2[k3++] = *p3++;
            itoa_base((u32)fat_drive_count(), l2 + k3, 10, false);
            lines[nl++] = l2;
        }
        for (int i = 0; i < nl; i++) {
            int yy = content_y + 6 + i * 22 - w->scroll;
            if (yy > w->y && yy < w->y + w->h - 18)
                gfx_text(w->x + 16, yy, lines[i], COL_TEXT);
        }
    }
}

static void draw_taskbar(int mx, int my, bool start_open)
{
    int ty = SCR_H - TASKBAR_H;
    gfx_fill(0, ty, SCR_W, TASKBAR_H, COL_TASK);
    gfx_bevel(0, ty, SCR_W, TASKBAR_H, COL_HI, COL_DK);
    /* 开始按钮 */
    int sx = 4, sy = ty + 5, sw = 80, sh = 20;
    draw_button(sx, sy, sw, sh, L("gui.start"),
                start_open || in_rect(mx, my, sx, sy, sw, sh));
    /* 窗口按钮 */
    int bx = sx + sw + 8;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].open) continue;
        int bw = 96;
        bool act = (top == i) && !wins[i].min;
        gfx_fill(bx, sy, bw, sh, act ? COL_HI : COL_FACE);
        gfx_bevel(bx, sy, bw, sh, COL_WHITE, COL_DK);
        const char *t = L(wins[i].title_key);
        gfx_text(bx + 4, sy + 2, t, COL_TEXT);
        bx += bw + 4;
    }
    /* 退出桌面按钮 (右侧) */
    int ex = SCR_W - 88;
    draw_button(ex, sy, 84, sh, L("gui.exit"),
                in_rect(mx, my, ex, sy, 84, sh));
    /* 时钟 */
    int yy2, mo, d, hh, mm, ss;
    rtc_read(&yy2, &mo, &d, &hh, &mm, &ss);
    char clk[8]; const char *dg = "0123456789";
    clk[0] = dg[hh / 10]; clk[1] = dg[hh % 10]; clk[2] = ':';
    clk[3] = dg[mm / 10]; clk[4] = dg[mm % 10]; clk[5] = 0;
    gfx_text(ex - 56, sy + 2, clk, COL_WHITE);
}

static void draw_startmenu(int mx, int my)
{
    int mw = 160, mh = 108;
    int mx0 = 4, my0 = SCR_H - TASKBAR_H - mh;
    gfx_fill(mx0, my0, mw, mh, COL_WIN);
    gfx_bevel(mx0, my0, mw, mh, COL_WHITE, COL_DK);
    struct { const char *key; int kind; } items[4] = {
        { "gui.demo", WK_DEMO }, { "gui.about", WK_ABOUT },
        { "gui.sys", WK_SYS },   { "gui.exit", -1 },
    };
    for (int i = 0; i < 4; i++) {
        int iy = my0 + 8 + i * 24;
        if (in_rect(mx, my, mx0 + 4, iy, mw - 8, 22))
            gfx_fill(mx0 + 4, iy, mw - 8, 22, COL_SEL);
        gfx_text(mx0 + 12, iy + 3, L(items[i].key),
                 in_rect(mx, my, mx0 + 4, iy, mw - 8, 22) ? COL_WHITE : COL_TEXT);
    }
}

/* 箭头指针 */
static void draw_cursor(int mx, int my)
{
    static const char *arrow[13] = {
        "X.......", "XX......", "XXX.....", "XXXX....", "XXXXX...",
        "XXXXXX..", "XXXXXXX.", "XXXXXXXX", "XXXXXX..", "XX.XX...",
        "X..XX...", "...XX...", "....X...",
    };
    for (int j = 0; j < 13; j++)
        for (int i = 0; i < 8; i++)
            if (arrow[j][i] == 'X') {
                gfx_fill(mx + i, my + j, 1, 1, COL_WHITE);
                if (i + 1 < 8 && arrow[j][i + 1] != 'X')
                    gfx_fill(mx + i + 1, my + j, 1, 1, COL_BLACK);
            }
    /* 黑色轮廓 */
    for (int j = 0; j < 13; j++)
        for (int i = 0; i < 8; i++)
            if (arrow[j][i] == 'X' && (i == 0 || j == 0))
                gfx_fill(mx + i, my + j, 1, 1, COL_BLACK);
}

/* ---------------------------------------------------------- 交互逻辑 */
static int drag_idx = -1;          /* 正在拖拽的窗口 */
static int drag_dx, drag_dy;
static bool start_open;

/* 命中测试: 返回被点窗口索引 (从顶到底), -1=未命中窗口 */
static int win_at(int x, int y)
{
    for (int z = 0; z < MAX_WIN; z++) {
        int i = zorder[z];
        struct win *w = &wins[i];
        if (!w->open || w->min) continue;
        if (in_rect(x, y, w->x, w->y, w->w, w->h)) return i;
    }
    return -1;
}

static void press_window(struct win *w, int x, int y)
{
    if (w->kind == WK_DEMO) {
        int r[4][4];
        demo_rects(w, r);
        if (in_rect(x, y, r[0][0], r[0][1], r[0][2], r[0][3])) {   /* +按钮 */
            w->counter++;
            return;
        }
        if (in_rect(x, y, r[1][0], r[1][1], r[1][2], r[1][3])) {   /* 复选 */
            w->chk = !w->chk;
            return;
        }
        if (in_rect(x, y, r[3][0], r[3][1], r[3][2], r[3][3])) {   /* 关闭本窗口 */
            win_close((int)(w - wins));
            return;
        }
    }
}

static void handle_press(int x, int y)
{
    int ty = SCR_H - TASKBAR_H;
    /* 任务栏 */
    if (y >= ty) {
        /* 开始 */
        if (in_rect(x, y, 4, ty + 5, 80, 20)) { start_open = !start_open; return; }
        /* 退出 */
        if (in_rect(x, y, SCR_W - 88, ty + 5, 84, 20)) { gui_on = false; return; }
        /* 窗口按钮 */
        int bx = 92;
        for (int i = 0; i < MAX_WIN; i++) {
            if (!wins[i].open) continue;
            if (in_rect(x, y, bx, ty + 5, 96, 20)) {
                if (top == i && !wins[i].min) wins[i].min = true;
                else win_open(wins[i].kind);
                return;
            }
            bx += 100;
        }
        return;
    }
    /* 开始菜单 */
    if (start_open) {
        int mw = 160, mh = 108, mx0 = 4, my0 = ty - mh;
        if (in_rect(x, y, mx0, my0, mw, mh)) {
            for (int i = 0; i < 4; i++) {
                int iy = my0 + 8 + i * 24;
                if (in_rect(x, y, mx0 + 4, iy, mw - 8, 22)) {
                    if (i == 3) gui_on = false;
                    else win_open(i);
                    start_open = false;
                    return;
                }
            }
            return;
        }
        start_open = false;
    }
    /* 窗口 (从顶到底) */
    int wi = win_at(x, y);
    if (wi >= 0) {
        struct win *w = &wins[wi];
        win_raise(wi);
        /* 关闭按钮 */
        int cx = w->x + w->w - 20, cy = w->y + 3;
        if (in_rect(x, y, cx, cy, 16, 16)) { win_close(wi); return; }
        /* 标题栏 -> 拖拽 */
        if (in_rect(x, y, w->x, w->y, w->w, TITLE_H + 2)) {
            drag_idx = wi; drag_dx = x - w->x; drag_dy = y - w->y;
            return;
        }
        press_window(w, x, y);
        return;
    }
    /* 桌面图标 */
    for (int i = 0; i < NICON; i++)
        if (in_rect(x, y, ICON_X(), icons[i].y, 32, 56)) {
            win_open(icons[i].kind);
            return;
        }
    top = -1;   /* 点桌面空白 */
}

static void handle_wheel(int dir)
{
    if (top >= 0) {
        struct win *w = &wins[top];
        if (w->kind == WK_DEMO) {
            w->counter += dir;
            if (w->counter < 0) w->counter = 0;
        } else {
            w->scroll -= dir * 22;
            if (w->scroll < 0) w->scroll = 0;
        }
    }
}

/* ---------------------------------------------------------- 主循环 */
void gui_run(void)
{
    if (!gfx_init()) {
        kprintf("GUI unavailable (no Bochs VBE).\n");
        return;
    }
    gui_on = true;
    SCR_W = gfx_w();
    SCR_H = gfx_h();
    win_init();
    win_open(WK_DEMO);            /* 默认打开控件演示 */
    mouse_warp(SCR_W / 2, SCR_H / 2);
    start_open = false;
    drag_idx = -1;

    int frame = 0;
    while (gui_on) {
        softirq_dispatch();

        /* 鼠标指针: 绝对坐标唯一数据源 */
        int mx = mouse_x(), my = mouse_y();
        int btns = mouse_buttons();

        /* 事件环: 只处理点击与滚轮 */
        int a, b, btn, ev;
        while ((ev = mouse_pop_event(&a, &b, &btn)) >= 0) {
            if (ev == 1) {                     /* 按键状态变化 */
                if (btn & 1) handle_press(a, b);
            } else if (ev == 2) {              /* 滚轮 */
                handle_wheel(a);
            }
        }

        /* 拖拽: 只要左键按住就跟随 */
        if (drag_idx >= 0) {
            if (btns & 1) {
                struct win *w = &wins[drag_idx];
                w->x = mx - drag_dx;
                w->y = my - drag_dy;
                if (w->x < 0) w->x = 0;
                if (w->y < MENUBAR_H) w->y = MENUBAR_H;
                if (w->x > SCR_W - 40) w->x = SCR_W - 40;
                if (w->y > SCR_H - TASKBAR_H - 20) w->y = SCR_H - TASKBAR_H - 20;
            } else {
                drag_idx = -1;
            }
        }

        /* 键盘: ESC 退出 */
        int k = keyboard_read();
        if (k == 27 || k == 'q' || k == 'Q' || k == KEY_REBOOT)
            gui_on = false;

        /* ---- 渲染到后台缓冲 ---- */
        draw_desktop();
        draw_menubar();
        for (int z = MAX_WIN - 1; z >= 0; z--) {   /* 从底到顶 */
            int i = zorder[z];
            if (wins[i].open && !wins[i].min)
                draw_window(&wins[i], mx, my);
        }
        draw_taskbar(mx, my, start_open);
        if (start_open) draw_startmenu(mx, my);
        draw_cursor(mx, my);
        gfx_flip();                               /* 一次提交, 无闪烁 */

        frame++;
        if ((frame & 1) == 0)
            timer_sleep_ms(16);
        else
            cpu_relax();
    }

    /* 退出: 文本控制台本身就在图形后端上, 直接重绘恢复 */
    gui_on = false;
    vga_refresh();
    kprintf("Left GUI, back to text console.\n");
}
