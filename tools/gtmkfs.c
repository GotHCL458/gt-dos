/* GT-DOS 构建期 exFAT 格式化 / 填充 / 加密工具 (独立纯 C, 不依赖构建脚本).
   用法:
     gtmkfs <image> <partition-lba> <sectors> <manifest>

   manifest 每行:
     D <虚拟路径> <属性hex> <所有者>
     F <虚拟路径> <宿主文件> <属性hex> <所有者> <是否加密 0|1>
   虚拟路径以 \ 分隔, 父目录须先声明 (D 行).

   写入内容:
     - exFAT 引导区 (含校验扇区)、FAT、分配位图、Up-case 表
     - 每个文件/目录附带 3 个厂商扩展目录项 (0xE0):
         1 加密元数据 (标志/算法/明文长度/CRC32)
         2 ACL (所有者/权限位)
         3 版本与 CRC32
     - 加密文件内容为 "GTE1" + u32 明文长 + AES-128-CBC 密文 (复用 kernel/crypto.c)
*/
#include "gt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEC       512
#define SPC       8
#define FAT_OFF   12
#define ROOT_CLUS 2
#define BM_CLUS   3
#define UP_CLUS   4
#define DATA_CLUS 5
#define CLUS_BYTES (SPC * SEC)

static u8  *vol;
static u32 *fat;
static u8  *bm;
static u32  heap_off;
static u32  fat_len;
static u32  cluster_count;
static u32  next_clus = DATA_CLUS;
static u32  used_clus;

#define CLUS_OFF(c) ((u64)((u64)heap_off + ((u64)(c) - 2) * SPC) * SEC)

/* ------------------------------------------------------------------ 小工具 */
static void wr16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void wr64(u8 *p, u64 v)
{
    wr32(p, (u32)v);
    wr32(p + 4, (u32)(v >> 32));
}

static u16 rr1_16(u16 h)
{
    return (u16)(((u32)h >> 1) | ((u32)h << 15));
}

static u32 rr1_32(u32 h)
{
    return (h >> 1) | (h << 31);
}

/* exFAT 名称哈希: 名称按 Up-case 表转换后逐字节参与 */
static u16 name_hash(const char *name)
{
    u16 h = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
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
        if (i == 2 || i == 3)            /* File 项的 SetChecksum 字段 */
            continue;
        cs = (u16)(rr1_16(cs) + ents[i]);
    }
    return cs;
}

static u32 table_checksum(const u8 *p, u32 n)
{
    u32 cs = 0;
    for (u32 i = 0; i < n; i++)
        cs = rr1_32(cs) + p[i];
    return cs;
}

static u32 now_stamp(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int y = tm->tm_year + 1900;
    if (y < 1980) y = 1980;
    u16 d = (u16)(((y - 1980) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    u16 tt = (u16)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    return ((u32)d << 16) | tt;
}

/* ------------------------------------------------------------ 簇 / 位图 */
static u32 fget(u32 c)
{
    return fat[c];
}

static void fset(u32 c, u32 v)
{
    fat[c] = v;
}

static int is_eoc(u32 v)
{
    return v >= 0xFFFFFFF8u;
}

static void bm_set(u32 c, int on)
{
    u32 bit = c - 2;
    if (on)
        bm[bit >> 3] |= (u8)(1u << (bit & 7));
    else
        bm[bit >> 3] &= (u8)~(1u << (bit & 7));
}

static u32 alloc_chain(u32 nclus, int zero)
{
    u32 first = 0, prev = 0;
    for (u32 i = 0; i < nclus; i++) {
        if (next_clus >= cluster_count + 2)
            return 0;
        u32 c = next_clus++;
        if (zero)
            memset(vol + CLUS_OFF(c), 0, CLUS_BYTES);
        bm_set(c, 1);
        used_clus++;
        if (!first)
            first = c;
        else
            fset(prev, c);
        fset(c, 0xFFFFFFFFu);
        prev = c;
    }
    return first;
}

static void write_cluster_data(u32 first, const u8 *data, u32 len)
{
    u32 c = first, off = 0;
    while (off < len) {
        u32 chunk = len - off > CLUS_BYTES ? CLUS_BYTES : len - off;
        memcpy(vol + CLUS_OFF(c), data + off, chunk);
        off += chunk;
        if (off < len)
            c = fget(c);
    }
}

/* ------------------------------------------------------------------ 目录 */
struct dir {
    u32 first;
    u32 nent;
};

#define MAX_DIRS 64
static struct dir dirs[MAX_DIRS];
static char dir_path[MAX_DIRS][192];
static int ndirs;

static u32 dir_capacity(const struct dir *d)
{
    u32 n = 0, c = d->first;
    while (c >= 2 && !is_eoc(c)) {
        n++;
        c = fget(c);
    }
    if (n == 0)
        n = 1;
    return n * (CLUS_BYTES / 32);
}

static u8 *dir_entry(struct dir *d, u32 idx)
{
    u32 per = CLUS_BYTES / 32;
    u32 c = d->first;
    for (u32 i = idx / per; i > 0; i--)
        c = fget(c);
    return vol + CLUS_OFF(c) + (idx % per) * 32;
}

static int dir_grow(struct dir *d)
{
    u32 c = d->first;
    while (!is_eoc(fget(c)))
        c = fget(c);
    u32 n = alloc_chain(1, 1);
    if (!n)
        return 0;
    fset(c, n);
    return 1;
}

static int dir_append(struct dir *d, const u8 *ents, u32 count)
{
    while (d->nent + count > dir_capacity(d)) {
        if (!dir_grow(d))
            return -1;
    }
    for (u32 i = 0; i < count; i++)
        memcpy(dir_entry(d, d->nent + i), ents + i * 32, 32);
    d->nent += count;
    return (int)(d->nent - count);
}

/* --------------------------------------------------------------- 目录项集 */
static u32 build_set(u8 *out, const char *name, u8 attr, u32 first, u64 size,
                     u32 plain_size, const char *owner, u16 mode,
                     u8 xflags, u32 crc)
{
    int nlen = (int)strlen(name);
    if (nlen > 255)
        nlen = 255;
    int nname = (nlen + 14) / 15;
    int secondary = 4 + nname;           /* stream + 名称 + 3 个扩展项 */
    int total = 1 + secondary;
    if (total > 12)
        return 0;
    memset(out, 0, (size_t)total * 32);

    u32 ts = now_stamp();

    /* File (0x85) */
    u8 *e = out;
    e[0] = 0x85;
    e[1] = (u8)secondary;
    wr16(e + 4, attr);
    wr32(e + 8, ts);
    wr32(e + 12, ts);
    wr32(e + 16, ts);

    /* Stream Extension (0xC0) */
    e = out + 32;
    e[0] = 0xC0;
    e[1] = (u8)(first ? 0x01 : 0x00);   /* AllocationPossible; 使用 FAT 链 */
    e[3] = (u8)nlen;
    wr16(e + 4, name_hash(name));
    wr64(e + 8, size);
    wr32(e + 20, first);
    wr64(e + 24, size);

    /* File Name (0xC1) */
    for (int i = 0; i < nname; i++) {
        e = out + 64 + i * 32;
        e[0] = 0xC1;
        for (int j = 0; j < 15; j++) {
            int k = i * 15 + j;
            u16 ch = (k < nlen) ? (u16)(u8)name[k] : 0;
            wr16(e + 2 + j * 2, ch);
        }
    }

    /* 厂商扩展: 加密 / ACL / 版本 */
    e = out + 64 + nname * 32;
    e[0] = 0xE0;
    e[1] = GT_XTYPE_ENCRYPT;
    e[4] = xflags;
    e[5] = 1;                           /* 算法: AES-128-CBC */
    wr32(e + 8, plain_size);
    wr32(e + 16, crc);

    e += 32;
    e[0] = 0xE0;
    e[1] = GT_XTYPE_ACL;
    wr16(e + 4, mode);
    strncpy((char *)e + 8, owner, 16);

    e += 32;
    e[0] = 0xE0;
    e[1] = GT_XTYPE_VERSION;
    wr32(e + 4, 1);
    wr32(e + 8, crc);

    wr16(out + 2, set_checksum(out, total));
    return (u32)total;
}

/* ------------------------------------------------------------- 目录表 */
static int dir_find(const char *path)
{
    for (int i = 0; i < ndirs; i++)
        if (strcmp(dir_path[i], path) == 0)
            return i;
    return -1;
}

static int dir_add(const char *path, u32 first)
{
    if (ndirs >= MAX_DIRS)
        return -1;
    strncpy(dir_path[ndirs], path, sizeof(dir_path[0]) - 1);
    dir_path[ndirs][sizeof(dir_path[0]) - 1] = 0;
    dirs[ndirs].first = first;
    dirs[ndirs].nent = 0;
    return ndirs++;
}

static int parent_of(const char *path, char *parent, const char **base)
{
    const char *slash = strrchr(path, '\\');
    if (!slash)
        slash = strrchr(path, '/');
    if (!slash) {
        parent[0] = 0;
        *base = path;
        return 0;
    }
    size_t n = (size_t)(slash - path);
    if (n >= 190)
        return -1;
    memcpy(parent, path, n);
    parent[n] = 0;
    *base = slash + 1;
    return 0;
}

static u32 create_dir(const char *path, const char *owner)
{
    char parent[192];
    const char *base;
    if (parent_of(path, parent, &base) < 0 || !base[0])
        return 0;
    int pi = dir_find(parent);
    if (pi < 0)
        return 0;

    u32 clus = alloc_chain(1, 1);
    if (!clus)
        return 0;

    u8 ents[12 * 32];
    u32 n = build_set(ents, base, ATTR_DIR, clus, 0, 0, owner, 0, 0,
                      gt_crc32("", 0));
    if (dir_append(&dirs[pi], ents, n) < 0)
        return 0;
    if (dir_add(path, clus) < 0)
        return 0;
    return clus;
}

static u32 resolve_dir(const char *path)
{
    int i = dir_find(path);
    if (i >= 0)
        return (u32)i;
    /* 逐级自动创建, 便于 F 行先于 D 行 */
    char tmp[192];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *p = tmp;
    while (*p == '\\' || *p == '/')
        p++;
    for (;;) {
        char *slash = strchr(p, '\\');
        if (!slash)
            slash = strchr(p, '/');
        if (!slash)
            break;
        *slash = 0;
        if (dir_find(tmp) < 0 && !create_dir(tmp, "SYSTEM"))
            return 0xFFFFFFFFu;
        *slash = '\\';
        p = slash + 1;
    }
    return (u32)dir_find(path);
}

/* --------------------------------------------------------------- 文件填充 */
static u8 *read_host_file(const char *path, u32 *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gtmkfs: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 24)) {
        fclose(f);
        return NULL;
    }
    u8 *buf = (u8 *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (u32)n;
    return buf;
}

static u8 *pack_encrypted(const u8 *raw, u32 len, u32 *out_len)
{
    u32 clen = (len / 16 + 1) * 16;
    u8 *buf = (u8 *)malloc((size_t)clen + 8);
    if (!buf)
        return NULL;
    buf[0] = 'G';
    buf[1] = 'T';
    buf[2] = 'E';
    buf[3] = '1';
    wr32(buf + 4, len);
    u32 got = gt_encrypt(raw, len, buf + 8);
    *out_len = 8 + got;
    return buf;
}

static int add_file(const char *vpath, const char *host, u8 attr,
                    const char *owner, int encrypt)
{
    char parent[192];
    const char *base;
    if (parent_of(vpath, parent, &base) < 0 || !base[0])
        return 0;
    u32 pi = resolve_dir(parent);
    if (pi == 0xFFFFFFFFu)
        return 0;

    u32 raw_len = 0;
    u8 *raw = read_host_file(host, &raw_len);
    if (!raw)
        return 0;

    u32 crc = gt_crc32(raw, raw_len);
    const u8 *data = raw;
    u32 data_len = raw_len;
    u8 *packed = NULL;
    if (encrypt) {
        packed = pack_encrypted(raw, raw_len, &data_len);
        if (!packed) {
            free(raw);
            return 0;
        }
        data = packed;
    }
    u32 nclus = data_len ? (data_len + CLUS_BYTES - 1) / CLUS_BYTES : 0;
    u32 first = nclus ? alloc_chain(nclus, 0) : 0;
    if (nclus && !first) {
        free(raw);
        free(packed);
        return 0;
    }
    if (nclus)
        write_cluster_data(first, data, data_len);

    u8 ents[12 * 32];
    u32 n = build_set(ents, base, attr, first, data_len, raw_len, owner, 0,
                      (u8)(encrypt ? GT_XFLAG_ENCRYPTED : 0), crc);
    int rc = dir_append(&dirs[pi], ents, n) >= 0;
    free(raw);
    free(packed);
    return rc;
}

/* --------------------------------------------------------------- 引导区 */
static const u16 upcase_tbl[7] = {
    0x0000, 0x0061,                     /* 0x0000..0x0060 恒等 */
    0x0041, 0x001A,                     /* 0x0061..0x007A -> 0x0041.. */
    0x0000, 0xFF85,                     /* 0x007B..0xFFFF 恒等 */
    0xFFFF
};

static void write_boot_sector(u64 volume_len)
{
    u8 *bs = vol;
    bs[0] = 0xEB;
    bs[1] = 0x76;
    bs[2] = 0x90;
    memcpy(bs + 3, "EXFAT   ", 8);

    wr64(bs + 0x40, 0);                 /* PartitionOffset */
    wr64(bs + 0x48, volume_len);
    wr32(bs + 0x50, FAT_OFF);
    wr32(bs + 0x54, fat_len);
    wr32(bs + 0x58, heap_off);
    wr32(bs + 0x5C, cluster_count);
    wr32(bs + 0x60, ROOT_CLUS);
    wr32(bs + 0x64, 0x47544453u);       /* "GTDS" 卷序列号 */
    wr16(bs + 0x68, 0x0100);            /* exFAT 1.0 */
    wr16(bs + 0x6A, 0);                 /* VolumeFlags */
    bs[0x6C] = 9;                       /* 512 字节/扇区 */
    bs[0x6D] = 3;                       /* 8 扇区/簇 */
    bs[0x6E] = 1;                       /* 1 个 FAT */
    bs[0x6F] = 0x80;
    bs[0x70] = (u8)(cluster_count ? (used_clus * 100u / cluster_count) : 0);
    bs[0x1FE] = 0x55;
    bs[0x1FF] = 0xAA;

    /* 引导校验: 扇区 0..10, 跳过 VolumeFlags/PercentInUse 4 字节 */
    u32 cs = 0;
    for (int i = 0; i < 11 * SEC; i++) {
        if (i == 106 || i == 107 || i == 112 || i == 113)
            continue;
        cs = rr1_32(cs) + bs[i];
    }
    for (int i = 0; i < SEC; i += 4)
        wr32(vol + 11 * SEC + i, cs);
}

static void init_root(void)
{
    struct dir *root = &dirs[0];
    u8 e[32];

    memset(e, 0, sizeof(e));
    e[0] = 0x83;
    e[1] = 6;
    const char *label = "GT-DOS";
    for (int i = 0; i < 6; i++)
        wr16(e + 2 + i * 2, (u16)label[i]);
    dir_append(root, e, 1);

    memset(e, 0, sizeof(e));
    e[0] = 0x81;
    wr32(e + 20, BM_CLUS);
    wr64(e + 24, (u64)((cluster_count + 7) / 8));
    dir_append(root, e, 1);

    memset(e, 0, sizeof(e));
    e[0] = 0x82;
    wr32(e + 4, table_checksum((const u8 *)upcase_tbl, sizeof(upcase_tbl)));
    wr32(e + 20, UP_CLUS);
    wr64(e + 24, sizeof(upcase_tbl));
    dir_append(root, e, 1);
}

/* ---------------------------------------------------------------- 入口 */
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

static char *cut_last(char *s)
{
    char *p = strrchr(s, ' ');
    if (!p)
        return NULL;
    *p = 0;
    return p + 1;
}

static int run_manifest(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "gtmkfs: cannot open manifest %s\n", path);
        return 0;
    }
    char line[1024];
    int ok = 1;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        if (line[0] == 'D' && line[1] == ' ') {
            char *rest = line + 2;
            char *owner = cut_last(rest);
            char *attr_s = cut_last(rest);
            if (!owner || !attr_s) { ok = 0; break; }
            (void)attr_s;               /* 目录统一使用 ATTR_DIR */
            if (dir_find(rest) < 0 && !create_dir(rest, owner))
                ok = 0;
        } else if (line[0] == 'F' && line[1] == ' ') {
            char *rest = line + 2;
            char *enc_s = cut_last(rest);
            char *owner_s = cut_last(rest);
            char *attr_s = cut_last(rest);
            if (!enc_s || !owner_s || !attr_s) { ok = 0; break; }
            char *sp = strchr(rest, ' ');
            if (!sp) { ok = 0; break; }
            *sp = 0;
            const char *vpath = rest;
            const char *host = sp + 1;
            u8 attr = (u8)strtoul(attr_s, NULL, 0);
            int enc = atoi(enc_s) != 0;
            if (!add_file(vpath, host, attr, owner_s, enc))
                ok = 0;
        }
    }
    fclose(f);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: gtmkfs <image> <lba> <sectors> <manifest>\n");
        return 1;
    }
    const char *img = argv[1];
    u64 lba = strtoull(argv[2], NULL, 0);
    u32 sectors = (u32)strtoul(argv[3], NULL, 0);

    /* 布局: 引导区 12 扇区, FAT 长度按簇数迭代 */
    fat_len = 64;
    for (int i = 0; i < 16; i++) {
        heap_off = FAT_OFF + fat_len;
        cluster_count = (sectors - heap_off) / SPC;
        u32 need = (u32)(((u64)(cluster_count + 2) * 4 + SEC - 1) / SEC);
        if (need <= fat_len)
            break;
        fat_len = need;
    }
    if (cluster_count < 16 ||
        (cluster_count + 7) / 8 > CLUS_BYTES) {   /* 分配位图须放入一个簇 */
        fprintf(stderr, "gtmkfs: partition size unsupported\n");
        return 1;
    }

    vol = (u8 *)calloc((size_t)sectors * SEC, 1);
    if (!vol) {
        fprintf(stderr, "gtmkfs: out of memory\n");
        return 1;
    }
    fat = (u32 *)(vol + (u64)FAT_OFF * SEC);
    bm = vol + CLUS_OFF(BM_CLUS);

    /* 系统保留簇 */
    bm_set(ROOT_CLUS, 1);
    bm_set(BM_CLUS, 1);
    bm_set(UP_CLUS, 1);
    used_clus = 3;
    fset(0, 0xFFFFFFF8u);
    fset(1, 0xFFFFFFFFu);
    fset(ROOT_CLUS, 0xFFFFFFFFu);
    fset(BM_CLUS, 0xFFFFFFFFu);
    fset(UP_CLUS, 0xFFFFFFFFu);
    memcpy(vol + CLUS_OFF(UP_CLUS), upcase_tbl, sizeof(upcase_tbl));

    dir_add("", ROOT_CLUS);
    init_root();

    if (!run_manifest(argv[4])) {
        fprintf(stderr, "gtmkfs: manifest failed\n");
        free(vol);
        return 1;
    }

    write_boot_sector(sectors);

    FILE *f = fopen(img, "r+b");
    if (!f) {
        fprintf(stderr, "gtmkfs: cannot open image %s\n", img);
        free(vol);
        return 1;
    }
    if (fseek(f, (long)(lba * SEC), SEEK_SET) != 0 ||
        fwrite(vol, 1, (size_t)sectors * SEC, f) != (size_t)sectors * SEC) {
        fprintf(stderr, "gtmkfs: image write failed\n");
        fclose(f);
        free(vol);
        return 1;
    }
    fclose(f);
    free(vol);

    printf("    exFAT 卷 : %u 簇 (已用 %u), FAT %u 扇区\n",
           cluster_count, used_clus, fat_len);
    return 0;
}
