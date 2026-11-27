/* Windows ICO 图标渲染.
 *
 * 解析 ICONDIR, 选取面积最大的条目 (同等面积优先更高色深). 条目可为:
 *   - 内嵌 PNG (0x89 "PNG"): 交给 png.c 解码;
 *   - BMP/DIB (BITMAPINFOHEADER): 支持 32/24/8/4/1 位, 底向上/顶向下,
 *     调色板, 以及 1bpp AND 透明掩码. 32 位条目若全图 alpha 均为 0 则回退
 *     到 AND 掩码 (兼容旧式图标).
 *
 * 结果按整数倍最近邻放大并在屏幕居中, 直接 alpha 混合到 gfx 后台缓冲.
 * 工作缓冲放在高内存 (26MB 起), 不占用内核 .bss/.highbss. */
#include "gt.h"

#define ICO_FILE_MAX (256 * 1024)
#define ICO_MEM_BUF  0x1A00000u

static u8 *const ico_buf = (u8 *)ICO_MEM_BUF;

static u32 ico_le16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 ico_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* 计算放大倍数与居中偏移 */
static void ico_fit(int iw, int ih, int *ps, int *pox, int *poy)
{
    int sw = gfx_w(), sh = gfx_h();
    int s = 1;
    if (iw > 0 && ih > 0) {
        int a = sw / iw, b = sh / ih;
        s = a < b ? a : b;
        if (s < 1) s = 1;
        if (s > 16) s = 16;
    }
    if (ps) *ps = s;
    if (pox) *pox = (sw - iw * s) / 2;
    if (poy) *poy = (sh - ih * s) / 2;
}

static void ico_block(int x, int y, int s, u32 rgb, u8 a)
{
    for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++)
            gfx_pixel_a(x + i, y + j, rgb, a);
}

/* BMP/DIB 条目 */
static bool ico_draw_dib(const u8 *b, u32 avail)
{
    if (avail < 40)
        return false;
    u32 hdr = ico_le32(b);
    if (hdr < 40 || hdr > avail)
        return false;
    int bw = (int)ico_le32(b + 4);
    int bh = (int)ico_le32(b + 8);
    int planes = (int)ico_le16(b + 12);
    int bpp = (int)ico_le16(b + 14);
    u32 comp = ico_le32(b + 16);
    if (bw <= 0 || bh == 0 || planes != 1 || comp != 0)
        return false;

    bool topdown = bh < 0;
    int h2 = topdown ? -bh : bh;
    int ih = h2 / 2;
    if (ih <= 0)
        ih = h2;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return false;

    u32 pal_n = 0;
    if (bpp <= 8) {
        pal_n = ico_le32(b + 32);
        if (pal_n == 0 || pal_n > 256)
            pal_n = 1u << bpp;
    }
    u32 pal_off = hdr;
    u32 pix_off = hdr + pal_n * 4;
    int rowbytes = ((bw * bpp + 31) / 32) * 4;
    int maskrow = ((bw + 31) / 32) * 4;
    u32 xor_bytes = (u32)rowbytes * (u32)ih;
    u32 mask_bytes = (u32)maskrow * (u32)ih;
    if (pix_off + xor_bytes + mask_bytes > avail)
        return false;

    const u8 *pal = b + pal_off;
    const u8 *pix = b + pix_off;
    const u8 *mask = pix + xor_bytes;

    /* 32 位: 是否存在真实的 alpha 通道 */
    bool use_alpha = false;
    if (bpp == 32) {
        for (u32 i = 0; i < xor_bytes; i += 4)
            if (pix[i + 3]) { use_alpha = true; break; }
    }

    int s, ox, oy;
    ico_fit(bw, ih, &s, &ox, &oy);

    for (int y = 0; y < ih; y++) {
        int sy = topdown ? y : (ih - 1 - y);
        const u8 *row = pix + (u32)sy * (u32)rowbytes;
        const u8 *mrow = mask + (u32)sy * (u32)maskrow;
        for (int x = 0; x < bw; x++) {
            u32 r = 0, g = 0, bl = 0;
            u8 a = 255;
            if (bpp == 32) {
                const u8 *p = row + x * 4;
                bl = p[0]; g = p[1]; r = p[2];
                a = use_alpha ? p[3] : 255;
            } else if (bpp == 24) {
                const u8 *p = row + x * 3;
                bl = p[0]; g = p[1]; r = p[2];
            } else {
                int idx;
                if (bpp == 8) {
                    idx = row[x];
                } else if (bpp == 4) {
                    u8 v = row[x >> 1];
                    idx = (x & 1) ? (v & 0x0F) : (v >> 4);
                } else {
                    u8 v = row[x >> 3];
                    idx = (v >> (7 - (x & 7))) & 1;
                }
                if ((u32)idx >= pal_n)
                    idx = 0;
                const u8 *pe = pal + idx * 4;
                bl = pe[0]; g = pe[1]; r = pe[2];
            }
            if ((bpp != 32 || !use_alpha) &&
                ((mrow[x >> 3] >> (7 - (x & 7))) & 1))
                a = 0;                          /* AND 掩码: 1=透明 */
            if (a == 0)
                continue;
            u32 rgb = (r << 16) | (g << 8) | bl;
            ico_block(ox + x * s, oy + y * s, s, rgb, a);
        }
    }
    return true;
}

bool ico_draw_file_fit(const char *path)
{
    if (!gfx_active())
        return false;
    struct fat_dirent de;
    if (!path || !fat_find(path, &de))
        return false;
    if (de.is_dir || de.size == 0 || de.size > ICO_FILE_MAX)
        return false;
    u32 len = 0;
    if (!fat_read_file(path, ico_buf, ICO_FILE_MAX, &len) || len < 6 + 16)
        return false;

    const u8 *d = ico_buf;
    if (ico_le16(d) != 0 || ico_le16(d + 2) != 1)
        return false;
    int count = (int)ico_le16(d + 4);
    if (count <= 0 || count > 256)
        return false;

    int best = -1;
    long bestscore = -1;
    for (int i = 0; i < count; i++) {
        const u8 *e = d + 6 + i * 16;
        int w = e[0] ? e[0] : 256;
        int h = e[1] ? e[1] : 256;
        int bc = (int)ico_le16(e + 6);
        long score = (long)w * h * 100L + bc;
        if (score > bestscore) {
            bestscore = score;
            best = i;
        }
    }
    if (best < 0)
        return false;
    const u8 *e = d + 6 + best * 16;
    u32 nbytes = ico_le32(e + 8);
    u32 off = ico_le32(e + 12);
    if (off > len || nbytes > len - off || nbytes < 8)
        return false;
    const u8 *img = d + off;

    /* 内嵌 PNG: 复用完整 PNG 解码 */
    if (img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G') {
        struct png_info pi;
        if (png_info(img, nbytes, &pi) != 0 || pi.w <= 0 || pi.h <= 0)
            return false;
        int s, ox, oy;
        ico_fit(pi.w, pi.h, &s, &ox, &oy);
        return png_draw_scaled(img, nbytes, ox, oy, s);
    }

    return ico_draw_dib(img, nbytes);
}

/* ------------------------------------- 小图标缓存 (桌面/面板/菜单用) */
#define ICON_MEM     0x1B00000u
#define ICON_SLOTS   8
#define ICON_MAXDIM  256
#define ICON_BYTES   (ICON_MAXDIM * ICON_MAXDIM * 4)

struct icon_slot {
    char path[24];
    u8  *rgba;
    int  w, h;
};

static struct icon_slot icon_slots[ICON_SLOTS];
static int icon_next;
static u8 *const icon_arena = (u8 *)ICON_MEM;

/* BMP/DIB -> RGBA (32/24/8/4/1 位), 顶向下输出到 dst */
static bool ico_bmp_rgba(const u8 *b, u32 avail, u8 *dst,
                         int capw, int caph, int *ow, int *oh)
{
    if (avail < 40)
        return false;
    u32 hdr = ico_le32(b);
    if (hdr < 40 || hdr > avail)
        return false;
    int bw = (int)ico_le32(b + 4);
    int bh = (int)ico_le32(b + 8);
    int bpp = (int)ico_le16(b + 14);
    u32 comp = ico_le32(b + 16);
    if (bw <= 0 || bh == 0 || comp != 0 || bw > capw)
        return false;
    bool topdown = bh < 0;
    int h2 = topdown ? -bh : bh;
    int ih = h2 / 2;
    if (ih <= 0)
        ih = h2;
    if (ih > caph)
        return false;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return false;

    u32 pal_n = 0;
    if (bpp <= 8) {
        pal_n = ico_le32(b + 32);
        if (pal_n == 0 || pal_n > 256)
            pal_n = 1u << bpp;
    }
    u32 pix_off = hdr + pal_n * 4;
    int rowbytes = ((bw * bpp + 31) / 32) * 4;
    int maskrow = ((bw + 31) / 32) * 4;
    u32 xor_bytes = (u32)rowbytes * (u32)ih;
    u32 mask_bytes = (u32)maskrow * (u32)ih;
    if (pix_off + xor_bytes + mask_bytes > avail)
        return false;
    const u8 *pal = b + hdr;
    const u8 *pix = b + pix_off;
    const u8 *mask = pix + xor_bytes;

    bool use_alpha = false;
    if (bpp == 32) {
        for (u32 i = 0; i < xor_bytes; i += 4)
            if (pix[i + 3]) { use_alpha = true; break; }
    }
    for (int y = 0; y < ih; y++) {
        int sy = topdown ? y : (ih - 1 - y);
        const u8 *row = pix + (u32)sy * (u32)rowbytes;
        const u8 *mrow = mask + (u32)sy * (u32)maskrow;
        for (int x = 0; x < bw; x++) {
            u32 r = 0, g = 0, bl = 0;
            u8 a = 255;
            if (bpp == 32) {
                const u8 *p = row + x * 4;
                bl = p[0]; g = p[1]; r = p[2];
                a = use_alpha ? p[3] : 255;
            } else if (bpp == 24) {
                const u8 *p = row + x * 3;
                bl = p[0]; g = p[1]; r = p[2];
            } else {
                int idx;
                if (bpp == 8)
                    idx = row[x];
                else if (bpp == 4) {
                    u8 v = row[x >> 1];
                    idx = (x & 1) ? (v & 0x0F) : (v >> 4);
                } else {
                    u8 v = row[x >> 3];
                    idx = (v >> (7 - (x & 7))) & 1;
                }
                if ((u32)idx >= pal_n)
                    idx = 0;
                const u8 *pe = pal + idx * 4;
                bl = pe[0]; g = pe[1]; r = pe[2];
            }
            if ((bpp != 32 || !use_alpha) &&
                ((mrow[x >> 3] >> (7 - (x & 7))) & 1))
                a = 0;
            u8 *o = dst + ((u32)y * bw + x) * 4;
            o[0] = (u8)r; o[1] = (u8)g; o[2] = (u8)bl; o[3] = a;
        }
    }
    *ow = bw;
    *oh = ih;
    return true;
}

static struct icon_slot *icon_find(const char *path)
{
    for (int i = 0; i < ICON_SLOTS; i++)
        if (icon_slots[i].w > 0 && !strcmp(icon_slots[i].path, path))
            return &icon_slots[i];
    return 0;
}

static struct icon_slot *icon_load(const char *path)
{
    struct fat_dirent de;
    if (!fat_find(path, &de) || de.is_dir || de.size == 0 ||
        de.size > ICO_FILE_MAX)
        return 0;
    u32 len = 0;
    if (!fat_read_file(path, ico_buf, ICO_FILE_MAX, &len) || len < 22)
        return 0;
    const u8 *d = ico_buf;
    if (ico_le16(d) != 0 || ico_le16(d + 2) != 1)
        return 0;
    int count = (int)ico_le16(d + 4);
    if (count <= 0 || count > 256)
        return 0;
    int best = -1;
    long bestscore = -1;
    for (int i = 0; i < count; i++) {
        const u8 *e = d + 6 + i * 16;
        int ew = e[0] ? e[0] : 256, eh = e[1] ? e[1] : 256;
        int bc = (int)ico_le16(e + 6);
        long score = (long)ew * eh * 100L + bc;
        if (score > bestscore) { bestscore = score; best = i; }
    }
    if (best < 0)
        return 0;
    const u8 *e = d + 6 + best * 16;
    u32 nbytes = ico_le32(e + 8);
    u32 off = ico_le32(e + 12);
    if (off > len || nbytes > len - off || nbytes < 40)
        return 0;
    const u8 *img = d + off;
    if (img[0] == 0x89 && img[1] == 'P')
        return 0;                              /* 内嵌 PNG: 走矢量/直接绘制 */

    int idx = icon_next;
    icon_next = (icon_next + 1) % ICON_SLOTS;
    struct icon_slot *s = &icon_slots[idx];
    s->rgba = icon_arena + (u32)idx * ICON_BYTES;
    int w = 0, h = 0;
    if (!ico_bmp_rgba(img, nbytes, s->rgba, ICON_MAXDIM, ICON_MAXDIM, &w, &h)) {
        s->w = s->h = 0;
        return 0;
    }
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->path[sizeof(s->path) - 1] = '\0';
    s->w = w;
    s->h = h;
    return s;
}

/* 把缓存的图标缩放绘制到 (x,y,w,h) 矩形内 (保持比例, 居中, 面积平均) */
bool ico_draw_file_at(const char *path, int x, int y, int w, int h)
{
    if (!gfx_active() || !path || w <= 0 || h <= 0)
        return false;
    struct icon_slot *s = icon_find(path);
    if (!s)
        s = icon_load(path);
    if (!s || s->w <= 0 || s->h <= 0)
        return false;
    int iw = s->w, ih = s->h;
    int dw, dh;
    if ((long)w * ih <= (long)h * iw) {
        dw = w;
        dh = (int)((long)ih * w / iw);
    } else {
        dh = h;
        dw = (int)((long)iw * h / ih);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = x + (w - dw) / 2;
    int oy = y + (h - dh) / 2;

    for (int dy = 0; dy < dh; dy++) {
        int sy0 = (int)((long)dy * ih / dh);
        int sy1 = (int)(((long)(dy + 1) * ih + dh - 1) / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > ih) sy1 = ih;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = (int)((long)dx * iw / dw);
            int sx1 = (int)(((long)(dx + 1) * iw + dw - 1) / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > iw) sx1 = iw;
            u32 sr = 0, sg = 0, sb = 0, sa = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const u8 *row = s->rgba + ((u32)sy * iw) * 4;
                for (int sx = sx0; sx < sx1; sx++) {
                    const u8 *p = row + sx * 4;
                    sr += p[0]; sg += p[1]; sb += p[2]; sa += p[3];
                    n++;
                }
            }
            if (!n || !sa)
                continue;
            u8 a = (u8)(sa / n);
            if (!a)
                continue;
            /* 按 alpha 加权求色 (避免透明像素拉黑) */
            u32 r = sr / n, g = sg / n, b = sb / n;
            gfx_pixel_a(ox + dx, oy + dy, (r << 16) | (g << 8) | b, a);
        }
    }
    return true;
}

/* 预加载图标到缓存 (避免在菜单/对话框绘制过程中读盘) */
bool ico_preload(const char *path)
{
    if (!path)
        return false;
    if (icon_find(path))
        return true;
    return icon_load(path) != 0;
}
