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
#define C_FRAME     0x23262b
#define C_FRAME_LT  0x3a3f46
#define C_FRAME_DK  0x0e1013
#define C_TITLE     0x2a2e36
#define C_TITLE2    0x32363f
#define C_CLIENT    0x000000
#define C_TASK      0x1b1e24
#define C_TASK_LT   0x3daee9
#define C_TEXT      0xeaecef
#define C_ACCENT    0x3daee9

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
static int MX_X, MX_Y, MX_W, MX_H;      /* 最大化/还原按钮 */
static int MINB_X, MINB_Y, MINB_W, MINB_H;  /* 最小化按钮 */
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
static bool minimized;                  /* 窗口已最小化 (控制台暂停绘制) */
/* 窗口动画: 1=max 2=unmax 3=minimize 4=unminimize */
static bool anim_active;
static int  anim_kind;
static int  anim_last_t;
static u32  anim_t0;
static int  ax0, ay0, aw0, ah0, ax1, ay1, aw1, ah1;
static bool in_redraw;
#define ANIM_MS 150
static int ICON_X, ICON_Y, ICON_W, ICON_H, ICON_STEP;  /* 桌面图标 */
static int icon_sel = -1;               /* 最近点击的图标 */

/* 图标位置 (可拖动) 与拖放状态 */
static int  icon_px[3], icon_py[3];
static bool icon_pos_ready;
static bool icon_drag;
static int  icon_drag_idx;
static int  icon_dx, icon_dy;           /* 按下点相对图标左上角 */
static int  icon_press_x, icon_press_y;
static bool icon_moved;
static bool icon_was_sel;               /* 按下前该图标已被选中 (再次单击=打开) */
static int  icon_last_click = -1;
static u32  icon_last_t;

/* 右键上下文菜单 */
static bool ctx_open;
static int  CTX_X, CTX_Y, CTX_W, CTX_H, CTX_IH, CTX_N, CTX_HOT = -1;
static int  ctx_target = -1;            /* -1=桌面, 0..2=图标 */

/* 属性对话框 */
static bool prop_open;
static int  prop_target = -1;
static int  PR_X, PR_Y, PR_W, PR_H;
static int  PR_OK_X, PR_OK_Y, PR_OK_W, PR_OK_H;

/* FILES 应用窗口 */
static bool files_open;
static int  FL_X, FL_Y, FL_W, FL_H, FL_ROW_Y, FL_ROW_H, FL_NROW;
static int  FL_CL_X, FL_CL_Y, FL_CL_W, FL_CL_H;
static struct fat_dirent files_ents[12];
static int  files_n, files_sel;

/* SYS 应用窗口 */
static bool sys_open;
static int  SY_X, SY_Y, SY_W, SY_H, SY_ROW_Y;
static int  SY_CL_X, SY_CL_Y, SY_CL_W, SY_CL_H;

static bool inside(int x, int y, int bx, int by, int bw, int bh);
static void redraw_desktop(void);
static void draw_overlays(void);
static void draw_ctx_menu(void);
static void draw_props(void);
static void draw_files(void);
static void draw_sys(void);
static void open_app(int idx);
static void ctx_show(int x, int y, int target);
static void ctx_activate(int target, int item);
static int  ctx_pick(int x, int y);
static void files_reload(void);
static void start_unminimize(void);

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
    if (menu_open || about_open || settings_open || widgets_open ||
        ctx_open || prop_open || files_open || sys_open) {
        draw_overlays();                                  /* 重画进后台缓冲 */
        if (menu_open)
            gfx_fb_restore(MN_X - 2, MN_Y - 2, MN_W + 4, MN_H + 4);
        if (about_open)
            gfx_fb_restore(AB_X - 2, AB_Y - 2, AB_W + 4, AB_H + 4);
        if (settings_open)
            gfx_fb_restore(SD_X - 2, SD_Y - 2, SD_W + 4, SD_H + 4);
        if (widgets_open)
            gfx_fb_restore(WD_X - 2, WD_Y - 2, WD_W + 4, WD_H + 4);
        if (ctx_open)
            gfx_fb_restore(CTX_X - 2, CTX_Y - 2, CTX_W + 4, CTX_H + 4);
        if (prop_open)
            gfx_fb_restore(PR_X - 2, PR_Y - 2, PR_W + 4, PR_H + 4);
        if (files_open)
            gfx_fb_restore(FL_X - 2, FL_Y - 2, FL_W + 4, FL_H + 4);
        if (sys_open)
            gfx_fb_restore(SY_X - 2, SY_Y - 2, SY_W + 4, SY_H + 4);
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
    int tw = gfx_text_w(L("gui.desktop"));
    gfx_text((gfx_w() - tw) / 2, 2 * ch, L("gui.desktop"), 0xdfefff);
    tw = gfx_text_w(L("gui.hint"));
    gfx_text((gfx_w() - tw) / 2, 3 * ch, L("gui.hint"), 0x8fb6d8);
    /* 左侧桌面图标 (无背景填充; 使用 SVG->ICO 高质量图标) */
    if (!show_icons)
        return;
    static const char *icos[3] = { "TERM.ICO", "FILES.ICO", "SYS.ICO" };
    static const char *keys[3] = { "gui.term", "gui.files", "gui.sys" };
    for (int i = 0; i < 3; i++) {
        int ix = icon_px[i], iy = icon_py[i];
        if (!ico_draw_file_at(icos[i], ix, iy, ICON_W, ICON_H)) {
            if (i == 0)
                draw_icon_term(ix, iy);
            else if (i == 1)
                draw_icon_files(ix, iy);
            else
                draw_icon_sys(ix, iy);
        }
        if (icon_sel == i)
            gfx_bevel(ix - 3, iy - 3, ICON_W + 6, ICON_H + 6,
                      C_ACCENT, C_ACCENT);
        const char *nm = L(keys[i]);
        int lw = gfx_text_w(nm);
        gfx_text(ix + (ICON_W - lw) / 2, iy + ICON_H + 2, nm,
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
    if (!in_redraw)
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

/* Linux 风格窗口按钮图标: 0=最小化 1=最大化 2=还原 3=关闭 */
static void draw_win_glyph(int x, int y, int sz, int kind)
{
    int m = sz / 4;
    if (kind == 0) {
        gfx_fill(x + m, y + sz / 2 - 1, sz - 2 * m, 2, C_TEXT);
    } else if (kind == 1) {
        gfx_frame(x + m, y + m, sz - 2 * m, sz - 2 * m, C_TEXT);
    } else if (kind == 2) {
        gfx_frame(x + m, y + m + 3, sz - 2 * m - 3, sz - 2 * m - 3, C_TEXT);
        gfx_frame(x + m + 3, y + m, sz - 2 * m - 3, sz - 2 * m - 3, C_TEXT);
    } else {
        for (int i = 0; i < sz - 2 * m; i++) {
            gfx_fill(x + m + i, y + m + i, 2, 2, C_TEXT);
            gfx_fill(x + sz - m - 1 - i, y + m + i, 2, 2, C_TEXT);
        }
    }
}

static void draw_win_button(int x, int y, int w, int h, int kind, u32 bg)
{
    gfx_fill(x, y, w, h, bg);
    gfx_bevel(x, y, w, h, 0x4a5058, 0x15181c);
    draw_win_glyph(x, y, (w < h ? w : h), kind);
}

/* ------------------------------------------------------- 浮层: 菜单/对话框 */
/* 开始菜单: 6 个可点项 (Terminal / Theme / About / Settings / Widgets / Exit) */
static void draw_menu(void)
{
    int ch = gfx_cell_h();
    static const char *keys[6] = {
        "gui.menu.terminal", "gui.menu.theme", "gui.menu.about",
        "gui.menu.settings", "gui.menu.widgets", "gui.menu.exit"
    };
    static const char *icos[6] = {
        "TERM.ICO", "WIDGETS.ICO", "ABOUT.ICO",
        "SETTINGS.ICO", "WIDGETS.ICO", "SYS.ICO"
    };
    u32 icol[6] = { 0x3daee9, 0x7fe0a0, 0x9fb3ff,
                    0xffc060, 0xc090ff, 0xff9090 };
    gfx_fill(MN_X, MN_Y, MN_W, MN_H, 0x23262b);
    gfx_bevel(MN_X, MN_Y, MN_W, MN_H, 0x4a5058, 0x0e1013);
    gfx_fill(MN_X + 2, MN_Y + 2, MN_W - 4, 2, C_ACCENT);
    for (int i = 0; i < 6; i++) {
        int y = MN_Y + 4 + i * MI_H;
        if (i == 5)
            gfx_fill(MN_X + 6, y - 2, MN_W - 12, 1, 0x4a5058);
        bool hot = i == menu_hot;
        gfx_fill(MN_X + 2, y + 1, MN_W - 4, MI_H - 2,
                 hot ? C_ACCENT : 0x2b3038);
        if (!ico_draw_file_at(icos[i], MN_X + 8, y + (MI_H - ch) / 2,
                              ch, ch))
            gfx_fill(MN_X + 8, y + (MI_H - 4) / 2, 4, 4, icol[i]);
        gfx_text(MN_X + 12 + ch, y + (MI_H - ch) / 2, L(keys[i]),
                 hot ? 0x101418 : (i == 5 ? 0xff9a9a : C_TEXT));
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
    gfx_fill(AB_X, AB_Y, AB_W, AB_H, 0x23262b);
    gfx_bevel(AB_X, AB_Y, AB_W, AB_H, 0x4a5058, 0x0e1013);
    gfx_fill(AB_X + 2, AB_Y + 2, AB_W - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(AB_X + 2, AB_Y + 2, AB_W - 4, 2, C_ACCENT);
    if (!ico_draw_file_at("ABOUT.ICO", AB_X + 8,
                          AB_Y + (TITLE_H - ch) / 2, ch, ch))
        draw_app_glyph(AB_X + 8, AB_Y + (TITLE_H - ch) / 2, ch);
    gfx_text(AB_X + 12 + ch, AB_Y + (TITLE_H - ch) / 2,
             L("gui.about_title"), C_TEXT);

    int x = AB_X + 12;
    int y = AB_Y + TITLE_H + 8;
    static const char *keys[6] = {
        "gui.about_l1", "gui.about_l2", "gui.about_l3",
        "gui.about_l4", "gui.about_l5", "gui.about_l6"
    };
    for (int i = 0; i < 6 && y + ch <= AB_OK_Y; i++, y += ch + 2)
        gfx_text(x, y, L(keys[i]), 0xcfe0f5);

    draw_button(AB_OK_X, AB_OK_Y, AB_OK_W, AB_OK_H, L("gui.ok"), 0x3a3f46);
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
    static const char *keys[3] = { "gui.blue", "gui.green", "gui.purple" };

    gfx_fill(SD_X, SD_Y, SD_W, SD_H, 0x23262b);
    gfx_bevel(SD_X, SD_Y, SD_W, SD_H, 0x4a5058, 0x0e1013);
    gfx_fill(SD_X + 2, SD_Y + 2, SD_W - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(SD_X + 2, SD_Y + 2, SD_W - 4, 2, C_ACCENT);
    if (!ico_draw_file_at("SETTINGS.ICO", SD_X + 8,
                          SD_Y + (TITLE_H - ch) / 2, ch, ch))
        draw_app_glyph(SD_X + 8, SD_Y + (TITLE_H - ch) / 2, ch);
    gfx_text(SD_X + 12 + ch, SD_Y + (TITLE_H - ch) / 2,
             L("gui.settings_title"), C_TEXT);

    /* 主题: < 名称 > */
    draw_button(SD_PREV_X, SD_PREV_Y, SD_BTN_W, SD_BTN_H, "<", 0x3a3f46);
    draw_button(SD_NEXT_X, SD_PREV_Y, SD_BTN_W, SD_BTN_H, ">", 0x3a3f46);
    int lx = SD_PREV_X + SD_BTN_W + 12;
    int ly = SD_PREV_Y + (SD_BTN_H - ch) / 2;
    gfx_text(lx, ly, L("gui.theme"), 0x8fa8c8);
    gfx_text(lx + gfx_text_w(L("gui.theme")) + 8, ly, L(keys[theme % 3]),
             0xdce9ff);

    /* 复选框 */
    draw_check(SD_C1_X, SD_C1_Y, show_icons, L("gui.icons"));
    draw_check(SD_C2_X, SD_C2_Y, clock24, L("gui.clock24"));

    /* 亮度滑块 */
    gfx_text(SD_X + 12, SD_SL_Y + (SD_SL_H - ch) / 2, L("gui.brightness"),
             0x8fa8c8);
    draw_slider();

    draw_button(SD_CLOSE_X, SD_CLOSE_Y, SD_CLOSE_W, SD_CLOSE_H,
                L("gui.close"), 0x3a3f46);
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
    static const char *rlab[3] = { "gui.blue", "gui.green", "gui.purple" };
    static const char *items[4] = { "Alpha", "Beta", "Gamma", "Delta" };

    gfx_fill(WD_X, WD_Y, WD_W, WD_H, 0x23262b);
    gfx_bevel(WD_X, WD_Y, WD_W, WD_H, 0x4a5058, 0x0e1013);
    gfx_fill(WD_X + 2, WD_Y + 2, WD_W - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(WD_X + 2, WD_Y + 2, WD_W - 4, 2, C_ACCENT);
    if (!ico_draw_file_at("WIDGETS.ICO", WD_X + 8,
                          WD_Y + (TITLE_H - ch) / 2, ch, ch))
        draw_app_glyph(WD_X + 8, WD_Y + (TITLE_H - ch) / 2, ch);
    gfx_text(WD_X + 12 + ch, WD_Y + (TITLE_H - ch) / 2,
             L("gui.widgets_title"), C_TEXT);

    for (int i = 0; i < 3; i++)
        draw_radio(WR1_X + i * WR_STEP, WR_Y, WR_SZ, theme == i, L(rlab[i]));

    gfx_text(WD_X + 12, WP_LAB_Y, L("gui.progress"), 0x8fa8c8);
    gfx_bar(WP_X, WP_Y, WP_W, WP_H, wdg_progress, C_ACCENT);

    /* 开关: 任务栏时钟 */
    gfx_text(WD_X + 12, WT_ROW_Y + 2, L("gui.taskbar_clock"),
             show_clock ? 0xdce9ff : 0x8fa8c8);
    draw_toggle(WT_X, WT_Y, WT_W, WT_H, show_clock);

    gfx_fill(WL_X, WL_Y, WL_W, WL_IH * 4, 0x15181c);
    gfx_bevel(WL_X, WL_Y, WL_W, WL_IH * 4, 0x4a5058, 0x0e1013);
    for (int i = 0; i < 4; i++) {
        int iy = WL_Y + i * WL_IH;
        bool on = i == list_sel;
        if (on)
            gfx_fill(WL_X + 2, iy + 1, WL_W - 4, WL_IH - 2, 0x3a3f46);
        gfx_text(WL_X + 8, iy + (WL_IH - ch) / 2, items[i],
                 on ? C_TEXT : 0x9fb3cc);
    }

    draw_button(WC_X, WC_Y, WC_W, WC_H, L("gui.close"), 0x3a3f46);
}

/* ------------------------------------------------- 小图形 / 文本工具 */
static void draw_mini_folder(int x, int y, int sz)
{
    int tb = sz / 3;
    if (tb < 2) tb = 2;
    int tw = sz * 2 / 3;
    gfx_fill(x, y, tw, tb, 0xd8b040);
    gfx_fill(x, y + tb, sz, sz - tb, 0xdfc060);
    gfx_bevel(x, y + tb, sz, sz - tb, 0xfff0b0, 0x806010);
}

static void draw_mini_file(int x, int y, int sz)
{
    gfx_fill(x, y, sz, sz, 0xd8e4f0);
    gfx_bevel(x, y, sz, sz, 0xffffff, 0x506070);
    for (int i = 0; i < 3; i++)
        gfx_fill(x + 3, y + 4 + i * (sz - 8) / 3, sz - 6, 1, 0x7086a0);
}

static int u32_str(u32 v, char *b, int cap)
{
    char t[12];
    int k = 0, i = 0;
    if (!v)
        t[k++] = '0';
    while (v && k < (int)sizeof(t)) {
        t[k++] = (char)('0' + v % 10u);
        v /= 10u;
    }
    while (k > 0 && i < cap - 1)
        b[i++] = t[--k];
    b[i] = '\0';
    return i;
}

static void join_label(char *out, int cap, const char *label, const char *val)
{
    int i = 0, j = 0;
    while (label[j] && i < cap - 1)
        out[i++] = label[j++];
    if (i < cap - 1) out[i++] = ':';
    if (i < cap - 1) out[i++] = ' ';
    j = 0;
    while (val[j] && i < cap - 1)
        out[i++] = val[j++];
    out[i] = '\0';
}

static bool has_ext(const char *name, const char *ext)
{
    int n = (int)strlen(name), e = (int)strlen(ext);
    if (n <= e)
        return false;
    return !strcasecmp(name + n - e, ext);
}

/* 根目录绝对路径: \NAME */
static void root_path(const char *name, char *out, int cap)
{
    int i = 0;
    if (cap < 2) {
        if (cap > 0) out[0] = '\0';
        return;
    }
    out[i++] = '\\';
    for (int j = 0; name[j] && i < cap - 1; j++)
        out[i++] = name[j];
    out[i] = '\0';
}

/* ------------------------------------------------------ 右键上下文菜单 */
static const char *ctx_label(int target, int item)
{
    static const char *dkeys[5] = {
        "gui.ctx.terminal", "gui.ctx.files", "gui.ctx.sys",
        "gui.ctx.settings", "gui.ctx.props"
    };
    if (target < 0) {
        if (item < 0) item = 0;
        if (item > 4) item = 4;
        return L(dkeys[item]);
    }
    return L(item == 0 ? "gui.ctx.open" : "gui.ctx.props");
}

static int ctx_count(int target) { return target < 0 ? 5 : 2; }

static void ctx_show(int x, int y, int target)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    ctx_target = target;
    CTX_N = ctx_count(target);
    CTX_IH = ch + 8;
    CTX_W = 10 * cw;
    for (int i = 0; i < CTX_N; i++) {
        int w = gfx_text_w(ctx_label(target, i)) + ch + 28;
        if (w > CTX_W)
            CTX_W = w;
    }
    CTX_H = CTX_N * CTX_IH + 6;
    CTX_X = x;
    CTX_Y = y;
    if (CTX_X + CTX_W > gfx_w() - 4)
        CTX_X = x - CTX_W;
    if (CTX_Y + CTX_H > gfx_h() - TASK_H - 4)
        CTX_Y = y - CTX_H;
    if (CTX_X < 4) CTX_X = 4;
    if (CTX_Y < 4) CTX_Y = 4;
    CTX_HOT = -1;
    ctx_open = true;
    kprintf("[gui] menu\n");
}

static void draw_ctx_menu(void)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    gfx_fill(CTX_X, CTX_Y, CTX_W, CTX_H, 0x23262b);
    gfx_bevel(CTX_X, CTX_Y, CTX_W, CTX_H, 0x4a5058, 0x0e1013);
    gfx_fill(CTX_X + 2, CTX_Y + 2, CTX_W - 4, 2, C_ACCENT);
    for (int i = 0; i < CTX_N; i++) {
        int y = CTX_Y + 4 + i * CTX_IH;
        bool hot = i == CTX_HOT;
        gfx_fill(CTX_X + 2, y + 1, CTX_W - 4, CTX_IH - 2,
                 hot ? C_ACCENT : 0x2b3038);
        gfx_fill(CTX_X + 8, y + (CTX_IH - 4) / 2, 4, 4,
                 hot ? 0x101418 : 0x6fa8d8);
        gfx_text(CTX_X + 12 + ch / 2, y + (CTX_IH - ch) / 2,
                 ctx_label(ctx_target, i), hot ? 0x101418 : C_TEXT);
    }
    (void)cw;
}

static int ctx_pick(int x, int y)
{
    if (!inside(x, y, CTX_X, CTX_Y, CTX_W, CTX_H))
        return -1;
    int idx = (y - (CTX_Y + 4)) / CTX_IH;
    if (idx < 0) idx = 0;
    if (idx >= CTX_N) idx = CTX_N - 1;
    return idx;
}

static void files_reload(void)
{
    files_n = 0;
    if (fat_ready()) {
        int n = fat_list_dir("\\", files_ents, 12);
        if (n > 0)
            files_n = n > 12 ? 12 : n;
    }
    files_sel = 0;
}

/* 桌面应用启动 (0=TERM 1=FILES 2=SYS) */
static void open_app(int idx)
{
    if (idx <= 0) {
        kprintf("[gui] open terminal\n");
        menu_open = about_open = settings_open = widgets_open = false;
        ctx_open = prop_open = files_open = sys_open = false;
        icon_sel = 0;
        if (minimized) {
            start_unminimize();
            return;
        }
        redraw_desktop();
        return;
    }
    if (idx == 1) {
        files_reload();
        files_open = true;
        sys_open = false;
    } else {
        sys_open = true;
        files_open = false;
    }
    prop_open = false;
    ctx_open = false;
    menu_open = about_open = settings_open = widgets_open = false;
    icon_sel = idx;
    kprintf(idx == 1 ? "[gui] open files\n" : "[gui] open system\n");
    redraw_desktop();
}

static void ctx_activate(int target, int item)
{
    if (target < 0) {
        if (item <= 2) {
            open_app(item);
            return;
        }
        if (item == 3) {
            settings_open = true;
            kprintf("[gui] settings\n");
        } else {
            prop_open = true;
            prop_target = -1;
            kprintf("[gui] properties\n");
        }
    } else if (item == 0) {
        open_app(target);
        return;
    } else {
        prop_open = true;
        prop_target = target;
        kprintf("[gui] properties\n");
    }
    redraw_desktop();
}

/* --------------------------------------------------------- 属性对话框 */
static void draw_props(void)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    static const char *keys[3] = { "gui.term", "gui.files", "gui.sys" };
    static const char *typs[3] = { "gui.app", "gui.folder", "gui.sys_title" };
    static const char *icos[3] = { "TERM.ICO", "FILES.ICO", "SYS.ICO" };
    static const char *themes[3] = { "gui.blue", "gui.green", "gui.purple" };
    char v[64], line[96], num[16], num2[16];

    gfx_fill(PR_X, PR_Y, PR_W, PR_H, 0x23262b);
    gfx_bevel(PR_X, PR_Y, PR_W, PR_H, 0x4a5058, 0x0e1013);
    gfx_fill(PR_X + 2, PR_Y + 2, PR_W - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(PR_X + 2, PR_Y + 2, PR_W - 4, 2, C_ACCENT);
    if (!ico_draw_file_at("ABOUT.ICO", PR_X + 8,
                          PR_Y + (TITLE_H - ch) / 2, ch, ch))
        draw_app_glyph(PR_X + 8, PR_Y + (TITLE_H - ch) / 2, ch);
    gfx_text(PR_X + 12 + ch, PR_Y + (TITLE_H - ch) / 2,
             L("gui.props_title"), C_TEXT);

    int x = PR_X + 12, y = PR_Y + TITLE_H + 10;

    if (prop_target >= 0) {
        int t = prop_target % 3;
        if (!ico_draw_file_at(icos[t], x, y, 3 * ch, 3 * ch))
            draw_app_glyph(x, y, 3 * ch);
        gfx_text(x + 3 * ch + 10, y + ch, L(keys[t]), 0xdce9ff);
        y += 3 * ch + 10;
        join_label(line, sizeof(line), L("gui.prop.name"), L(keys[t]));
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        join_label(line, sizeof(line), L("gui.prop.type"), L(typs[t]));
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        struct fat_dirent de;
        if (t < 2 && fat_find(icos[t], &de))
            u32_str(de.size, num, sizeof(num));
        else {
            num[0] = '-'; num[1] = '\0';
        }
        join_label(v, sizeof(v), num, L("gui.bytes"));
        join_label(line, sizeof(line), L("gui.prop.size"), v);
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        join_label(line, sizeof(line), L("gui.prop.loc"), L("gui.desk"));
        gfx_text(x, y, line, 0xcfe0f5);
    } else {
        join_label(line, sizeof(line), L("gui.prop.name"), L("gui.desk"));
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        {
            int i = 0;
            const char *a = num, *b = num2;
            u32_str((u32)gfx_w(), num, sizeof(num));
            u32_str((u32)gfx_h(), num2, sizeof(num2));
            while (*a && i < (int)sizeof(v) - 1) v[i++] = *a++;
            if (i < (int)sizeof(v) - 1) v[i++] = 'x';
            while (*b && i < (int)sizeof(v) - 1) v[i++] = *b++;
            v[i] = '\0';
        }
        join_label(line, sizeof(line), L("gui.prop.res"), v);
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        u32_str(timer_uptime_ms() / 1000u, num, sizeof(num));
        join_label(v, sizeof(v), num, L("gui.uptime_s"));
        join_label(line, sizeof(line), L("gui.prop.uptime"), v);
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        join_label(line, sizeof(line), L("gui.prop.theme"),
                   L(themes[theme % 3]));
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        if (fat_ready()) {
            int i = 0;
            const char *a = num, *b = num2;
            u32_str(fat_free_clusters(), num, sizeof(num));
            u32_str(fat_total_clusters(), num2, sizeof(num2));
            while (*a && i < (int)sizeof(v) - 1) v[i++] = *a++;
            if (i < (int)sizeof(v) - 1) v[i++] = '/';
            while (*b && i < (int)sizeof(v) - 1) v[i++] = *b++;
            v[i] = '\0';
            join_label(line, sizeof(line), L("gui.prop.disk"), v);
            gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        }
        join_label(line, sizeof(line), L("gui.prop.lang"),
                   i18n_lang() ? "zh-CN" : "en");
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
        join_label(line, sizeof(line), L("gui.prop.ver"), "0.10");
        gfx_text(x, y, line, 0xcfe0f5);
    }
    (void)cw;
    draw_button(PR_OK_X, PR_OK_Y, PR_OK_W, PR_OK_H, L("gui.ok"), 0x3a3f46);
}

/* ------------------------------------------------------- FILES 应用窗口 */
static void draw_files(void)
{
    int ch = gfx_cell_h();
    char title[64], num[16];

    gfx_fill(FL_X, FL_Y, FL_W, FL_H, C_FRAME);
    gfx_bevel(FL_X, FL_Y, FL_W, FL_H, 0x4a5058, 0x0e1013);
    gfx_fill(FL_X + 2, FL_Y + 2, FL_W - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(FL_X + 2, FL_Y + 2, FL_W - 4, 2, C_ACCENT);
    if (!ico_draw_file_at("FILES.ICO", FL_X + 8,
                          FL_Y + (TITLE_H - ch) / 2, ch, ch))
        draw_icon_files(FL_X + 8, FL_Y + (TITLE_H - ch) / 2);
    {
        int i = 0;
        const char *a = L("gui.files_title"), *b = L("gui.desk.c");
        while (*a && i < (int)sizeof(title) - 6) title[i++] = *a++;
        if (i < (int)sizeof(title) - 4) { title[i++] = ' '; title[i++] = '-'; title[i++] = ' '; }
        while (*b && i < (int)sizeof(title) - 1) title[i++] = *b++;
        title[i] = '\0';
    }
    gfx_text(FL_X + 12 + ch, FL_Y + (TITLE_H - ch) / 2, title, C_TEXT);
    draw_win_button(FL_CL_X, FL_CL_Y, FL_CL_W, FL_CL_H, 3, 0x5a2a2e);
    gfx_text(FL_X + 12, FL_Y + TITLE_H + 4, L("gui.desk.c"), 0x8fb6d8);

    if (files_n <= 0)
        gfx_text(FL_X + 12, FL_ROW_Y, L("gui.empty"), 0x9fb3cc);
    for (int i = 0; i < files_n && i < FL_NROW; i++) {
        int y = FL_ROW_Y + i * FL_ROW_H;
        struct fat_dirent *e = &files_ents[i];
        bool on = i == files_sel;
        if (on)
            gfx_fill(FL_X + 6, y, FL_W - 12, FL_ROW_H, 0x3a3f46);
        int isz = ch;
        int ix = FL_X + 12, iy = y + (FL_ROW_H - isz) / 2;
        if (e->is_dir) {
            draw_mini_folder(ix, iy, isz);
        } else if (has_ext(e->name, ".gtl")) {
            char p[32];
            root_path(e->name, p, sizeof(p));
            if (!ico_draw_gtl_icon_at(p, ix, iy, isz, isz))
                draw_mini_file(ix, iy, isz);
        } else {
            draw_mini_file(ix, iy, isz);
        }
        gfx_text(FL_X + 20 + ch, y + (FL_ROW_H - ch) / 2, e->name,
                 on ? C_TEXT : 0xa8c0d8);
        if (e->is_dir) {
            const char *d = L("gui.dir");
            gfx_text(FL_X + FL_W - 12 - gfx_text_w(d),
                     y + (FL_ROW_H - ch) / 2, d, on ? C_TEXT : 0x9fb3cc);
        } else {
            u32_str(e->size, num, sizeof(num));
            gfx_text(FL_X + FL_W - 12 - gfx_text_w(num),
                     y + (FL_ROW_H - ch) / 2, num, on ? C_TEXT : 0x9fb3cc);
        }
    }
}

/* --------------------------------------------------------- SYS 应用窗口 */
static void draw_sys(void)
{
    int ch = gfx_cell_h();
    static const char *themes[3] = { "gui.blue", "gui.green", "gui.purple" };
    char line[96], num[16], num2[16], v[48];
    int x = SY_X + 24, y = SY_ROW_Y;

    gfx_fill(SY_X, SY_Y, SY_W, SY_H, C_FRAME);
    gfx_bevel(SY_X, SY_Y, SY_W, SY_H, 0x4a5058, 0x0e1013);
    gfx_fill(SY_X + 2, SY_Y + 2, SY_W - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(SY_X + 2, SY_Y + 2, SY_W - 4, 2, C_ACCENT);
    if (!ico_draw_file_at("SYS.ICO", SY_X + 8, SY_Y + (TITLE_H - ch) / 2, ch, ch))
        draw_icon_sys(SY_X + 8, SY_Y + (TITLE_H - ch) / 2);
    gfx_text(SY_X + 12 + ch, SY_Y + (TITLE_H - ch) / 2,
             L("gui.sys_title"), C_TEXT);
    draw_win_button(SY_CL_X, SY_CL_Y, SY_CL_W, SY_CL_H, 3, 0x5a2a2e);

    gfx_text(x, y, "GT-DOS 0.10  (x86-64)", 0xdce9ff); y += ch + 6;
    join_label(line, sizeof(line), L("gui.kernel"), "GTK1");
    gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
    {
        int i = 0;
        const char *a = num, *b = num2;
        u32_str((u32)gfx_w(), num, sizeof(num));
        u32_str((u32)gfx_h(), num2, sizeof(num2));
        while (*a && i < (int)sizeof(v) - 1) v[i++] = *a++;
        if (i < (int)sizeof(v) - 1) v[i++] = 'x';
        while (*b && i < (int)sizeof(v) - 1) v[i++] = *b++;
        v[i] = '\0';
    }
    join_label(line, sizeof(line), L("gui.prop.res"), v);
    gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
    u32_str(timer_uptime_ms() / 1000u, num, sizeof(num));
    join_label(v, sizeof(v), num, L("gui.uptime_s"));
    join_label(line, sizeof(line), L("gui.prop.uptime"), v);
    gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
    join_label(line, sizeof(line), L("gui.prop.theme"), L(themes[theme % 3]));
    gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
    join_label(line, sizeof(line), L("gui.prop.lang"),
               i18n_lang() ? "zh-CN" : "en");
    gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
    if (fat_ready()) {
        int i = 0;
        const char *a = num, *b = num2;
        u32_str(fat_free_clusters(), num, sizeof(num));
        u32_str(fat_total_clusters(), num2, sizeof(num2));
        while (*a && i < (int)sizeof(v) - 1) v[i++] = *a++;
        if (i < (int)sizeof(v) - 1) v[i++] = '/';
        while (*b && i < (int)sizeof(v) - 1) v[i++] = *b++;
        v[i] = '\0';
        join_label(line, sizeof(line), L("gui.prop.disk"), v);
        gfx_text(x, y, line, 0xcfe0f5); y += ch + 4;
    }
    join_label(line, sizeof(line), L("gui.prop.ver"), "0.10");
    gfx_text(x, y, line, 0xcfe0f5);
}

/* 浮层绘制 (菜单/对话框): 画在后台缓冲, 随后 gfx_flip 一并提交.
   这样指针移动时 gfx_fb_restore 能从后台缓冲正确恢复, 不会擦掉菜单. */
static void draw_overlays(void)
{
    if (files_open)
        draw_files();
    if (sys_open)
        draw_sys();
    if (menu_open)
        draw_menu();
    if (about_open)
        draw_about();
    if (settings_open)
        draw_settings();
    if (widgets_open)
        draw_widgets();
    if (prop_open)
        draw_props();
    if (ctx_open)
        draw_ctx_menu();
}

/* 最大化/还原 (带动画; 保存并恢复窗口位置与尺寸) */
static int ilerp(int a, int b, int t) { return a + (b - a) * t / 100; }

static void anim_begin(int kind, int x1, int y1, int w1, int h1)
{
    anim_kind = kind;
    ax0 = WX; ay0 = WY; aw0 = WW; ah0 = WH;
    ax1 = x1; ay1 = y1; aw1 = w1; ah1 = h1;
    anim_t0 = timer_uptime_ms();
    anim_last_t = -1;
    anim_active = true;
}

static int  min_x0, min_y0, min_w0, min_h0;
static bool min_was_max;

static void toggle_max(void)
{
    int tx, ty, tw, th;
    if (!maximized) {
        norm_x = WX; norm_y = WY; norm_w = WW; norm_h = WH;
        tx = 0; ty = 0; tw = gfx_w(); th = gfx_h() - TASK_H;
        maximized = true;
        anim_begin(1, tx, ty, tw, th);
    } else {
        tx = norm_x; ty = norm_y; tw = norm_w; th = norm_h;
        maximized = false;
        anim_begin(2, tx, ty, tw, th);
    }
}

static void start_minimize(void)
{
    if (minimized || anim_active)
        return;
    min_x0 = WX; min_y0 = WY; min_w0 = WW; min_h0 = WH;
    min_was_max = maximized;
    anim_begin(3, TK_X + 8, TK_Y + 6, 40, 30);
}

static void start_unminimize(void)
{
    if (!minimized || anim_active)
        return;
    minimized = false;
    vga_hold_display(true);               /* 动画期间客户区留空 */
    WX = TK_X + 8;
    WY = TK_Y + 6;
    WW = 40;
    WH = 30;
    int tx, ty, tw, th;
    if (min_was_max) {
        tx = 0; ty = 0; tw = gfx_w(); th = gfx_h() - TASK_H;
    } else {
        tx = min_x0; ty = min_y0; tw = min_w0; th = min_h0;
    }
    anim_begin(4, tx, ty, tw, th);
}

static void anim_finish(void)
{
    WX = ax1; WY = ay1; WW = aw1; WH = ah1;
    if (anim_kind == 3) {
        minimized = true;
        vga_hold_display(true);
    } else if (anim_kind == 4) {
        minimized = false;
    }
    anim_kind = 0;
    redraw_desktop();
    if (!minimized)
        vga_hold_display(false);
}

static void anim_step(void)
{
    u32 el = timer_uptime_ms() - anim_t0;
    int t = (int)(el * 100u / ANIM_MS);
    if (t > 100) t = 100;
    if (t == anim_last_t)
        return;
    anim_last_t = t;
    WX = ilerp(ax0, ax1, t);
    WY = ilerp(ay0, ay1, t);
    WW = ilerp(aw0, aw1, t);
    WH = ilerp(ah0, ah1, t);
    redraw_desktop();
    if (t >= 100) {
        anim_active = false;
        anim_finish();
    }
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

    BT_W = TITLE_H - 8;
    BT_H = BT_W;
    BT_X = WX + WW - BT_W - 8;
    BT_Y = WY + (TITLE_H - BT_H) / 2;

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
    MX_X = BT_X - MX_W - 4;
    MX_Y = BT_Y;
    MINB_W = BT_W;
    MINB_H = BT_H;
    MINB_X = MX_X - MINB_W - 4;
    MINB_Y = BT_Y;

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
    if (!icon_pos_ready) {
        for (int i = 0; i < 3; i++) {
            icon_px[i] = ICON_X;
            icon_py[i] = ICON_Y + i * ICON_STEP;
        }
        icon_pos_ready = true;
    }

    /* 属性对话框 */
    PR_W = 44 * cw;
    if (PR_W > gw - 40) PR_W = gw - 40;
    PR_H = 13 * ch;
    if (PR_H > gh - 40) PR_H = gh - 40;
    PR_X = (gw - PR_W) / 2;
    PR_Y = (gh - PR_H) / 2;
    PR_OK_W = 6 * cw;
    PR_OK_H = ch + 8;
    PR_OK_X = PR_X + PR_W - PR_OK_W - 12;
    PR_OK_Y = PR_Y + PR_H - PR_OK_H - 12;

    /* FILES 应用窗口 */
    FL_W = 64 * cw;
    if (FL_W > gw - 40) FL_W = gw - 40;
    FL_H = 18 * ch;
    if (FL_H > gh - 40) FL_H = gh - 40;
    FL_X = (gw - FL_W) / 2;
    FL_Y = (gh - FL_H) / 2;
    FL_CL_W = TITLE_H - 8;
    FL_CL_H = FL_CL_W;
    FL_CL_X = FL_X + FL_W - FL_CL_W - 8;
    FL_CL_Y = FL_Y + (TITLE_H - FL_CL_H) / 2;
    FL_ROW_Y = FL_Y + TITLE_H + 4 + ch + 4;
    FL_ROW_H = ch + 4;
    FL_NROW = (FL_Y + FL_H - 10 - FL_ROW_Y) / FL_ROW_H;
    if (FL_NROW < 1) FL_NROW = 1;
    if (FL_NROW > 12) FL_NROW = 12;

    /* SYS 应用窗口 */
    SY_W = 44 * cw;
    if (SY_W > gw - 40) SY_W = gw - 40;
    SY_H = 15 * ch;
    if (SY_H > gh - 40) SY_H = gh - 40;
    SY_X = (gw - SY_W) / 2;
    SY_Y = (gh - SY_H) / 2;
    SY_CL_W = TITLE_H - 8;
    SY_CL_H = SY_CL_W;
    SY_CL_X = SY_X + SY_W - SY_CL_W - 8;
    SY_CL_Y = SY_Y + (TITLE_H - SY_CL_H) / 2;
    SY_ROW_Y = SY_Y + TITLE_H + 10;
}

static void draw_desktop(void)
{
    int ch = gfx_cell_h();
    int gw = gfx_w(), gh = gfx_h();

    layout();
    draw_wallpaper();

    if (!minimized) {
        /* 窗口阴影/边框/标题栏 (Linux 风格: 居中标题 + 右侧图标按钮) */
        gfx_fill(WX + 10, WY + 10, WW, WH, 0x080c14);
        gfx_fill(WX, WY, WW, WH, C_FRAME);
        gfx_bevel(WX, WY, WW, WH, C_FRAME_LT, C_FRAME_DK);
        gfx_fill(WX + 2, WY + 2, WW - 4, TITLE_H - 2, C_TITLE);
        gfx_fill(WX + 2, WY + 2, WW - 4, 2, C_TITLE2);
        int tw = gfx_text_w(L("gui.terminal"));
        int tx = WX + (WW - tw) / 2;
        if (!ico_draw_file_at("TERM.ICO", tx - ch - 4,
                              WY + (TITLE_H - ch) / 2, ch, ch))
            draw_app_glyph(tx - ch - 4, WY + (TITLE_H - ch) / 2, ch);
        gfx_text(tx, WY + (TITLE_H - ch) / 2, L("gui.terminal"), C_TEXT);

        draw_win_button(MINB_X, MINB_Y, MINB_W, MINB_H, 0, 0x3a3f46);
        draw_win_button(MX_X, MX_Y, MX_W, MX_H, maximized ? 2 : 1, 0x3a3f46);
        draw_win_button(BT_X, BT_Y, BT_W, BT_H, 3, 0x5a2a2e);
        gfx_fill(WX + 2, WY + TITLE_H - 1, WW - 4, 1, C_ACCENT);

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
    }

    /* 面板 (Linux 风格底部栏): 深色 + 顶部强调线 */
    int ty0 = gh - TASK_H;
    gfx_fill(0, ty0, gw, TASK_H, C_TASK);
    gfx_fill(0, ty0, gw, TASK_H / 2, 0x23262e);
    gfx_fill(0, ty0, gw, 2, C_TASK_LT);

    /* 开始按钮: 应用网格图标 + Start */
    gfx_fill(MB_X, MB_Y + 3, MB_W, MB_H - 6, 0x2b3038);
    gfx_bevel(MB_X, MB_Y + 3, MB_W, MB_H - 6, 0x4a5058, 0x101418);
    {
        int isz = TASK_H - 8;
        if (!ico_draw_file_at("START.ICO", MB_X + 5, MB_Y + 4, isz, isz)) {
            int sq = ch / 4;
            if (sq < 3) sq = 3;
            int gap = 2;
            int lx = MB_X + 8;
            int ly = MB_Y + (TASK_H - (sq * 2 + gap)) / 2;
            u32 lc[4] = { 0x3daee9, 0x7fe0a0, 0xffc060, 0xc090ff };
            gfx_fill(lx, ly, sq, sq, lc[0]);
            gfx_fill(lx + sq + gap, ly, sq, sq, lc[1]);
            gfx_fill(lx, ly + sq + gap, sq, sq, lc[2]);
            gfx_fill(lx + sq + gap, ly + sq + gap, sq, sq, lc[3]);
        }
        gfx_text(MB_X + isz + 10, MB_Y + (TASK_H - ch) / 2,
                 L("gui.start"), C_TEXT);
    }

    /* 面板窗口按钮 (活动窗口; 点击最小化/还原) */
    gfx_fill(TK_X, TK_Y + 3, TK_W, TK_H - 6,
             minimized ? 0x2b3038 : 0x3a3f46);
    gfx_bevel(TK_X, TK_Y + 3, TK_W, TK_H - 6, 0x4a5058, 0x101418);
    if (!minimized)
        gfx_fill(TK_X, TK_Y + TASK_H - 4, TK_W, 2, C_ACCENT);
    {
        int isz = ch;
        if (isz > TK_H - 6) isz = TK_H - 6;
        if (!ico_draw_file_at("TERM.ICO", TK_X + 6,
                              TK_Y + (TASK_H - isz) / 2, isz, isz))
            draw_app_glyph(TK_X + 6, TK_Y + (TASK_H - isz) / 2, isz);
        gfx_text(TK_X + 10 + isz, TK_Y + (TASK_H - ch) / 2,
                 L("gui.menu.terminal"), minimized ? 0x9fb3cc : C_TEXT);
    }

    /* 时钟分隔线 + TEXT 按钮 */
    gfx_fill(CK_X - 8, ty0 + 4, 1, TASK_H - 8, 0x4a5058);
    draw_button(EX_X, EX_Y, EX_W, EX_H, L("gui.text"), 0x2b3038);
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
    in_redraw = true;
    vga_defer_flip(true);                 /* 避免中途局部提交 -> 闪烁 */
    draw_desktop();
    if (!minimized)
        vga_gui_viewport(CL_X, CL_Y, CL_W, CL_H);
    draw_overlays();
    vga_defer_flip(false);
    in_redraw = false;
    gfx_flip();
    gfx_poked = false;
    cursor_show(mouse_x(), mouse_y());
}

/* --------------------------------------------------------------- 接口 */
bool gui_active(void) { return active; }

void gui_redraw(void)
{
    if (active && !app_canvas)
        redraw_desktop();
}

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
    ctx_open = false;
    prop_open = false;
    files_open = false;
    sys_open = false;
    icon_pos_ready = false;
    icon_drag = false;
    icon_moved = false;
    icon_last_click = -1;
    CTX_HOT = -1;
    maximized = false;
    theme = 0;
    icon_sel = -1;
    menu_hot = -1;
    slider_drag = false;
    resizing = false;
    app_canvas = false;
    minimized = false;
    anim_active = false;
    anim_kind = 0;
    vga_hold_display(false);
    list_sel = 0;
    wdg_progress = 0;
    show_icons = true;
    show_clock = true;
    clock24 = true;
    bright = 100;
    gfx_poked = false;
    {
        static const char *icos[7] = {
            "TERM.ICO", "FILES.ICO", "SYS.ICO", "SETTINGS.ICO",
            "ABOUT.ICO", "WIDGETS.ICO", "START.ICO"
        };
        for (int i = 0; i < 7; i++)
            ico_preload(icos[i]);
    }
    in_redraw = true;
    vga_defer_flip(true);
    draw_desktop();
    vga_gui_viewport(CL_X, CL_Y, CL_W, CL_H);
    vga_defer_flip(false);
    in_redraw = false;
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
    ctx_open = false;
    prop_open = false;
    files_open = false;
    sys_open = false;
    icon_drag = false;
    icon_moved = false;
    maximized = false;
    menu_hot = -1;
    CTX_HOT = -1;
    slider_drag = false;
    resizing = false;
    app_canvas = false;
    minimized = false;
    anim_active = false;
    anim_kind = 0;
    /* 顺序很关键: 先恢复指针, 再静默解除暂停, 最后只做一次全屏文本提交.
       (此前先 vga_hold_display(false) 会在视口内多刷一帧, 再恢复桌面矩形,
        造成 GUI→CLI 可见闪烁) */
    if (cur_x >= 0)
        gfx_fb_restore(cur_x - 1, cur_y - 1, cs_w + 2, cs_h + 2);
    cur_x = cur_y = -1;
    vga_hold_display_quiet(false);
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
    ctx_open = false;
    prop_open = false;
    files_open = false;
    sys_open = false;
    icon_drag = false;
    menu_hot = -1;
    CTX_HOT = -1;
    app_canvas = true;
    vga_hold_display(true);          /* 程序 printf 不再画到画布上 */
    return true;
}

void gui_canvas_end(void)
{
    if (!app_canvas)
        return;
    app_canvas = false;
    vga_hold_display_quiet(false);   /* 只解除暂停; 随后由桌面重绘一次性提交 */
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
        if (inside(x, y, icon_px[i], icon_py[i], ICON_W, ICON_H + ch))
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
    if (anim_active) {              /* 动画进行中: 只推进动画 */
        anim_step();
        return;
    }

    int btn = mouse_buttons();
    int x = mouse_x(), y = mouse_y();
    bool left = (btn & 1) != 0;
    bool right = (btn & 2) != 0;
    bool pressed = left && !(last_btn & 1);
    bool rpressed = right && !(last_btn & 2);
    last_btn = btn;

    /* 右键: 任意右键先关闭现有浮层, 否则打开上下文菜单 */
    if (rpressed) {
        if (ctx_open) {
            ctx_show(x, y, icon_hit(x, y));      /* 再次右键: 重新定位 */
        } else if (prop_open || files_open || sys_open || about_open ||
                   settings_open || widgets_open || menu_open) {
            prop_open = files_open = sys_open = about_open = settings_open =
                widgets_open = menu_open = false;
            menu_hot = -1;
        } else {
            ctx_show(x, y, icon_hit(x, y));
        }
        redraw_desktop();
        return;
    }

    if (pressed) {
        if (ctx_open) {
            int it = ctx_pick(x, y);
            int tgt = ctx_target;
            ctx_open = false;
            CTX_HOT = -1;
            if (it >= 0) {
                ctx_activate(tgt, it);
                return;
            }
            redraw_desktop();
        } else if (prop_open) {
            if (inside(x, y, PR_OK_X, PR_OK_Y, PR_OK_W, PR_OK_H) ||
                !inside(x, y, PR_X, PR_Y, PR_W, PR_H)) {
                prop_open = false;
                redraw_desktop();
            }
        } else if (files_open) {
            int row = -1;
            for (int i = 0; i < files_n && i < FL_NROW; i++)
                if (inside(x, y, FL_X + 6, FL_ROW_Y + i * FL_ROW_H,
                           FL_W - 12, FL_ROW_H))
                    row = i;
            if (!inside(x, y, FL_X, FL_Y, FL_W, FL_H)) {
                files_open = false;
                redraw_desktop();
            } else if (inside(x, y, FL_CL_X, FL_CL_Y, FL_CL_W, FL_CL_H)) {
                files_open = false;
                redraw_desktop();
            } else if (row >= 0) {
                files_sel = row;
                redraw_desktop();
            }
        } else if (sys_open) {
            if (inside(x, y, SY_CL_X, SY_CL_Y, SY_CL_W, SY_CL_H) ||
                !inside(x, y, SY_X, SY_Y, SY_W, SY_H)) {
                sys_open = false;
                redraw_desktop();
            }
        } else if (about_open) {
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
        } else if (inside(x, y, EX_X, EX_Y, EX_W, EX_H)) {
            gui_leave();                          /* TEXT: 返回纯文本 */
            return;
        } else if (!minimized && inside(x, y, BT_X, BT_Y, BT_W, BT_H)) {
            gui_leave();                          /* 窗口关闭按钮 */
            return;
        } else if (inside(x, y, TK_X, TK_Y, TK_W, TK_H)) {
            if (minimized)
                start_unminimize();               /* 面板按钮: 还原 */
            else
                toggle_max();                     /* 或最大化 */
        } else if (!minimized && inside(x, y, MINB_X, MINB_Y, MINB_W, MINB_H)) {
            start_minimize();                     /* 最小化 */
        } else if (!minimized && inside(x, y, MX_X, MX_Y, MX_W, MX_H)) {
            toggle_max();                         /* 最大化 / 还原 */
        } else if (icon_hit(x, y) >= 0) {
            int ih = icon_hit(x, y);
            icon_was_sel = (icon_sel == ih);       /* 再次单击 = 打开 */
            icon_sel = ih;
            icon_drag = true;                     /* 进入拖动/点击判定 */
            icon_drag_idx = ih;
            icon_press_x = x;
            icon_press_y = y;
            icon_dx = x - icon_px[ih];
            icon_dy = y - icon_py[ih];
            icon_moved = false;
            redraw_desktop();
        } else if (!minimized && !maximized &&
                   inside(x, y, GX, GY, GWH, GWH)) {
            resizing = true;                      /* 右下角缩放握把 */
            resize_base_w = WW;
            resize_base_h = WH;
            resize_px = x;
            resize_py = y;
        } else if (!minimized && inside(x, y, WX, WY, WW, TITLE_H)) {
            dragging = true;
            drag_dx = x - WX;
            drag_dy = y - WY;
        }
    }
    if (!left) {
        if (icon_drag) {
            int idx = icon_drag_idx;
            bool moved = icon_moved;
            bool reopen = icon_was_sel;
            icon_drag = false;
            icon_moved = false;
            if (moved) {
                /* 拖入终端客户区 = 打开该应用 (drop) */
                if (inside(x, y, CL_X, CL_Y, CL_W, CL_H)) {
                    static const char *dkeys[3] = {
                        "gui.term", "gui.files", "gui.sys"
                    };
                    kprintf("[gui] drop %s\n", L(dkeys[idx]));
                    open_app(idx);
                    return;
                }
                redraw_desktop();
            } else {
                u32 now = timer_uptime_ms();
                bool dbl = icon_last_click == idx &&
                           (now - icon_last_t) <= 600u;
                icon_last_click = idx;
                icon_last_t = now;
                if (dbl || reopen) {
                    icon_last_click = -1;
                    icon_was_sel = false;
                    open_app(idx);
                    return;
                }
                redraw_desktop();
            }
        }
        dragging = false;
        slider_drag = false;
        resizing = false;
    }

    /* 图标拖动: 实时移动位置 (夹在桌面区域内) */
    if (icon_drag && left) {
        int idx = icon_drag_idx;
        int nx = x - icon_dx;
        int ny = y - icon_dy;
        int maxx = gfx_w() - ICON_W;
        int maxy = gfx_h() - TASK_H - ICON_H;
        if (maxx < 0) maxx = 0;
        if (maxy < 0) maxy = 0;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > maxx) nx = maxx;
        if (ny > maxy) ny = maxy;
        if (x - icon_press_x > 4 || icon_press_x - x > 4 ||
            y - icon_press_y > 4 || icon_press_y - y > 4)
            icon_moved = true;
        if (nx != icon_px[idx] || ny != icon_py[idx]) {
            icon_px[idx] = nx;
            icon_py[idx] = ny;
            redraw_desktop();
            return;
        }
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

    /* 右键菜单悬停高亮 */
    if (ctx_open && !pressed) {
        int h = ctx_pick(x, y);
        if (h != CTX_HOT) {
            CTX_HOT = h;
            draw_ctx_menu();
            gfx_flip_rect(CTX_X - 2, CTX_Y - 2, CTX_W + 4, CTX_H + 4);
            if (cur_x >= 0)
                cursor_show(cur_x, cur_y);
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
