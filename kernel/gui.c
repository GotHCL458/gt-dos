/* 基础 GUI 桌面: 壁纸 + 图标 + 终端窗口 + 任务栏 + 鼠标指针.
 *
 * 终端窗口复用文本控制台: vga_gui_viewport() 把文本网格限制在客户区内,
 * 所以 Shell / TC 程序 / 编辑器仍然照常工作, 只是输出被裁剪到窗口里.
 *
 * 鼠标指针直接写显存 (gfx_fb_pixel), 移动时先从后台缓冲恢复旧矩形,
 * 不污染窗口内容; 控制台重绘后由 gfx_poked 触发重画.
 * 输入空闲时 (ui_read_key) 调用 gui_pump() 处理鼠标与时钟.
 *
 * 退出: 点击标题栏关闭按钮 / 任务栏右侧 TEXT 按钮, 或 Shell 的 TEXTMODE. */
#include "gt.h"

#define CUR_W 12
#define CUR_H 19

/* 配色 */
static const u32 wall_top[3] = { 0x101a30, 0x14251a, 0x2a1424 };
static const u32 wall_bot[3] = { 0x1d3a55, 0x2f5a38, 0x5a2f48 };
#define C_FRAME     0x2a3446
#define C_FRAME_LT  0x9aa8c0
#define C_FRAME_DK  0x0c1220
#define C_TITLE     0x1f4e8a
#define C_TITLE2    0x2a6ab0
#define C_CLIENT    0x000000
#define C_TASK      0x141c2a
#define C_TASK_LT   0x3c5a80
#define C_TEXT      0xe8f0ff
#define C_ACCENT    0x66ccff

static bool active;
static int WX, WY, WW, WH;              /* 窗口外框 */
static int TITLE_H, TASK_H;
static int CL_X, CL_Y, CL_W, CL_H;      /* 客户区 (控制台视口) */
static int BT_X, BT_Y, BT_W, BT_H;      /* 标题栏关闭按钮 */
static int EX_X, EX_Y, EX_W, EX_H;      /* 任务栏 TEXT 按钮 */
static int CK_X, CK_Y;                  /* 时钟 */
static int cur_x = -1, cur_y = -1;      /* 指针当前位置 */
static int cs_w = CUR_W, cs_h = CUR_H;  /* 按字模缩放后的指针尺寸 */
static int last_btn;
static u32 pump_guard;
static u32 clock_sec = 0xFFFFFFFFu;
static bool dragging;
static int drag_dx, drag_dy;
static bool menu_open;                  /* 开始菜单 */
static bool about_open;                 /* 关于对话框 */
static bool maximized;                  /* 窗口最大化 */
static int theme;                       /* 壁纸配色 */
static int menu_hot = -1;               /* 鼠标悬停的菜单项 */
static int norm_x, norm_y, norm_w, norm_h;
static int MB_X, MB_Y, MB_W, MB_H;      /* 开始按钮 */
static int MN_X, MN_Y, MN_W, MN_H, MI_H;/* 开始菜单及项高 */
static int MX_X, MX_Y, MX_W, MX_H;      /* 最大化按钮 */
static int AB_X, AB_Y, AB_W, AB_H;      /* 关于对话框 */
static int AB_OK_X, AB_OK_Y, AB_OK_W, AB_OK_H;
static int ICON_X, ICON_Y, ICON_W, ICON_H, ICON_STEP;  /* 桌面图标 */
static int icon_sel = -1;               /* 最近点击的图标 */

static bool inside(int x, int y, int bx, int by, int bw, int bh);
static void redraw_desktop(void);
static void draw_overlays(void);

/* 经典箭头指针 (12x19, 每行低 12 位, 最高位为最左像素).
   逐像素拆成"白色主体"和"黑色描边"两张位图, 还原标准 Windows 箭头:
   尖端在左上, 左缘竖直, 右缘斜向展开, 下方是向右下延伸的斜尾.
   (之前用 bit + 邻域扩张生成的描边会出现粗竖条, 看起来像"旗子".) */
static const u16 cur_white[CUR_H] = {
    0x000, 0x000, 0x400, 0x600, 0x700, 0x780, 0x7C0, 0x7E0,
    0x7F0, 0x7F8, 0x7FC, 0x7FE, 0x780, 0x6C0, 0x4C0, 0x060,
    0x030, 0x030, 0x000
};
static const u16 cur_black[CUR_H] = {
    0x800, 0xC00, 0xA00, 0x900, 0x880, 0x840, 0x820, 0x810,
    0x808, 0x804, 0x802, 0x801, 0x83F, 0x920, 0xA90, 0xC90,
    0x848, 0x048, 0x030
};

static void fb_block(int x, int y, int s, u32 c)
{
    for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++)
            gfx_fb_pixel(x + i, y + j, c);
}

/* 白体 + 黑边 (经典指针配色), 深色/浅色背景上都清晰.
   按字模缩放倍数放大 (最大 2 倍), 高分屏上不会显得过小. */
static void cursor_draw(int x, int y)
{
    int s = gfx_scale();
    if (s < 1) s = 1;
    if (s > 2) s = 2;
    for (int r = 0; r < CUR_H; r++) {
        u16 w = cur_white[r], b = cur_black[r];
        if (!(w | b))
            continue;
        for (int c = 0; c < CUR_W; c++) {
            u16 m = (u16)(1u << (CUR_W - 1 - c));
            if (b & m)
                fb_block(x + c * s, y + r * s, s, 0x000000);
            else if (w & m)
                fb_block(x + c * s, y + r * s, s, 0xFFFFFF);
        }
    }
}

static void cursor_show(int x, int y)
{
    if (!gfx_active())
        return;
    int s = gfx_scale();
    if (s < 1) s = 1;
    if (s > 2) s = 2;
    cs_w = CUR_W * s;
    cs_h = CUR_H * s;
    if (cur_x >= 0)
        gfx_fb_restore(cur_x - 1, cur_y - 1, cs_w + 2, cs_h + 2);
    int maxx = gfx_w() - cs_w - 2;
    int maxy = gfx_h() - cs_h - 2;
    if (maxx < 1) maxx = 1;
    if (maxy < 1) maxy = 1;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    cur_x = x;
    cur_y = y;
    cursor_draw(x, y);
}

/* 控制台刚提交过矩形: 立即重画指针, 避免指针被覆盖后出现一帧闪烁.
   同时把菜单/对话框重新叠加回去 —— 控制台提交会重绘视口区域, 遮住浮层. */
void gui_cursor_refresh(void)
{
    if (!active)
        return;
    if (menu_open || about_open) {
        draw_overlays();                                  /* 重画进后台缓冲 */
        if (menu_open)
            gfx_fb_restore(MN_X - 2, MN_Y - 2, MN_W + 4, MN_H + 4);
        if (about_open)
            gfx_fb_restore(AB_X - 2, AB_Y - 2, AB_W + 4, AB_H + 4);
    }
    if (cur_x < 0)
        return;
    gfx_fb_restore(cur_x - 1, cur_y - 1, cs_w + 2, cs_h + 2);
    cursor_draw(cur_x, cur_y);
}

/* --------------------------------------------------------------- 绘制 */
static u32 lerp437(u32 a, u32 b, int t)      /* t: 0..255 */
{
    u32 ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    u32 br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    u32 r = ar + ((br - ar) * (u32)t) / 255u;
    u32 g = ag + ((bg - ag) * (u32)t) / 255u;
    u32 bl = ab + ((bb - ab) * (u32)t) / 255u;
    return (r << 16) | (g << 8) | bl;
}

static void draw_wallpaper(void)
{
    int H = gfx_h();
    int t = theme % 3;
    for (int y = 0; y < H; y += 4) {
        u32 c = lerp437(wall_top[t], wall_bot[t], (y * 255) / (H > 1 ? H - 1 : 1));
        gfx_fill(0, y, gfx_w(), 4, c);
    }
    int ch = gfx_cell_h();
    int tw = gfx_text_w("GT-DOS Desktop");
    gfx_text((gfx_w() - tw) / 2, 2 * ch, "GT-DOS Desktop", 0xdfefff);
    gfx_text((gfx_w() - gfx_text_w("drag title bar | click to capture mouse")) / 2,
             3 * ch, "drag title bar | click to capture mouse", 0x8fb6d8);
    /* 左侧桌面图标 (可点击) */
    const char *names[3] = { "TERM", "FILES", "SYS" };
    u32 cols[3] = { 0x2f6f9f, 0x3f8f5f, 0x8f6f2f };
    for (int i = 0; i < 3; i++) {
        int y = ICON_Y + i * ICON_STEP;
        gfx_fill(ICON_X, y, ICON_W, ICON_H, cols[i]);
        gfx_bevel(ICON_X, y, ICON_W, ICON_H, 0xffffff, 0x202020);
        if (icon_sel == i)
            gfx_bevel(ICON_X - 3, y - 3, ICON_W + 6, ICON_H + 6,
                      C_ACCENT, C_ACCENT);
        int lw = gfx_text_w(names[i]);
        gfx_text(ICON_X + (ICON_W - lw) / 2, y + ICON_H + 2, names[i],
                 0xd8e8ff);
    }
}

static void draw_clock(void)
{
    u32 hz = timer_hz();
    if (!hz) hz = 100;
    u32 sec = timer_ticks() / hz;
    if (sec == clock_sec)
        return;
    clock_sec = sec;
    char b[9];
    int hh = (int)((sec / 3600u) % 100u);
    int mm = (int)((sec / 60u) % 60u);
    int ss = (int)(sec % 60u);
    b[0] = (char)('0' + hh / 10); b[1] = (char)('0' + hh % 10); b[2] = ':';
    b[3] = (char)('0' + mm / 10); b[4] = (char)('0' + mm % 10); b[5] = ':';
    b[6] = (char)('0' + ss / 10); b[7] = (char)('0' + ss % 10); b[8] = 0;
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    gfx_fill(CK_X, CK_Y, 8 * cw, ch, C_TASK);
    gfx_text(CK_X, CK_Y, b, 0xcfe4ff);
    gfx_flip_rect(CK_X, CK_Y, 8 * cw, ch);
}

static void draw_button(int x, int y, int w, int h, const char *label,
                        u32 bg)
{
    gfx_fill(x, y, w, h, bg);
    gfx_bevel(x, y, w, h, 0xbfd0e8, 0x101820);
    if (label) {
        int lw = gfx_text_w(label);
        gfx_text(x + (w - lw) / 2, y + (h - gfx_cell_h()) / 2, label, C_TEXT);
    }
}

/* ------------------------------------------------------- 浮层: 菜单/对话框 */
/* 开始菜单: 4 个可点项 (Terminal / Theme / About / Exit GUI) */
static void draw_menu(void)
{
    int ch = gfx_cell_h();
    static const char *items[4] = { "Terminal", "Theme", "About", "Exit GUI" };
    gfx_fill(MN_X, MN_Y, MN_W, MN_H, 0x1b2434);
    gfx_bevel(MN_X, MN_Y, MN_W, MN_H, 0xbfd0e8, 0x0c1220);
    for (int i = 0; i < 4; i++) {
        int y = MN_Y + 4 + i * MI_H;
        bool hot = i == menu_hot;
        gfx_fill(MN_X + 2, y + 1, MN_W - 4, MI_H - 2,
                 hot ? 0x2a4a70 : 0x223a58);
        gfx_text(MN_X + 10, y + (MI_H - ch) / 2, items[i],
                 i == 3 ? 0xff9a9a : 0xdce9ff);
    }
}

/* 命中开始菜单项; 返回 0..3, 未命中 -1 */
static int menu_pick(int x, int y)
{
    if (inside(x, y, MN_X, MN_Y, MN_W, MN_H)) {
        int idx = (y - (MN_Y + 4)) / MI_H;
        if (idx < 0) idx = 0;
        if (idx > 3) idx = 3;
        return idx;
    }
    return -1;
}

/* 关于对话框 (模态): 版本/运行环境/操作说明 + OK 按钮 */
static void draw_about(void)
{
    int ch = gfx_cell_h();
    gfx_fill(AB_X, AB_Y, AB_W, AB_H, 0x1b2434);
    gfx_bevel(AB_X, AB_Y, AB_W, AB_H, 0xbfd0e8, 0x0c1220);
    gfx_fill(AB_X + 2, AB_Y + 2, AB_W - 4, TITLE_H - 2, C_TITLE);
    gfx_text(AB_X + 10, AB_Y + (TITLE_H - ch) / 2, "About GT-DOS", C_TEXT);

    int x = AB_X + 12;
    int y = AB_Y + TITLE_H + 8;
    const char *lines[6] = {
        GT_VERSION "  (x86-64 kernel)",
        "GUI desktop demo: wallpaper, icons, window",
        "Mouse: PS/2, captured on hover (Ctrl+Alt+G)",
        "Drag title bar to move, M to maximize",
        "Start menu -> Theme / About / Exit GUI",
        "Click TEXT or the title-bar x to exit GUI"
    };
    for (int i = 0; i < 6 && y + ch <= AB_OK_Y; i++, y += ch + 2)
        gfx_text(x, y, lines[i], 0xcfe0f5);

    draw_button(AB_OK_X, AB_OK_Y, AB_OK_W, AB_OK_H, "OK", 0x2f6f9f);
}

/* 浮层绘制 (菜单/对话框): 画在后台缓冲, 随后 gfx_flip 一并提交.
   这样指针移动时 gfx_fb_restore 能从后台缓冲正确恢复, 不会擦掉菜单. */
static void draw_overlays(void)
{
    if (menu_open)
        draw_menu();
    if (about_open)
        draw_about();
}

/* 最大化/还原 (保存并恢复窗口位置与尺寸) */
static void toggle_max(void)
{
    if (!maximized) {
        norm_x = WX; norm_y = WY; norm_w = WW; norm_h = WH;
        WX = 0;
        WY = 0;
        WW = gfx_w();
        WH = gfx_h() - TASK_H;
        maximized = true;
    } else {
        WX = norm_x; WY = norm_y;
        WW = norm_w; WH = norm_h;
        maximized = false;
    }
    redraw_desktop();
}

/* 按当前 WX/WY 计算所有派生矩形; WX/WY<0 时居中, WW/WH<=0 时按 3/4 屏 */
static void layout(void)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    int gw = gfx_w(), gh = gfx_h();

    TASK_H = ch + 10;
    TITLE_H = ch + 8;
    if (WW <= 0) WW = gw * 3 / 4;
    if (WH <= 0) WH = gh * 3 / 4;
    if (WX < 0) WX = (gw - WW) / 2;
    if (WY < 0) WY = (gh - TASK_H - WH) / 2;
    CL_X = WX + 6;
    CL_Y = WY + TITLE_H;
    CL_W = WW - 12;
    CL_H = WH - TITLE_H - 6;

    BT_W = TITLE_H - 10;
    BT_H = BT_W;
    BT_X = WX + WW - BT_W - 6;
    BT_Y = WY + 5;

    /* 任务栏 TEXT 按钮贴住右下角 (更容易点中, 也便于自动化测试) */
    EX_W = 5 * cw + 12;
    EX_H = TASK_H;
    EX_X = gw - EX_W;
    EX_Y = gh - TASK_H;
    CK_X = EX_X - 8 * cw - 18;
    CK_Y = gh - TASK_H + (TASK_H - ch) / 2;

    /* 开始按钮 / 菜单 / 最大化按钮 / 关于对话框 */
    MB_X = 4;
    MB_Y = gh - TASK_H;
    MB_W = 10 * cw;
    MB_H = TASK_H;
    MI_H = ch + 8;
    MN_W = 16 * cw;
    MN_H = 4 * MI_H + 8;
    MN_X = 4;
    MN_Y = gh - TASK_H - MN_H;
    MX_W = BT_W;
    MX_H = BT_H;
    MX_X = BT_X - MX_W - 6;
    MX_Y = BT_Y;

    AB_W = 46 * cw;
    if (AB_W > gw - 40) AB_W = gw - 40;
    AB_H = 12 * ch;
    if (AB_H > gh - 40) AB_H = gh - 40;
    AB_X = (gw - AB_W) / 2;
    AB_Y = (gh - AB_H) / 2;
    AB_OK_W = 6 * cw;
    AB_OK_H = ch + 8;
    AB_OK_X = AB_X + AB_W - AB_OK_W - 12;
    AB_OK_Y = AB_Y + AB_H - AB_OK_H - 12;

    /* 左侧桌面图标: 4x3 格, 纵向间隔 5 行 */
    ICON_X = 2 * cw;
    ICON_Y = 5 * ch;
    ICON_W = 4 * cw;
    ICON_H = 3 * ch;
    ICON_STEP = 5 * ch;
}

static void draw_desktop(void)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    int gw = gfx_w(), gh = gfx_h();

    layout();
    draw_wallpaper();

    /* 窗口阴影/边框/标题栏 */
    gfx_fill(WX + 10, WY + 10, WW, WH, 0x080c14);
    gfx_fill(WX, WY, WW, WH, C_FRAME);
    gfx_bevel(WX, WY, WW, WH, C_FRAME_LT, C_FRAME_DK);
    gfx_fill(WX + 2, WY + 2, WW - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(WX + 2, WY + 2, WW - 4, (TITLE_H - 2) / 2, C_TITLE2);
    gfx_text(WX + 10, WY + (TITLE_H - ch) / 2, "GT-DOS Terminal", C_TEXT);
    draw_button(MX_X, MX_Y, MX_W, MX_H, maximized ? "R" : "M", 0x2f5a8a);
    draw_button(BT_X, BT_Y, BT_W, BT_H, "x", 0x8a2f2f);

    /* 客户区 (控制台视口) */
    gfx_fill(CL_X - 2, CL_Y - 2, CL_W + 4, CL_H + 4, C_CLIENT);

    /* 任务栏 */
    gfx_fill(0, gh - TASK_H, gw, TASK_H, C_TASK);
    gfx_fill(0, gh - TASK_H, gw, 2, C_TASK_LT);
    draw_button(MB_X, MB_Y, MB_W, MB_H, "GT-DOS", 0x203850);
    gfx_text(MB_X + MB_W + 3 * cw,
             gh - TASK_H + (TASK_H - ch) / 2,
             "start | terminal", 0x8fa8c8);
    draw_button(EX_X, EX_Y, EX_W, EX_H, "TEXT", 0x304050);
    clock_sec = 0xFFFFFFFFu;
    draw_clock();
}

/* 重绘桌面 (拖动/最大化/菜单切换): 恢复指针 -> 重画背景与窗口 -> 重设视口
   最后叠加菜单/对话框 (必须在视口重绘之后, 否则会被控制台内容覆盖) */
static void redraw_desktop(void)
{
    if (cur_x >= 0) {
        gfx_fb_restore(cur_x - 1, cur_y - 1, cs_w + 2, cs_h + 2);
        cur_x = cur_y = -1;
    }
    draw_desktop();
    vga_gui_viewport(CL_X, CL_Y, CL_W, CL_H);
    draw_overlays();
    gfx_flip();
    gfx_poked = false;
    cursor_show(mouse_x(), mouse_y());
}

/* --------------------------------------------------------------- 接口 */
bool gui_active(void) { return active; }

bool gui_enter(void)
{
    if (!gfx_active()) {
        kprintf("[gui] no graphics mode, desktop unavailable\n");
        return false;
    }
    if (active)
        return true;
    cur_x = cur_y = -1;
    WX = WY = -1;
    WW = WH = 0;
    dragging = false;
    menu_open = false;
    about_open = false;
    maximized = false;
    theme = 0;
    icon_sel = -1;
    menu_hot = -1;
    gfx_poked = false;
    draw_desktop();
    vga_gui_viewport(CL_X, CL_Y, CL_W, CL_H);
    gfx_flip();
    active = true;
    last_btn = mouse_buttons();
    mouse_warp(CL_X + CL_W / 2, CL_Y + CL_H / 2);
    cursor_show(mouse_x(), mouse_y());
    kprintf("[gui] desktop on (%dx%d, click TEXT to exit)\n",
            gfx_w(), gfx_h());
    return true;
}

void gui_leave(void)
{
    if (!active)
        return;
    active = false;
    dragging = false;
    menu_open = false;
    about_open = false;
    maximized = false;
    menu_hot = -1;
    if (cur_x >= 0)
        gfx_fb_restore(cur_x - 1, cur_y - 1, cs_w + 2, cs_h + 2);
    cur_x = cur_y = -1;
    vga_gui_leave();
    kprintf("[gui] text mode\n");
}

static bool inside(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* 命中桌面图标 (含下方文字标签); 返回 0..2, 未命中 -1 */
static int icon_hit(int x, int y)
{
    int ch = gfx_cell_h();
    for (int i = 0; i < 3; i++) {
        int iy = ICON_Y + i * ICON_STEP;
        if (inside(x, y, ICON_X, iy, ICON_W, ICON_H + ch))
            return i;
    }
    return -1;
}

void gui_pump(void)
{
    if (!active)
        return;

    int btn = mouse_buttons();
    int x = mouse_x(), y = mouse_y();
    bool left = (btn & 1) != 0;
    bool pressed = left && !(last_btn & 1);
    last_btn = btn;

    if (pressed) {
        if (about_open) {
            /* 模态: 只响应 OK 或点击对话框外部关闭 */
            if (inside(x, y, AB_OK_X, AB_OK_Y, AB_OK_W, AB_OK_H) ||
                !inside(x, y, AB_X, AB_Y, AB_W, AB_H)) {
                about_open = false;
                redraw_desktop();
            }
        } else if (menu_open) {
            int idx = menu_pick(x, y);
            if (idx >= 0) {
                menu_open = false;
                menu_hot = -1;
                if (idx == 1) {
                    theme = (theme + 1) % 3;      /* Theme: 换壁纸配色 */
                    redraw_desktop();
                } else if (idx == 2) {
                    about_open = true;            /* About: 打开对话框 */
                    redraw_desktop();
                } else if (idx == 3) {
                    gui_leave();                  /* Exit GUI */
                    return;
                } else {
                    redraw_desktop();             /* Terminal: 收起菜单 */
                }
            } else if (inside(x, y, MB_X, MB_Y, MB_W, MB_H)) {
                menu_open = false;                /* 再点开始按钮收起 */
                menu_hot = -1;
                redraw_desktop();
            } else {
                menu_open = false;                /* 点其它地方也收起 */
                menu_hot = -1;
                redraw_desktop();
            }
        } else if (inside(x, y, MB_X, MB_Y, MB_W, MB_H)) {
            menu_open = true;                     /* 打开开始菜单 */
            menu_hot = -1;
            redraw_desktop();
        } else if (inside(x, y, BT_X, BT_Y, BT_W, BT_H) ||
                   inside(x, y, EX_X, EX_Y, EX_W, EX_H)) {
            gui_leave();                          /* 关闭按钮 / TEXT */
            return;
        } else if (inside(x, y, MX_X, MX_Y, MX_W, MX_H)) {
            toggle_max();                         /* 最大化 / 还原 */
        } else if (icon_hit(x, y) >= 0) {
            icon_sel = icon_hit(x, y);            /* 桌面图标: 高亮反馈 */
            redraw_desktop();
        } else if (inside(x, y, WX, WY, WW, TITLE_H)) {
            dragging = true;
            drag_dx = x - WX;
            drag_dy = y - WY;
        }
    }
    if (!left)
        dragging = false;

    /* 菜单悬停高亮: 鼠标移到哪一项就高亮哪一项 */
    if (menu_open && !pressed) {
        int h = menu_pick(x, y);
        if (h != menu_hot) {
            menu_hot = h;
            draw_menu();
            gfx_flip_rect(MN_X - 2, MN_Y - 2, MN_W + 4, MN_H + 4);
        }
    }

    if (dragging) {
        int nx = x - drag_dx;
        int ny = y - drag_dy;
        int maxx = gfx_w() - WW;
        int maxy = gfx_h() - TASK_H - WH;
        if (maxx < 0) maxx = 0;
        if (maxy < 0) maxy = 0;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > maxx) nx = maxx;
        if (ny > maxy) ny = maxy;
        if (nx != WX || ny != WY) {
            WX = nx;
            WY = ny;
            redraw_desktop();
            return;
        }
    }

    if (x != cur_x || y != cur_y || gfx_poked) {
        gfx_poked = false;
        cursor_show(x, y);
    }

    if (++pump_guard >= 512) {
        pump_guard = 0;
        draw_clock();
    }
}
