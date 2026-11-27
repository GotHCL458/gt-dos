/* 完整 PNG 解码/渲染.
 *
 * 支持: 8/16 位及 1/2/4 位灰度/调色板; 颜色类型 0/2/3/4/6; tRNS 透明;
 *       5 种行过滤 (None/Sub/Up/Average/Paeth); Adam7 隔行; zlib inflate
 *       (存储块/固定 Huffman/动态 Huffman).
 *
 * 解码采用流式: 逐行解压 -> 反过滤 -> 逐像素按 alpha 混合写入 gfx 后台缓冲,
 * 不需要整幅 RGBA 输出缓冲; 仅保留 32KB 滑动窗口与两行原始像素.
 * 渲染后由调用方执行 gfx_flip() 提交显存. */
#include "gt.h"

#define PNG_ROW_MAX  4096                    /* 单行最大像素数 */
#define PNG_ROW_BYTES (PNG_ROW_MAX * 8)      /* 单行最大原始字节 (16bit RGBA) */
#define PNG_LINE_MAX  (PNG_ROW_BYTES + 8)    /* 含过滤字节 */
#define PNG_FILE_MAX  (512 * 1024)           /* FAT 读入上限 */
#define ZWIN_SIZE     32768                  /* DEFLATE 滑动窗口 */

/* PNG 工作缓冲放在高内存 (24MB 起, 32MB 机器上安全; 后台缓冲只占 8..24MB).
   这样不占用内核 .highbss (字库/编译器已接近 LIB_BASE 上限). */
#define PNG_MEM_FILE  0x1800000u
#define PNG_MEM_PREV  0x1880000u
#define PNG_MEM_CUR   0x1888000u
#define PNG_MEM_LINE  0x1890000u
#define PNG_MEM_WIN   0x18A0000u

static u8 *const png_file = (u8 *)PNG_MEM_FILE;
static u8 *const png_prev = (u8 *)PNG_MEM_PREV;
static u8 *const png_cur  = (u8 *)PNG_MEM_CUR;
static u8 *const png_line = (u8 *)PNG_MEM_LINE;
static u8 *const zwin     = (u8 *)PNG_MEM_WIN;

/* IHDR 与调色板/tRNS 状态 (每次解析刷新) */
struct ihdr {
    int w, h;
    int depth, ctype, comp, filt, inter;
    int channels;
};

static u8  pal[256 * 3];
static int pal_n;
static u8  trns[256];
static int trns_n;
static u32 trns_gray, trns_r, trns_g, trns_b;
static bool has_trns_gray, has_trns_rgb;

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static bool valid_ihdr(struct ihdr *h)
{
    if (h->w <= 0 || h->h <= 0 || h->w > PNG_ROW_MAX || h->h > 8192)
        return false;
    if (h->comp != 0 || h->filt != 0 || h->inter > 1)
        return false;
    int d = h->depth;
    switch (h->ctype) {
    case 0:
        if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16) return false;
        h->channels = 1;
        break;
    case 2:
        if (d != 8 && d != 16) return false;
        h->channels = 3;
        break;
    case 3:
        if (d != 1 && d != 2 && d != 4 && d != 8) return false;
        h->channels = 1;
        break;
    case 4:
        if (d != 8 && d != 16) return false;
        h->channels = 2;
        break;
    case 6:
        if (d != 8 && d != 16) return false;
        h->channels = 4;
        break;
    default:
        return false;
    }
    return true;
}

/* 解析 IHDR/PLTE/tRNS, 刷新调色板与透明色状态 */
static bool png_parse(const u8 *data, u32 len, struct ihdr *h)
{
    static const u8 sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (len < 8 || memcmp(data, sig, 8) != 0)
        return false;

    bool got_ihdr = false;
    pal_n = 0;
    trns_n = 0;
    has_trns_gray = has_trns_rgb = false;

    u32 pos = 8;
    while (pos + 12 <= len) {
        u32 l = be32(data + pos);
        u32 t = be32(data + pos + 4);
        const u8 *d = data + pos + 8;
        if (l > 0x7FFFFFFFu || pos + 12u + l > len)
            break;
        if (t == 0x49484452u) {                 /* IHDR */
            if (l < 13)
                return false;
            h->w = (int)be32(d);
            h->h = (int)be32(d + 4);
            h->depth = d[8];
            h->ctype = d[9];
            h->comp = d[10];
            h->filt = d[11];
            h->inter = d[12];
            if (!valid_ihdr(h))
                return false;
            got_ihdr = true;
        } else if (t == 0x504C5445u) {          /* PLTE */
            int n = (int)(l / 3);
            if (n > 256) n = 256;
            pal_n = n;
            memcpy(pal, d, (size_t)(n * 3));
        } else if (t == 0x74524E53u) {          /* tRNS */
            if (h->ctype == 3) {
                trns_n = (int)l;
                if (trns_n > 256) trns_n = 256;
                memcpy(trns, d, (size_t)trns_n);
            } else if (h->ctype == 0) {
                if (l >= 2) {
                    trns_gray = ((u32)d[0] << 8) | d[1];
                    has_trns_gray = true;
                }
            } else if (h->ctype == 2) {
                if (l >= 6) {
                    trns_r = ((u32)d[0] << 8) | d[1];
                    trns_g = ((u32)d[2] << 8) | d[3];
                    trns_b = ((u32)d[4] << 8) | d[5];
                    has_trns_rgb = true;
                }
            }
        } else if (t == 0x49454E44u) {          /* IEND */
            break;
        }
        pos += 12u + l;
    }
    return got_ihdr;
}

int png_info(const u8 *data, u32 len, struct png_info *out)
{
    struct ihdr h;
    if (!out || !png_parse(data, len, &h))
        return -1;
    out->w = h.w;
    out->h = h.h;
    out->depth = (u8)h.depth;
    out->ctype = (u8)h.ctype;
    out->interlace = (u8)h.inter;
    return 0;
}

/* ---------------------------------------------------- IDAT 串联读取器 */
struct zsrc {
    const u8 *d;
    u32 len;
    u32 pos;        /* 当前 IDAT 数据内的读偏移 */
    u32 remain;     /* 当前 IDAT 块剩余字节 */
};

static bool zsrc_init(struct zsrc *z, const u8 *d, u32 len)
{
    z->d = d;
    z->len = len;
    z->pos = 8;
    z->remain = 0;
    while (z->pos + 12u <= len) {
        u32 l = be32(d + z->pos);
        u32 t = be32(d + z->pos + 4);
        u32 ds = z->pos + 8;
        if (ds + l + 4u > len)
            return false;
        if (t == 0x49444154u) {                 /* IDAT */
            z->pos = ds;
            z->remain = l;
            return true;
        }
        z->pos = ds + l + 4u;
    }
    return false;
}

/* 当前 IDAT 用尽: 跳到后续连续 IDAT 块 */
static bool zsrc_next(struct zsrc *z)
{
    u32 p = z->pos + 4u;                        /* 跳过上一块 CRC */
    while (p + 12u <= z->len) {
        u32 l = be32(z->d + p);
        u32 t = be32(z->d + p + 4);
        u32 ds = p + 8;
        if (ds + l + 4u > z->len)
            return false;
        if (t == 0x49444154u) {
            z->pos = ds;
            z->remain = l;
            return true;
        }
        p = ds + l + 4u;
    }
    return false;
}

static int zsrc_byte(struct zsrc *z)
{
    if (z->remain == 0 && !zsrc_next(z))
        return -1;
    if (z->pos >= z->len || z->remain == 0)
        return -1;
    z->remain--;
    return z->d[z->pos++];
}

/* ------------------------------------------------------- inflate (puff) */
struct huff {
    short count[16];
    short symbol[320];
};

struct zin {
    struct zsrc *src;
    u32 bitbuf;
    int bitcnt;
    int mode;                                   /* 0=hdr 1=stored 2=data 3=done -1=err */
    int final_done;
    int stored_left;
    int match_left;
    u32 match_dist;
    struct huff lh, dh;
    struct huff fl, fd;
    u32 wpos;
};

enum { ZM_HDR = 0, ZM_STORED = 1, ZM_DATA = 2, ZM_DONE = 3, ZM_ERR = -1 };

static const u16 len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u8 len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const u16 dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};
static const u8 dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const u8 cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int zget(struct zin *z, int n)
{
    if (n == 0)
        return 0;
    while (z->bitcnt < n) {
        int b = zsrc_byte(z->src);
        if (b < 0)
            return -1;
        z->bitbuf |= (u32)b << z->bitcnt;
        z->bitcnt += 8;
    }
    int v = (int)(z->bitbuf & ((1u << n) - 1u));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

/* 规范 Huffman 构造 (puff 风格) */
static int zbuild(struct huff *h, const u8 *lengths, int n)
{
    for (int i = 0; i < 16; i++)
        h->count[i] = 0;
    for (int s = 0; s < n; s++)
        h->count[lengths[s]]++;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;                          /* 过度订阅 */
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int s = 0; s < n; s++)
        if (lengths[s])
            h->symbol[offs[lengths[s]]++] = (short)s;
    return 0;
}

static int zdecode(struct zin *z, struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = zget(z, 1);
        if (b < 0)
            return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void zbuild_fixed(struct zin *z)
{
    u8 l[288];
    int i;
    for (i = 0; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    zbuild(&z->fl, l, 288);
    for (i = 0; i < 32; i++) l[i] = 5;
    zbuild(&z->fd, l, 32);
}

static bool zdynamic(struct zin *z)
{
    int hlit = zget(z, 5), hdist = zget(z, 5), hclen = zget(z, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0)
        return false;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30)
        return false;

    u8 cl[19];
    memset(cl, 0, sizeof(cl));
    for (int i = 0; i < hclen; i++) {
        int v = zget(z, 3);
        if (v < 0)
            return false;
        cl[cl_order[i]] = (u8)v;
    }
    struct huff clh;
    if (zbuild(&clh, cl, 19) < 0)
        return false;

    u8 lens[320];
    memset(lens, 0, sizeof(lens));
    int i = 0, total = hlit + hdist;
    while (i < total) {
        int sym = zdecode(z, &clh);
        if (sym < 0)
            return false;
        if (sym < 16) {
            lens[i++] = (u8)sym;
        } else if (sym == 16) {
            if (i == 0) return false;
            int e = zget(z, 2);
            if (e < 0) return false;
            int rep = 3 + e;
            u8 p = lens[i - 1];
            while (rep-- > 0 && i < total)
                lens[i++] = p;
        } else if (sym == 17) {
            int e = zget(z, 3);
            if (e < 0) return false;
            int rep = 3 + e;
            while (rep-- > 0 && i < total)
                lens[i++] = 0;
        } else if (sym == 18) {
            int e = zget(z, 7);
            if (e < 0) return false;
            int rep = 11 + e;
            while (rep-- > 0 && i < total)
                lens[i++] = 0;
        } else {
            return false;
        }
    }
    if (zbuild(&z->lh, lens, hlit) < 0)
        return false;
    if (zbuild(&z->dh, lens + hlit, hdist) < 0)
        return false;
    return true;
}

/* 输出 n 字节; 匹配可跨行, 状态跨调用保持. 返回产出字节数, -1=错误. */
static int zread(struct zin *z, u8 *out, int n)
{
    int got = 0;
    while (got < n) {
        if (z->mode == ZM_ERR)
            return -1;
        if (z->mode == ZM_DONE)
            break;
        if (z->mode == ZM_HDR) {
            if (z->final_done) {
                z->mode = ZM_DONE;
                continue;
            }
            int bfinal = zget(z, 1), btype = zget(z, 2);
            if (bfinal < 0 || btype < 0) {
                z->mode = ZM_ERR;
                return -1;
            }
            z->final_done = bfinal;
            if (btype == 0) {
                int drop = z->bitcnt & 7;       /* 对齐到字节边界 */
                if (drop) {
                    z->bitbuf >>= drop;
                    z->bitcnt -= drop;
                }
                int lo = zsrc_byte(z->src), hi = zsrc_byte(z->src);
                int nlo = zsrc_byte(z->src), nhi = zsrc_byte(z->src);
                if (lo < 0 || hi < 0 || nlo < 0 || nhi < 0) {
                    z->mode = ZM_ERR;
                    return -1;
                }
                int blen = lo | (hi << 8);
                int nlen = nlo | (nhi << 8);
                if ((blen ^ 0xFFFF) != nlen) {
                    z->mode = ZM_ERR;
                    return -1;
                }
                z->stored_left = blen;
                z->mode = ZM_STORED;
            } else if (btype == 1) {
                zbuild_fixed(z);
                z->lh = z->fl;
                z->dh = z->fd;
                z->mode = ZM_DATA;
            } else if (btype == 2) {
                if (!zdynamic(z)) {
                    z->mode = ZM_ERR;
                    return -1;
                }
                z->mode = ZM_DATA;
            } else {
                z->mode = ZM_ERR;
                return -1;
            }
            continue;
        }
        if (z->mode == ZM_STORED) {
            while (z->stored_left > 0 && got < n) {
                int b = zsrc_byte(z->src);
                if (b < 0) {
                    z->mode = ZM_ERR;
                    return -1;
                }
                zwin[z->wpos & (ZWIN_SIZE - 1)] = (u8)b;
                z->wpos++;
                out[got++] = (u8)b;
                z->stored_left--;
            }
            if (z->stored_left == 0)
                z->mode = z->final_done ? ZM_DONE : ZM_HDR;
            continue;
        }
        /* ZM_DATA */
        if (z->match_left > 0) {
            u8 b = zwin[(z->wpos - z->match_dist) & (ZWIN_SIZE - 1)];
            zwin[z->wpos & (ZWIN_SIZE - 1)] = b;
            z->wpos++;
            out[got++] = b;
            z->match_left--;
            continue;
        }
        int sym = zdecode(z, &z->lh);
        if (sym < 0) {
            z->mode = ZM_ERR;
            return -1;
        }
        if (sym < 256) {
            zwin[z->wpos & (ZWIN_SIZE - 1)] = (u8)sym;
            z->wpos++;
            out[got++] = (u8)sym;
        } else if (sym == 256) {
            z->mode = z->final_done ? ZM_DONE : ZM_HDR;
        } else {
            int li = sym - 257;
            if (li > 28) {
                z->mode = ZM_ERR;
                return -1;
            }
            int e = zget(z, len_extra[li]);
            if (e < 0) {
                z->mode = ZM_ERR;
                return -1;
            }
            int length = len_base[li] + e;
            int ds = zdecode(z, &z->dh);
            if (ds < 0 || ds > 29) {
                z->mode = ZM_ERR;
                return -1;
            }
            int de = zget(z, dist_extra[ds]);
            if (de < 0) {
                z->mode = ZM_ERR;
                return -1;
            }
            z->match_dist = (u32)dist_base[ds] + (u32)de;
            z->match_left = length;
        }
    }
    return got;
}

/* ------------------------------------------------------------- 反过滤 */
static int abs_i(int v) { return v < 0 ? -v : v; }

static void unfilter(int filter, u8 *cur, const u8 *prev, int n, int bpp)
{
    if (filter == 0)
        return;
    for (int i = 0; i < n; i++) {
        int a = (i >= bpp) ? cur[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = (i >= bpp && prev) ? prev[i - bpp] : 0;
        int v = cur[i];
        switch (filter) {
        case 1:
            v += a;
            break;
        case 2:
            v += b;
            break;
        case 3:
            v += (a + b) >> 1;
            break;
        case 4: {
            int p = a + b - c;
            int pa = abs_i(p - a), pb = abs_i(p - b), pc = abs_i(p - c);
            int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            v += pr;
            break;
        }
        default:
            break;
        }
        cur[i] = (u8)v;
    }
}

/* ------------------------------------------------------------- 像素取值 */
static u32 scale8(u32 v, int depth)
{
    if (depth == 16)
        return v >> 8;
    if (depth == 8)
        return v;
    int maxv = (1 << depth) - 1;
    return v * 255u / (u32)maxv;
}

static void pixel_rgba(const u8 *row, int i, const struct ihdr *h,
                       u32 *out_rgb, u8 *out_a)
{
    int ch = h->channels;
    int depth = h->depth;
    u32 s[4] = { 0, 0, 0, 0 };

    if (depth == 16) {
        int off = i * ch * 2;
        for (int k = 0; k < ch; k++)
            s[k] = ((u32)row[off + k * 2] << 8) | row[off + k * 2 + 1];
    } else if (depth == 8) {
        int off = i * ch;
        for (int k = 0; k < ch; k++)
            s[k] = row[off + k];
    } else {
        int bitpos = i * depth;
        int byteidx = bitpos >> 3;
        int shift = 8 - depth - (bitpos & 7);
        int v = (row[byteidx] >> shift) & ((1 << depth) - 1);
        s[0] = (u32)v;
    }

    u32 rgb = 0;
    u8 a = 255;
    switch (h->ctype) {
    case 0: {                                   /* 灰度 */
        u32 g = scale8(s[0], depth);
        rgb = (g << 16) | (g << 8) | g;
        if (has_trns_gray && s[0] == trns_gray)
            a = 0;
        break;
    }
    case 2: {                                   /* 真彩 */
        u32 r = scale8(s[0], depth), g = scale8(s[1], depth),
            b = scale8(s[2], depth);
        rgb = (r << 16) | (g << 8) | b;
        if (has_trns_rgb && s[0] == trns_r && s[1] == trns_g && s[2] == trns_b)
            a = 0;
        break;
    }
    case 3: {                                   /* 调色板 */
        u32 idx = s[0];
        if ((int)idx < pal_n)
            rgb = ((u32)pal[idx * 3] << 16) | ((u32)pal[idx * 3 + 1] << 8) |
                  pal[idx * 3 + 2];
        if ((int)idx < trns_n)
            a = trns[idx];
        break;
    }
    case 4: {                                   /* 灰度 + alpha */
        u32 g = scale8(s[0], depth);
        rgb = (g << 16) | (g << 8) | g;
        a = (u8)scale8(s[1], depth);
        break;
    }
    case 6: {                                   /* 真彩 + alpha */
        u32 r = scale8(s[0], depth), g = scale8(s[1], depth),
            b = scale8(s[2], depth);
        rgb = (r << 16) | (g << 8) | b;
        a = (u8)scale8(s[3], depth);
        break;
    }
    default:
        break;
    }
    *out_rgb = rgb;
    *out_a = a;
}

/* ------------------------------------------------------------- 渲染 */
static bool png_render(const u8 *data, u32 len, int ox, int oy, int s)
{
    if (!gfx_active())
        return false;

    struct ihdr h;
    if (!png_parse(data, len, &h))
        return false;

    struct zsrc src;
    if (!zsrc_init(&src, data, len))
        return false;
    /* IDAT 是 zlib 流 (RFC1950): 跳过 2 字节 CMF/FLG, 之后才是 DEFLATE */
    if (zsrc_byte(&src) < 0 || zsrc_byte(&src) < 0)
        return false;

    static struct zin zi;
    memset(&zi, 0, sizeof(zi));
    zi.src = &src;
    zi.mode = ZM_HDR;
    memset(zwin, 0, ZWIN_SIZE);

    static const int a7x0[7] = { 0, 4, 0, 2, 0, 1, 0 };
    static const int a7y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int a7dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
    static const int a7dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

    int passes = h.inter ? 7 : 1;
    int bpp = (h.channels * h.depth + 7) / 8;
    if (bpp < 1) bpp = 1;

    for (int pass = 0; pass < passes; pass++) {
        int x0 = 0, y0 = 0, dx = 1, dy = 1;
        if (h.inter) {
            x0 = a7x0[pass]; y0 = a7y0[pass];
            dx = a7dx[pass]; dy = a7dy[pass];
        }
        int pw = (h.w - x0 + dx - 1) / dx;
        int ph = (h.h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0)
            continue;
        int row_bytes = (pw * h.channels * h.depth + 7) / 8;
        if (row_bytes > PNG_ROW_BYTES)
            return false;
        memset(png_prev, 0, (size_t)row_bytes);

        for (int r = 0; r < ph; r++) {
            int n = zread(&zi, png_line, row_bytes + 1);
            if (n != row_bytes + 1)
                return false;
            u8 filter = png_line[0];
            memcpy(png_cur, png_line + 1, (size_t)row_bytes);
            unfilter(filter, png_cur, png_prev, row_bytes, bpp);

            int sy = y0 + r * dy;
            int py = oy + sy * s;
            for (int c = 0; c < pw; c++) {
                u32 rgb;
                u8 a;
                pixel_rgba(png_cur, c, &h, &rgb, &a);
                if (a == 0)
                    continue;
                int sx = x0 + c * dx;
                int px = ox + sx * s;
                if (s == 1) {
                    gfx_pixel_a(px, py, rgb, a);
                } else {
                    for (int yy = 0; yy < s; yy++)
                        for (int xx = 0; xx < s; xx++)
                            gfx_pixel_a(px + xx, py + yy, rgb, a);
                }
            }
            memcpy(png_prev, png_cur, (size_t)row_bytes);
        }
    }
    return true;
}

bool png_draw(const u8 *data, u32 len, int dx, int dy)
{
    return png_render(data, len, dx, dy, 1);
}

/* 以整数倍最近邻放大绘制 (ICO 内嵌 PNG 等共用) */
bool png_draw_scaled(const u8 *data, u32 len, int ox, int oy, int s)
{
    if (s < 1)
        s = 1;
    if (s > 16)
        s = 16;
    return png_render(data, len, ox, oy, s);
}

static bool png_load(const char *path, u32 *out_len)
{
    struct fat_dirent de;
    if (!path || !fat_find(path, &de))
        return false;
    if (de.is_dir || de.size == 0 || de.size > PNG_FILE_MAX)
        return false;
    u32 got = 0;
    if (!fat_read_file(path, png_file, PNG_FILE_MAX, &got) || got == 0)
        return false;
    *out_len = got;
    return true;
}

bool png_draw_file(const char *path, int dx, int dy)
{
    u32 len;
    if (!png_load(path, &len))
        return false;
    return png_draw(png_file, len, dx, dy);
}

bool png_draw_file_fit(const char *path)
{
    u32 len;
    if (!png_load(path, &len))
        return false;
    struct ihdr h;
    if (!png_parse(png_file, len, &h))
        return false;

    int sw = gfx_w(), sh = gfx_h();
    int s = 1;
    if (h.w <= sw && h.h <= sh) {
        int sx = sw / h.w, sy = sh / h.h;
        s = sx < sy ? sx : sy;
        if (s < 1) s = 1;
        if (s > 8) s = 8;
    }
    int dw = h.w * s, dh = h.h * s;
    int ox = (sw - dw) / 2;
    int oy = (sh - dh) / 2;
    return png_render(png_file, len, ox, oy, s);
}
