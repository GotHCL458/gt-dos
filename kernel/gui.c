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
#define C_WALL_TOP  0x101a30
#define C_WALL_BOT  0x1d3a55
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
static int last_btn;
static u32 pump_guard;
static u32 clock_sec = 0xFFFFFFFFu;

/* 经典箭头指针 (12x19, 每行低 12 位, 最高位为最左像素) */
static const u16 cur_shape[CUR_H] = {
    0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xFF8, 0xFFC, 0xFFE, 0xC00, 0xE00, 0x600, 0x700, 0x300,
    0x380, 0x180, 0x100
};

static int bit_at(int r, int c)
{
    if (r < 0 || r >= CUR_H || c < 0 || c >= CUR_W)
        return 0;
    return (cur_shape[r] >> (CUR_W - 1 - c)) & 1;
}

static void cursor_draw(int x, int y)
{
    for (int r = 0; r < CUR_H; r++) {
        for (int c = 0; c < CUR_W; c++) {
            if (!bit_at(r, c))
                continue;
            bool border = !bit_at(r, c - 1) || !bit_at(r, c + 1) ||
                          !bit_at(r - 1, c) || !bit_at(r + 1, c);
            gfx_fb_pixel(x + c, y + r, border ? 0xFFFFFF : 0x101010);
        }
    }
}

static void cursor_show(int x, int y)
{
    if (!gfx_active())
        return;
    if (cur_x >= 0)
        gfx_fb_restore(cur_x, cur_y, CUR_W, CUR_H);
    int maxx = gfx_w() - CUR_W - 1;
    int maxy = gfx_h() - CUR_H - 1;
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    cur_x = x;
    cur_y = y;
    cursor_draw(x, y);
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
    for (int y = 0; y < H; y += 4) {
        u32 c = lerp437(C_WALL_TOP, C_WALL_BOT, (y * 255) / (H > 1 ? H - 1 : 1));
        gfx_fill(0, y, gfx_w(), 4, c);
    }
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    int tw = gfx_text_w("GT-DOS Desktop");
    gfx_text((gfx_w() - tw) / 2, 2 * ch, "GT-DOS Desktop", 0xdfefff);
    gfx_text((gfx_w() - gfx_text_w("basic GUI / PS2 mouse")) / 2,
             3 * ch, "basic GUI / PS2 mouse", 0x8fb6d8);
    /* 左侧桌面图标 */
    const char *names[3] = { "TERM", "FILES", "SYS" };
    u32 cols[3] = { 0x2f6f9f, 0x3f8f5f, 0x8f6f2f };
    int ix = 2 * cw, iy = 5 * ch;
    for (int i = 0; i < 3; i++) {
        int y = iy + i * (5 * ch);
        gfx_fill(ix, y, 4 * cw, 3 * ch, cols[i]);
        gfx_bevel(ix, y, 4 * cw, 3 * ch, 0xffffff, 0x202020);
        int lw = gfx_text_w(names[i]);
        gfx_text(ix + (4 * cw - lw) / 2, y + 3 * ch + 2, names[i], 0xd8e8ff);
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

static void draw_desktop(void)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    int gw = gfx_w(), gh = gfx_h();

    TASK_H = ch + 10;
    TITLE_H = ch + 8;
    WW = gw * 3 / 4;
    WH = gh * 3 / 4;
    WX = (gw - WW) / 2;
    WY = (gh - TASK_H - WH) / 2;
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

    draw_wallpaper();

    /* 窗口阴影/边框/标题栏 */
    gfx_fill(WX + 10, WY + 10, WW, WH, 0x080c14);
    gfx_fill(WX, WY, WW, WH, C_FRAME);
    gfx_bevel(WX, WY, WW, WH, C_FRAME_LT, C_FRAME_DK);
    gfx_fill(WX + 2, WY + 2, WW - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(WX + 2, WY + 2, WW - 4, (TITLE_H - 2) / 2, C_TITLE2);
    gfx_text(WX + 10, WY + (TITLE_H - ch) / 2, "GT-DOS Terminal", C_TEXT);
    draw_button(BT_X, BT_Y, BT_W, BT_H, "x", 0x8a2f2f);

    /* 客户区 (控制台视口) */
    gfx_fill(CL_X - 2, CL_Y - 2, CL_W + 4, CL_H + 4, C_CLIENT);

    /* 任务栏 */
    gfx_fill(0, gh - TASK_H, gw, TASK_H, C_TASK);
    gfx_fill(0, gh - TASK_H, gw, 2, C_TASK_LT);
    gfx_text(10, gh - TASK_H + (TASK_H - ch) / 2, "GT-DOS", C_ACCENT);
    char info[64];
    int n = 0;
    const char *s = "GUI desktop";
    while (*s && n < 60) info[n++] = *s++;
    info[n] = 0;
    gfx_text(10 + gfx_text_w("GT-DOS") + 3 * cw,
             gh - TASK_H + (TASK_H - ch) / 2, info, 0x8fa8c8);
    draw_button(EX_X, EX_Y, EX_W, EX_H, "TEXT", 0x304050);
    clock_sec = 0xFFFFFFFFu;
    draw_clock();
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
    if (cur_x >= 0)
        gfx_fb_restore(cur_x, cur_y, CUR_W, CUR_H);
    cur_x = cur_y = -1;
    vga_gui_leave();
    kprintf("[gui] text mode\n");
}

static bool inside(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void gui_pump(void)
{
    if (!active)
        return;

    int btn = mouse_buttons();
    if ((btn & 1) && !(last_btn & 1)) {
        int x = mouse_x(), y = mouse_y();
        if (inside(x, y, BT_X, BT_Y, BT_W, BT_H) ||
            inside(x, y, EX_X, EX_Y, EX_W, EX_H)) {
            gui_leave();
            return;
        }
    }
    last_btn = btn;

    int x = mouse_x(), y = mouse_y();
    if (x != cur_x || y != cur_y || gfx_poked) {
        gfx_poked = false;
        cursor_show(x, y);
    }

    if (++pump_guard >= 512) {
        pump_guard = 0;
        draw_clock();
    }
}
