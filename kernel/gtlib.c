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
struct gtl_lib { bool used; u64 base; int nsym; struct gtl_sym syms[32]; };

static struct gtl_lib libs[LIB_MAXLIBS];
static u64 lib_used;                    /* 已占用的库区字节数 */

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

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
    const u8 *h = (const u8 *)base;
    if (!(h[0] == 'G' && h[1] == 'T' && h[2] == 'L' && h[3] == '1'))
        return -1;

    u32 image_size = rd32(h + 8);
    u32 count = rd32(h + 12);
    if (count > 32) return -1;
    u32 imgoff = 16 + 28 * count;
    if (image_size == 0 || imgoff + image_size > got) return -1;
    if (lib_used + image_size > LIB_MAX) return -1;

    /* 先解析导出表 (memmove 会覆盖头部), 再前移 image */
    static u32 rvas[32];
    for (u32 i = 0; i < count; i++) {
        const u8 *e = h + 16 + 28 * i;
        int n = 0;
        while (n < 23 && e[n]) {
            libs[slot].syms[i].name[n] = (char)e[n];
            n++;
        }
        libs[slot].syms[i].name[n] = '\0';
        rvas[i] = rd32(e + 24);
    }

    u8 *img = (u8 *)(base + imgoff);
    memmove((void *)base, img, image_size);

    libs[slot].used = true;
    libs[slot].base = base;
    libs[slot].nsym = (int)count;
    for (u32 i = 0; i < count; i++)
        libs[slot].syms[i].addr = base + rvas[i];
    lib_used += (image_size + 15u) & ~15u;
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
