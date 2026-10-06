/* ==========================================================================
 *  GT-DOS 共享图形层 (Bochs VBE + 双缓冲 + 抗锯齿 + 多分辨率)
 *  ---------------------------------------------------------------------------
 *  - 分辨率运行时可选: 640x480 / 800x600 / 1024x768 / 1280x1024 (32bpp),
 *    线性帧缓冲地址由 PCI BAR0 探测.
 *  - 所有绘制先进后台缓冲, gfx_flip()/gfx_flip_rect() 提交到显存 -> 无闪烁.
 *  - 字模为 2bit 灰度 (mkfont.py 4x 超采样生成), 与前景色 alpha 混合 -> 圆滑.
 *  - 文本控制台与 GUI 桌面都建立在本层之上.
 * ========================================================================== */
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

#define BACK_BASE 0x800000u           /* 后台缓冲基址 8MB (字库占 1MB..2.4MB) */

static volatile u32 *fb;
static u32 *back __attribute__((section(".highbss")));   /* 1.2MB 后台缓冲 */
static bool gfx_ok;
static int gw = 640, gh = 480;

/* 支持的分辨率表 (宽, 高) */
const struct gfx_mode gfx_modes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 },
};
int gfx_mode_count(void) { return (int)(sizeof(gfx_modes) / sizeof(gfx_modes[0])); }
void gfx_mode_size(int idx, int *w, int *h)
{
    if (idx < 0 || idx >= gfx_mode_count()) { *w = *h = 0; return; }
    *w = gfx_modes[idx].w;
    *h = gfx_modes[idx].h;
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
        return lfb ? lfb : LFB_DEFAULT;
    }
    return 0;
}

bool gfx_available(void)
{
    u16 id = dispi_read(VBE_DISPI_INDEX_ID);
    return (id & 0xFFF0u) == 0xB0C0u;
}

static bool mode_apply(int w, int h)
{
    u32 lfb = pci_find_lfb();
    if (!lfb)
        lfb = LFB_DEFAULT;
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
    dispi_write(VBE_DISPI_INDEX_XRES, (u16)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (u16)h);
    dispi_write(VBE_DISPI_INDEX_BPP, 32);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    fb = (volatile u32 *)(u64)lfb;
    back = (u32 *)BACK_BASE;
    gw = w;
    gh = h;
    return true;
}

bool gfx_init(void)
{
    if (gfx_ok)
        return true;
    if (!gfx_available())
        return false;
    if (!mode_apply(640, 480))
        return false;
    gfx_ok = true;
    return true;
}

/* 切换分辨率 (启动后调用; 失败保持原分辨率) */
bool gfx_set_mode(int w, int h)
{
    if (!gfx_ok)
        return false;
    bool found = false;
    for (int i = 0; i < gfx_mode_count(); i++)
        if (gfx_modes[i].w == w && gfx_modes[i].h == h) { found = true; break; }
    if (!found)
        return false;
    mode_apply(w, h);
    return true;
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

void gfx_clear(u32 c)
{
    for (int i = 0; i < gw * gh; i++)
        back[i] = c;
}

void gfx_fill(int x, int y, int w, int h, u32 c)
{
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= (unsigned)gh) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if ((unsigned)xx < (unsigned)gw)
                back[yy * gw + xx] = c;
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

/* 画一个字符 (16x16 单元, 4bit 灰度字模): ASCII 占 8px, 宽字符占 16px */
int gfx_char(int x, int y, u32 cp, u32 fg)
{
    const u8 *g = vga_glyph(cp);
    int adv = (cp >= 32 && cp < 128) ? 8 : 16;
    if (!g) { gfx_frame(x, y, adv - 1, 16, fg); return adv; }
    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < adv; c++) {
            u8 byte = g[r * 8 + (c >> 1)];
            u8 lvl = (u8)((c & 1) ? (byte & 0x0F) : (byte >> 4));
            if (lvl)
                px_blend(x + c, y + r, lvl, fg);
        }
    }
    return adv;
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
            w += 16;
        } else {
            w += 8;
        }
    }
    return w;
}

/* 提交后台缓冲到显存 (一次整屏拷贝, 无撕裂/闪烁) */
void gfx_flip(void)
{
    if (!gfx_ok)
        return;
    volatile u32 *d = fb;
    const u32 *src = back;
    for (int i = 0; i < gw * gh; i++)
        d[i] = src[i];
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
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        volatile u32 *d = &fb[yy * gw + x];
        const u32 *s = &back[yy * gw + x];
        for (int i = 0; i < w; i++)
            d[i] = s[i];
    }
}
