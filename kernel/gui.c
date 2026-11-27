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
static bool settings_open;              /* 设置对话框 */
static bool widgets_open;               /* 控件演示对话框 */
static bool maximized;                  /* 窗口最大化 */
static bool show_icons = true;          /* 显示桌面图标 */
static bool show_clock = true;          /* 显示任务栏时钟 */
static bool clock24 = true;             /* 24 小时制时钟 */
static int theme;                       /* 壁纸配色 */
static int bright = 100;                /* 壁纸亮度 40..100 */
static int menu_hot = -1;               /* 鼠标悬停的菜单项 */
static bool slider_drag;                /* 正在拖动亮度滑块 */
static bool resizing;                   /* 正在拖动右下角握把 */
static int resize_base_w, resize_base_h;
static int resize_px, resize_py;
static int norm_x, norm_y, norm_w, norm_h;
static int MB_X, MB_Y, MB_W, MB_H;      /* 开始按钮 */
static int TK_X, TK_Y, TK_W, TK_H;      /* 任务栏窗口按钮 */
static int MN_X, MN_Y, MN_W, MN_H, MI_H;/* 开始菜单及项高 */
static int MX_X, MX_Y, MX_W, MX_H;      /* 最大化按钮 */
static int AB_X, AB_Y, AB_W, AB_H;      /* 关于对话框 */
static int AB_OK_X, AB_OK_Y, AB_OK_W, AB_OK_H;
static int SD_X, SD_Y, SD_W, SD_H;      /* 设置对话框 */
static int SD_PREV_X, SD_PREV_Y, SD_BTN_W, SD_BTN_H;
static int SD_NEXT_X;
static int SD_C1_X, SD_C1_Y, SD_CHK_W, SD_CHK_H;
static int SD_C2_X, SD_C2_Y;
static int SD_SL_X, SD_SL_Y, SD_SL_W, SD_SL_H;
static int SD_CLOSE_X, SD_CLOSE_Y, SD_CLOSE_W, SD_CLOSE_H;
static int GX, GY, GWH;                 /* 窗口右下角缩放握把 */
static int WD_X, WD_Y, WD_W, WD_H;      /* 控件演示对话框 */
static int WT_X, WT_Y, WT_W, WT_H, WT_ROW_Y;  /* 时钟开关 (Toggle) */
static int WR1_X, WR_Y, WR_SZ, WR_STEP; /* 单选组 */
static int WP_LAB_Y, WP_X, WP_Y, WP_W, WP_H;  /* 进度条 */
static int WL_X, WL_Y, WL_W, WL_IH;     /* 列表 */
static int WC_X, WC_Y, WC_W, WC_H;      /* 关闭按钮 */
static int list_sel;                    /* 列表选中项 */
static int wdg_progress;                /* 演示进度 0..100 */
static u32 wdg_last;                    /* 上次进度刷新 tick */
static bool app_canvas;                 /* 用户程序占用全屏画布 */
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
    if (app_canvas)
        return;                       /* 用户画布/看图: 不叠加桌面指针 */
    if (menu_open || about_open || settings_open || widgets_open) {
        draw_overlays();                                  /* 重画进后台缓冲 */
        if (menu_open)
            gfx_fb_restore(MN_X - 2, MN_Y - 2, MN_W + 4, MN_H + 4);
        if (about_open)
            gfx_fb_restore(AB_X - 2, AB_Y - 2, AB_W + 4, AB_H + 4);
        if (settings_open)
            gfx_fb_restore(SD_X - 2, SD_Y - 2, SD_W + 4, SD_H + 4);
        if (widgets_open)
            gfx_fb_restore(WD_X - 2, WD_Y - 2, WD_W + 4, WD_H + 4);
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

static u32 color_scale(u32 c, int pct)
{
    u32 r = ((c >> 16) & 255u) * (u32)pct / 100u;
    u32 g = ((c >> 8) & 255u) * (u32)pct / 100u;
    u32 b = (c & 255u) * (u32)pct / 100u;
    return (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------- 桌面图标图形 */
/* TERM: 迷你终端窗口 (标题栏 + ">" 提示符 + 方块光标).
   提示符用像素绘制而不是字模: 字号在半高的小图标里会显得过大且 "_" 会掉到
   基线以下, 看起来像一根游离的横线. */
static void draw_icon_term(int x, int y)
{
    int ch = gfx_cell_h();
    int s = gfx_scale();
    if (s < 1) s = 1;
    int px = x + 4, py = y + 6;
    int pw = ICON_W - 8, ph = ICON_H - 12;
    int tb = ch / 2;                          /* 标题栏高度 */

    gfx_fill(px, py, pw, ph, 0x0a141e);
    gfx_bevel(px, py, pw, ph, 0xdfefff, 0x101820);
    gfx_fill(px + 1, py + 1, pw - 2, tb, 0x1e4a72);

    /* 标题栏三个按钮点 */
    for (int i = 0; i < 3; i++)
        gfx_fill(px + 3 + i * (s + 2), py + 1 + (tb - s) / 2, s, s, 0xdce9ff);

    /* ">" 提示符: 上下两段斜线在中间汇合 */
    int wv = (pw - 8) / 3;
    if (wv > 6 * s) wv = 6 * s;
    if (wv < 3) wv = 3;
    int cvh = wv * 2;
    int cmd_y = py + tb + 2;
    if (cmd_y + cvh > py + ph - 2)
        cvh = py + ph - 2 - cmd_y;
    if (cvh < 4) cvh = 4;
    wv = cvh / 2;
    int bx = px + 4;
    for (int i = 0; i < wv; i++) {
        gfx_fill(bx + i, cmd_y + i, s, s, 0x7fe0a0);
        gfx_fill(bx + i, cmd_y + cvh - 1 - i, s, s, 0x7fe0a0);
    }

    /* 方块光标 */
    int kx = bx + wv + 2 * s;
    int klen = pw - (kx - px) - 4;
    if (klen > wv) klen = wv;
    if (klen < 3) klen = 3;
    gfx_fill(kx, cmd_y + cvh / 2 - s / 2, klen, s, 0x7fe0a0);
}

/* FILES: 文件夹 (标签 + 主体 + 文件线) */
static void draw_icon_files(int x, int y)
{
    int ch = gfx_cell_h();
    int fx = x + 4, fy = y + 8;
    int fw = ICON_W - 8, fh = ICON_H - 16;
    int tab_h = ch / 3;
    int tab_w = fw / 2;
    gfx_fill(fx, fy, tab_w, tab_h, 0xd8b040);
    gfx_fill(fx, fy + tab_h, fw, fh - tab_h, 0xdfc060);
    gfx_bevel(fx, fy + tab_h, fw, fh - tab_h, 0xfff0b0, 0x806010);
    for (int i = 0; i < 3; i++)
        gfx_fill(fx + 4, fy + tab_h + 4 + i * (fh - tab_h) / 4,
                 fw - 8 - i * 3, 2, 0xb09030);
}

/* SYS: 芯片 (引脚 + 内核) */
static void draw_icon_sys(int x, int y)
{
    int sx = x + 8, sy = y + 10;
    int sw = ICON_W - 16, sh = ICON_H - 20;
    gfx_fill(sx, sy, sw, sh, 0x2f7f7f);
    gfx_bevel(sx, sy, sw, sh, 0xd0f0f0, 0x103838);
    gfx_fill(sx + sw / 4, sy + sh / 4, sw / 2, sh / 2, 0x102028);
    gfx_bevel(sx + sw / 4, sy + sh / 4, sw / 2, sh / 2, 0x60b0b0, 0x081818);
    for (int i = 1; i < 4; i++) {
        int off = sw * i / 4;
        gfx_fill(sx + off - 1, sy - 3, 2, 3, 0xc0e0e0);
        gfx_fill(sx + off - 1, sy + sh, 2, 3, 0xc0e0e0);
    }
    for (int i = 1; i < 3; i++) {
        int off = sh * i / 3;
        gfx_fill(sx - 3, sy + off - 1, 3, 2, 0xc0e0e0);
        gfx_fill(sx + sw, sy + off - 1, 3, 2, 0xc0e0e0);
    }
}

static void draw_wallpaper(void)
{
    int H = gfx_h();
    int t = theme % 3;
    for (int y = 0; y < H; y += 4) {
        u32 c = lerp437(wall_top[t], wall_bot[t], (y * 255) / (H > 1 ? H - 1 : 1));
        gfx_fill(0, y, gfx_w(), 4, color_scale(c, bright));
    }
    int ch = gfx_cell_h();
    int tw = gfx_text_w("GT-DOS Desktop");
    gfx_text((gfx_w() - tw) / 2, 2 * ch, "GT-DOS Desktop", 0xdfefff);
    gfx_text((gfx_w() - gfx_text_w("drag title bar | click to capture mouse")) / 2,
             3 * ch, "drag title bar | click to capture mouse", 0x8fb6d8);
    /* 左侧桌面图标 (可点击; 设置里可以隐藏) */
    if (!show_icons)
        return;
    const char *names[3] = { "TERM", "FILES", "SYS" };
    u32 cols[3] = { 0x2f6f9f, 0x3f8f5f, 0x8f6f2f };
    for (int i = 0; i < 3; i++) {
        int y = ICON_Y + i * ICON_STEP;
        gfx_fill(ICON_X + 3, y + 3, ICON_W, ICON_H, 0x080c14);
        gfx_fill(ICON_X, y, ICON_W, ICON_H, cols[i]);
        gfx_bevel(ICON_X, y, ICON_W, ICON_H, 0xffffff, 0x202020);
        if (i == 0)
            draw_icon_term(ICON_X, y);
        else if (i == 1)
            draw_icon_files(ICON_X, y);
        else
            draw_icon_sys(ICON_X, y);
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
    if (!show_clock)
        return;
    u32 hz = timer_hz();
    if (!hz) hz = 100;
    u32 sec = timer_ticks() / hz;
    if (sec == clock_sec)
        return;
    clock_sec = sec;
    int hh = (int)((sec / 3600u) % 100u);
    int mm = (int)((sec / 60u) % 60u);
    int ss = (int)(sec % 60u);
    int hv = clock24 ? hh : (hh % 12 == 0 ? 12 : hh % 12);
    char b[16];
    int n = 0;
    b[n++] = (char)('0' + hv / 10); b[n++] = (char)('0' + hv % 10); b[n++] = ':';
    b[n++] = (char)('0' + mm / 10); b[n++] = (char)('0' + mm % 10); b[n++] = ':';
    b[n++] = (char)('0' + ss / 10); b[n++] = (char)('0' + ss % 10);
    if (!clock24) {
        b[n++] = ' ';
        b[n++] = (char)(hh < 12 ? 'A' : 'P');
        b[n++] = 'M';
    }
    b[n] = '\0';
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    gfx_fill(CK_X, CK_Y, 12 * cw, ch, C_TASK);
    gfx_text(CK_X, CK_Y, b, 0xcfe4ff);
    gfx_flip_rect(CK_X, CK_Y, 12 * cw, ch);
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

/* 小型终端应用图标 (标题栏/任务栏共用) */
static void draw_app_glyph(int x, int y, int sz)
{
    int tb = sz / 3;
    if (tb < 2) tb = 2;
    gfx_fill(x, y, sz, sz, 0x0a141e);
    gfx_bevel(x, y, sz, sz, 0xcfe0f5, 0x101820);
    gfx_fill(x + 1, y + 1, sz - 2, tb, 0x2f6f9f);
    for (int i = 0; i < 3; i++)
        gfx_fill(x + 2 + i * (sz / 8 + 1), y + 2, 1, 1, 0xdce9ff);
    int bx = x + sz / 4, by = y + tb + sz / 6;
    int wv = sz / 5;
    if (wv < 2) wv = 2;
    for (int i = 0; i < wv; i++) {
        gfx_fill(bx + i, by + i, 1, 1, 0x7fe0a0);
        gfx_fill(bx + i, by + wv * 2 - 1 - i, 1, 1, 0x7fe0a0);
    }
    gfx_fill(bx + wv + 1, by + wv - 1, wv, 1, 0x7fe0a0);
}

/* 开关 (Toggle): 轨道 + 滑块 */
static void draw_toggle(int x, int y, int w, int h, bool on)
{
    gfx_fill(x, y, w, h, on ? 0x2f8f5f : 0x3a4658);
    gfx_bevel(x, y, w, h, 0xbfd0e8, 0x101820);
    int k = h - 4;
    if (k < 4) k = 4;
    int kx = on ? x + w - 2 - k : x + 2;
    gfx_fill(kx, y + 2, k, k, 0xe8f0ff);
    gfx_bevel(kx, y + 2, k, k, 0xffffff, 0x606870);
}

/* ------------------------------------------------------- 浮层: 菜单/对话框 */
/* 开始菜单: 6 个可点项 (Terminal / Theme / About / Settings / Widgets / Exit) */
static void draw_menu(void)
{
    int ch = gfx_cell_h();
    static const char *items[6] = {
        "Terminal", "Theme", "About", "Settings", "Widgets", "Exit GUI"
    };
    gfx_fill(MN_X, MN_Y, MN_W, MN_H, 0x1b2434);
    gfx_bevel(MN_X, MN_Y, MN_W, MN_H, 0xbfd0e8, 0x0c1220);
    u32 icol[6] = { 0x66ccff, 0x7fe0a0, 0x9fb3ff,
                    0xffc060, 0xc090ff, 0xff9090 };
    for (int i = 0; i < 6; i++) {
        int y = MN_Y + 4 + i * MI_H;
        if (i == 5)
            gfx_fill(MN_X + 6, y - 2, MN_W - 12, 1, 0x3c5a80);
        bool hot = i == menu_hot;
        gfx_fill(MN_X + 2, y + 1, MN_W - 4, MI_H - 2,
                 hot ? 0x2a4a70 : 0x223a58);
        gfx_fill(MN_X + 8, y + (MI_H - 4) / 2, 4, 4, icol[i]);
        gfx_text(MN_X + 18, y + (MI_H - ch) / 2, items[i],
                 i == 5 ? 0xff9a9a : 0xdce9ff);
    }
}

/* 命中开始菜单项; 返回 0..5, 未命中 -1 */
static int menu_pick(int x, int y)
{
    if (inside(x, y, MN_X, MN_Y, MN_W, MN_H)) {
        int idx = (y - (MN_Y + 4)) / MI_H;
        if (idx < 0) idx = 0;
        if (idx > 5) idx = 5;
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
        "Start menu -> Theme / About / Settings",
        "Click TEXT or the title-bar x to exit GUI"
    };
    for (int i = 0; i < 6 && y + ch <= AB_OK_Y; i++, y += ch + 2)
        gfx_text(x, y, lines[i], 0xcfe0f5);

    draw_button(AB_OK_X, AB_OK_Y, AB_OK_W, AB_OK_H, "OK", 0x2f6f9f);
}

/* 复选框: 方框 + 勾 + 右侧文字 */
static void draw_check(int x, int y, bool on, const char *label)
{
    int ch = gfx_cell_h();
    int box = ch - 4;
    gfx_fill(x, y + 2, box, box, 0x0e1622);
    gfx_bevel(x, y + 2, box, box, 0xbfd0e8, 0x101820);
    if (on) {
        for (int i = 0; i < box / 3; i++)
            gfx_fill(x + box / 5 + i, y + 2 + box / 2 + i, 2, 2, 0x9fe0ff);
        for (int i = 0; i < box / 2; i++)
            gfx_fill(x + box / 3 + i, y + 2 + box - box / 4 - i, 2, 2,
                     0x9fe0ff);
    }
    gfx_text(x + box + 6, y, label, on ? 0xdce9ff : 0x8fa8c8);
}

/* 滑块: 轨道 + 已填充段 + 圆角旋钮 */
static void draw_slider(void)
{
    int ch = gfx_cell_h();
    int ty = SD_SL_Y + (SD_SL_H - 6) / 2;
    gfx_fill(SD_SL_X, ty, SD_SL_W, 6, 0x0e1622);
    gfx_bevel(SD_SL_X, ty, SD_SL_W, 6, 0xbfd0e8, 0x101820);
    int kx = SD_SL_X + (bright - 40) * (SD_SL_W - 8) / 60;
    gfx_fill(SD_SL_X, ty + 1, kx - SD_SL_X, 4, 0x4f9fdf);
    gfx_fill(kx, SD_SL_Y + 1, 8, ch - 2, 0xdce9ff);
    gfx_bevel(kx, SD_SL_Y + 1, 8, ch - 2, 0xffffff, 0x101820);
}

/* 设置对话框 (模态): 主题切换按钮 + 两个复选框 + 亮度滑块 + 关闭 */
static void draw_settings(void)
{
    int ch = gfx_cell_h();
    static const char *names[3] = { "Blue", "Green", "Purple" };

    gfx_fill(SD_X, SD_Y, SD_W, SD_H, 0x1b2434);
    gfx_bevel(SD_X, SD_Y, SD_W, SD_H, 0xbfd0e8, 0x0c1220);
    gfx_fill(SD_X + 2, SD_Y + 2, SD_W - 4, TITLE_H - 2, C_TITLE);
    gfx_text(SD_X + 10, SD_Y + (TITLE_H - ch) / 2, "Settings", C_TEXT);

    /* 主题: < 名称 > */
    draw_button(SD_PREV_X, SD_PREV_Y, SD_BTN_W, SD_BTN_H, "<", 0x2f5a8a);
    draw_button(SD_NEXT_X, SD_PREV_Y, SD_BTN_W, SD_BTN_H, ">", 0x2f5a8a);
    int lx = SD_PREV_X + SD_BTN_W + 12;
    int ly = SD_PREV_Y + (SD_BTN_H - ch) / 2;
    gfx_text(lx, ly, "Theme:", 0x8fa8c8);
    gfx_text(lx + 8 * gfx_cell_w(), ly, names[theme % 3], 0xdce9ff);

    /* 复选框 */
    draw_check(SD_C1_X, SD_C1_Y, show_icons, "Desktop icons");
    draw_check(SD_C2_X, SD_C2_Y, clock24, "24-hour clock");

    /* 亮度滑块 */
    gfx_text(SD_X + 12, SD_SL_Y + (SD_SL_H - ch) / 2, "Brightness", 0x8fa8c8);
    draw_slider();

    draw_button(SD_CLOSE_X, SD_CLOSE_Y, SD_CLOSE_W, SD_CLOSE_H, "Close",
                0x2f6f9f);
}

/* 单选按钮: 方框 + 选中时的中心方块 */
static void draw_radio(int x, int y, int sz, bool on, const char *label)
{
    int ch = gfx_cell_h();
    gfx_fill(x, y, sz, sz, 0x0e1622);
    gfx_bevel(x, y, sz, sz, 0xbfd0e8, 0x101820);
    if (on)
        gfx_fill(x + sz / 4, y + sz / 4, sz / 2, sz / 2, C_ACCENT);
    gfx_text(x + sz + 6, y + (sz - ch) / 2, label, on ? 0xdce9ff : 0x8fa8c8);
}

/* 控件演示对话框: 单选组 + 进度条 + 列表 (点击选择) */
static void draw_widgets(void)
{
    int ch = gfx_cell_h();
    static const char *rlab[3] = { "Blue", "Green", "Purple" };
    static const char *items[4] = { "Alpha", "Beta", "Gamma", "Delta" };

    gfx_fill(WD_X, WD_Y, WD_W, WD_H, 0x1b2434);
    gfx_bevel(WD_X, WD_Y, WD_W, WD_H, 0xbfd0e8, 0x0c1220);
    gfx_fill(WD_X + 2, WD_Y + 2, WD_W - 4, TITLE_H - 2, C_TITLE);
    gfx_text(WD_X + 10, WD_Y + (TITLE_H - ch) / 2, "Widgets", C_TEXT);

    for (int i = 0; i < 3; i++)
        draw_radio(WR1_X + i * WR_STEP, WR_Y, WR_SZ, theme == i, rlab[i]);

    gfx_text(WD_X + 12, WP_LAB_Y, "Progress", 0x8fa8c8);
    gfx_bar(WP_X, WP_Y, WP_W, WP_H, wdg_progress, 0x4f9fdf);

    /* 开关: 任务栏时钟 */
    gfx_text(WD_X + 12, WT_ROW_Y + 2, "Taskbar clock",
             show_clock ? 0xdce9ff : 0x8fa8c8);
    draw_toggle(WT_X, WT_Y, WT_W, WT_H, show_clock);

    gfx_fill(WL_X, WL_Y, WL_W, WL_IH * 4, 0x0e1622);
    gfx_bevel(WL_X, WL_Y, WL_W, WL_IH * 4, 0xbfd0e8, 0x101820);
    for (int i = 0; i < 4; i++) {
        int iy = WL_Y + i * WL_IH;
        bool on = i == list_sel;
        if (on)
            gfx_fill(WL_X + 2, iy + 1, WL_W - 4, WL_IH - 2, 0x2a4a70);
        gfx_text(WL_X + 8, iy + (WL_IH - ch) / 2, items[i],
                 on ? 0xdce9ff : 0x9fb3cc);
    }

    draw_button(WC_X, WC_Y, WC_W, WC_H, "Close", 0x2f6f9f);
}

/* 浮层绘制 (菜单/对话框): 画在后台缓冲, 随后 gfx_flip 一并提交.
   这样指针移动时 gfx_fb_restore 能从后台缓冲正确恢复, 不会擦掉菜单. */
static void draw_overlays(void)
{
    if (menu_open)
        draw_menu();
    if (about_open)
        draw_about();
    if (settings_open)
        draw_settings();
    if (widgets_open)
        draw_widgets();
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
    CK_X = EX_X - 12 * cw - 18;
    CK_Y = gh - TASK_H + (TASK_H - ch) / 2;

    /* 开始按钮 / 菜单 / 最大化按钮 / 关于对话框 */
    MB_X = 4;
    MB_Y = gh - TASK_H;
    MB_W = 10 * cw;
    MB_H = TASK_H;
    /* 任务栏窗口按钮 (活动窗口) */
    TK_X = MB_X + MB_W + 6;
    TK_Y = MB_Y;
    TK_W = 12 * cw;
    TK_H = TASK_H;
    MI_H = ch + 8;
    MN_W = 16 * cw;
    MN_H = 6 * MI_H + 8;
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

    /* 设置对话框及其控件 */
    SD_W = 44 * cw;
    if (SD_W > gw - 40) SD_W = gw - 40;
    SD_H = 12 * ch;
    if (SD_H > gh - 40) SD_H = gh - 40;
    SD_X = (gw - SD_W) / 2;
    SD_Y = (gh - SD_H) / 2;
    SD_BTN_W = 4 * cw;
    SD_BTN_H = ch + 6;
    SD_PREV_X = SD_X + 12;
    SD_PREV_Y = SD_Y + TITLE_H + 10;
    SD_NEXT_X = SD_X + SD_W - 12 - SD_BTN_W;
    SD_CHK_W = 20 * cw;
    SD_CHK_H = ch + 4;
    SD_C1_X = SD_X + 12;
    SD_C1_Y = SD_PREV_Y + SD_BTN_H + 8;
    SD_C2_X = SD_X + 12;
    SD_C2_Y = SD_C1_Y + ch + 8;
    SD_SL_X = SD_X + 12 + 11 * cw;
    SD_SL_Y = SD_C2_Y + SD_CHK_H + 8;
    SD_SL_W = SD_X + SD_W - 12 - SD_SL_X;
    SD_SL_H = ch + 4;
    SD_CLOSE_W = 6 * cw;
    SD_CLOSE_H = ch + 8;
    SD_CLOSE_X = SD_X + SD_W - SD_CLOSE_W - 12;
    SD_CLOSE_Y = SD_Y + SD_H - SD_CLOSE_H - 12;

    /* 控件演示对话框 */
    WD_W = 40 * cw;
    if (WD_W > gw - 40) WD_W = gw - 40;
    WD_H = 17 * ch;
    if (WD_H > gh - 40) WD_H = gh - 40;
    WD_X = (gw - WD_W) / 2;
    WD_Y = (gh - WD_H) / 2;
    WR_SZ = ch;
    WR_Y = WD_Y + TITLE_H + 8;
    WR1_X = WD_X + 12;
    WR_STEP = 11 * cw;
    WP_LAB_Y = WR_Y + WR_SZ + 10;
    WP_X = WD_X + 12;
    WP_Y = WP_LAB_Y + ch + 2;
    WP_W = WD_W - 24;
    WP_H = ch;
    WT_ROW_Y = WP_Y + WP_H + 8;
    WT_W = 2 * ch;
    WT_H = ch - 4;
    WT_X = WD_X + WD_W - 12 - WT_W;
    WT_Y = WT_ROW_Y + (ch + 4 - WT_H) / 2;
    WL_X = WD_X + 12;
    WL_Y = WT_ROW_Y + (ch + 4) + 8;
    WL_W = WD_W - 24;
    WL_IH = ch + 4;
    WC_W = 6 * cw;
    WC_H = ch + 8;
    WC_X = WD_X + WD_W - WC_W - 12;
    WC_Y = WD_Y + WD_H - WC_H - 12;

    /* 窗口右下角缩放握把 */
    GWH = 16;
    GX = WX + WW - GWH;
    GY = WY + WH - GWH;

    /* 左侧桌面图标: 4x3 格, 纵向间隔 5 行 */
    ICON_X = 2 * cw;
    ICON_Y = 5 * ch;
    ICON_W = 4 * cw;
    ICON_H = 3 * ch;
    ICON_STEP = 5 * ch;
}

static void draw_desktop(void)
{
    int ch = gfx_cell_h();
    int gw = gfx_w(), gh = gfx_h();

    layout();
    draw_wallpaper();

    /* 窗口阴影/边框/标题栏 */
    gfx_fill(WX + 10, WY + 10, WW, WH, 0x080c14);
    gfx_fill(WX, WY, WW, WH, C_FRAME);
    gfx_bevel(WX, WY, WW, WH, C_FRAME_LT, C_FRAME_DK);
    gfx_fill(WX + 2, WY + 2, WW - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(WX + 2, WY + 2, WW - 4, (TITLE_H - 2) / 2, C_TITLE2);
    gfx_fill(WX + 2, WY + 2, WW - 4, 1, 0xdfefff);
    gfx_text(WX + 10, WY + (TITLE_H - ch) / 2, "GT-DOS Terminal", C_TEXT);
    gfx_fill(WX + 2, WY + TITLE_H - 3, WW - 4, 1, 0x4f9fdf);
    draw_button(MX_X, MX_Y, MX_W, MX_H, maximized ? "R" : "M", 0x2f5a8a);
    draw_button(BT_X, BT_Y, BT_W, BT_H, "x", 0x8a2f2f);

    /* 客户区 (控制台视口) */
    gfx_fill(CL_X - 2, CL_Y - 2, CL_W + 4, CL_H + 4, C_CLIENT);

    /* 右下角缩放握把 (最大化时隐藏) */
    if (!maximized) {
        for (int i = 0; i < 3; i++) {
            int d = 5 + i * 4;
            gfx_fill(WX + WW - d - 2, WY + WH - 6, 2, 4, 0x9aa8c0);
            gfx_fill(WX + WW - 6, WY + WH - d - 2, 4, 2, 0x9aa8c0);
        }
    }

    /* 任务栏: 渐变底 + 顶部高光 */
    int ty0 = gh - TASK_H;
    gfx_fill(0, ty0, gw, TASK_H, 0x141c2a);
    gfx_fill(0, ty0, gw, TASK_H / 2, 0x1b2740);
    gfx_fill(0, ty0, gw, 2, 0x3c5a80);

    /* 开始按钮: 四色 logo + Start */
    gfx_fill(MB_X, MB_Y, MB_W, MB_H, 0x22344f);
    gfx_bevel(MB_X, MB_Y, MB_W, MB_H, 0xbfd0e8, 0x101820);
    {
        int sq = ch / 4;
        if (sq < 3) sq = 3;
        int gap = 2;
        int lx = MB_X + 8;
        int ly = MB_Y + (TASK_H - (sq * 2 + gap)) / 2;
        u32 lc[4] = { 0x66ccff, 0x7fe0a0, 0xffc060, 0xc090ff };
        gfx_fill(lx, ly, sq, sq, lc[0]);
        gfx_fill(lx + sq + gap, ly, sq, sq, lc[1]);
        gfx_fill(lx, ly + sq + gap, sq, sq, lc[2]);
        gfx_fill(lx + sq + gap, ly + sq + gap, sq, sq, lc[3]);
        gfx_text(lx + 2 * sq + gap + 6, MB_Y + (TASK_H - ch) / 2,
                 "Start", C_TEXT);
    }

    /* 任务栏窗口按钮 (活动窗口, 点击切换最大化) */
    gfx_fill(TK_X, TK_Y, TK_W, TK_H, 0x2a4a70);
    gfx_bevel(TK_X, TK_Y, TK_W, TK_H, 0x9fc0e8, 0x0c1220);
    {
        int isz = ch;
        if (isz > TK_H - 4) isz = TK_H - 4;
        draw_app_glyph(TK_X + 5, TK_Y + (TASK_H - isz) / 2, isz);
        gfx_text(TK_X + 7 + isz, TK_Y + (TASK_H - ch) / 2,
                 "Terminal", 0xdce9ff);
    }

    /* 时钟分隔线 + TEXT 按钮 */
    gfx_fill(CK_X - 8, ty0 + 4, 1, TASK_H - 8, 0x3c5a80);
    draw_button(EX_X, EX_Y, EX_W, EX_H, "TEXT", 0x2a3a4e);
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
    settings_open = false;
    widgets_open = false;
    maximized = false;
    theme = 0;
    icon_sel = -1;
    menu_hot = -1;
    slider_drag = false;
    resizing = false;
    app_canvas = false;
    vga_hold_display(false);
    list_sel = 0;
    wdg_progress = 0;
    show_icons = true;
    show_clock = true;
    clock24 = true;
    bright = 100;
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
    settings_open = false;
    widgets_open = false;
    maximized = false;
    menu_hot = -1;
    slider_drag = false;
    resizing = false;
    app_canvas = false;
    vga_hold_display(false);
    if (cur_x >= 0)
        gfx_fb_restore(cur_x - 1, cur_y - 1, cs_w + 2, cs_h + 2);
    cur_x = cur_y = -1;
    vga_gui_leave();
    kprintf("[gui] text mode\n");
}

/* ---------------------------------------------- 用户程序全屏画布 (GUI 库) */
/* 程序调用 SYS_GUI_BEGIN 接管屏幕: 桌面停止处理输入与时钟重绘, 程序自己
   通过 gfx 系统调用绘制并 flush; SYS_GUI_END 恢复桌面. */
bool gui_canvas_begin(void)
{
    if (!gfx_active() || !active)
        return false;
    menu_open = false;
    about_open = false;
    settings_open = false;
    widgets_open = false;
    menu_hot = -1;
    app_canvas = true;
    vga_hold_display(true);          /* 程序 printf 不再画到画布上 */
    return true;
}

void gui_canvas_end(void)
{
    if (!app_canvas)
        return;
    app_canvas = false;
    vga_hold_display(false);         /* 恢复控制台绘制 */
    if (active)
        redraw_desktop();
}

/* exec 返回后兜底: 程序未调用 GUI_END 或异常退出时也恢复桌面 */
void gui_canvas_reset(void)
{
    gui_canvas_end();
}

bool gui_canvas_active(void) { return app_canvas; }

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

/* 按鼠标 x 设置亮度滑块 (40..100) */
static void slider_set(int x)
{
    int v = 40 + (x - SD_SL_X) * 60 / (SD_SL_W > 1 ? SD_SL_W : 1);
    if (v < 40) v = 40;
    if (v > 100) v = 100;
    if (v != bright) {
        bright = v;
        redraw_desktop();
    }
}

void gui_pump(void)
{
    if (!active)
        return;
    if (app_canvas)
        return;                     /* 用户程序接管屏幕, 桌面不处理输入 */

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
        } else if (settings_open) {
            /* 模态设置面板: 按钮 / 复选框 / 滑块 */
            if (inside(x, y, SD_PREV_X, SD_PREV_Y, SD_BTN_W, SD_BTN_H)) {
                theme = (theme + 2) % 3;
                redraw_desktop();
            } else if (inside(x, y, SD_NEXT_X, SD_PREV_Y, SD_BTN_W, SD_BTN_H)) {
                theme = (theme + 1) % 3;
                redraw_desktop();
            } else if (inside(x, y, SD_C1_X, SD_C1_Y, SD_CHK_W, SD_CHK_H)) {
                show_icons = !show_icons;
                if (!show_icons)
                    icon_sel = -1;
                redraw_desktop();
            } else if (inside(x, y, SD_C2_X, SD_C2_Y, SD_CHK_W, SD_CHK_H)) {
                clock24 = !clock24;
                redraw_desktop();
            } else if (inside(x, y, SD_SL_X, SD_SL_Y, SD_SL_W, SD_SL_H)) {
                slider_drag = true;
                slider_set(x);
            } else if (inside(x, y, SD_CLOSE_X, SD_CLOSE_Y,
                              SD_CLOSE_W, SD_CLOSE_H) ||
                       !inside(x, y, SD_X, SD_Y, SD_W, SD_H)) {
                settings_open = false;
                redraw_desktop();
            }
        } else if (widgets_open) {
            /* 模态控件演示: 单选组 / 列表 / 关闭 */
            int r = -1, li = -1;
            for (int i = 0; i < 3; i++)
                if (inside(x, y, WR1_X + i * WR_STEP, WR_Y, WR_STEP, WR_SZ))
                    r = i;
            for (int i = 0; i < 4; i++)
                if (inside(x, y, WL_X, WL_Y + i * WL_IH, WL_W, WL_IH))
                    li = i;
            if (r >= 0) {
                theme = r;
                redraw_desktop();
            } else if (li >= 0) {
                list_sel = li;
                redraw_desktop();
            } else if (inside(x, y, WD_X + 12, WT_ROW_Y,
                              WT_X + WT_W - (WD_X + 12),
                              gfx_cell_h() + 4)) {
                show_clock = !show_clock;           /* Toggle 开关 */
                redraw_desktop();
            } else if (inside(x, y, WC_X, WC_Y, WC_W, WC_H) ||
                       !inside(x, y, WD_X, WD_Y, WD_W, WD_H)) {
                widgets_open = false;
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
                    settings_open = true;         /* Settings: 设置面板 */
                    redraw_desktop();
                } else if (idx == 4) {
                    widgets_open = true;          /* Widgets: 控件演示 */
                    wdg_progress = 0;
                    wdg_last = timer_ticks();
                    redraw_desktop();
                } else if (idx == 5) {
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
        } else if (inside(x, y, TK_X, TK_Y, TK_W, TK_H)) {
            toggle_max();                         /* 任务栏窗口按钮 */
        } else if (inside(x, y, MX_X, MX_Y, MX_W, MX_H)) {
            toggle_max();                         /* 最大化 / 还原 */
        } else if (icon_hit(x, y) >= 0) {
            icon_sel = icon_hit(x, y);            /* 桌面图标: 高亮反馈 */
            redraw_desktop();
        } else if (!maximized && inside(x, y, GX, GY, GWH, GWH)) {
            resizing = true;                      /* 右下角缩放握把 */
            resize_base_w = WW;
            resize_base_h = WH;
            resize_px = x;
            resize_py = y;
        } else if (inside(x, y, WX, WY, WW, TITLE_H)) {
            dragging = true;
            drag_dx = x - WX;
            drag_dy = y - WY;
        }
    }
    if (!left) {
        dragging = false;
        slider_drag = false;
        resizing = false;
    }

    if (slider_drag && left)
        slider_set(x);

    if (resizing && left) {
        int rw = resize_base_w + (x - resize_px);
        int rh = resize_base_h + (y - resize_py);
        int minw = 40 * gfx_cell_w();
        int minh = 12 * gfx_cell_h();
        int maxw = gfx_w() - WX;
        int maxh = gfx_h() - TASK_H - WY;
        if (rw < minw) rw = minw;
        if (rh < minh) rh = minh;
        if (rw > maxw) rw = maxw;
        if (rh > maxh) rh = maxh;
        if (rw != WW || rh != WH) {
            WW = rw;
            WH = rh;
            norm_w = WW;
            norm_h = WH;
            redraw_desktop();
            return;
        }
    }

    /* 菜单悬停高亮: 鼠标移到哪一项就高亮哪一项 */
    if (menu_open && !pressed) {
        int h = menu_pick(x, y);
        if (h != menu_hot) {
            menu_hot = h;
            draw_menu();
            gfx_flip_rect(MN_X - 2, MN_Y - 2, MN_W + 4, MN_H + 4);
        }
    }

    /* 控件演示: 进度条按 1/8 秒步进 (只在变化时重画对话框) */
    if (widgets_open) {
        u32 hz = timer_hz();
        if (!hz) hz = 100;
        u32 step = hz / 8;
        if (!step) step = 1;
        if (timer_ticks() - wdg_last >= step) {
            wdg_last = timer_ticks();
            wdg_progress += 5;
            if (wdg_progress > 100)
                wdg_progress = 0;
            draw_widgets();
            gfx_flip_rect(WD_X - 2, WD_Y - 2, WD_W + 4, WD_H + 4);
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
        if (show_clock)
            draw_clock();
    }
}
