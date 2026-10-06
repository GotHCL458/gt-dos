/* ==========================================================================
 *  GT-DOS FAT 文件系统 (FAT16 / FAT32, 自动识别)
 *  ---------------------------------------------------------------------------
 *  单卷: C: = ATA dev0 的 FAT16 分区 (MBR 分区表由内核解析).
 *  盘尾隐藏启动分区 (type 0x11) 不是 FAT 卷, 挂载时自动跳过.
 *  系统保护: \GT-DOS\ 目录 (含任意层级) 禁止创建/删除/改属性.
 *  路径规则:
 *    "C:\DIR\FILE"  带盘符前缀 -> 切到该卷
 *    其余路径按当前卷解析; 相对路径由 shell 拼成绝对路径后传入.
 * ========================================================================== */
#include "gt.h"

#define SEC_SIZE 512
#define MAX_VOL  2

struct raw_dir {
    u8  name[11];
    u8  attr;
    u8  reserved;
    u8  crt_tenth;
    u8  crt_time[2];
    u8  crt_date[2];
    u8  last_acc[2];
    u16 hi_clus;
    u8  wr_time[2];
    u8  wr_date[2];
    u16 lo_clus;
    u32 size;
} __attribute__((packed));

typedef char raw_dir_must_be_32[1 - 2 * (sizeof(struct raw_dir) != 32)];

/* ------------------------------------------------------------------ 卷 */
struct vol {
    char letter;
    bool mounted;
    int  dev;
    bool is32;
    u32  part_lba;
    u8   sec_per_clus;
    u16  root_entries;
    u32  fat_size;
    u32  fat_base;
    u32  data_start;
    u32  root_dir_start;
    u32  root_clus;
    u32  cluster_count;
};

static struct vol vols[MAX_VOL];
static int nvol;
static int cur;                       /* 当前卷索引 (shell 盘符) */
static int work;                      /* 本次操作卷索引 (路径解析用) */
static bool show_all;                 /* DIR /A: 显示隐藏/系统条目 */
static bool allow_sys;                /* 系统内部写 \GT-DOS\ 状态文件 */

static u8   scratch_a[SEC_SIZE];      /* FAT 读改写 */
static u8   scratch_b[SEC_SIZE];      /* 目录读改写 */
static u8   scratch_c[SEC_SIZE];      /* 数据读 */

#define V (&vols[work])
#define WOK (work >= 0 && work < nvol && vols[work].mounted)

static u32 rd_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u16 rd_le16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
static void wr_le16(u8 *p, u16 val) { p[0] = (u8)val; p[1] = (u8)(val >> 8); }
static void wr_le32(u8 *p, u32 val)
{
    p[0] = (u8)val; p[1] = (u8)(val >> 8);
    p[2] = (u8)(val >> 16); p[3] = (u8)(val >> 24);
}

/* ---------------------------------------------------------- 路径盘符 */
/* 若 path 以 "X:" 开头, 把操作卷 work 切到该卷并返回剩余路径; 否则 work=当前卷.
 * 注意: 只改 work, 不改 cur (shell 的当前盘符). 卷不存在返回 0. */
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
    work = cur;                       /* 无前缀: 用当前盘 */
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
char fat_drive_letter(int i)
{
    return (i >= 0 && i < nvol) ? vols[i].letter : 0;
}

/* ------------------------------------------------------------------ 底层 */
static bool read_sect(u32 lba, void *buf)
{
    return ata_read_dev(V->dev, lba, 1, buf);
}
static bool write_sect(u32 lba, const void *buf)
{
    return ata_write_dev(V->dev, lba, 1, buf);
}

static u32 eoc_marker(void) { return V->is32 ? 0x0FFFFFF8u : 0xFFF8u; }
static bool is_eoc(u32 x) { return x >= eoc_marker(); }

static u32 fat_get(u32 n)
{
    u32 byte_off = V->is32 ? n * 4 : n * 2;
    u32 fat_sect = byte_off / SEC_SIZE;
    if (!read_sect(V->part_lba + V->fat_base + fat_sect, scratch_a))
        return 0;
    u32 in = byte_off % SEC_SIZE;
    if (V->is32)
        return rd_le32(scratch_a + in) & 0x0FFFFFFFu;
    return rd_le16(scratch_a + in);
}

static bool fat_set(u32 n, u32 val)
{
    u32 byte_off = V->is32 ? n * 4 : n * 2;
    u32 fat_sect = byte_off / SEC_SIZE;
    u32 lba = V->part_lba + V->fat_base + fat_sect;
    if (!read_sect(lba, scratch_a))
        return false;
    u32 in = byte_off % SEC_SIZE;
    if (V->is32) {
        u32 keep = rd_le32(scratch_a + in) & 0xF0000000u;
        wr_le32(scratch_a + in, (val & 0x0FFFFFFFu) | keep);
    } else {
        wr_le16(scratch_a + in, (u16)val);
    }
    return write_sect(lba, scratch_a);
}

static u32 clus_to_sect(u32 c)
{
    return V->data_start + (c - 2) * (u32)V->sec_per_clus;
}

static u32 root_dir_clus(void) { return V->is32 ? V->root_clus : 0; }

/* ------------------------------------------------------------------ 挂载 */
static bool looks_like_fat_bs(const u8 *bs)
{
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        return false;
    u16 bps = rd_le16(bs + 11);
    if (bps != SEC_SIZE)
        return false;
    u8 spc = bs[13];
    if (spc == 0 || (spc & (spc - 1)))
        return false;
    if (bs[16] == 0)
        return false;
    return true;
}

/* 挂载设备 dev 到盘符 letter (扫描 MBR 分区表, 找第一个 FAT 分区) */
static bool mount_one(int dev, char letter)
{
    if (!ata_present_dev(dev))
        return false;

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
    work = cur = (int)(nv - vols);    /* 挂载期间读写走 V=vols[work] */

    bool ok = false;
    do {
        u8 s0[SEC_SIZE];
        if (!read_sect(0, s0))
            break;
        if (s0[510] != 0x55 || s0[511] != 0xAA)
            break;

        u32 part_lba = 0;
        if (!looks_like_fat_bs(s0)) {
            bool found = false;
            for (int i = 0; i < 4; i++) {
                const u8 *pe = s0 + 0x1BE + i * 16;
                u8 type = pe[4];
                if (type == 0x06 || type == 0x0B || type == 0x0C ||
                    type == 0x0E || type == 0x04 || type == 0x01) {
                    part_lba = rd_le32(pe + 8);
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        }

        u8 bs[SEC_SIZE];
        if (part_lba == 0) {
            memcpy(bs, s0, SEC_SIZE);
        } else if (!read_sect(part_lba, bs)) {
            break;
        }
        if (!looks_like_fat_bs(bs))
            break;

        nv->part_lba = part_lba;
        nv->sec_per_clus = bs[13];
        nv->root_entries = rd_le16(bs + 17);
        u16 reserved = rd_le16(bs + 14);
        u8  num_fats = bs[16];
        u32 fat_sz = rd_le16(bs + 22);
        if (fat_sz == 0)
            fat_sz = rd_le32(bs + 36);
        nv->fat_size = fat_sz;
        nv->fat_base = reserved;

        u32 total = rd_le16(bs + 19);
        if (total == 0)
            total = rd_le32(bs + 32);
        nv->root_clus = rd_le32(bs + 44);

        u32 root_sect = (nv->root_entries * 32 + SEC_SIZE - 1) / SEC_SIZE;
        nv->data_start = part_lba + reserved + (u32)num_fats * fat_sz + root_sect;
        nv->root_dir_start = part_lba + reserved + (u32)num_fats * fat_sz;

        u32 clus_area =
            (total > (u32)reserved + (u32)num_fats * fat_sz + root_sect)
            ? (total - (u32)reserved - (u32)num_fats * fat_sz - root_sect)
            : 0;
        nv->cluster_count = clus_area / nv->sec_per_clus;

        nv->is32 = (nv->root_entries == 0) || (nv->cluster_count >= 65525);
        if (!nv->is32 && nv->cluster_count < 4085)
            break;                    /* FAT12 不支持 */

        nv->mounted = true;
        ok = true;
    } while (0);

    if (!ok && nv == &vols[nvol - 1])
        nvol--;
    return ok;
}

bool fat_mount(void)
{
    nvol = 0;
    cur = 0;
    work = 0;
    char l0 = gcfg.sys_drive ? gcfg.sys_drive : 'C';
    bool a = mount_one(0, l0);

    for (int i = 0; i < nvol; i++)
        if (vols[i].dev == 0 && vols[i].mounted)
            cur = i;
    work = cur;
    return a;
}

u32 fat_total_clusters(void) { work = cur; return V->cluster_count; }
u32 fat_free_clusters(void)
{
    work = cur;
    if (!WOK)
        return 0;
    u32 free_n = 0;
    for (u32 c = 2; c < V->cluster_count + 2; c++)
        if (fat_get(c) == 0)
            free_n++;
    return free_n;
}

/* ------------------------------------------------------------ 名字转换 */
static bool name_to_raw(const char *name, u8 out[11])
{
    for (int i = 0; i < 11; i++)
        out[i] = ' ';
    const char *dot = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') { dot = p; break; }
    }
    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (base_len > 8 || base_len == 0)
        return false;
    for (int i = 0; i < base_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[i] = (u8)c;
    }
    if (dot) {
        int ext_len = (int)strlen(dot + 1);
        if (ext_len > 3)
            return false;
        for (int j = 0; j < ext_len; j++) {
            char c = dot[1 + j];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[8 + j] = (u8)c;
        }
    }
    return true;
}

static void raw_to_name(const u8 raw[11], char *out)
{
    int k = 0;
    for (int i = 0; i < 8 && raw[i] != ' ' && raw[i] != 0; i++) {
        char c = (char)raw[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[k++] = c;
    }
    bool has_ext = false;
    for (int i = 8; i < 11 && raw[i] != ' ' && raw[i] != 0; i++)
        has_ext = true;
    if (has_ext) {
        out[k++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' ' && raw[i] != 0; i++) {
            char c = (char)raw[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[k++] = c;
        }
    }
    out[k] = 0;
}

/* ---------------------------------------------------------- 目录遍历 */
struct dir_iter {
    u32 clus;
    u32 sect_in;
    u32 ent;
    u32 lba;
    bool valid;
    bool done;
};

static void di_init(struct dir_iter *it, u32 dir_clus)
{
    it->clus = dir_clus;
    it->sect_in = 0;
    it->ent = 0;
    it->valid = false;
    it->done = false;
    it->lba = 0;
}

static const struct raw_dir *di_cur(struct dir_iter *it)
{
    if (it->clus == 0) {
        u32 total_sect = (V->root_entries * 32 + SEC_SIZE - 1) / SEC_SIZE;
        if (it->sect_in >= total_sect) { it->done = true; return 0; }
        it->lba = V->root_dir_start + it->sect_in;
    } else {
        it->lba = clus_to_sect(it->clus) + it->sect_in;
    }
    if (!read_sect(it->lba, scratch_b)) { it->done = true; return 0; }
    it->valid = true;
    return (const struct raw_dir *)(scratch_b + it->ent * 32);
}

static bool di_next(struct dir_iter *it)
{
    if (it->done)
        return false;
    if (!it->valid)
        return true;

    it->ent++;
    if (it->ent < 16)
        return true;

    it->ent = 0;
    it->valid = false;

    if (it->clus == 0) {
        u32 total_sect = (V->root_entries * 32 + SEC_SIZE - 1) / SEC_SIZE;
        it->sect_in++;
        if (it->sect_in >= total_sect) { it->done = true; return false; }
        return true;
    }

    it->sect_in++;
    if (it->sect_in >= (u32)V->sec_per_clus) {
        u32 nxt = fat_get(it->clus);
        it->sect_in = 0;
        if (is_eoc(nxt) || nxt < 2) { it->done = true; return false; }
        it->clus = nxt;
    }
    return true;
}

static bool dir_find(u32 dir_clus, const u8 raw[11], struct raw_dir *out,
                     u32 *sect_lba, u32 *sect_off)
{
    struct dir_iter it;
    di_init(&it, dir_clus);
    for (;;) {
        const struct raw_dir *d = di_cur(&it);
        if (!d)
            return false;
        if (d->name[0] == 0x00)
            return false;
        if (d->name[0] != 0xE5 && memcmp(d->name, raw, 11) == 0) {
            if (out) *out = *d;
            if (sect_lba) *sect_lba = it.lba;
            if (sect_off) *sect_off = it.ent * 32;
            return true;
        }
        if (!di_next(&it))
            return false;
    }
}

static u32 resolve_parent(const char *path, u8 raw_name[11])
{
    char comp[16];
    u32 dir_clus = root_dir_clus();

    const char *slash = 0;
    for (const char *q = path; *q; q++)
        if (*q == '\\' || *q == '/')
            slash = q;

    const char *base = slash ? slash + 1 : path;
    const char *end = slash ? slash : path;

    const char *p = path;
    while (p < end) {
        const char *sep = p;
        while (sep < end && *sep != '\\' && *sep != '/')
            sep++;
        size_t n = (size_t)(sep - p);
        if (n == 0) { p = sep + 1; continue; }
        if (n >= sizeof(comp))
            return 0xFFFFFFFFu;
        memcpy(comp, p, n);
        comp[n] = 0;
        u8 raw[11];
        if (!name_to_raw(comp, raw))
            return 0xFFFFFFFFu;
        struct raw_dir d;
        if (!dir_find(dir_clus, raw, &d, 0, 0))
            return 0xFFFFFFFFu;
        if (!(d.attr & ATTR_DIR))
            return 0xFFFFFFFFu;
        dir_clus = ((u32)d.hi_clus << 16) | d.lo_clus;
        p = sep + 1;
    }

    size_t n = strlen(base);
    if (n == 0 || n >= sizeof(comp))
        return 0xFFFFFFFFu;
    memcpy(comp, base, n);
    comp[n] = 0;
    if (!name_to_raw(comp, raw_name))
        return 0xFFFFFFFFu;
    return dir_clus;
}

static u32 dirent_clus(const struct raw_dir *d)
{
    return ((u32)d->hi_clus << 16) | d->lo_clus;
}

static bool get_dirent(const char *path, struct raw_dir *d,
                       u32 *lba, u32 *off)
{
    u8 raw[11];
    u32 dir_clus = resolve_parent(path, raw);
    if (dir_clus == 0xFFFFFFFFu)
        return false;
    return dir_find(dir_clus, raw, d, lba, off);
}

/* --------------------------------------------------------------- 公开 API */

/* 系统保护: 路径首段为 "GT-DOS" (任意层级) 视为系统目录, 禁止写操作.
 * path 为去掉盘符后的路径 (可能以 \ 或 / 开头). */
static bool path_protected(const char *path)
{
    while (*path == '\\' || *path == '/')
        path++;
    /* 比较前 6 个字符 (不区分大小写), 且其后必须是分隔符或结尾 */
    if (!(path[0] == 'G' || path[0] == 'g')) return false;
    const char *ref = "GT-DOS";
    for (int i = 0; i < 6; i++)
        if (toupper_(path[i]) != ref[i])
            return false;
    char nxt = path[6];
    return (nxt == '\0' || nxt == '\\' || nxt == '/');
}

static bool sys_write_denied(const char *path)
{
    if (allow_sys)                 /* 系统自身写 GT-DOS 状态文件时放行 */
        return false;
    if (path_protected(path)) {
        kprintf("System file/folder protected.\n");
        return true;
    }
    return false;
}

void fat_set_allow_sys(bool on) { allow_sys = on; }

int fat_list_dir(const char *path, struct fat_dirent *out, int max)
{
    path = use_path(path);
    if (!path || !WOK)
        return -1;

    /* 跳过前导分隔符: "C:\" 或 "\" 都表示根目录 */
    while (*path == '\\' || *path == '/')
        path++;

    u32 dir_clus = root_dir_clus();
    if (path[0]) {
        struct raw_dir d;
        if (!get_dirent(path, &d, 0, 0))
            return -1;
        if (!(d.attr & ATTR_DIR))
            return -1;
        dir_clus = dirent_clus(&d);
    }

    int n = 0;
    struct dir_iter it;
    di_init(&it, dir_clus);
    for (;;) {
        const struct raw_dir *d = di_cur(&it);
        if (!d)
            break;
        if (d->name[0] == 0x00)
            break;
        if (d->name[0] != 0xE5 && !(d->attr & ATTR_VOL)) {
            int dot = (d->name[0] == '.' &&
                       (d->name[1] == ' ' ||
                        (d->name[1] == '.' && d->name[2] == ' ')));
            if (!dot && !show_all && (d->attr & (ATTR_HID | ATTR_SYS)))
                ;                        /* 默认过滤隐藏/系统条目 */
            else if (!dot && n < max) {
                raw_to_name(d->name, out[n].name);
                out[n].size = d->size;
                out[n].first_cluster = (u16)dirent_clus(d);
                out[n].attrs = d->attr;
                out[n].is_dir = (d->attr & ATTR_DIR) != 0;
                n++;
            }
        }
        if (!di_next(&it))
            break;
    }
    return n;
}

bool fat_find(const char *name, struct fat_dirent *out)
{
    name = use_path(name);
    if (!name || !WOK)
        return false;
    struct raw_dir d;
    if (!get_dirent(name, &d, 0, 0))
        return false;
    raw_to_name(d.name, out->name);
    out->size = d.size;
    out->first_cluster = (u16)dirent_clus(&d);
    out->attrs = d.attr;
    out->is_dir = (d.attr & ATTR_DIR) != 0;
    return true;
}

bool fat_read_file(const char *name, void *buf, u32 max, u32 *out_len)
{
    return fat_read_dir_file(0, name, buf, max, out_len);
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

    struct raw_dir d;
    if (!get_dirent(p, &d, 0, 0))
        return false;
    if (d.attr & ATTR_DIR)
        return false;

    u32 c = dirent_clus(&d);
    u32 remain = d.size;
    u8 *dst = (u8 *)buf;
    u32 got = 0;

    if (c == 0 || remain == 0) {
        if (out_len) *out_len = 0;
        return true;
    }

    while (remain > 0) {
        u32 base = clus_to_sect(c);
        for (u32 s = 0; s < (u32)V->sec_per_clus && remain > 0; s++) {
            u32 chunk = remain > SEC_SIZE ? SEC_SIZE : remain;
            if (got + chunk > max)
                chunk = max - got;
            if (!read_sect(base + s, scratch_c))
                return false;
            memcpy(dst + got, scratch_c, chunk);
            got += chunk;
            remain -= chunk;
            if (got >= max) {
                if (out_len) *out_len = got;
                return true;
            }
        }
        u32 nxt = fat_get(c);
        if (is_eoc(nxt) || nxt < 2)
            break;
        c = nxt;
    }
    if (out_len) *out_len = got;
    return true;
}

static u32 alloc_cluster(void)
{
    for (u32 c = 2; c < V->cluster_count + 2; c++)
        if (fat_get(c) == 0)
            return c;
    return 0;
}

static void free_chain(u32 c)
{
    while (c >= 2) {
        u32 nxt = fat_get(c);
        fat_set(c, 0);
        if (is_eoc(nxt) || nxt < 2)
            break;
        c = nxt;
    }
}

static void dos_time(u8 t[2], u8 dt[2])
{
    int y, mo, d, h, mi, s;
    rtc_read(&y, &mo, &d, &h, &mi, &s);
    if (y < 1980) y = 1980;
    if (mo < 1) mo = 1;
    if (d < 1) d = 1;
    u16 tt = (u16)((h << 11) | (mi << 5) | (s / 2));
    u16 dd = (u16)(((y - 1980) << 9) | (mo << 5) | d);
    wr_le16(t, tt);
    wr_le16(dt, dd);
}

static bool find_free_slot(u32 dir_clus, u32 *lba, u32 *off)
{
    struct dir_iter it;
    di_init(&it, dir_clus);
    for (;;) {
        const struct raw_dir *d = di_cur(&it);
        if (!d)
            return false;
        if (d->name[0] == 0x00 || d->name[0] == 0xE5) {
            *lba = it.lba;
            *off = it.ent * 32;
            return true;
        }
        if (!di_next(&it))
            return false;
    }
}

static bool append_dir_cluster(u32 dir_clus, u32 *new_clus)
{
    if (dir_clus == 0)
        return false;
    u32 c = dir_clus;
    for (;;) {
        u32 nxt = fat_get(c);
        if (is_eoc(nxt) || nxt < 2)
            break;
        c = nxt;
    }
    u32 n = alloc_cluster();
    if (n == 0)
        return false;
    fat_set(c, n);
    fat_set(n, eoc_marker());
    u8 zero[SEC_SIZE];
    memset(zero, 0, sizeof(zero));
    for (u32 s = 0; s < V->sec_per_clus; s++)
        write_sect(clus_to_sect(n) + s, zero);
    *new_clus = n;
    return true;
}

static bool dir_write_entry(u32 dir_clus, const struct raw_dir *d)
{
    u32 lba, off;
    if (!find_free_slot(dir_clus, &lba, &off)) {
        u32 nclus;
        if (!append_dir_cluster(dir_clus, &nclus))
            return false;
        if (!find_free_slot(nclus, &lba, &off))
            return false;
    }
    if (!read_sect(lba, scratch_b))
        return false;
    memcpy(scratch_b + off, d, 32);
    return write_sect(lba, scratch_b);
}

bool fat_create(const char *name, const void *data, u32 len)
{
    name = use_path(name);
    if (!name || !WOK)
        return false;
    if (sys_write_denied(name)) return false;

    u8 raw[11];
    u32 dir_clus = resolve_parent(name, raw);
    if (dir_clus == 0xFFFFFFFFu)
        return false;

    struct raw_dir old;
    u32 old_lba, old_off;
    if (dir_find(dir_clus, raw, &old, &old_lba, &old_off)) {
        if (old.attr & ATTR_DIR)
            return false;
        if (old.attr & (ATTR_RO | ATTR_SYS))
            return false;
        free_chain(dirent_clus(&old));
        if (read_sect(old_lba, scratch_b)) {
            scratch_b[old_off] = 0xE5;
            write_sect(old_lba, scratch_b);
        }
    }

    u32 c = 0, first = 0;
    u32 remain = len;
    u32 woff = 0;
    const u8 *src = (const u8 *)data;

    if (remain == 0) {
        first = 0;
    } else {
        while (remain > 0) {
            u32 n = alloc_cluster();
            if (n == 0) {
                if (first) free_chain(first);
                return false;
            }
            if (first == 0)
                first = n;
            else
                fat_set(c, n);
            c = n;

            u32 csize = (u32)V->sec_per_clus * SEC_SIZE;
            u32 chunk = remain > csize ? csize : remain;
            u8 buf[SEC_SIZE];
            u32 done = 0;
            while (done < chunk) {
                u32 this = chunk - done > SEC_SIZE ? SEC_SIZE : chunk - done;
                memset(buf, 0, SEC_SIZE);
                memcpy(buf, src + woff + done, this);
                if (!write_sect(clus_to_sect(n) + done / SEC_SIZE, buf)) {
                    free_chain(first);
                    return false;
                }
                done += this;
            }
            woff += chunk;
            remain -= chunk;
        }
        fat_set(c, eoc_marker());
    }

    struct raw_dir d;
    memset(&d, 0, sizeof(d));
    memcpy(d.name, raw, 11);
    d.attr = ATTR_ARC;
    d.size = len;
    d.lo_clus = (u16)(first & 0xFFFF);
    d.hi_clus = (u16)(first >> 16);
    dos_time(d.wr_time, d.wr_date);
    memcpy(d.crt_time, d.wr_time, 4);
    if (!dir_write_entry(dir_clus, &d)) {
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
    if (sys_write_denied(name)) return false;
    struct raw_dir d;
    u32 lba, off;
    if (!get_dirent(name, &d, &lba, &off))
        return false;
    if (d.attr & ATTR_DIR)
        return false;
    if (d.attr & (ATTR_RO | ATTR_SYS))
        return false;                 /* 只读/系统文件保护 */

    free_chain(dirent_clus(&d));

    if (!read_sect(lba, scratch_b))
        return false;
    scratch_b[off] = 0xE5;
    return write_sect(lba, scratch_b);
}

bool fat_mkdir(const char *name)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    if (sys_write_denied(name)) return false;

    u8 raw[11];
    u32 dir_clus = resolve_parent(name, raw);
    if (dir_clus == 0xFFFFFFFFu)
        return false;

    struct raw_dir exist;
    if (dir_find(dir_clus, raw, &exist, 0, 0))
        return false;

    u32 n = alloc_cluster();
    if (n == 0)
        return false;
    fat_set(n, eoc_marker());

    u8 buf[SEC_SIZE];
    memset(buf, 0, SEC_SIZE);
    struct raw_dir *dot = (struct raw_dir *)buf;
    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = ATTR_DIR;
    dot->lo_clus = (u16)(n & 0xFFFF);
    dot->hi_clus = (u16)(n >> 16);
    struct raw_dir *dotdot = dot + 1;
    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = ATTR_DIR;
    dotdot->lo_clus = (u16)(dir_clus & 0xFFFF);
    dotdot->hi_clus = (u16)(dir_clus >> 16);
    dos_time(dot->wr_time, dot->wr_date);
    memcpy(dotdot->wr_time, dot->wr_time, 4);
    memcpy(dot->crt_time, dot->wr_time, 4);
    memcpy(dotdot->crt_time, dot->wr_time, 4);
    if (!write_sect(clus_to_sect(n), buf)) {
        fat_set(n, 0);
        return false;
    }

    struct raw_dir d;
    memset(&d, 0, sizeof(d));
    memcpy(d.name, raw, 11);
    d.attr = ATTR_DIR;
    d.size = 0;
    d.lo_clus = (u16)(n & 0xFFFF);
    d.hi_clus = (u16)(n >> 16);
    dos_time(d.wr_time, d.wr_date);
    if (!dir_write_entry(dir_clus, &d)) {
        fat_set(n, 0);
        return false;
    }
    return true;
}

/* rmdir: 目录必须为空 (只有 . 与 ..) */
bool fat_rmdir(const char *name)
{
    name = use_path(name);
    if (!name || !WOK)
        return false;
    if (sys_write_denied(name)) return false;
    if (!name[0])
        return false;                 /* 不能删根 */
    struct raw_dir d;
    u32 lba, off;
    if (!get_dirent(name, &d, &lba, &off))
        return false;
    if (!(d.attr & ATTR_DIR))
        return false;

    u32 c = dirent_clus(&d);
    struct dir_iter it;
    di_init(&it, c);
    for (;;) {
        const struct raw_dir *e = di_cur(&it);
        if (!e)
            break;
        if (e->name[0] == 0x00)
            break;
        if (e->name[0] != 0xE5) {
            int dot = (e->name[0] == '.' &&
                       (e->name[1] == ' ' ||
                        (e->name[1] == '.' && e->name[2] == ' ')));
            if (!dot)
                return false;         /* 非空 */
        }
        if (!di_next(&it))
            break;
    }

    free_chain(c);
    if (!read_sect(lba, scratch_b))
        return false;
    scratch_b[off] = 0xE5;
    return write_sect(lba, scratch_b);
}

bool fat_setattr(const char *name, u8 attrs)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    if (sys_write_denied(name)) return false;
    struct raw_dir d;
    u32 lba, off;
    if (!get_dirent(name, &d, &lba, &off))
        return false;
    if (!read_sect(lba, scratch_b))
        return false;
    scratch_b[off + 11] = attrs;
    return write_sect(lba, scratch_b);
}

bool fat_getattr(const char *name, u8 *attrs)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    struct raw_dir d;
    if (!get_dirent(name, &d, 0, 0))
        return false;
    *attrs = d.attr;
    return true;
}

bool fat_touch(const char *name)
{
    name = use_path(name);
    if (!name || !name[0] || !WOK)
        return false;
    if (sys_write_denied(name)) return false;
    struct raw_dir d;
    u32 lba, off;
    if (get_dirent(name, &d, &lba, &off)) {
        u8 t[2], dt[2];
        dos_time(t, dt);
        if (!read_sect(lba, scratch_b))
            return false;
        memcpy(scratch_b + off + 22, t, 2);
        memcpy(scratch_b + off + 24, dt, 2);
        return write_sect(lba, scratch_b);
    }
    return fat_create(name, "", 0);
}
