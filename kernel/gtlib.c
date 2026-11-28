/* .gtl 共享库装载器.
   文件格式:
     0   "GTL1"
     4   u32 version
     8   u32 image_size
     12  u32 export_count
     16  exports: { char name[24]; u32 rva; } * export_count
     ..  image (image_size 字节), rva 相对 image 起始
   装载: 文件先读入库区, 再把 image 前移到库基址, 导出表登记为 基址+rva. */
#include "gt.h"

extern char __highbss_end[];

struct gtl_sym { char name[24]; u64 addr; };
struct gtl_lib {
    bool used;
    u64 base;
    int nsym;
    struct gtl_sym syms[32];
    const u8 *icon;                 /* 内嵌图标 RGBA (无则 0) */
    int iw, ih;
};

static struct gtl_lib libs[LIB_MAXLIBS];
static u64 lib_used;                    /* 已占用的库区字节数 */

void lib_reset(void)
{
    for (int i = 0; i < LIB_MAXLIBS; i++)
        libs[i].used = false;
    lib_used = 0;
}

int lib_load(const char *path)
{
    if (!path || !path[0]) return -1;
    if (LIB_BASE <= (u64)__highbss_end)
        return -1;                       /* 库区与内核大缓冲冲突 */

    int slot = -1;
    for (int i = 0; i < LIB_MAXLIBS; i++)
        if (!libs[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    struct gt_stat st;
    if (fat_stat_pub(path, &st) != 0 || st.is_dir) return -1;
    if (st.size < 16 || st.size > LIB_MAX - lib_used) return -1;

    u64 base = LIB_BASE + lib_used;
    u32 got = 0;
    if (!fat_read_file(path, (void *)base, (u32)(LIB_MAX - lib_used), &got))
        return -1;
    /* 头部与导出表由 Rust 解析 (格式/边界全部在 gt_gtl_parse 内判断) */
    static struct gt_gtl_info gi;
    if (gt_gtl_parse((const u8 *)base, got, &gi) != 0)
        return -1;
    if (lib_used + gi.image_size > LIB_MAX)
        return -1;

    /* 镜像前移到库基址 (导出表已复制到 gi, 覆盖头部安全) */
    memmove((void *)base, (const u8 *)(base + gi.image_off), gi.image_size);

    libs[slot].used = true;
    libs[slot].base = base;
    libs[slot].nsym = (int)gi.count;
    for (u32 i = 0; i < gi.count; i++) {
        int n = 0;
        while (n < 23 && gi.exports[i].name[n]) {
            libs[slot].syms[i].name[n] = gi.exports[i].name[n];
            n++;
        }
        libs[slot].syms[i].name[n] = '\0';
        libs[slot].syms[i].addr = base + gi.exports[i].rva;
    }
    /* 内嵌图标位于镜像之后; memmove 只覆盖 [base, base+image_size),
       图标区不受影响, 可直接指向库区. */
    if (gi.icon_len) {
        libs[slot].icon = (const u8 *)(base + gi.icon_off + 8);
        libs[slot].iw = (int)gi.icon_w;
        libs[slot].ih = (int)gi.icon_h;
    } else {
        libs[slot].icon = 0;
        libs[slot].iw = libs[slot].ih = 0;
    }
    /* 占用长度必须覆盖图标, 否则后续库会覆盖它 */
    u32 span = gi.image_size;
    if (gi.icon_len)
        span = (gi.icon_off - gi.image_off) + gi.icon_len;
    lib_used += (span + 15u) & ~15u;
    return slot + 1;                     /* 句柄从 1 开始 */
}

u64 lib_sym(int handle, const char *name)
{
    if (handle < 1 || handle > LIB_MAXLIBS || !name) return 0;
    struct gtl_lib *l = &libs[handle - 1];
    if (!l->used) return 0;
    for (int i = 0; i < l->nsym; i++)
        if (!strcmp(l->syms[i].name, name))
            return l->syms[i].addr;
    return 0;
}

bool lib_icon(int handle, const u8 **rgba, int *w, int *h)
{
    if (handle < 1 || handle > LIB_MAXLIBS)
        return false;
    struct gtl_lib *l = &libs[handle - 1];
    if (!l->used || !l->icon || l->iw <= 0 || l->ih <= 0)
        return false;
    if (rgba) *rgba = l->icon;
    if (w) *w = l->iw;
    if (h) *h = l->ih;
    return true;
}
