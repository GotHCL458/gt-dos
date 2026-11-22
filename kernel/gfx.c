/* Bochs VBE 线性帧缓冲, LFB 地址与显存容量由 PCI BAR0 探测.
   模式自适配: 优先 1920x1080, 显存/模式不支持时回退 1024x768 -> 800x600
   -> 640x480. 字模按整数倍缩放, 使文本网格铺满整个屏幕.
   绘制写入后台缓冲, gfx_flip/gfx_flip_rect 提交显存.
   字模为 4bit 灰度, 与前景按 16 级 alpha 混合. 文本控制台建立在本层之上. */
#include "gt.h"

#define VBE_DISPI_INDEX_ID     0
#define VBE_DISPI_INDEX_XRES   1
#define VBE_DISPI_INDEX_YRES   2
#define VBE_DISPI_INDEX_BPP    3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40
#define VBE_DISPI_IOPORT_INDEX 0x1CE
#define VBE_DISPI_IOPORT_DATA  0x1CF
#define LFB_DEFAULT            0xE0000000u

#define BACK_BASE 0x800000u           /* 后台缓冲基址 8MB (字库占 1MB..3.6MB) */
#define BACK_MAX  (16u * 1024u * 1024u)  /* 8MB..24MB, 容纳 1920x1080x4 */

#define MODE_COLS_MAX 160             /* 文本网格最大列数 (hist 缓冲上限) */

static volatile u32 *fb;
static u32 *back __attribute__((section(".highbss")));   /* 后台缓冲 */
static bool gfx_ok;
static int gw = 640, gh = 480;
static int gs = 1;                    /* 字模整数倍缩放 */

/* 宿主可用显示区域提示 (build.py 就地写入, 0 = 不限制).
   窗口模式下若分辨率超过桌面可用区域, QEMU 窗口会被任务栏/屏幕边缘裁切,
   因此这里把候选模式限制在宿主可完整显示的大小内. */
volatile const u8 gfx_host_hint[12] __attribute__((used)) = {
    'G', 'T', 'M', 'H', 0, 0, 0, 0, 0, 0, 0, 0
};

static u32 hint_u32(int off)
{
    return (u32)gfx_host_hint[off] |
           ((u32)gfx_host_hint[off + 1] << 8) |
           ((u32)gfx_host_hint[off + 2] << 16) |
           ((u32)gfx_host_hint[off + 3] << 24);
}

static void dispi_write(u16 idx, u16 val)
{
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    outw(VBE_DISPI_IOPORT_DATA, val);
}
static u16 dispi_read(u16 idx)
{
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off)
{
    outl(0xCF8, (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11) |
                ((u32)fn << 8) | (off & 0xFCu));
    return inl(0xCFC);
}
static void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 val)
{
    outl(0xCF8, (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11) |
                ((u32)fn << 8) | (off & 0xFCu));
    outl(0xCFC, val);
}

static u32 lfb_size;                   /* BAR0 容量 (字节), 0=未知 */

/* 通过写全 1 再读回掩码探测 BAR 容量, 之后恢复原值 */
static u32 pci_bar_size(u8 dev, u8 off)
{
    u32 orig = pci_read(0, dev, 0, off);
    pci_write(0, dev, 0, off, 0xFFFFFFF0u);
    u32 mask = pci_read(0, dev, 0, off);
    pci_write(0, dev, 0, off, orig);
    mask &= 0xFFFFFFF0u;
    if (!mask)
        return 0;
    return (~mask) + 1u;
}

static u32 pci_find_lfb(void)
{
    for (int dev = 0; dev < 32; dev++) {
        u32 vend = pci_read(0, (u8)dev, 0, 0x00);
        if ((vend & 0xFFFF) == 0xFFFFu || vend == 0)
            continue;
        u32 cls = pci_read(0, (u8)dev, 0, 0x08);
        if ((cls >> 24) != 0x03)
            continue;
        u32 bar = pci_read(0, (u8)dev, 0, 0x10);
        u32 lfb = bar & 0xFFFFFFF0u;
        u32 cmd = pci_read(0, (u8)dev, 0, 0x04);
        pci_write(0, (u8)dev, 0, 0x04, cmd | 0x06);
        lfb_size = pci_bar_size((u8)dev, 0x10);
        return lfb ? lfb : LFB_DEFAULT;
    }
    return 0;
}

bool gfx_available(void)
{
    u16 id = dispi_read(VBE_DISPI_INDEX_ID);
    return (id & 0xFFF0u) == 0xB0C0u;
}

/* 选字模缩放: 列数不超过 MODE_COLS_MAX, 行数至少 24, 尽量铺满屏幕 */
static int scale_for(int w, int h)
{
    for (int s = 1; s <= 8; s++) {
        int cols = w / (8 * s);
        int rows = h / (16 * s);
        if (cols > MODE_COLS_MAX || rows - 1 < 24)
            continue;
        int nc = w / (8 * (s + 1));
        int nr = h / (16 * (s + 1));
        if (nc >= 80 && nr - 1 >= 25)
            continue;                  /* 还能再放大一级 */
        return s;
    }
    return 1;
}

static bool mode_apply(int w, int h, int scale)
{
    u32 need = (u32)w * (u32)h * 4u;
    if (need > BACK_MAX)
        return false;
    if (lfb_size && need > lfb_size) {
        kprintf("[gfx] %dx%d needs %u KB, vram %u KB\n",
                w, h, need / 1024u, lfb_size / 1024u);
        return false;                  /* 显存不足, 交给上层回退 */
    }

    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
    dispi_write(VBE_DISPI_INDEX_XRES, (u16)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (u16)h);
    dispi_write(VBE_DISPI_INDEX_BPP, 32);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* 读回校验: 模式不被硬件接受时立即回退 */
    if (dispi_read(VBE_DISPI_INDEX_XRES) != (u16)w ||
        dispi_read(VBE_DISPI_INDEX_YRES) != (u16)h ||
        dispi_read(VBE_DISPI_INDEX_BPP) != 32) {
        dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
        return false;
    }

    fb = (volatile u32 *)(u64)pci_find_lfb();
    if (!fb)
        fb = (volatile u32 *)(u64)LFB_DEFAULT;
    back = (u32 *)BACK_BASE;
    gw = w;
    gh = h;
    gs = scale;
    return true;
}

bool gfx_init(void)
{
    if (gfx_ok)
        return true;
    if (!gfx_available())
        return false;
    /* 分辨率自适配: 优先 1920x1080, 失败回退 1024x768, 再退 800x600/640x480.
       窗口模式下再按宿主可用区域 (build.py 写入) 过滤, 避免窗口被裁切. */
    static const u16 modes[][2] = {
        { 1920, 1080 }, { 1024, 768 }, { 800, 600 }, { 640, 480 }
    };
    u32 max_w = hint_u32(4), max_h = hint_u32(8);
    for (int i = 0; i < (int)(sizeof(modes) / sizeof(modes[0])); i++) {
        if (max_w && (u32)modes[i][0] > max_w)
            continue;
        if (max_h && (u32)modes[i][1] > max_h)
            continue;
        if (mode_apply(modes[i][0], modes[i][1],
                       scale_for(modes[i][0], modes[i][1]))) {
            gfx_ok = true;
            return true;
        }
    }
    return false;
}

void gfx_shutdown(void)
{
    if (!gfx_ok)
        return;
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
    gfx_ok = false;
}

bool gfx_active(void) { return gfx_ok; }
int  gfx_w(void) { return gw; }
int  gfx_h(void) { return gh; }
int  gfx_scale(void) { return gs; }
int  gfx_cell_w(void) { return 8 * gs; }
int  gfx_cell_h(void) { return 16 * gs; }

void gfx_clear(u32 c)
{
    u64 v = ((u64)c << 32) | c;
    u64 *p = (u64 *)back;
    int n = gw * gh / 2;
    for (int i = 0; i < n; i++)
        p[i] = v;
}

void gfx_fill(int x, int y, int w, int h, u32 c)
{
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= (unsigned)gh) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = x + w;
        if (x1 > gw) x1 = gw;
        if (x0 >= x1) continue;
        u32 *row = &back[yy * gw];
        if (x0 == 0 && x1 == gw) {             /* 整行: 按 64 位写加速 */
            u64 v = ((u64)c << 32) | c;
            u64 *q = (u64 *)row;
            int half = gw / 2;
            for (int i = 0; i < half; i++)
                q[i] = v;
            if (gw & 1)
                row[gw - 1] = c;
        } else {
            for (int i = x0; i < x1; i++)
                row[i] = c;
        }
    }
}

void gfx_frame(int x, int y, int w, int h, u32 c)
{
    for (int i = 0; i < w; i++) {
        if ((unsigned)(x + i) < (unsigned)gw) {
            if ((unsigned)y < (unsigned)gh)             back[y * gw + x + i] = c;
            if ((unsigned)(y + h - 1) < (unsigned)gh)   back[(y + h - 1) * gw + x + i] = c;
        }
    }
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= (unsigned)gh) continue;
        if ((unsigned)x < (unsigned)gw)                 back[yy * gw + x] = c;
        if ((unsigned)(x + w - 1) < (unsigned)gw)       back[yy * gw + x + w - 1] = c;
    }
}

void gfx_bevel(int x, int y, int w, int h, u32 light, u32 dark)
{
    for (int i = 0; i < w - 1; i++) {
        if ((unsigned)(x + i) < (unsigned)gw) {
            if ((unsigned)y < (unsigned)gh)             back[y * gw + x + i] = light;
            if ((unsigned)(y + h - 1) < (unsigned)gh)   back[(y + h - 1) * gw + x + i] = dark;
        }
    }
    for (int j = 0; j < h - 1; j++) {
        int yy = y + j;
        if ((unsigned)yy >= (unsigned)gh) continue;
        if ((unsigned)x < (unsigned)gw)                 back[yy * gw + x] = light;
        if ((unsigned)(x + w - 1) < (unsigned)gw)       back[yy * gw + x + w - 1] = dark;
    }
}

/* 进度条: 外框 + 已填充比例 (供内核控件和用户 GUI 库共用) */
void gfx_bar(int x, int y, int w, int h, int pct, u32 c)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_fill(x, y, w, h, 0x0e1622);
    gfx_bevel(x, y, w, h, 0xbfd0e8, 0x101820);
    int iw = w - 4;
    if (iw < 0) iw = 0;
    gfx_fill(x + 2, y + 2, iw * pct / 100, h - 4, c);
}

/* 4bit 灰度 alpha 混合画一个像素: lvl 0..15 -> 0..100% */
static inline void px_blend(int x, int y, u8 lvl, u32 fg)
{
    if ((unsigned)x >= (unsigned)gw || (unsigned)y >= (unsigned)gh)
        return;
    u32 *p = &back[y * gw + x];
    if (lvl >= 15) { *p = fg; return; }
    u32 bg = *p;
    u32 a = lvl, ia = 15 - lvl;
    u32 r = (((bg >> 16) & 0xFF) * ia + ((fg >> 16) & 0xFF) * a) / 15;
    u32 g = (((bg >> 8) & 0xFF) * ia + ((fg >> 8) & 0xFF) * a) / 15;
    u32 b = ((bg & 0xFF) * ia + (fg & 0xFF) * a) / 15;
    *p = (r << 16) | (g << 8) | b;
}

/* 画一个字符 (16x16 单元, 4bit 灰度字模): ASCII 占 8px, 宽字符占 16px.
   单元再按整数倍 gs 放大, 返回值同样放大. */
int gfx_char(int x, int y, u32 cp, u32 fg)
{
    const u8 *g = vga_glyph(cp);
    int adv = (cp >= 32 && cp < 128) ? 8 : 16;
    int s = gs;
    if (!g) { gfx_frame(x, y, adv * s - 1, 16 * s, fg); return adv * s; }
    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < adv; c++) {
            u8 byte = g[r * 8 + (c >> 1)];
            u8 lvl = (u8)((c & 1) ? (byte & 0x0F) : (byte >> 4));
            if (!lvl)
                continue;
            if (s == 1) {
                px_blend(x + c, y + r, lvl, fg);
            } else {
                for (int dy = 0; dy < s; dy++)
                    for (int dx = 0; dx < s; dx++)
                        px_blend(x + c * s + dx, y + r * s + dy, lvl, fg);
            }
        }
    }
    return adv * s;
}

int gfx_text(int x, int y, const char *s, u32 fg)
{
    int w = 0;
    while (*s) {
        u8 b = (u8)*s++;
        u32 cp = b;
        if (b >= 0xC0) {
            int need = (b & 0xF0) == 0xE0 ? 2 : ((b & 0xF8) == 0xF0 ? 3 : 1);
            cp = b & (0x3F >> need);
            for (int i = 0; i < need && *s; i++)
                cp = (cp << 6) | ((u8)*s++ & 0x3F);
        }
        w += gfx_char(x + w, y, cp, fg);
    }
    return w;
}

int gfx_text_w(const char *s)
{
    int w = 0;
    while (*s) {
        u8 b = (u8)*s++;
        if (b >= 0xC0) {
            int need = (b & 0xF0) == 0xE0 ? 2 : ((b & 0xF8) == 0xF0 ? 3 : 1);
            for (int i = 0; i < need && *s; i++) s++;
            w += 16 * gs;
        } else {
            w += 8 * gs;
        }
    }
    return w;
}

/* GUI 光标提示: 任一提交后置位, 以便重画鼠标指针 */
volatile bool gfx_poked;

/* 提交后台缓冲到显存 (一次整屏拷贝, 无撕裂/闪烁) */
void gfx_flip(void)
{
    if (!gfx_ok)
        return;
    volatile u64 *d = (volatile u64 *)fb;
    const u64 *src = (const u64 *)back;
    int n = gw * gh / 2;
    for (int i = 0; i < n; i++)
        d[i] = src[i];
    gfx_poked = true;
    gui_cursor_refresh();              /* GUI: 防止指针被整屏提交覆盖 */
}

/* 只提交一个矩形区域 (局部刷新, 用于单字符输出) */
void gfx_flip_rect(int x, int y, int w, int h)
{
    if (!gfx_ok)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gw) w = gw - x;
    if (y + h > gh) h = gh - y;
    int x2 = x + (w & ~1);
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        volatile u64 *d = (volatile u64 *)&fb[yy * gw + x];
        const u64 *s = (const u64 *)&back[yy * gw + x];
        int n = (x2 - x) / 2;
        for (int i = 0; i < n; i++)
            d[i] = s[i];
        for (int i = x2; i < x + w; i++)             /* 奇数尾像素 */
            fb[yy * gw + i] = back[yy * gw + i];
    }
    gfx_poked = true;
    gui_cursor_refresh();              /* GUI: 立即补画指针, 避免一帧闪烁 */
}

/* GUI 鼠标指针: 直接写显存 (不进入后台缓冲), 恢复时从后台重拷.
   这样控制台重绘不会破坏指针，指针也不会污染窗口内容. */
void gfx_fb_pixel(int x, int y, u32 c)
{
    if (!gfx_ok || (unsigned)x >= (unsigned)gw || (unsigned)y >= (unsigned)gh)
        return;
    fb[y * gw + x] = c;
}

void gfx_fb_restore(int x, int y, int w, int h)
{
    if (!gfx_ok)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gw) w = gw - x;
    if (y + h > gh) h = gh - y;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        for (int i = 0; i < w; i++)
            fb[yy * gw + x + i] = back[yy * gw + x + i];
    }
}
