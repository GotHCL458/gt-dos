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
