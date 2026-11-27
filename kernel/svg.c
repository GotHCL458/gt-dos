/* SVG 矢量渲染子集.
 *
 * 支持: <svg> viewBox/width/height 自适应缩放居中; <g> 样式/变换继承;
 *   <rect> <circle> <ellipse> <line> <polyline> <polygon> <path>;
 *   path 命令 M/L/H/V/Z/C/Q (曲线按折线细分) 与相对形式;
 *   fill/stroke/stroke-width/opacity/fill-opacity/stroke-opacity;
 *   transform 的 translate/scale/matrix; 颜色 #rgb/#rrggbb 与常见命名色.
 * 未支持: 文本、渐变/图案、滤镜、裁剪、圆弧 A、CSS 选择器 (忽略).
 *
 * 形状先变换到屏幕整数坐标, 再用扫描线填充 + 粗线段描边, 通过
 * gfx_pixel_a 做 alpha 混合. 解析缓冲放在高内存 (26MB 起). */
#include "gt.h"

#define SVG_FILE_MAX (256 * 1024)
#define SVG_MEM_FILE 0x1A40000u
#define SVG_PT_MAX   2048
#define SVG_MEM_PTS  0x1A80000u
#define SVG_MEM_XS   0x1A90000u

struct svg_pt { int x, y; };

static u8 *const svg_buf = (u8 *)SVG_MEM_FILE;
static struct svg_pt *const svg_pts = (struct svg_pt *)SVG_MEM_PTS;
static int *const svg_xs = (int *)SVG_MEM_XS;

struct xf { i64 a, b, c, d, e, f; };

struct style {
    u32 fill, stroke;
    int fill_a, stroke_a;
    bool fill_off, stroke_off;
    i64 sw;                                  /* 用户单位, Q16.16 */
};

static struct xf g_xf;
static struct style g_st;
static struct xf g_xf_stk[8];
static struct style g_st_stk[8];
static int g_depth;
static i64 g_scale;

/* --------------------------------------------------------- 数值/颜色 */
static i64 num_rng(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == ',' || *p == '\t' ||
                       *p == '\n' || *p == '\r'))
        p++;
    int neg = 0;
    if (p < end && (*p == '-' || *p == '+')) {
        neg = (*p == '-');
        p++;
    }
    i64 ip = 0;
    bool any = false;
    while (p < end && *p >= '0' && *p <= '9') {
        ip = ip * 10 + (*p - '0');
        p++;
        any = true;
    }
    i64 frac = 0, fdiv = 1;
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            frac = frac * 10 + (*p - '0');
            fdiv *= 10;
            p++;
            any = true;
        }
    }
    while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       *p == '%'))
        p++;
    i64 v = (ip << 16) + (frac << 16) / fdiv;
    *pp = p;
    if (!any)
        return 0;
    return neg ? -v : v;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct named_color { const char *n; u32 rgb; };
static const struct named_color named[] = {
    { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 },
    { "green", 0x008000 }, { "lime", 0x00ff00 }, { "blue", 0x0000ff },
    { "yellow", 0xffff00 }, { "cyan", 0x00ffff }, { "aqua", 0x00ffff },
    { "magenta", 0xff00ff }, { "fuchsia", 0xff00ff }, { "gray", 0x808080 },
    { "grey", 0x808080 }, { "silver", 0xc0c0c0 }, { "maroon", 0x800000 },
    { "olive", 0x808000 }, { "navy", 0x000080 }, { "teal", 0x008080 },
    { "purple", 0x800080 }, { "orange", 0xffa500 }, { "pink", 0xffc0cb },
    { "brown", 0xa52a2a }, { "gold", 0xffd700 }, { "indigo", 0x4b0082 },
    { 0, 0 }
};

/* 1=有效颜色, 0=none, -1=无法识别 */
static int color_parse(const char *s, int n, u32 *out)
{
    char t[32];
    int k = 0;
    while (n-- > 0 && *s && k < 31) {
        if (*s != ' ' && *s != '\t')
            t[k++] = *s;
        s++;
    }
    t[k] = 0;
    if (!t[0])
        return -1;
    if (t[0] == 'n' && t[1] == 'o' && t[2] == 'n' && t[3] == 'e')
        return 0;
    if (t[0] == '#') {
        if (k == 4) {
            int r = hexval(t[1]), g = hexval(t[2]), b = hexval(t[3]);
            if (r < 0 || g < 0 || b < 0) return -1;
            *out = ((u32)(r * 17) << 16) | ((u32)(g * 17) << 8) |
                   (u32)(b * 17);
            return 1;
        }
        if (k == 7) {
            int r1 = hexval(t[1]), r2 = hexval(t[2]);
            int g1 = hexval(t[3]), g2 = hexval(t[4]);
            int b1 = hexval(t[5]), b2 = hexval(t[6]);
            if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0)
                return -1;
            *out = ((u32)(r1 * 16 + r2) << 16) |
                   ((u32)(g1 * 16 + g2) << 8) | (u32)(b1 * 16 + b2);
            return 1;
        }
        return -1;
    }
    for (int i = 0; named[i].n; i++) {
        const char *a = named[i].n;
        int j = 0;
        while (a[j] && t[j] && a[j] == t[j]) j++;
        if (!a[j] && !t[j]) {
            *out = named[i].rgb;
            return 1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------- 变换 */
static void xf_point(const struct xf *m, i64 x, i64 y, int *px, int *py)
{
    i64 X = ((m->a * x + m->c * y) >> 16) + m->e;  /* Q16.16 屏幕 */
    i64 Y = ((m->b * x + m->d * y) >> 16) + m->f;
    if (px) *px = (int)(X >> 16);
    if (py) *py = (int)(Y >> 16);
}

static struct xf xf_mul(struct xf p, struct xf t)
{
    struct xf r;
    r.a = ((p.a * t.a) >> 16) + ((p.c * t.b) >> 16);
    r.b = ((p.b * t.a) >> 16) + ((p.d * t.b) >> 16);
    r.c = ((p.a * t.c) >> 16) + ((p.c * t.d) >> 16);
    r.d = ((p.b * t.c) >> 16) + ((p.d * t.d) >> 16);
    r.e = ((p.a * t.e) >> 16) + ((p.c * t.f) >> 16) + p.e;
    r.f = ((p.b * t.e) >> 16) + ((p.d * t.f) >> 16) + p.f;
    return r;
}

/* ------------------------------------------------------------- 绘制 */
static void fill_poly(struct svg_pt *p, int n, u32 rgb, int a)
{
    if (n < 3 || a <= 0)
        return;
    int miny = p[0].y, maxy = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].y < miny) miny = p[i].y;
        if (p[i].y > maxy) maxy = p[i].y;
    }
    if (miny < 0) miny = 0;
    if (maxy >= gfx_h()) maxy = gfx_h() - 1;
    for (int y = miny; y <= maxy; y++) {
        int cnt = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            int y1 = p[j].y, y2 = p[i].y;
            if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
                int x1 = p[j].x, x2 = p[i].x;
                int x = x1 + (int)(((long long)(y - y1) * (x2 - x1)) /
                                   (y2 - y1));
                if (cnt < SVG_PT_MAX)
                    svg_xs[cnt++] = x;
            }
        }
        for (int i = 1; i < cnt; i++) {          /* 插入排序 */
            int v = svg_xs[i], j = i - 1;
            while (j >= 0 && svg_xs[j] > v) {
                svg_xs[j + 1] = svg_xs[j];
                j--;
            }
            svg_xs[j + 1] = v;
        }
        for (int i = 0; i + 1 < cnt; i += 2) {
            int xa = svg_xs[i], xb = svg_xs[i + 1];
            if (xa < 0) xa = 0;
            if (xb >= gfx_w()) xb = gfx_w() - 1;
            for (int x = xa; x <= xb; x++)
                gfx_pixel_a(x, y, rgb, (u8)a);
        }
    }
}

static void stroke_seg(int x1, int y1, int x2, int y2, int w, u32 rgb, int a)
{
    if (a <= 0)
        return;
    if (w < 1) w = 1;
    if (w > 12) w = 12;
    int dx = x2 - x1, dy = y2 - y1;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps < 1) steps = 1;
    int hw = w / 2;
    for (int i = 0; i <= steps; i++) {
        int x = x1 + (int)((long long)dx * i / steps);
        int y = y1 + (int)((long long)dy * i / steps);
        for (int yy = -hw; yy <= hw; yy++)
            for (int xx = -hw; xx <= hw; xx++)
                gfx_pixel_a(x + xx, y + yy, rgb, (u8)a);
    }
}

static int stroke_width_px(const struct style *st)
{
    i64 v = (st->sw * g_scale) >> 32;
    int w = (int)v;
    if (w < 1) w = 1;
    if (w > 12) w = 12;
    return w;
}

static void draw_shape(struct svg_pt *p, int n, bool closed,
                       const struct style *st)
{
    if (n <= 0)
        return;
    if (!st->fill_off && st->fill_a > 0 && n >= 3)
        fill_poly(p, n, st->fill, st->fill_a);
    if (!st->stroke_off && st->stroke_a > 0) {
        int w = stroke_width_px(st);
        for (int i = 0; i + 1 < n; i++)
            stroke_seg(p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, w,
                       st->stroke, st->stroke_a);
        if (closed && n > 1)
            stroke_seg(p[n - 1].x, p[n - 1].y, p[0].x, p[0].y, w,
                       st->stroke, st->stroke_a);
    }
}

/* --------------------------------------------------------- 属性解析 */
struct attr { char name[14]; const char *v; int n; };

static const char *attr_get(struct attr *at, int na, const char *name,
                            int *len)
{
    for (int i = 0; i < na; i++) {
        const char *a = at[i].name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b) {
            if (len) *len = at[i].n;
            return at[i].v;
        }
    }
    return 0;
}

static i64 attr_num(struct attr *at, int na, const char *name, i64 def)
{
    int len;
    const char *v = attr_get(at, na, name, &len);
    if (!v)
        return def;
    const char *p = v, *end = v + len;
    return num_rng(&p, end);
}

static int attr_alpha(const char *v, int len)
{
    const char *p = v, *end = v + len;
    i64 q = num_rng(&p, end);
    if (q < 0) q = 0;
    if (q > 65536) q = 65536;
    return (int)((q * 255) >> 16);
}

/* 应用样式属性到 st (颜色/宽度/透明度/style=) */
static void apply_style(struct style *st, struct attr *at, int na)
{
    int len;
    const char *v;
    u32 col;

    if ((v = attr_get(at, na, "fill", &len))) {
        int r = color_parse(v, len, &col);
        if (r == 1) { st->fill = col; st->fill_off = false; }
        else if (r == 0) st->fill_off = true;
    }
    if ((v = attr_get(at, na, "stroke", &len))) {
        int r = color_parse(v, len, &col);
        if (r == 1) { st->stroke = col; st->stroke_off = false; }
        else if (r == 0) st->stroke_off = true;
    }
    if ((v = attr_get(at, na, "stroke-width", &len))) {
        const char *p = v, *end = v + len;
        i64 q = num_rng(&p, end);
        if (q > 0) st->sw = q;
    }
    if ((v = attr_get(at, na, "fill-opacity", &len)))
        st->fill_a = attr_alpha(v, len);
    if ((v = attr_get(at, na, "stroke-opacity", &len)))
        st->stroke_a = attr_alpha(v, len);
    if ((v = attr_get(at, na, "opacity", &len))) {
        int a = attr_alpha(v, len);
        st->fill_a = st->fill_a * a / 255;
        st->stroke_a = st->stroke_a * a / 255;
    }
    /* style="fill:#...;stroke:...;..." */
    if ((v = attr_get(at, na, "style", &len))) {
        const char *p = v, *end = v + len;
        while (p < end) {
            const char *ks = p;
            while (p < end && *p != ':' && *p != ';') p++;
            if (p >= end) break;
            int klen = (int)(p - ks);
            if (*p == ';') { p++; continue; }
            p++;                              /* ':' */
            const char *vs = p;
            while (p < end && *p != ';') p++;
            int vlen = (int)(p - vs);
            if (p < end) p++;
            struct attr one[1];
            int kk = klen < 13 ? klen : 13;
            for (int i = 0; i < kk; i++) one[0].name[i] = ks[i];
            one[0].name[kk] = 0;
            one[0].v = vs;
            one[0].n = vlen;
            apply_style(st, one, 1);
        }
    }
}

/* 解析并组合 transform 属性 (translate/scale/matrix) */
static void apply_transform(struct xf *xf, struct attr *at, int na)
{
    int len;
    const char *v = attr_get(at, na, "transform", &len);
    if (!v)
        return;
    const char *p = v, *end = v + len;
    struct xf t = { 1 << 16, 0, 0, 1 << 16, 0, 0 };
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t' ||
                           *p == '\n' || *p == '\r'))
            p++;
        const char *ns = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') ||
                           (*p >= 'A' && *p <= 'Z')))
            p++;
        int nlen = (int)(p - ns);
        while (p < end && *p != '(') p++;
        if (p >= end)
            break;
        p++;                                  /* '(' */
        i64 av[6];
        int ac = 0;
        for (;;) {
            while (p < end && (*p == ' ' || *p == ',' || *p == '\t' ||
                               *p == '\n' || *p == '\r'))
                p++;
            if (p >= end || *p == ')')
                break;
            i64 val = num_rng(&p, end);
            if (ac < 6)
                av[ac++] = val;
        }
        if (p < end && *p == ')')
            p++;
        if (ac < 1)
            continue;
        struct xf e = { 1 << 16, 0, 0, 1 << 16, 0, 0 };
        if (nlen == 9 && ns[0] == 't' && ns[1] == 'r') {       /* translate */
            e.e = av[0];
            e.f = ac >= 2 ? av[1] : 0;
        } else if (nlen == 5 && ns[0] == 's' && ns[1] == 'c') { /* scale */
            e.a = av[0];
            e.d = ac >= 2 ? av[1] : av[0];
        } else if (nlen == 6 && ns[0] == 'm' && ac >= 6) {      /* matrix */
            e.a = av[0]; e.b = av[1]; e.c = av[2];
            e.d = av[3]; e.e = av[4]; e.f = av[5];
        } else {
            continue;
        }
        t = xf_mul(t, e);
    }
    *xf = xf_mul(*xf, t);
}

/* --------------------------------------------------------- 图形元素 */
static int add_pt(int n, i64 x, i64 y)
{
    if (n >= SVG_PT_MAX)
        return n;
    xf_point(&g_xf, x, y, &svg_pts[n].x, &svg_pts[n].y);
    return n + 1;
}

static void el_rect(struct attr *at, int na)
{
    i64 x = attr_num(at, na, "x", 0);
    i64 y = attr_num(at, na, "y", 0);
    i64 w = attr_num(at, na, "width", 0);
    i64 h = attr_num(at, na, "height", 0);
    if (w <= 0 || h <= 0)
        return;
    int n = 0;
    n = add_pt(n, x, y);
    n = add_pt(n, x + w, y);
    n = add_pt(n, x + w, y + h);
    n = add_pt(n, x, y + h);
    draw_shape(svg_pts, n, true, &g_st);
}

static void el_ellipse(struct attr *at, int na, bool circle)
{
    i64 cx = attr_num(at, na, "cx", 0);
    i64 cy = attr_num(at, na, "cy", 0);
    i64 rx = circle ? attr_num(at, na, "r", 0) : attr_num(at, na, "rx", 0);
    i64 ry = circle ? rx : attr_num(at, na, "ry", 0);
    if (rx <= 0 || ry <= 0)
        return;
    /* 32 段圆 (Q15 三角表, 11.25° 步长) */
    static const int co[32] = {
        32767, 32138, 30274, 27246, 23170, 18205, 12540, 6393,
        0, -6393, -12540, -18205, -23170, -27246, -30274, -32138,
        -32767, -32138, -30274, -27246, -23170, -18205, -12540, -6393,
        0, 6393, 12540, 18205, 23170, 27246, 30274, 32138
    };
    static const int si[32] = {
        0, 6393, 12540, 18205, 23170, 27246, 30274, 32138,
        32767, 32138, 30274, 27246, 23170, 18205, 12540, 6393,
        0, -6393, -12540, -18205, -23170, -27246, -30274, -32138,
        -32767, -32138, -30274, -27246, -23170, -18205, -12540, -6393
    };
    int n = 0;
    for (int i = 0; i < 32; i++) {
        i64 c = (i64)co[i] * 65536 / 32767;
        i64 s = (i64)si[i] * 65536 / 32767;
        n = add_pt(n, cx + ((rx * c) >> 16), cy + ((ry * s) >> 16));
    }
    draw_shape(svg_pts, n, true, &g_st);
}

static void el_line(struct attr *at, int na)
{
    struct style st = g_st;
    st.fill_off = true;                       /* 线不填充 */
    if (st.sw <= 0)
        st.sw = 1 << 16;
    int n = 0;
    n = add_pt(n, attr_num(at, na, "x1", 0), attr_num(at, na, "y1", 0));
    n = add_pt(n, attr_num(at, na, "x2", 0), attr_num(at, na, "y2", 0));
    draw_shape(svg_pts, n, false, &st);
}

static void el_points(struct attr *at, int na, bool closed)
{
    int len;
    const char *v = attr_get(at, na, "points", &len);
    if (!v)
        return;
    const char *p = v, *end = v + len;
    int n = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t' ||
                           *p == '\n' || *p == '\r'))
            p++;
        if (p >= end)
            break;
        i64 x = num_rng(&p, end);
        i64 y = num_rng(&p, end);
        n = add_pt(n, x, y);
    }
    draw_shape(svg_pts, n, closed, &g_st);
}

/* path 曲线细分辅助 */
static int cubic_pts(int n, i64 x0, i64 y0, i64 x1, i64 y1, i64 x2, i64 y2,
                     i64 x3, i64 y3)
{
    for (int k = 1; k <= 12; k++) {
        i64 t = (i64)(k * 65536) / 12;
        i64 u = 65536 - t;
        i64 u2 = (u * u) >> 16, t2 = (t * t) >> 16;
        i64 u3 = (u2 * u) >> 16, t3 = (t2 * t) >> 16;
        i64 u2t = (u2 * t) >> 16, ut2 = (u * t2) >> 16;
        i64 x = ((u3 * x0) >> 16) + 3 * ((u2t * x1) >> 16) +
                3 * ((ut2 * x2) >> 16) + ((t3 * x3) >> 16);
        i64 y = ((u3 * y0) >> 16) + 3 * ((u2t * y1) >> 16) +
                3 * ((ut2 * y2) >> 16) + ((t3 * y3) >> 16);
        n = add_pt(n, x, y);
    }
    return n;
}

static int quad_pts(int n, i64 x0, i64 y0, i64 x1, i64 y1, i64 x2, i64 y2)
{
    for (int k = 1; k <= 12; k++) {
        i64 t = (i64)(k * 65536) / 12;
        i64 u = 65536 - t;
        i64 u2 = (u * u) >> 16, t2 = (t * t) >> 16;
        i64 ut = (u * t) >> 16;
        i64 x = ((u2 * x0) >> 16) + 2 * ((ut * x1) >> 16) +
                ((t2 * x2) >> 16);
        i64 y = ((u2 * y0) >> 16) + 2 * ((ut * y1) >> 16) +
                ((t2 * y2) >> 16);
        n = add_pt(n, x, y);
    }
    return n;
}

static void el_path(struct attr *at, int na)
{
    int len;
    const char *v = attr_get(at, na, "d", &len);
    if (!v)
        return;
    const char *p = v, *end = v + len;
    i64 cx = 0, cy = 0, sx = 0, sy = 0;
    int n = 0;
    bool closed = false;
    char cmd = 0;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t' ||
                           *p == '\n' || *p == '\r'))
            p++;
        if (p >= end)
            break;
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cmd = c;
            p++;
        } else if (cmd == 0) {
            p++;
            continue;
        }
        bool rel = (cmd >= 'a' && cmd <= 'z');
        char uc = (cmd >= 'a' && cmd <= 'z') ? (char)(cmd - 32) : cmd;
        if (uc == 'M') {
            i64 x = num_rng(&p, end), y = num_rng(&p, end);
            if (rel) { x += cx; y += cy; }
            if (n > 0)
                draw_shape(svg_pts, n, closed, &g_st);
            n = 0;
            cx = x; cy = y; sx = x; sy = y;
            n = add_pt(n, x, y);
            closed = false;
            cmd = rel ? 'l' : 'L';
        } else if (uc == 'L') {
            i64 x = num_rng(&p, end), y = num_rng(&p, end);
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            n = add_pt(n, x, y);
        } else if (uc == 'H') {
            i64 x = num_rng(&p, end);
            if (rel) x += cx;
            cx = x;
            n = add_pt(n, cx, cy);
        } else if (uc == 'V') {
            i64 y = num_rng(&p, end);
            if (rel) y += cy;
            cy = y;
            n = add_pt(n, cx, cy);
        } else if (uc == 'C') {
            i64 x1 = num_rng(&p, end), y1 = num_rng(&p, end);
            i64 x2 = num_rng(&p, end), y2 = num_rng(&p, end);
            i64 x3 = num_rng(&p, end), y3 = num_rng(&p, end);
            if (rel) {
                x1 += cx; y1 += cy; x2 += cx; y2 += cy; x3 += cx; y3 += cy;
            }
            n = cubic_pts(n, cx, cy, x1, y1, x2, y2, x3, y3);
            cx = x3; cy = y3;
        } else if (uc == 'Q') {
            i64 x1 = num_rng(&p, end), y1 = num_rng(&p, end);
            i64 x2 = num_rng(&p, end), y2 = num_rng(&p, end);
            if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; }
            n = quad_pts(n, cx, cy, x1, y1, x2, y2);
            cx = x2; cy = y2;
        } else if (uc == 'S' || uc == 'T') {
            int cnt = (uc == 'S') ? 4 : 2;
            i64 x = 0, y = 0;
            for (int i = 0; i < cnt; i++) {
                i64 val = num_rng(&p, end);
                if (i == cnt - 2) x = val;
                if (i == cnt - 1) y = val;
            }
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            n = add_pt(n, cx, cy);            /* 近似: 直线到终点 */
        } else if (uc == 'A') {
            i64 ex = 0, ey = 0;
            for (int i = 0; i < 7; i++) {
                i64 val = num_rng(&p, end);
                if (i == 5) ex = val;
                if (i == 6) ey = val;
            }
            if (rel) { ex += cx; ey += cy; }
            cx = ex; cy = ey;
            n = add_pt(n, cx, cy);            /* 圆弧按直线近似 */
        } else if (uc == 'Z') {
            if (n > 0)
                draw_shape(svg_pts, n, true, &g_st);
            n = 0;
            cx = sx; cy = sy;
            closed = false;
        } else {
            /* 未知命令: 跳过 */
            num_rng(&p, end);
        }
    }
    if (n > 0)
        draw_shape(svg_pts, n, false, &g_st);
}

/* ------------------------------------------------------------- 主循环 */
static void svg_root(struct attr *at, int na)
{
    i64 minx = 0, miny = 0, vbw = 0, vbh = 0;
    int len;
    const char *v = attr_get(at, na, "viewBox", &len);
    if (!v)
        v = attr_get(at, na, "viewbox", &len);
    if (v) {
        const char *p = v, *end = v + len;
        minx = num_rng(&p, end);
        miny = num_rng(&p, end);
        vbw = num_rng(&p, end);
        vbh = num_rng(&p, end);
    }
    if (vbw <= 0 || vbh <= 0) {
        vbw = attr_num(at, na, "width", 300 << 16);
        vbh = attr_num(at, na, "height", 150 << 16);
        minx = miny = 0;
    }
    if (vbw <= 0 || vbh <= 0) {
        vbw = gfx_w() << 16;
        vbh = gfx_h() << 16;
    }
    int sw = gfx_w(), sh = gfx_h();
   /* 缩放比 (Q16.16) = 屏幕像素 / 用户单位 */
    i64 sx = (((i64)sw) << 32) / vbw;
    i64 sy = (((i64)sh) << 32) / vbh;
    i64 sc = sx < sy ? sx : sy;
    if (sc < 1) sc = 1;
    g_scale = sc;
    int dw = (int)((vbw * sc) >> 32);
    int dh = (int)((vbh * sc) >> 32);
    int ox = (sw - dw) / 2, oy = (sh - dh) / 2;
    g_xf.a = sc; g_xf.b = 0; g_xf.c = 0; g_xf.d = sc;
    g_xf.e = ((i64)ox << 16) - ((minx * sc) >> 16);
    g_xf.f = ((i64)oy << 16) - ((miny * sc) >> 16);
    apply_style(&g_st, at, na);
}

static bool tag_is(const char *name, const char *want)
{
    int i = 0;
    while (name[i] && want[i] && name[i] == want[i]) i++;
    return !name[i] && !want[i];
}

static void svg_parse(void)
{
    const char *p = (const char *)svg_buf;
    while (*p) {
        while (*p && *p != '<') p++;
        if (!*p)
            break;
        p++;
        if (*p == '!') {                       /* 注释/声明 */
            if (p[1] == '-' && p[2] == '-') {
                p += 3;
                while (*p && !(p[0] == '-' && p[1] == '-' && p[2] == '>'))
                    p++;
                if (*p) p += 3;
            } else {
                while (*p && *p != '>') p++;
                if (*p) p++;
            }
            continue;
        }
        if (*p == '?') {
            while (*p && !(p[0] == '?' && p[1] == '>')) p++;
            if (*p) p += 2;
            continue;
        }
        bool close = (*p == '/');
        if (close) p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        char name[16];
        int nl = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
               *p != '>' && *p != '/' && nl < 15)
            name[nl++] = *p++;
        name[nl] = 0;

        struct attr at[32];
        int na = 0;
        while (*p && *p != '>') {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
                   *p == '/')
                p++;
            if (*p == '>' || !*p)
                break;
            char an[14];
            int al = 0;
            while (*p && *p != '=' && *p != ' ' && *p != '\t' && *p != '\r' &&
                   *p != '\n' && *p != '>' && *p != '/' && al < 13)
                an[al++] = *p++;
            an[al] = 0;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                    p++;
                const char *vs;
                int vn;
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    vs = p;
                    while (*p && *p != q) p++;
                    vn = (int)(p - vs);
                    if (*p) p++;
                } else {
                    vs = p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '\r' &&
                           *p != '\n' && *p != '>')
                        p++;
                    vn = (int)(p - vs);
                }
                if (na < 32 && al > 0) {
                    for (int i = 0; i < al; i++) at[na].name[i] = an[i];
                    at[na].name[al] = 0;
                    at[na].v = vs;
                    at[na].n = vn;
                    na++;
                }
            }
        }
        if (*p == '>') p++;

        if (close) {
            if (tag_is(name, "g") && g_depth > 0) {
                g_depth--;
                g_st = g_st_stk[g_depth];
                g_xf = g_xf_stk[g_depth];
            }
            continue;
        }
        if (tag_is(name, "svg")) {
            svg_root(at, na);
        } else if (tag_is(name, "g")) {
            if (g_depth < 8) {
                g_st_stk[g_depth] = g_st;
                g_xf_stk[g_depth] = g_xf;
                g_depth++;
            }
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
        } else if (tag_is(name, "rect")) {
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
            el_rect(at, na);
        } else if (tag_is(name, "circle")) {
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
            el_ellipse(at, na, true);
        } else if (tag_is(name, "ellipse")) {
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
            el_ellipse(at, na, false);
        } else if (tag_is(name, "line")) {
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
            el_line(at, na);
        } else if (tag_is(name, "polyline")) {
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
            el_points(at, na, false);
        } else if (tag_is(name, "polygon")) {
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
            el_points(at, na, true);
        } else if (tag_is(name, "path")) {
            apply_style(&g_st, at, na);
            apply_transform(&g_xf, at, na);
            el_path(at, na);
        }
        /* 其它标签 (defs/style/title/text/...) 忽略 */
    }
}

bool svg_draw_file_fit(const char *path)
{
    if (!gfx_active())
        return false;
    struct fat_dirent de;
    if (!path || !fat_find(path, &de))
        return false;
    if (de.is_dir || de.size == 0 || de.size >= SVG_FILE_MAX)
        return false;
    u32 len = 0;
    if (!fat_read_file(path, svg_buf, SVG_FILE_MAX - 1, &len) || len == 0)
        return false;
    svg_buf[len] = 0;

    g_xf.a = 1 << 16; g_xf.b = 0; g_xf.c = 0; g_xf.d = 1 << 16;
    g_xf.e = 0; g_xf.f = 0;
    g_st.fill = 0x000000; g_st.stroke = 0x000000;
    g_st.fill_a = 255; g_st.stroke_a = 255;
    g_st.fill_off = false; g_st.stroke_off = true;
    g_st.sw = 1 << 16;
    g_scale = 1 << 16;
    g_depth = 0;

    svg_parse();
    return true;
}
