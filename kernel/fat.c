/* exFAT 文件系统. C:=dev0 的 exFAT 分区, MBR 分区表由内核解析; 盘尾隐藏
   启动分区 (type 0x11) 非本驱动挂载对象. 路径 "C:\DIR\FILE" 切到该卷,
   其余按当前卷, 相对路径由 shell 拼成绝对路径传入.

   目录项支持 GT-DOS 厂商扩展 (类型 0xE0):
     1 加密元数据 (GTE1 + AES-128-CBC)
     2 ACL (所有者与权限位)
     3 版本与 CRC32
   文件名按 ASCII 处理 (使用与 mkfs 一致的 Up-case 表映射 a-z). */
#include "gt.h"

#define SEC_SIZE 512
#define MAX_VOL  2
#define DIR_CACHE_SZ 32768

/* exFAT 目录项类型 */
#define ET_END     0x00
#define ET_BITMAP  0x81
#define ET_UPCASE  0x82
#define ET_LABEL   0x83
#define ET_FILE    0x85
#define ET_STREAM  0xC0
#define ET_NAME    0xC1
#define ET_VENDOR  0xE0

#define MAX_SET_ENTS 12
#define MAX_NAME     63

/* ------------------------------------------------------------------ 卷 */
struct vol {
    char letter;
    bool mounted;
    int  dev;
    u32  part_lba;
    u32  part_sectors;
    u8   sec_per_clus;
    u32  fat_offset;
    u32  fat_len;
    u32  heap_offset;
    u32  cluster_count;
    u32  root_clus;
    u32  bitmap_clus;
    u32  bitmap_len;
};

static struct vol vols[MAX_VOL];
static int nvol;
static int cur;                       /* 当前卷索引 (shell 盘符) */
static int work;                      /* 本次操作卷索引 (路径解析用) */
static bool show_all;
static bool allow_sys;

static u8 scratch_a[SEC_SIZE];
static u8 scratch_b[SEC_SIZE];
static u8 scratch_c[SEC_SIZE];

static u8 bm_cache[8192] __attribute__((section(".highbss")));
static u8 dir_cache[DIR_CACHE_SZ] __attribute__((section(".highbss")));

#define V (&vols[work])
#define WOK (work >= 0 && work < nvol && vols[work].mounted)
#define CLUS_BYTES ((u32)V->sec_per_clus * SEC_SIZE)

/* ------------------------------------------------------------------ 工具 */
static u16 rd_le16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
static u32 rd_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd_le64(const u8 *p)
{
    return (u64)rd_le32(p) | ((u64)rd_le32(p + 4) << 32);
}
static void wr_le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr_le32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static void wr_le64(u8 *p, u64 v)
{
    wr_le32(p, (u32)v);
    wr_le32(p + 4, (u32)(v >> 32));
}

static u16 rr1_16(u16 h)
{
    return (u16)(((u32)h >> 1) | ((u32)h << 15));
}

static u32 rr1_32(u32 h)
{
    return (h >> 1) | (h << 31);
}

static u16 name_hash(const char *name)
{
    u16 h = 0;
    for (const u8 *p = (const u8 *)name; *p; p++) {
        u16 u = (u16)*p;
        if (u >= 'a' && u <= 'z')
            u = (u16)(u - 32);
        h = (u16)(rr1_16(h) + (u & 0xFF));
        h = (u16)(rr1_16(h) + (u >> 8));
    }
    return h;
}

static u16 set_checksum(const u8 *ents, int count)
{
    u16 cs = 0;
    for (int i = 0; i < count * 32; i++) {
        if (i == 2 || i == 3)
            continue;
        cs = (u16)(rr1_16(cs) + ents[i]);
    }
    return cs;
}

static u32 exfat_stamp(void)
{
    int y, mo, d, h, mi, s;
    rtc_read(&y, &mo, &d, &h, &mi, &s);
    if (y < 1980) y = 1980;
    u16 dt = (u16)(((y - 1980) << 9) | (mo << 5) | d);
    u16 tm = (u16)((h << 11) | (mi << 5) | (s / 2));
    return ((u32)dt << 16) | tm;
}

/* ---------------------------------------------------------- 路径盘符 */
static const char *use_path(const char *path)
{
    if (path && path[0] && path[1] == ':') {
        char l = (char)toupper_(path[0]);
        int i = 0;
        for (; i < nvol; i++)
            if (vols[i].letter == l && vols[i].mounted)
                break;
        if (i >= nvol)
            return 0;
        work = i;
        return path + 2;
    }
    work = cur;
    if (!WOK)
        return 0;
    return path;
}

bool fat_ready(void) { return cur < nvol && vols[cur].mounted; }

bool fat_ready_drive(char letter)
{
    letter = (char)toupper_(letter);
    for (int i = 0; i < nvol; i++)
        if (vols[i].letter == letter && vols[i].mounted)
            return true;
    return false;
}

bool fat_set_drive(char letter)
{
    letter = (char)toupper_(letter);
    for (int i = 0; i < nvol; i++) {
        if (vols[i].letter == letter && vols[i].mounted) {
            cur = i;
            return true;
        }
    }
    return false;
}

char fat_drive(void) { return vols[cur].letter; }
void fat_set_show_all(bool on) { show_all = on; }
int fat_drive_count(void) { return nvol; }
char fat_drive_letter(int i) { return (i >= 0 && i < nvol) ? vols[i].letter : 0; }

/* ------------------------------------------------------------------ 底层 */
static bool read_sect(u32 lba, void *buf)
{
    return ata_read_dev(V->dev, lba, 1, buf);
}
static bool write_sect(u32 lba, const void *buf)
{
    return ata_write_dev(V->dev, lba, 1, buf);
}

static u32 clus_to_sect(u32 c)
{
    return V->part_lba + V->heap_offset + (c - 2) * (u32)V->sec_per_clus;
}

static bool is_eoc(u32 x) { return x >= 0xFFFFFFF8u; }

static u32 fat_get(u32 n)
{
    u32 byte_off = n * 4;
    u32 lba = V->part_lba + V->fat_offset + byte_off / SEC_SIZE;
    if (!read_sect(lba, scratch_a))
        return 0;
    return rd_le32(scratch_a + byte_off % SEC_SIZE);
}

static bool fat_set(u32 n, u32 val)
{
    u32 byte_off = n * 4;
    u32 lba = V->part_lba + V->fat_offset + byte_off / SEC_SIZE;
    if (!read_sect(lba, scratch_a))
        return false;
    wr_le32(scratch_a + byte_off % SEC_SIZE, val);
    return write_sect(lba, scratch_a);
}

/* --------------------------------------------------------- 分配位图 */
static bool bm_load(void)
{
    if (V->bitmap_len == 0 || V->bitmap_len > sizeof(bm_cache))
        return false;
    u32 c = V->bitmap_clus;
    u32 done = 0;
    while (done < V->bitmap_len) {
        for (u32 s = 0; s < (u32)V->sec_per_clus && done + s * SEC_SIZE < V->bitmap_len; s++) {
            u32 chunk = V->bitmap_len - done - s * SEC_SIZE;
            if (chunk > SEC_SIZE) chunk = SEC_SIZE;
            if (!read_sect(clus_to_sect(c) + s, bm_cache + done + s * SEC_SIZE))
                return false;
            (void)chunk;
        }
        done += CLUS_BYTES;
        if (done < V->bitmap_len) {
            u32 nxt = fat_get(c);
            if (is_eoc(nxt) || nxt < 2)
                return false;
            c = nxt;
        }
    }
    return true;
}

static bool bm_flush(void)
{
    u32 c = V->bitmap_clus;
    u32 done = 0;
    while (done < V->bitmap_len) {
        for (u32 s = 0; s < (u32)V->sec_per_clus && done + s * SEC_SIZE < V->bitmap_len; s++) {
            u8 sect[SEC_SIZE];
            u32 chunk = V->bitmap_len - done - s * SEC_SIZE;
            if (chunk > SEC_SIZE) chunk = SEC_SIZE;
            memset(sect, 0, SEC_SIZE);
            memcpy(sect, bm_cache + done + s * SEC_SIZE, chunk);
            if (!write_sect(clus_to_sect(c) + s, sect))
                return false;
        }
        done += CLUS_BYTES;
        if (done < V->bitmap_len) {
            u32 nxt = fat_get(c);
            if (is_eoc(nxt) || nxt < 2)
                return false;
            c = nxt;
        }
    }
    return true;
}

static bool bm_test(u32 c)
{
    u32 bit = c - 2;
    if ((bit >> 3) >= V->bitmap_len)
        return true;
    return (bm_cache[bit >> 3] & (u8)(1u << (bit & 7))) != 0;
}

static void bm_mark(u32 c, int on)
{
    u32 bit = c - 2;
    if ((bit >> 3) >= V->bitmap_len)
        return;
    if (on)
        bm_cache[bit >> 3] |= (u8)(1u << (bit & 7));
    else
        bm_cache[bit >> 3] &= (u8)~(1u << (bit & 7));
}

static u32 alloc_cluster(void)
{
    for (u32 c = 2; c < V->cluster_count + 2; c++) {
        if (!bm_test(c)) {
            bm_mark(c, 1);
            bm_flush();
            return c;
        }
    }
    return 0;
}

static void free_chain(u32 c)
{
    bool dirty = false;
    while (c >= 2 && c < V->cluster_count + 2) {
        u32 nxt = fat_get(c);
        fat_set(c, 0);
        bm_mark(c, 0);
        dirty = true;
        if (is_eoc(nxt) || nxt < 2)
            break;
        c = nxt;
    }
    if (dirty)
        bm_flush();
}

/* ------------------------------------------------------------ 目录载入 */
struct dload {
    u32 first;
    u32 nclus;
    u32 nent;
};

static bool dir_load(u32 first, struct dload *dl)
{
    u32 c = first;
    u32 off = 0;
    u32 n = 0;
    for (;;) {
        if (off + CLUS_BYTES > DIR_CACHE_SZ)
            return false;
        for (u32 s = 0; s < (u32)V->sec_per_clus; s++) {
            if (!read_sect(clus_to_sect(c) + s, dir_cache + off + s * SEC_SIZE))
                return false;
        }
        off += CLUS_BYTES;
        n++;
        u32 nxt = fat_get(c);
        if (is_eoc(nxt) || nxt < 2)
            break;
        c = nxt;
    }
    if (off < DIR_CACHE_SZ)
        memset(dir_cache + off, 0, DIR_CACHE_SZ - off);
    dl->first = first;
    dl->nclus = n;
    dl->nent = off / 32;
    return true;
}

static bool dir_flush(const struct dload *dl)
{
    u32 c = dl->first;
    u32 off = 0;
    for (u32 i = 0; i < dl->nclus; i++) {
        for (u32 s = 0; s < (u32)V->sec_per_clus; s++) {
            if (!write_sect(clus_to_sect(c) + s, dir_cache + off + s * SEC_SIZE))
                return false;
        }
        off += CLUS_BYTES;
        u32 nxt = fat_get(c);
        if (is_eoc(nxt) || nxt < 2)
            break;
        c = nxt;
    }
    return true;
}

static bool dir_extend(struct dload *dl)
{
    u32 c = dl->first;
    for (;;) {
        u32 nxt = fat_get(c);
        if (is_eoc(nxt) || nxt < 2)
            break;
        c = nxt;
    }
    u32 off = dl->nclus * CLUS_BYTES;
    if (off + CLUS_BYTES > DIR_CACHE_SZ)
        return false;
    u32 n = alloc_cluster();
    if (n == 0)
        return false;
    fat_set(c, n);
    fat_set(n, 0xFFFFFFFFu);
    memset(dir_cache + off, 0, CLUS_BYTES);
    dl->nclus++;
    dl->nent = (off + CLUS_BYTES) / 32;
    return true;
}

static int dir_find_free(const struct dload *dl, int count)
{
    int run = 0;
    for (u32 i = 0; i < dl->nent; i++) {
        u8 t = dir_cache[i * 32];
        if (t & 0x80) {
            run = 0;
        } else {
            if (++run >= count)
                return (int)(i + 1 - count);
        }
    }
    return -1;
}

/* ------------------------------------------------------------ 目录项集 */
struct finfo {
    bool used;
    int  idx;
    int  count;
    char name[MAX_NAME + 1];
    u8   attr;
    u32  first;
    u64  size;
    bool no_fat;
    u32  plain_size;
    u8   xflags;
    u32  version;
    u32  crc;
    char owner[16];
    u16  mode;
};

static void set_defaults(struct finfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    strncpy(fi->owner, "SYSTEM", sizeof(fi->owner) - 1);
    fi->version = 1;
}

/* 解析 file 目录项及其后续项, idx 为 file 项在目录中的下标 */
static bool set_parse(int idx, struct finfo *fi)
{
    const u8 *e = dir_cache + idx * 32;
    int count = e[1];
    if (count < 2 || count > MAX_SET_ENTS - 1)
        return false;
    if ((u32)(idx + 1 + count) * 32 > DIR_CACHE_SZ)
        return false;

    if (set_checksum(e, 1 + count) != rd_le16(e + 2))
        return false;

    set_defaults(fi);
    fi->used = true;
    fi->idx = idx;
    fi->count = 1 + count;
    fi->attr = (u8)rd_le16(e + 4);

    const u8 *stream = 0;
    int nlen = 0;
    for (int i = 1; i <= count; i++) {
        const u8 *s = e + i * 32;
        if (s[0] == ET_STREAM) {
            stream = s;
            nlen = s[3];
            fi->no_fat = (s[1] & 0x02) != 0;
            fi->size = rd_le64(s + 8);
            fi->first = rd_le32(s + 20);
            if (rd_le64(s + 24) > fi->size)
                fi->size = rd_le64(s + 24);
        }
    }
    if (!stream)
        return false;

    int n = 0;
    for (int i = 1; i <= count && n < nlen; i++) {
        const u8 *s = e + i * 32;
        if (s[0] != ET_NAME)
            continue;
        for (int j = 0; j < 15 && n < nlen && n < MAX_NAME; j++) {
            u16 ch = rd_le16(s + 2 + j * 2);
            fi->name[n++] = (ch && ch < 128) ? (char)ch : '?';
        }
    }
    fi->name[n] = 0;
    if (n == 0)
        return false;

    fi->plain_size = (u32)(fi->size > 0xFFFFFFFFu ? 0xFFFFFFFFu : fi->size);
    for (int i = 1; i <= count; i++) {
        const u8 *s = e + i * 32;
        if (s[0] != ET_VENDOR)
            continue;
        if (s[1] == GT_XTYPE_ENCRYPT) {
            fi->xflags = s[4];
            fi->plain_size = rd_le32(s + 8);
            fi->crc = rd_le32(s + 16);
        } else if (s[1] == GT_XTYPE_ACL) {
            fi->mode = rd_le16(s + 4);
            memcpy(fi->owner, s + 8, 16);
            fi->owner[15] = 0;
        } else if (s[1] == GT_XTYPE_VERSION) {
            fi->version = rd_le32(s + 4);
            fi->crc = rd_le32(s + 8);
        }
    }
    if (!fi->owner[0])
        strncpy(fi->owner, "SYSTEM", sizeof(fi->owner) - 1);
    return true;
}

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper_((u8)*a) != toupper_((u8)*b))
            return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* 在已载入目录中查找名字: 返回 1=文件, 0=未找到 */
static int dir_lookup(const char *name, struct finfo *out)
{
    u32 i = 0;
    while (i * 32 < DIR_CACHE_SZ) {
        u8 t = dir_cache[i * 32];
        if (t == ET_END)
            break;
        if (t == ET_FILE) {
            struct finfo fi;
            if (set_parse((int)i, &fi)) {
                if (name_eq(fi.name, name)) {
                    if (out) *out = fi;
                    return 1;
                }
                i += (u32)fi.count;
                continue;
            }
        }
        i++;
    }
    return 0;
}

/* 构建目录项集, 返回项数 */
static int build_set(u8 *out, const char *name, u8 attr, u32 first, u64 size,
                     u32 plain_size, const char *owner, u16 mode,
                     u8 xflags, u32 crc)
{
    int nlen = (int)strlen(name);
    if (nlen > MAX_NAME)
        nlen = MAX_NAME;
    int nname = (nlen + 14) / 15;
    int secondary = 4 + nname;           /* stream + 名称 + 3 个扩展项 */
    int total = 1 + secondary;
    if (total > MAX_SET_ENTS)
        return 0;
    memset(out, 0, (size_t)total * 32);

    u32 ts = exfat_stamp();
    u8 *e = out;
    e[0] = ET_FILE;
    e[1] = (u8)secondary;
    wr_le16(e + 4, attr);
    wr_le32(e + 8, ts);
    wr_le32(e + 12, ts);
    wr_le32(e + 16, ts);

    e = out + 32;
    e[0] = ET_STREAM;
    e[1] = (u8)(first ? 0x01 : 0x00);
    e[3] = (u8)nlen;
    wr_le16(e + 4, name_hash(name));
    wr_le64(e + 8, size);
    wr_le32(e + 20, first);
    wr_le64(e + 24, size);

    for (int i = 0; i < nname; i++) {
        e = out + 64 + i * 32;
        e[0] = ET_NAME;
        for (int j = 0; j < 15; j++) {
            int k = i * 15 + j;
            wr_le16(e + 2 + j * 2, (k < nlen) ? (u16)(u8)name[k] : 0);
        }
    }

    e = out + 64 + nname * 32;
    e[0] = ET_VENDOR;
    e[1] = GT_XTYPE_ENCRYPT;
    e[4] = xflags;
    e[5] = 1;
    wr_le32(e + 8, plain_size);
    wr_le32(e + 16, crc);

    e += 32;
    e[0] = ET_VENDOR;
    e[1] = GT_XTYPE_ACL;
    wr_le16(e + 4, mode);
    strncpy((char *)e + 8, owner, 16);

    e += 32;
    e[0] = ET_VENDOR;
    e[1] = GT_XTYPE_VERSION;
    wr_le32(e + 4, 1);
    wr_le32(e + 8, crc);

    wr_le16(out + 2, set_checksum(out, total));
    return total;
}

static bool dir_insert(u32 dir_clus, const u8 *ents, int count)
{
    struct dload dl;
    if (!dir_load(dir_clus, &dl))
        return false;
    int slot = dir_find_free(&dl, count);
    if (slot < 0) {
        if (!dir_extend(&dl))
            return false;
        slot = dir_find_free(&dl, count);
        if (slot < 0)
            return false;
    }
    memcpy(dir_cache + (u32)slot * 32, ents, (size_t)count * 32);
    return dir_flush(&dl);
}

static bool dir_remove(u32 dir_clus, int idx, int count)
{
    struct dload dl;
    if (!dir_load(dir_clus, &dl))
        return false;
    for (int i = 0; i < count; i++)
        dir_cache[(u32)(idx + i) * 32] &= 0x7F;
    return dir_flush(&dl);
}

/* ---------------------------------------------------------- 路径解析 */
static u32 resolve_parent(const char *path, char *base, int cap)
{
    char comp[MAX_NAME + 1];
    u32 dir_clus = V->root_clus;

    const char *slash = 0;
    for (const char *q = path; *q; q++)
        if (*q == '\\' || *q == '/')
            slash = q;
    const char *b = slash ? slash + 1 : path;
    const char *end = slash ? slash : path;

    const char *p = path;
    while (p < end) {
        const char *sep = p;
        while (sep < end && *sep != '\\' && *sep != '/')
            sep++;
        size_t n = (size_t)(sep - p);
        if (n == 0) { p = sep + 1; continue; }
        if (n > MAX_NAME)
            return 0xFFFFFFFFu;
        memcpy(comp, p, n);
        comp[n] = 0;

        struct dload dl;
        if (!dir_load(dir_clus, &dl))
            return 0xFFFFFFFFu;
        struct finfo fi;
        if (!dir_lookup(comp, &fi))
            return 0xFFFFFFFFu;
        if (!(fi.attr & ATTR_DIR))
            return 0xFFFFFFFFu;
        dir_clus = fi.first;
        p = sep + 1;
    }

    size_t n = strlen(b);
    if (n == 0 || n > MAX_NAME || (int)n >= cap)
        return 0xFFFFFFFFu;
    memcpy(base, b, n);
    base[n] = 0;
    return dir_clus;
}

static bool get_entry(const char *path, struct finfo *fi, u32 *parent)
{
    char base[MAX_NAME + 1];
    u32 dir_clus = resolve_parent(path, base, sizeof(base));
    if (dir_clus == 0xFFFFFFFFu)
        return false;
    struct dload dl;
    if (!dir_load(dir_clus, &dl))
        return false;
    if (!dir_lookup(base, fi))
        return false;
    if (parent) *parent = dir_clus;
    return true;
}

/* --------------------------------------------------------------- 保护 */
static bool path_protected(const char *path)
{
    while (*path == '\\' || *path == '/')
        path++;
    if (!(path[0] == 'G' || path[0] == 'g')) return false;
    const char *ref = "GT-DOS";
    for (int i = 0; i < 6; i++)
        if (toupper_(path[i]) != ref[i])
            return false;
    char nxt = path[6];
    return (nxt == '\0' || nxt == '\\' || nxt == '/');
}

static bool outside_user_home(const char *path)
{
    if (user_is_admin())
        return false;
    const char *name = user_name();
    if (!name || !name[0])
        return true;
    while (*path == '\\' || *path == '/')
        path++;
    const char *ref = "USERS\\";
    for (int i = 0; i < 6; i++)
        if (toupper_(path[i]) != ref[i])
            return true;
    path += 6;
    int j = 0;
    while (name[j]) {
        if (path[j] == '\0' || toupper_(path[j]) != toupper_(name[j]))
            return true;
        j++;
    }
    char nxt = path[j];
    if (nxt != '\0' && nxt != '\\' && nxt != '/')
        return true;
    return false;
}

static bool acl_denied(const struct finfo *fi)
{
    if (user_is_admin())
        return false;
    const char *u = user_name();
    if (!u || !u[0])
        return true;
    return !name_eq(fi->owner, u);
}

static bool sys_write_denied(const char *path)
{
    if (allow_sys)
        return false;
    if (path_protected(path)) {
        kprintf("System file/folder protected.\n");
        return true;
    }
    if (outside_user_home(path)) {
        kprintf("Permission denied: outside your home directory.\n");
        return true;
    }
    return false;
}

void fat_set_allow_sys(bool on) { allow_sys = on; }

bool fat_path_protected(const char *path)
{
    if (path && path[0] && path[1] == ':')
        path += 2;
    return path_protected(path);
}

/* --------------------------------------------------------------- 挂载 */
static bool boot_checksum_ok(u32 part_lba)
{
    u32 cs = 0;
    for (u32 s = 0; s < 11; s++) {
        if (!read_sect(part_lba + s, scratch_a))
            return false;
        for (u32 i = 0; i < SEC_SIZE; i++) {
            u32 gi = s * SEC_SIZE + i;
            if (gi == 106 || gi == 107 || gi == 112 || gi == 113)
                continue;
            cs = rr1_32(cs) + scratch_a[i];
        }
    }
    if (!read_sect(part_lba + 11, scratch_b))
        return false;
    for (u32 i = 0; i < SEC_SIZE; i += 4)
        if (rd_le32(scratch_b + i) != cs)
            return false;
    return true;
}

static bool mount_part(int dev, char letter, u32 part_lba, u32 part_sectors)
{
    struct vol *nv = 0;
    for (int i = 0; i < nvol; i++)
        if (vols[i].dev == dev) { nv = &vols[i]; break; }
    if (!nv) {
        if (nvol >= MAX_VOL)
            return false;
        nv = &vols[nvol];
        nvol++;
    }
    memset(nv, 0, sizeof(*nv));
    nv->dev = dev;
    nv->letter = (char)toupper_(letter);
    nv->part_lba = part_lba;
    nv->part_sectors = part_sectors;
    work = cur = (int)(nv - vols);

    u8 *bs = scratch_a;
    if (!read_sect(part_lba, bs))
        goto fail;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0)
        goto fail;
    if (bs[0x6C] != 9)                    /* 512 字节/扇区 */
        goto fail;
    if (bs[0x6D] < 1 || bs[0x6D] > 8)
        goto fail;

    nv->sec_per_clus = (u8)(1u << bs[0x6D]);
    nv->fat_offset = rd_le32(bs + 0x50);
    nv->fat_len = rd_le32(bs + 0x54);
    nv->heap_offset = rd_le32(bs + 0x58);
    nv->cluster_count = rd_le32(bs + 0x5C);
    nv->root_clus = rd_le32(bs + 0x60);
    if (nv->cluster_count < 8 || nv->root_clus < 2)
        goto fail;
    if ((u64)nv->heap_offset + (u64)nv->cluster_count * nv->sec_per_clus > part_sectors)
        goto fail;

    if (!boot_checksum_ok(part_lba))
        goto fail;

    /* 从根目录找分配位图 */
    struct dload dl;
    if (!dir_load(nv->root_clus, &dl))
        goto fail;
    bool found = false;
    for (u32 i = 0; i < dl.nent; i++) {
        u8 *e = dir_cache + i * 32;
        if (e[0] == ET_BITMAP) {
            nv->bitmap_clus = rd_le32(e + 20);
            nv->bitmap_len = rd_le32(e + 24);
            found = true;
            break;
        }
        if (e[0] == ET_END)
            break;
    }
    if (!found || nv->bitmap_clus < 2 || nv->bitmap_len == 0)
        goto fail;
    if (!bm_load())
        goto fail;

    nv->mounted = true;
    return true;

fail:
    if (nv == &vols[nvol - 1])
        nvol--;
    return false;
}

bool fat_mount(void)
{
    nvol = 0;
    cur = 0;
    work = 0;

    if (!ata_present_dev(0))
        return false;
    u8 mbr[SEC_SIZE];
    work = 0;
    if (!ata_read_dev(0, 0, 1, mbr))
        return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return false;

    char l0 = gcfg.sys_drive ? gcfg.sys_drive : 'C';
    for (int i = 0; i < 4; i++) {
        const u8 *pe = mbr + 0x1BE + i * 16;
        u32 lba = rd_le32(pe + 8);
        u32 cnt = rd_le32(pe + 12);
        if (pe[4] == 0 || lba == 0 || cnt == 0)
            continue;
        if (pe[4] != 0x07)               /* 优先 exFAT 分区类型 */
            continue;
        if (mount_part(0, l0, lba, cnt))
            return true;
    }
    /* 分区类型不规范时按引导扇区探测 */
    for (int i = 0; i < 4; i++) {
        const u8 *pe = mbr + 0x1BE + i * 16;
        u32 lba = rd_le32(pe + 8);
        u32 cnt = rd_le32(pe + 12);
        if (pe[4] == 0 || lba == 0 || cnt == 0)
            continue;
        if (mount_part(0, l0, lba, cnt))
            return true;
    }
    return false;
}

u32 fat_total_clusters(void) { work = cur; return WOK ? V->cluster_count : 0; }

u32 fat_free_clusters(void)
{
    work = cur;
    if (!WOK)
        return 0;
    u32 free_n = 0;
    for (u32 c = 2; c < V->cluster_count + 2; c++)
        if (!bm_test(c))
            free_n++;
    return free_n;
}

/* ---------------------------------------------------------------- 列出 */
int fat_list_dir(const char *path, struct fat_dirent *out, int max)
{
    path = use_path(path);
    if (!path || !WOK)
        return -1;
    while (*path == '\\' || *path == '/')
        path++;

    u32 dir_clus = V->root_clus;
    if (path[0]) {
        struct finfo fi;
        if (!get_entry(path, &fi, 0))
            return -1;
        if (!(fi.attr & ATTR_DIR))
            return -1;
        dir_clus = fi.first;
    }

    struct dload dl;
    if (!dir_load(dir_clus, &dl))
        return -1;

    int n = 0;
    u32 i = 0;
    while (i < dl.nent) {
        u8 *e = dir_cache + i * 32;
        if (e[0] == ET_END)
            break;
        if (e[0] == ET_FILE) {
            struct finfo fi;
            if (set_parse((int)i, &fi)) {
                if (!show_all && (fi.attr & (ATTR_HID | ATTR_SYS))) {
                    /* 过滤隐藏/系统项 */
                } else if (n < max) {
                    int k = 0;
                    while (fi.name[k] && k < FAT_NAME_LEN - 1) {
                        out[n].name[k] = fi.name[k];
                        k++;
                    }
                    out[n].name[k] = 0;
                    u32 sz = (fi.xflags & GT_XFLAG_ENCRYPTED) ? fi.plain_size
                                                              : (u32)fi.size;
                    out[n].size = sz;
                    out[n].first_cluster = (u16)fi.first;
                    out[n].attrs = fi.attr;
                    out[n].is_dir = (fi.attr & ATTR_DIR) != 0;
                    n++;
                }
                i += (u32)fi.count;
                continue;
            }
        }
        i++;
    }
    return n;
}

bool fat_find(const char *name, struct fat_dirent *out)
{
    name = use_path(name);
    if (!name || !WOK)
        return false;
    struct finfo fi;
    if (!get_entry(name, &fi, 0))
        return false;
    int k = 0;
    while (fi.name[k] && k < FAT_NAME_LEN - 1) {
        out->name[k] = fi.name[k];
        k++;
    }
    out->name[k] = 0;
    out->size = (fi.xflags & GT_XFLAG_ENCRYPTED) ? fi.plain_size : (u32)fi.size;
    out->first_cluster = (u16)fi.first;
    out->attrs = fi.attr;
    out->is_dir = (fi.attr & ATTR_DIR) != 0;
    return true;
}

/* ---------------------------------------------------------------- 读取 */
static bool read_data(const struct finfo *fi, void *buf, u32 max, u32 *out_len)
{
    u8 *dst = (u8 *)buf;
    u64 remain = fi->size;
    u32 got = 0;
    u32 c = fi->first;

    if (fi->size == 0 || fi->first < 2) {
        if (out_len) *out_len = 0;
        return true;
    }

    while (remain > 0 && c >= 2 && c < V->cluster_count + 2) {
        u32 base = clus_to_sect(c);
        for (u32 s = 0; s < (u32)V->sec_per_clus && remain > 0; s++) {
            u32 chunk = (remain > SEC_SIZE) ? SEC_SIZE : (u32)remain;
            if (got + chunk > max)
                chunk = max - got;
            if (chunk == 0) {
                if (out_len) *out_len = got;
                return true;
            }
            if (!read_sect(base + s, scratch_c))
                return false;
            memcpy(dst + got, scratch_c, chunk);
            got += chunk;
            remain -= ((remain > SEC_SIZE) ? SEC_SIZE : remain);
        }
        if (fi->no_fat)
            c++;
        else
            c = fat_get(c);
    }
    if (out_len) *out_len = got;
    return true;
}

bool fat_read_file(const char *name, void *buf, u32 max, u32 *out_len)
{
    u32 got = 0;
    if (!fat_read_dir_file(0, name, buf, max, &got))
        return false;
    u8 *b = (u8 *)buf;
    if (got >= 8 && b[0] == 'G' && b[1] == 'T' && b[2] == 'E' && b[3] == '1') {
        u32 plen = rd_le32(b + 4);
        u32 dlen = gt_decrypt(b + 8, got - 8, b);
        if (dlen)
            got = (plen < dlen) ? plen : dlen;
    }
    if (out_len) *out_len = got;
    return true;
}

bool fat_read_dir_file(const char *dir, const char *name, void *buf,
                       u32 max, u32 *out_len)
{
    char path[160];
    const char *p;

    if (dir && dir[0]) {
        if (strlen(dir) + strlen(name) + 2 >= sizeof(path))
            return false;
        strncpy(path, dir, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        size_t l = strlen(path);
        path[l] = '\\';
        strncpy(path + l + 1, name, sizeof(path) - l - 2);
        p = path;
    } else {
        p = name;
    }

    p = use_path(p);
    if (!p || !WOK)
        return false;

    struct finfo fi;
    if (!get_entry(p, &fi, 0))
        return false;
    if (fi.attr & ATTR_DIR)
        return false;
    return read_data(&fi, buf, max, out_len);
}

/* ---------------------------------------------------------------- 写入 */
static u8 enc_buf[4200] __attribute__((section(".highbss")));

bool fat_create(const char *name, const void *data, u32 len)
{
    name = use_path(name);
    if (!name || !WOK)
        return false;
    if (sys_write_denied(name))
        return false;

    char base[MAX_NAME + 1];
    u32 parent = resolve_parent(name, base, sizeof(base));
    if (parent == 0xFFFFFFFFu)
        return false;

    struct finfo old;
    bool exists = false;
    {
        struct dload dl;
        if (!dir_load(parent, &dl))
            return false;
        exists = dir_lookup(base, &old) != 0;
    }
    if (exists) {
        if (old.attr & ATTR_DIR)
            return false;
        if (old.attr & (ATTR_RO | ATTR_SYS))
            return false;
        if (acl_denied(&old))
            return false;
    }

    u32 crc = gt_crc32(data, len);
    const u8 *src = (const u8 *)data;
    u32 dlen = len;
    u8 xflags = 0;

    if (path_protected(name)) {
        if (len + 8 + 16 > sizeof(enc_buf))
            return false;
        enc_buf[0] = 'G'; enc_buf[1] = 'T'; enc_buf[2] = 'E'; enc_buf[3] = '1';
        wr_le32(enc_buf + 4, len);
        u32 clen = gt_encrypt((const u8 *)data, len, enc_buf + 8);
        src = enc_buf;
        dlen = 8 + clen;
        xflags = GT_XFLAG_ENCRYPTED;
    }

    const char *owner = "SYSTEM";
    u16 mode = 0;
    const char *un = user_name();
    if (!path_protected(name) && un && un[0]) {
        owner = un;
        mode = GT_ACL_OWNER_WRITE | GT_ACL_OTHER_READ;
    }
    if (exists && old.owner[0] && !allow_sys && !path_protected(name)) {
        owner = old.owner;
        mode = old.mode;
    }

    if (exists)
        dir_remove(parent, old.idx, old.count);

    u32 first = 0, last = 0;
    u64 remain = dlen;
    u32 woff = 0;
    while (remain > 0) {
        u32 c = alloc_cluster();
        if (c == 0) {
            if (first) free_chain(first);
            return false;
        }
        fat_set(c, 0xFFFFFFFFu);
        if (!first)
            first = c;
        else
            fat_set(last, c);
        last = c;

        u32 chunk = (remain > CLUS_BYTES) ? CLUS_BYTES : (u32)remain;
        u32 done = 0;
        while (done < chunk) {
            u32 this = chunk - done > SEC_SIZE ? SEC_SIZE : chunk - done;
            memset(scratch_c, 0, SEC_SIZE);
            memcpy(scratch_c, src + woff + done, this);
            if (!write_sect(clus_to_sect(c) + done / SEC_SIZE, scratch_c)) {
                free_chain(first);
                return false;
            }
            done += this;
        }
        woff += chunk;
        remain -= chunk;
    }

    u8 ents[MAX_SET_ENTS * 32];
    int n = build_set(ents, base, ATTR_ARC, first, dlen, len, owner, mode,
                      xflags, crc);
    if (n == 0) {
        if (first) free_chain(first);
        return false;
    }
    if (!dir_insert(parent, ents, n)) {
        if (first) free_chain(first);
        return false;
    }
    return true;
}

bool fat_delete(const char *name)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    if (sys_write_denied(name))
        return false;

    struct finfo fi;
    u32 parent;
    if (!get_entry(name, &fi, &parent))
        return false;
    if (fi.attr & ATTR_DIR)
        return false;
    if (fi.attr & (ATTR_RO | ATTR_SYS))
        return false;
    if (acl_denied(&fi))
        return false;

    if (!dir_remove(parent, fi.idx, fi.count))
        return false;
    free_chain(fi.first);
    return true;
}

bool fat_mkdir(const char *name)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    if (sys_write_denied(name))
        return false;

    char base[MAX_NAME + 1];
    u32 parent = resolve_parent(name, base, sizeof(base));
    if (parent == 0xFFFFFFFFu)
        return false;

    struct dload pl;
    if (!dir_load(parent, &pl))
        return false;
    if (dir_lookup(base, 0))
        return false;

    u32 c = alloc_cluster();
    if (c == 0)
        return false;
    fat_set(c, 0xFFFFFFFFu);
    memset(scratch_c, 0, SEC_SIZE);
    for (u32 s = 0; s < (u32)V->sec_per_clus; s++)
        if (!write_sect(clus_to_sect(c) + s, scratch_c)) {
            free_chain(c);
            return false;
        }

    const char *owner = "SYSTEM";
    const char *un = user_name();
    if (un && un[0] && !allow_sys) {
        owner = un;
    }
    u8 ents[MAX_SET_ENTS * 32];
    int n = build_set(ents, base, ATTR_DIR, c, 0, 0, owner,
                      GT_ACL_OWNER_WRITE | GT_ACL_OTHER_READ, 0,
                      gt_crc32("", 0));
    if (n == 0 || !dir_insert(parent, ents, n)) {
        free_chain(c);
        return false;
    }
    return true;
}

bool fat_rmdir(const char *name)
{
    name = use_path(name);
    if (!name || !WOK)
        return false;
    if (sys_write_denied(name))
        return false;
    if (!name[0])
        return false;

    struct finfo fi;
    u32 parent;
    if (!get_entry(name, &fi, &parent))
        return false;
    if (!(fi.attr & ATTR_DIR))
        return false;
    if (acl_denied(&fi))
        return false;

    struct dload dl;
    if (!dir_load(fi.first, &dl))
        return false;
    u32 i = 0;
    while (i < dl.nent) {
        u8 t = dir_cache[i * 32];
        if (t == ET_END)
            break;
        if (t & 0x80)
            return false;                 /* 非空 */
        i++;
    }

    if (!dir_remove(parent, fi.idx, fi.count))
        return false;
    free_chain(fi.first);
    return true;
}

bool fat_setattr(const char *name, u8 attrs)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    if (sys_write_denied(name))
        return false;

    struct finfo fi;
    u32 parent;
    if (!get_entry(name, &fi, &parent))
        return false;
    if (acl_denied(&fi))
        return false;

    struct dload dl;
    if (!dir_load(parent, &dl))
        return false;
    u8 *e = dir_cache + (u32)fi.idx * 32;
    wr_le16(e + 4, attrs);
    wr_le16(e + 2, set_checksum(e, fi.count));
    return dir_flush(&dl);
}

bool fat_getattr(const char *name, u8 *attrs)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    struct finfo fi;
    if (!get_entry(name, &fi, 0))
        return false;
    *attrs = fi.attr;
    return true;
}

bool fat_touch(const char *name)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    if (sys_write_denied(name))
        return false;

    struct finfo fi;
    u32 parent;
    if (get_entry(name, &fi, &parent)) {
        if (acl_denied(&fi))
            return false;
        struct dload dl;
        if (!dir_load(parent, &dl))
            return false;
        u8 *e = dir_cache + (u32)fi.idx * 32;
        u32 ts = exfat_stamp();
        wr_le32(e + 12, ts);
        wr_le16(e + 2, set_checksum(e, fi.count));
        return dir_flush(&dl);
    }
    return fat_create(name, "", 0);
}

/* ------------------------------------------------------- 元数据 (公开) */
int fat_stat_pub(const char *path, struct gt_stat *out)
{
    if (!path || !path[0] || !out) return -1;
    struct fat_dirent d;
    if (!fat_find(path, &d)) return -1;
    out->size = d.size;
    out->attr = d.attrs;
    out->is_dir = d.is_dir ? 1 : 0;
    return 0;
}

int fat_statx(const char *path, struct gt_statx *out)
{
    if (!path || !path[0] || !out) return -1;
    path = use_path(path);
    if (!path || !WOK) return -1;
    struct finfo fi;
    if (!get_entry(path, &fi, 0)) return -1;
    memset(out, 0, sizeof(*out));
    out->size = (fi.xflags & GT_XFLAG_ENCRYPTED) ? fi.plain_size : (u32)fi.size;
    out->attr = fi.attr;
    out->is_dir = (fi.attr & ATTR_DIR) ? 1 : 0;
    out->flags = fi.xflags;
    out->version = fi.version;
    out->crc = fi.crc;
    strncpy(out->owner, fi.owner, 15);
    out->mode = fi.mode;
    return 0;
}

/* ---------------------------------------------------- 文件描述符层 (INT 0x21) */
#define FD_BUFSZ 4096
static u8 fd_buf[FD_MAX][FD_BUFSZ] __attribute__((section(".highbss")));
struct fd_ent {
    bool used; u32 len; u32 pos; bool dirty; bool isdir; u32 dirpos; char path[160];
};
static struct fd_ent fdtab[FD_MAX];

int fat_fd_open(const char *path, int flags)
{
    if (!path || !path[0]) return -1;
    int slot = -1;
    for (int i = 0; i < FD_MAX; i++)
        if (!fdtab[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    struct fat_dirent d;
    bool exists = fat_find(path, &d);
    if (!exists) {
        if (!(flags & O_CREAT)) return -1;
        if (!fat_create(path, "", 0)) return -1;
    } else if (flags & O_TRUNC) {
        fat_create(path, "", 0);
    }
    u32 got = 0;
    if (!fat_read_file(path, fd_buf[slot], FD_BUFSZ, &got)) got = 0;
    fdtab[slot].used = true;
    fdtab[slot].isdir = exists && d.is_dir;
    fdtab[slot].dirpos = 0;
    fdtab[slot].len = fdtab[slot].isdir ? 0 : got;
    fdtab[slot].pos = 0;
    fdtab[slot].dirty = false;
    int n = 0;
    while (path[n] && n < 159) { fdtab[slot].path[n] = path[n]; n++; }
    fdtab[slot].path[n] = '\0';
    return slot + 3;
}

int fat_fd_read(int fd, void *buf, u32 n)
{
    if (fd < 3 || fd >= 3 + FD_MAX) return -1;
    GT_ASSERT_BOUNDED(fd - 3, FD_MAX);
    struct fd_ent *e = &fdtab[fd - 3];
    if (!e->used) return -1;
    u8 *dst = (u8 *)buf;
    u32 c = 0;
    while (c < n && e->pos < e->len) { dst[c++] = fd_buf[fd - 3][e->pos++]; }
    return (int)c;
}

int fat_fd_write(int fd, const void *buf, u32 n)
{
    if (fd < 3 || fd >= 3 + FD_MAX) return -1;
    GT_ASSERT_BOUNDED(fd - 3, FD_MAX);
    struct fd_ent *e = &fdtab[fd - 3];
    if (!e->used) return -1;
    const u8 *src = (const u8 *)buf;
    u32 c = 0;
    while (c < n && e->pos < FD_BUFSZ) {
        fd_buf[fd - 3][e->pos++] = src[c++];
        if (e->pos > e->len) e->len = e->pos;
    }
    e->dirty = true;
    return (int)c;
}

int fat_fd_close(int fd)
{
    if (fd < 3 || fd >= 3 + FD_MAX) return -1;
    struct fd_ent *e = &fdtab[fd - 3];
    if (!e->used) return -1;
    int rc = 0;
    if (e->dirty) {
        if (!fat_create(e->path, fd_buf[fd - 3], e->len)) rc = -1;
        e->dirty = false;
    }
    e->used = false; e->len = 0; e->pos = 0;
    e->isdir = false; e->dirpos = 0;
    return rc;
}

int fat_fd_seek(int fd, int offset, int whence)
{
    if (fd < 3 || fd >= 3 + FD_MAX) return -1;
    struct fd_ent *e = &fdtab[fd - 3];
    if (!e->used || e->isdir) return -1;
    long np;
    if (whence == 1)      np = (long)e->pos + offset;
    else if (whence == 2) np = (long)e->len + offset;
    else                  np = offset;
    if (np < 0 || (u32)np > e->len) return -1;
    e->pos = (u32)np;
    return (int)e->pos;
}

int fat_fd_opendir(const char *path)
{
    return fat_fd_open(path, O_RDONLY);
}

int fat_fd_readdir(int fd, struct fat_dirent *out)
{
    if (fd < 3 || fd >= 3 + FD_MAX || !out) return -1;
    struct fd_ent *e = &fdtab[fd - 3];
    if (!e->used || !e->isdir) return -1;
    static struct fat_dirent arr[32];
    int n = fat_list_dir(e->path, arr, 32);
    if (n < 0) n = 0;
    if (e->dirpos >= (u32)n) return 0;
    *out = arr[e->dirpos++];
    return 1;
}
