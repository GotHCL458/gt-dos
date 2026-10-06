/* ==========================================================================
 *  GT-DOS 命令行解释器 (类 DOS Shell)
 *  ---------------------------------------------------------------------------
 *  特性:
 *    - 行编辑 / 命令历史 / 彩色输出 / 滚轮回看 (输入层在 ui.c)
 *    - 真实磁盘 FAT16/32 (DIR/CAT/PUT/DEL/MKDIR/DF/DISK/MOUNT)
 *    - 用户系统 (whoami/users/login/logout/passwd/useradd/userdel)
 *    - 配置持久化 (config/oobe/hostname)
 *    - 电源 (reboot/shutdown; Ctrl+Alt+Del 热重启)
 * ========================================================================== */
#include "gt.h"

#define LINE_MAX   160
#define ARG_MAX    8

/* PIT 通道2 分频, 约 896 Hz 的提示音 */
#define PIT_FREQ_DIV 1331

static const char *version =
    "GT-DOS 0.7 [x64 long mode, NASM + clang/LLVM]";

/* ============================================================ 回滚视图 */
/* 统一的滚动入口: delta>0 向上翻 (看历史), 带上下限钳制与状态指示.
 * 注意: 即使被钳制 (位置没变) 也要刷新指示条, 否则到底后残留旧提示。 */
static void scroll_console(int delta)
{
    vga_scroll_view(delta);
    int off = vga_view_offset();

    if (off <= 0) {
        vga_set_scroll_msg("");
        return;
    }

    int max_off = vga_view_max();
    char msg[44];
    int k = 0;
    const char *pre = " SCROLL +";
    while (*pre) msg[k++] = *pre++;
    char num[8];
    itoa_base((u32)off, num, 10, false);
    for (char *q = num; *q; q++) msg[k++] = *q;
    const char *mid = "/";
    while (*mid) msg[k++] = *mid++;
    itoa_base((u32)max_off, num, 10, false);
    for (char *q = num; *q; q++) msg[k++] = *q;
    const char *post = (off >= max_off) ? " TOP " : " Ctrl+D/wheel=down ";
    while (*post && k < 43) msg[k++] = *post++;
    msg[k] = '\0';
    vga_set_scroll_msg(msg);
}

void shell_wheel_cb(int delta)
{
    scroll_console(delta * 3);        /* 每格滚 3 行 */
}

/* ============================================================ 路径工具 */
#define PATHMAX 160

static char cwd[PATHMAX] = "";           /* 当前目录 (不含盘符), "" = 根 */

/* 规范化绝对路径 "C:\A\.\B\..\C" -> "C:\A\C"; 返回 false 表示非法 */
static bool path_normalize(const char *in, char *out)
{
    /* 拆盘符 */
    char drv[3] = { 0, 0, 0 };
    const char *rest = in;
    if (in[0] && in[1] == ':') {
        drv[0] = (char)toupper_(in[0]);
        drv[1] = ':';
        rest = in + 2;
    } else {
        drv[0] = fat_drive();
        drv[1] = ':';
    }

    char stack[16][16];
    int depth = 0;

    const char *p = rest;
    while (*p) {
        while (*p == '\\' || *p == '/' || *p == ' ')
            p++;
        if (!*p)
            break;
        const char *seg = p;
        while (*p && *p != '\\' && *p != '/')
            p++;
        size_t n = (size_t)(p - seg);
        if (n == 1 && seg[0] == '.')
            continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0)
                depth--;
            continue;
        }
        if (n == 0 || n >= sizeof(stack[0]) || depth >= 16)
            return false;
        memcpy(stack[depth], seg, n);
        stack[depth][n] = '\0';
        depth++;
    }

    int k = 0;
    out[k++] = drv[0];
    out[k++] = drv[1];
    if (depth == 0) {
        out[k++] = '\\';
        out[k] = '\0';
        return true;
    }
    for (int i = 0; i < depth; i++) {
        out[k++] = '\\';
        for (const char *s = stack[i]; *s; s++)
            out[k++] = *s;
    }
    out[k] = '\0';
    return true;
}

/* 把参数解析成绝对路径 (考虑当前盘/当前目录) */
static void resolve_path(const char *arg, char *out)
{
    char raw[PATHMAX];
    int k = 0;
    if (arg[0] && arg[1] == ':') {
        raw[k++] = (char)toupper_(arg[0]);
        raw[k++] = ':';
        const char *c = arg + 2;
        if (*c != '\\' && *c != '/')
            raw[k++] = '\\';          /* "D:INCLUDE" -> "D:\INCLUDE" */
        while (*c && k < PATHMAX - 1)
            raw[k++] = *c++;
        raw[k] = '\0';
    } else if (arg[0] == '\\' || arg[0] == '/') {
        raw[0] = fat_drive();
        raw[1] = ':';
        strncpy(raw + 2, arg, PATHMAX - 4);
        raw[PATHMAX - 1] = '\0';
    } else {
        raw[0] = fat_drive();
        raw[1] = ':';
        k = 2;
        if (cwd[0]) {
            const char *c = cwd;
            while (*c && k < PATHMAX - 2)
                raw[k++] = *c++;
        }
        raw[k++] = '\\';
        const char *c = arg;
        while (*c && k < PATHMAX - 1)
            raw[k++] = *c++;
        raw[k] = '\0';
    }
    if (!path_normalize(raw, out))
        out[0] = '\0';
}

/* 当前目录的绝对路径 (显示用) */
static void cwd_abs(char *out)
{
    char raw[PATHMAX];
    raw[0] = fat_drive();
    raw[1] = ':';
    int k = 2;
    const char *c = cwd;
    while (*c && k < PATHMAX - 2)
        raw[k++] = *c++;
    raw[k++] = '\\';
    raw[k] = '\0';
    path_normalize(raw, out);
}

static void set_cwd_from_abs(const char *abs)
{
    /* abs 形如 "C:\A\B" -> cwd = "\A\B"; 根 -> "" */
    const char *p = abs + 2;
    if (*p == '\\') p++;
    if (!*p) {
        cwd[0] = '\0';
        return;
    }
    cwd[0] = '\\';
    strncpy(cwd + 1, p, PATHMAX - 2);
    cwd[PATHMAX - 1] = '\0';
}

/* ============================================================ 命令解析 */
static int tokenize(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return n;
}

#define err ui_err
#define ok  ui_ok

static bool need_admin(void)
{
    if (!user_is_admin()) {
        err("Permission denied: admin required.\n");
        return false;
    }
    return true;
}

/* ============================================================ 内置命令 */
/* HELP: 图形文本模式无字模槽限制, 单页完整输出 (历史可回滚翻阅) */
static void cmd_help(void)
{
    gt_color(C_LCYAN);
    console_puts(L("help.text"));
    gt_color_reset();
}

static void cmd_ver(void)
{
    gt_color(C_LMAGENTA);
    kprintf("%s\n", version);
    gt_color_reset();
}

static void cmd_date(void)
{
    int y, mo, d, h, mi, s;
    rtc_read(&y, &mo, &d, &h, &mi, &s);
    kprintf("Current date: %04d-%02d-%02d\n", y, mo, d);
}

static void cmd_time(void)
{
    int y, mo, d, h, mi, s;
    rtc_read(&y, &mo, &d, &h, &mi, &s);
    kprintf("Current time: %02d:%02d:%02d\n", h, mi, s);
}

static void cmd_mem(void)
{
    u16 conv_kb = *(volatile u16 *)0x413;
    kprintf("Conventional memory : %u KB (reported by BIOS)\n", (u32)conv_kb);
    kprintf("Kernel base         : %p\n", (void *)0x10000);
    kprintf("VGA text buffer     : %p\n", (void *)0xB8000);
    kprintf("Kernel stack        : 16384 bytes\n");
}

static void cmd_cpuid(void)
{
    u32 a, b, c, d;
    char vendor[13];

    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';

    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));

    kprintf("CPU vendor : %s\n", vendor);
    kprintf("Family/Model/Stepping : %u / %u / %u\n",
            (a >> 8) & 0xF, (a >> 4) & 0xF, a & 0xF);
}

static void cmd_beep(void)
{
    u8 tmp = inb(0x61);
    if ((tmp & 3) != 3)
        outb(0x61, (u8)(tmp | 3));
    u32 div = PIT_FREQ_DIV;
    outb(0x43, 0xB6);
    outb(0x42, (u8)(div & 0xFF));
    outb(0x42, (u8)((div >> 8) & 0xFF));
    timer_sleep_ms(120);
    tmp = inb(0x61);
    outb(0x61, (u8)(tmp & ~3));
}

static void cmd_uptime(void)
{
    u32 ms = timer_uptime_ms();
    kprintf("Uptime : %u.%03u s (%u ticks)\n",
            ms / 1000, ms % 1000, timer_ticks());
}

static void cmd_color(char *arg)
{
    if (!arg) {
        err("Usage: COLOR <0-15>\n");
        return;
    }
    int v = atoi_dec(arg);
    if (v < 0 || v > 15 || !isdigit_(arg[0])) { err("Color must be 0-15\n"); return; }
    gt_color((u8)v);
    kprintf("Text color set to %d\n", v);
}

/* ------------------------------------------------------------ 用户命令 */
static void cmd_whoami(void)
{
    const struct gt_user *u = user_current();
    kprintf("%s%s\n", u->name, (u->flags & UF_ADMIN) ? " (admin)" : "");
}

static void cmd_users(void)
{
    int n = user_count();
    if (n == 0) {
        kprintf("No users registered (default 'admin' in use).\n");
        kprintf("Run OOBE or USERADD to create accounts.\n");
        return;
    }
    kprintf("%-16s %-8s %s\n", "USER", "PASSWD", "ROLE");
    for (int i = 0; i < n; i++) {
        const struct gt_user *u = user_get(i);
        kprintf("%-16s %-8s %s%s\n",
                u->name,
                u->hash ? "yes" : "none",
                (u->flags & UF_ADMIN) ? "admin" : "user",
                !strcmp(u->name, user_name()) ? "  <= current" : "");
    }
}

static void do_login(void)
{
    const struct gt_user *u = ui_login_screen();
    user_login(u);
    vga_scroll_reset();
    cfg_apply_theme();
}

static void cmd_passwd(int argc, char **argv)
{
    const char *target = user_name();
    if (argc > 1) {
        target = argv[1];
        if (strcmp(target, user_name()) && !user_is_admin()) {
            err("Only admin can change other users' passwords.\n");
            return;
        }
    }
    if (!user_exists(target)) {
        err("No such user: %s\n", target);
        return;
    }

    char p1[64], p2[64];
    if (ui_read_line("New password (empty = none): ", p1, sizeof(p1), '*',
                     false) != 0)
        return;
    if (p1[0]) {
        if (ui_read_line("Repeat password:           ", p2, sizeof(p2), '*',
                         false) != 0) {
            return;
        }
        if (strcmp(p1, p2)) {
            err("Passwords do not match.\n");
            return;
        }
        if (strlen(p1) < 3) {
            err("Password too short (min 3).\n");
            return;
        }
    }
    user_setpass(target, p1);
    memset(p1, 0, sizeof(p1));
    memset(p2, 0, sizeof(p2));
    if (user_save())
        ok("Password updated for '%s'.\n", target);
    else
        ui_info("Password updated (not saved: no disk).\n");
}

static void cmd_useradd(int argc, char **argv)
{
    if (!need_admin()) return;
    if (argc < 2) { err("Usage: USERADD <name> [-a]\n"); return; }
    u8 flags = 0;
    if (argc > 2 && !strcmp(argv[2], "-a"))
        flags = UF_ADMIN;

    char p1[64], p2[64];
    if (ui_read_line("Password (empty = none): ", p1, sizeof(p1), '*',
                     false) != 0)
        return;
    if (p1[0]) {
        if (ui_read_line("Repeat:                    ", p2, sizeof(p2), '*',
                         false) != 0) {
            return;
        }
        if (strcmp(p1, p2)) { err("Passwords do not match.\n"); return; }
    }
    if (user_add(argv[1], p1, flags)) {
        memset(p1, 0, sizeof(p1));
        memset(p2, 0, sizeof(p2));
        if (user_save())
            ok("User '%s' created (%s).\n", argv[1],
               flags ? "admin" : "normal");
        else
            ui_info("User created (not saved: no disk).\n");
    } else {
        err("Cannot create '%s' (invalid name, exists, or table full).\n",
            argv[1]);
    }
}

static void cmd_userdel(int argc, char **argv)
{
    if (!need_admin()) return;
    if (argc < 2) { err("Usage: USERDEL <name>\n"); return; }
    if (!strcmp(argv[1], user_name())) {
        err("Cannot delete the logged-in user. Logout first.\n");
        return;
    }
    if (user_del(argv[1])) {
        user_save();
        ok("User '%s' deleted.\n", argv[1]);
    } else {
        err("No such user: %s\n", argv[1]);
    }
}

/* ------------------------------------------------------------ 系统命令 */
static void cmd_hostname(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("%s\n", gcfg.hostname);
        return;
    }
    if (!need_admin()) return;
    size_t l = strlen(argv[1]);
    if (l == 0 || l >= sizeof(gcfg.hostname)) {
        err("Name too long (max %d).\n", (int)sizeof(gcfg.hostname) - 1);
        return;
    }
    for (size_t i = 0; i < l; i++) {
        char c = argv[1][i];
        if (!(isalpha_(c) || isdigit_(c) || c == '-' || c == '_')) {
            err("Only letters/digits/-/_ allowed.\n");
            return;
        }
    }
    strncpy(gcfg.hostname, argv[1], l);
    gcfg.hostname[l] = '\0';
    if (cfg_save())
        ok("Host name set to '%s'.\n", gcfg.hostname);
    else
        ui_info("Host name set (not saved: no disk).\n");
}

static void cmd_config(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "show";
    if (!strncasecmp(sub, "show", 8)) {
        kprintf("oobe_done  = %s\n", gcfg.oobe_done ? "yes" : "no");
        kprintf("lang       = %s\n", gcfg.lang ? "zh" : "en");
        kprintf("theme      = fg %u / bg %u\n", gcfg.theme_fg, gcfg.theme_bg);
        kprintf("hostname   = %s\n", gcfg.hostname);
        kprintf("last_user  = %s\n", gcfg.last_user);
        kprintf("storage    = %s\n", fat_ready() ? "\\GT-DOS\\GTOS.CFG on FAT"
                                                 : "memory only (no disk)");
    } else if (!strncasecmp(sub, "reset", 8)) {
        if (!need_admin()) return;
        gcfg.oobe_done = false;
        gcfg.theme_fg = C_LGRAY;
        gcfg.theme_bg = C_BLACK;
        strncpy(gcfg.hostname, "GT-DOS", sizeof(gcfg.hostname) - 1);
        cfg_save();
        ok("Config reset. OOBE will run on next boot.\n");
    } else {
        err("Usage: CONFIG SHOW|RESET\n");
    }
}

static void cmd_oobe(void)
{
    if (!need_admin()) return;
    oobe_run();
}

/* ------------------------------------------------------------ 磁盘命令 */
static void cmd_disk(void)
{
    int n = ata_ndev();
    if (n == 0) {
        err("No ATA disk found.\n");
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (!ata_present_dev(i)) continue;
        u32 sec = ata_sectors_dev(i);
        gt_color(C_LCYAN);
        kprintf("ATA dev%d : %s\n", i, ata_model_dev(i));
        gt_color_reset();
        kprintf("  sectors=%u  size=%u MB\n", sec,
                (sec * 512u) / (1024u * 1024u));
    }
}

static void cmd_mount(void)
{
    if (ata_ndev() == 0) {
        err("No ATA disk found.\n");
        return;
    }
    if (fat_mount()) {
        ok("Mounted %d drive(s):", fat_drive_count());
        for (int i = 0; i < fat_drive_count(); i++)
            kprintf(" %c:", fat_drive_letter(i));
        kprintf("\n");
    } else {
        err("Mount failed: no FAT16/32 volume.\n");
    }
}

static void cmd_df(void)
{
    if (!fat_ready()) {
        err("No volume mounted. Type MOUNT first.\n");
        return;
    }
    struct fat_dirent d;
    char p[40];
    int k = 0;
    p[k++] = fat_drive(); p[k++] = ':';
    const char *cf = "\\GT-DOS\\GTOS.CFG";
    while (*cf) p[k++] = *cf++;
    p[k] = '\0';
    u32 total = fat_total_clusters();
    u32 free_c = fat_free_clusters();
    kprintf("Drive %c:\n", fat_drive());
    kprintf("  Clusters total : %u\n", total);
    kprintf("  Clusters free  : %u\n", free_c);
    kprintf("  Config file    : %s\n", fat_find(p, &d) ? "GTOS.CFG" : "(none)");
    k = 0;
    p[k++] = fat_drive(); p[k++] = ':';
    const char *uf = "\\GT-DOS\\USERS.SYS";
    while (*uf) p[k++] = *uf++;
    p[k] = '\0';
    kprintf("  User database  : %s\n", fat_find(p, &d) ? "USERS.SYS" : "(none)");
}

static void list_dir(const char *path, bool showall)
{
    char abs[PATHMAX];
    if (path && path[0])
        resolve_path(path, abs);
    else
        cwd_abs(abs);

    if (!fat_ready()) {
        err("No volume mounted. Type MOUNT first.\n");
        return;
    }
    struct fat_dirent ents[64];
    fat_set_show_all(showall);
    int n = fat_list_dir(abs, ents, 64);
    fat_set_show_all(false);
    if (n < 0) {
        err("Cannot open directory: %s\n", abs);
        return;
    }
    kprintf(" Directory of %s\n\n", abs);
    u32 total = 0, nfiles = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir) {
            gt_color(C_LBLUE);
            kprintf("  %-14s %8s  %c%c%c%c <DIR>\n", ents[i].name, "",
                    (ents[i].attrs & ATTR_RO)  ? 'r' : '-',
                    (ents[i].attrs & ATTR_HID) ? 'h' : '-',
                    (ents[i].attrs & ATTR_SYS) ? 's' : '-',
                    (ents[i].attrs & ATTR_ARC) ? 'a' : '-');
            gt_color_reset();
        } else {
            gt_color((ents[i].attrs & ATTR_RO) ? C_DGRAY : C_WHITE);
            kprintf("  %-14s %8u  %c%c%c%c\n", ents[i].name, ents[i].size,
                    (ents[i].attrs & ATTR_RO)  ? 'r' : '-',
                    (ents[i].attrs & ATTR_HID) ? 'h' : '-',
                    (ents[i].attrs & ATTR_SYS) ? 's' : '-',
                    (ents[i].attrs & ATTR_ARC) ? 'a' : '-');
            gt_color_reset();
            total += ents[i].size;
            nfiles++;
        }
    }
    kprintf("\n  %u file(s), %u bytes\n", nfiles, total);
}

/* ---------------------------------------------------------------- cd/pwd */
static bool cmd_cd(char *arg)
{
    if (!arg) {
        char abs[PATHMAX];
        cwd_abs(abs);
        kprintf("%s\n", abs);
        return true;
    }

    char abs[PATHMAX];
    resolve_path(arg, abs);
    if (!abs[0]) { err("Invalid path: %s\n", arg); return true; }

    /* 跨盘 cd: 先切换盘符 */
    if (arg[0] && arg[1] == ':' && toupper_(arg[0]) != fat_drive()) {
        if (!fat_ready_drive((char)toupper_(arg[0]))) {
            err("Invalid drive %c:\n", toupper_(arg[0]));
            return true;
        }
        fat_set_drive((char)toupper_(arg[0]));
        cwd[0] = '\0';
    }

    /* 盘根目录特判: "X:\" */
    if (abs[2] == '\\' && abs[3] == '\0') {
        if (!fat_ready()) { err("Invalid drive.\n"); return true; }
        set_cwd_from_abs(abs);
        return true;
    }

    struct fat_dirent d;
    if (!fat_find(abs, &d)) {
        err("Path not found: %s\n", abs);
        return true;
    }
    if (!d.is_dir) {
        err("Not a directory: %s\n", abs);
        return true;
    }
    set_cwd_from_abs(abs);
    return true;
}

static void cmd_pwd(void)
{
    char abs[PATHMAX];
    cwd_abs(abs);
    kprintf("%s\n", abs);
}

/* 切换盘符: "C:" / "D:" */
static bool cmd_drive_switch(const char *arg)
{
    char l = (char)toupper_(arg[0]);
    if (!fat_ready_drive(l)) {
        err("Invalid drive %c:\n", l);
        return true;
    }
    fat_set_drive(l);
    cwd[0] = '\0';                     /* 每个盘独立根目录起点 */
    return true;
}

static void cmd_dir_d(void)
{
    kprintf(" Drives:\n");
    for (int i = 0; i < fat_drive_count(); i++) {
        char l = fat_drive_letter(i);
        kprintf("   %c: (%s)\n", l, l == fat_drive() ? "current" : "mounted");
    }
}

/* --------------------------------------------------------- busybox 风格 */
static void cmd_touch(char *arg)
{
    if (!arg) { err("Usage: TOUCH <file>\n"); return; }
    char abs[PATHMAX];
    resolve_path(arg, abs);
    if (fat_touch(abs))
        ok("Touched %s\n", abs);
    else
        err("Cannot touch %s\n", abs);
}

static void cmd_rmdir(char *arg)
{
    if (!arg) { err("Usage: RMDIR <dir>\n"); return; }
    char abs[PATHMAX];
    resolve_path(arg, abs);
    if (fat_rmdir(abs))
        ok("Removed %s\n", abs);
    else
        err("Cannot remove %s (missing or not empty)\n", abs);
}

static void cmd_attrib(int argc, char **argv)
{
    if (argc < 2) { err("Usage: ATTRIB <file> [+r|-r][+h|-h][+s|-s][+a|-a]\n");
        return; }
    char abs[PATHMAX];
    resolve_path(argv[1], abs);

    u8 attrs;
    if (!fat_getattr(abs, &attrs)) {
        err("File not found: %s\n", abs);
        return;
    }
    if (argc >= 3) {
        for (int i = 2; i < argc; i++) {
            char *m = argv[i];
            if (m[0] != '+' && m[0] != '-') {
                err("Bad mode: %s\n", m);
                return;
            }
            u8 bit = 0;
            char c = (char)tolower_(m[1]);
            if (c == 'r') bit = ATTR_RO;
            else if (c == 'h') bit = ATTR_HID;
            else if (c == 's') bit = ATTR_SYS;
            else if (c == 'a') bit = ATTR_ARC;
            else { err("Bad mode: %s\n", m); return; }
            if (m[0] == '+') attrs |= bit;
            else attrs &= (u8)~bit;
        }
        if (!fat_setattr(abs, attrs)) {
            err("Cannot set attributes.\n");
            return;
        }
    }
    kprintf("%c%c%c%c  %s\n",
            (attrs & ATTR_RO)  ? 'R' : '-',
            (attrs & ATTR_HID) ? 'H' : '-',
            (attrs & ATTR_SYS) ? 'S' : '-',
            (attrs & ATTR_ARC) ? 'A' : '-',
            abs);
}

/* 文件复制: 读 src -> 写 dst */
static void cmd_cp(int argc, char **argv)
{
    if (argc < 3) { err("Usage: CP <src> <dst>\n"); return; }
    char sa[PATHMAX], da[PATHMAX];
    resolve_path(argv[1], sa);
    resolve_path(argv[2], da);

    static u8 buf[4096];
    u32 got = 0;
    if (!fat_read_file(sa, buf, sizeof(buf), &got)) {
        err("Cannot read %s\n", sa);
        return;
    }
    if (!fat_create(da, buf, got)) {
        err("Cannot write %s\n", da);
        return;
    }
    ok("Copied %u bytes: %s -> %s\n", got, sa, da);
}

static void cmd_mv(int argc, char **argv)
{
    if (argc < 3) { err("Usage: MV <src> <dst>\n"); return; }
    char sa[PATHMAX], da[PATHMAX];
    resolve_path(argv[1], sa);
    resolve_path(argv[2], da);

    static u8 buf[4096];
    u32 got = 0;
    if (!fat_read_file(sa, buf, sizeof(buf), &got)) {
        err("Cannot read %s\n", sa);
        return;
    }
    if (!fat_create(da, buf, got)) {
        err("Cannot write %s\n", da);
        return;
    }
    if (!fat_delete(sa)) {
        err("Moved but cannot delete source %s\n", sa);
        return;
    }
    ok("Moved %s -> %s\n", sa, da);
}

static void cmd_head(int argc, char **argv)
{
    if (argc < 2) { err("Usage: HEAD [-n] <file>\n"); return; }
    int lines = 10;
    int fi = 1;
    if (argv[1][0] == '-' && isdigit_(argv[1][1])) {
        lines = atoi_dec(argv[1] + 1);
        fi = 2;
        if (argc < 3) { err("Usage: HEAD -n <file>\n"); return; }
    }
    char abs[PATHMAX];
    resolve_path(argv[fi], abs);

    static u8 buf[4096];
    u32 got = 0;
    if (!fat_read_file(abs, buf, sizeof(buf) - 1, &got)) {
        err("File not found: %s\n", abs);
        return;
    }
    buf[got] = '\0';
    int shown = 0;
    char *p = (char *)buf;
    while (*p && shown < lines) {
        char *nl = p;
        while (*nl && *nl != '\n')
            nl++;
        int is_last = (*nl == '\0');
        *nl = '\0';
        console_puts(p);
        console_putc('\n');
        shown++;
        if (is_last)
            break;
        p = nl + 1;
    }
}

static void cmd_wc(int argc, char **argv)
{
    if (argc < 2) { err("Usage: WC <file>\n"); return; }
    char abs[PATHMAX];
    resolve_path(argv[1], abs);

    static u8 buf[4096];
    u32 got = 0;
    if (!fat_read_file(abs, buf, sizeof(buf) - 1, &got)) {
        err("File not found: %s\n", abs);
        return;
    }
    buf[got] = '\0';
    u32 lines = 0, words = 0, bytes = got;
    bool in_w = false;
    for (u32 i = 0; i < got; i++) {
        char c = (char)buf[i];
        if (c == '\n')
            lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_w = false;
        } else if (!in_w) {
            in_w = true;
            words++;
        }
    }
    kprintf("%u %u %u %s\n", lines, words, bytes, abs);
}

static void cmd_tree(const char *path)
{
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    char abs[PATHMAX];
    if (path && path[0]) resolve_path(path, abs);
    else cwd_abs(abs);

    struct fat_dirent ents[64];
    int n = fat_list_dir(abs, ents, 64);
    if (n < 0) { err("Cannot open %s\n", abs); return; }
    kprintf("%s\n", abs);
    for (int i = 0; i < n; i++) {
        if (!ents[i].is_dir) continue;
        char sub[PATHMAX];
        int k = 0;
        const char *a = abs;
        while (*a && k < PATHMAX - 20) sub[k++] = *a++;
        if (k > 1 && sub[k - 1] == '\\') k--;
        sub[k++] = '\\';
        const char *nm = ents[i].name;
        while (*nm) sub[k++] = *nm++;
        sub[k] = '\0';
        gt_color(C_LBLUE);
        kprintf("  |- %s\n", ents[i].name);
        gt_color_reset();
        cmd_tree(sub);
    }
}

/* ------------------------------------------------------------ 磁盘命令 */
static void cmd_type(const char *arg)
{
    if (!arg) { err("Usage: TYPE <file>\n"); return; }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }

    char abs[PATHMAX];
    resolve_path(arg, abs);
    static u8 buf[4096];
    u32 got = 0;
    if (!fat_read_file(abs, buf, sizeof(buf) - 1, &got)) {
        err("File not found: %s\n", abs);
        return;
    }
    buf[got] = '\0';
    console_write((const char *)buf, got);
    if (got && buf[got - 1] != '\n')
        console_putc('\n');
}

static void cmd_put(const char *name, const char *text)
{
    if (!name) { err("Usage: PUT <file> <text>\n"); return; }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    if (!text) text = "";
    char abs[PATHMAX];
    resolve_path(name, abs);
    u32 len = (u32)strlen(text);
    if (fat_create(abs, text, len))
        ok("Wrote %u bytes to %s\n", len, abs);
    else
        err("Failed to create %s\n", abs);
}

static void cmd_del(const char *name)
{
    if (!name) { err("Usage: DEL <file>\n"); return; }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    char abs[PATHMAX];
    resolve_path(name, abs);
    if (fat_delete(abs))
        ok("Deleted %s\n", abs);
    else
        err("Cannot delete %s (missing / read-only / protected)\n", abs);
}

static void cmd_mkdir(const char *name)
{
    if (!name) { err("Usage: MKDIR <dir>\n"); return; }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    char abs[PATHMAX];
    resolve_path(name, abs);
    if (fat_mkdir(abs))
        ok("Created directory %s\n", abs);
    else
        err("Cannot create %s\n", abs);
}

/* ---------------------------------------------------------- 分辨率命令 */
static void cmd_res(int argc, char **argv)
{
    if (argc < 2 || !strncasecmp(argv[1], "list", 8)) {
        kprintf("Supported resolutions (32bpp):\n");
        for (int i = 0; i < gfx_mode_count(); i++) {
            int w, h;
            gfx_mode_size(i, &w, &h);
            kprintf("  %2d) %dx%d%s\n", i + 1, w, h,
                    (w == gfx_w() && h == gfx_h()) ? "  <- current" : "");
        }
        return;
    }
    if (!gfx_active()) {
        err("Text mode active, no VBE. RES requires graphics console.\n");
        return;
    }
    int w = 0, h = 0;
    const char *p = argv[1];
    while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
    if (*p == 'x' || *p == 'X') {
        p++;
        while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    }
    if (!w || !h) {
        err("Usage: RES <W>x<H> | RES list   (e.g. RES 1024x768)\n");
        return;
    }
    if (!gfx_set_mode(w, h)) {
        err("Unsupported mode %dx%d (try RES list)\n", w, h);
        return;
    }
    vga_relayout();                     /* 文本网格随分辨率重排 */
    ok("Resolution set to %dx%d.\n", gfx_w(), gfx_h());
}

/* ------------------------------------------------------------ 中断命令 */
static void cmd_apic(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strncasecmp(sub, "on", 8)) {
        if (!need_admin()) return;
        if (apic_enable())
            ok("APIC mode enabled (IO-APIC + local APIC, priority vectors).\n");
        else
            err("APIC unavailable (no CPUID flag / MMIO).\n");
    } else if (!strncasecmp(sub, "off", 8)) {
        if (!need_admin()) return;
        if (apic_active()) {
            apic_disable();
            pic_init();                 /* 回到 8259A */
            ok("APIC disabled, PIC restored.\n");
        } else {
            ui_info("Already in PIC mode.\n");
        }
    } else {
        kprintf("APIC hardware : %s\n", apic_available() ? "present" : "absent");
        kprintf("Current mode  : %s\n", apic_active() ? "IO-APIC + LAPIC"
                                                      : "8259A PIC");
    }
}

static void cmd_irqstat(void)
{
    kprintf("IRQ controller : %s\n", irq_using_apic() ? "APIC" : "8259A PIC");
    kprintf("In service now : %d\n", irq_in_service());
    kprintf("Max nesting    : %d\n", irq_nest_max());
}

/* ============================================================ 分发 */
static void execute(char *line)
{
    char *argv[ARG_MAX];
    int argc = tokenize(line, argv, ARG_MAX);
    if (argc == 0)
        return;

    const char *cmd = argv[0];

    /* 盘符切换: "C:" / "D:" */
    if (cmd[1] == ':' && cmd[2] == '\0' && isalpha_(cmd[0])) {
        cmd_drive_switch(cmd);
        return;
    }

    if (!strncasecmp(cmd, "help", 8) || !strcmp(cmd, "?")) {
        cmd_help();
    } else if (!strncasecmp(cmd, "ver", 8)) {
        cmd_ver();
    } else if (!strncasecmp(cmd, "cls", 8)) {
        console_clear();
    } else if (!strncasecmp(cmd, "echo", 8)) {
        for (int i = 1; i < argc; i++)
            kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
        kprintf("\n");
    } else if (!strncasecmp(cmd, "cd", 8)) {
        cmd_cd(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "pwd", 8)) {
        cmd_pwd();
    } else if (!strncasecmp(cmd, "drives", 8)) {
        cmd_dir_d();
    } else if (!strncasecmp(cmd, "date", 8)) {
        cmd_date();
    } else if (!strncasecmp(cmd, "time", 8)) {
        cmd_time();
    } else if (!strncasecmp(cmd, "mem", 8)) {
        cmd_mem();
    } else if (!strncasecmp(cmd, "cpuid", 8)) {
        cmd_cpuid();
    } else if (!strncasecmp(cmd, "beep", 8)) {
        cmd_beep();
    } else if (!strncasecmp(cmd, "uptime", 8)) {
        cmd_uptime();
    } else if (!strncasecmp(cmd, "color", 8)) {
        cmd_color(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "serial", 8)) {
        if (argc > 1 && !strncasecmp(argv[1], "off", 8)) {
            console_set_serial(false);
            console_puts("Serial output disabled\n");
        } else {
            console_set_serial(true);
            console_puts("Serial output enabled\n");
        }
    } else if (!strncasecmp(cmd, "whoami", 8)) {
        cmd_whoami();
    } else if (!strncasecmp(cmd, "users", 8) || !strncasecmp(cmd, "userlist", 10)) {
        cmd_users();
    } else if (!strncasecmp(cmd, "login", 8)) {
        do_login();
    } else if (!strncasecmp(cmd, "logout", 8) || !strncasecmp(cmd, "exit", 8)) {
        ok("Logging out '%s'.\n", user_name());
        do_login();
    } else if (!strncasecmp(cmd, "passwd", 8) || !strncasecmp(cmd, "password", 10)) {
        cmd_passwd(argc, argv);
    } else if (!strncasecmp(cmd, "useradd", 8)) {
        cmd_useradd(argc, argv);
    } else if (!strncasecmp(cmd, "userdel", 8)) {
        cmd_userdel(argc, argv);
    } else if (!strncasecmp(cmd, "hostname", 8)) {
        cmd_hostname(argc, argv);
    } else if (!strncasecmp(cmd, "config", 8)) {
        cmd_config(argc, argv);
    } else if (!strncasecmp(cmd, "oobe", 8)) {
        cmd_oobe();
    } else if (!strncasecmp(cmd, "gui", 8)) {
        gui_run();
    } else if (!strncasecmp(cmd, "res", 8)) {
        cmd_res(argc, argv);
    } else if (!strncasecmp(cmd, "apic", 8)) {
        cmd_apic(argc, argv);
    } else if (!strncasecmp(cmd, "irqstat", 8)) {
        cmd_irqstat();
    } else if (!strncasecmp(cmd, "disk", 8)) {
        cmd_disk();
    } else if (!strncasecmp(cmd, "mount", 8)) {
        cmd_mount();
    } else if (!strncasecmp(cmd, "df", 8)) {
        cmd_df();
    } else if (!strncasecmp(cmd, "dir", 8) || !strncasecmp(cmd, "ls", 8)) {
        bool showall = false;
        const char *arg = argc > 1 ? argv[1] : "";
        if (arg[0] == '/' && (arg[1] == 'a' || arg[1] == 'A')) {
            showall = true;
            arg = argc > 2 ? argv[2] : "";
        }
        list_dir(arg, showall);
    } else if (!strncasecmp(cmd, "type", 8) || !strncasecmp(cmd, "cat", 8)) {
        cmd_type(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "put", 8)) {
        static char text[LINE_MAX];
        text[0] = '\0';
        size_t used = 0;
        for (int i = 2; i < argc; i++) {
            size_t l = strlen(argv[i]);
            if (used + l + 2 >= sizeof(text))
                break;
            if (used)
                text[used++] = ' ';
            memcpy(text + used, argv[i], l);
            used += l;
            text[used] = '\0';
        }
        cmd_put(argc > 1 ? argv[1] : 0, text);
    } else if (!strncasecmp(cmd, "del", 8) || !strncasecmp(cmd, "rm", 8)) {
        cmd_del(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "mkdir", 8) || !strncasecmp(cmd, "md", 8)) {
        cmd_mkdir(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "rmdir", 8)) {
        cmd_rmdir(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "touch", 8)) {
        cmd_touch(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "attrib", 8)) {
        cmd_attrib(argc, argv);
    } else if (!strncasecmp(cmd, "cp", 8) || !strncasecmp(cmd, "copy", 8)) {
        cmd_cp(argc, argv);
    } else if (!strncasecmp(cmd, "mv", 8) || !strncasecmp(cmd, "ren", 8) ||
               !strncasecmp(cmd, "rename", 8)) {
        cmd_mv(argc, argv);
    } else if (!strncasecmp(cmd, "head", 8)) {
        cmd_head(argc, argv);
    } else if (!strncasecmp(cmd, "wc", 8)) {
        cmd_wc(argc, argv);
    } else if (!strncasecmp(cmd, "tree", 8)) {
        cmd_tree(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "reboot", 8)) {
        if (!need_admin()) return;
        power_reboot();
    } else if (!strncasecmp(cmd, "shutdown", 10) || !strncasecmp(cmd, "poweroff", 10)) {
        if (!need_admin()) return;
        power_shutdown();
    } else if (!strncasecmp(cmd, "div0", 8)) {
        kprintf("Raising divide-by-zero...\n");
        __asm__ __volatile__("xorl %%edx, %%edx\n\t"
                             "xorl %%ecx, %%ecx\n\t"
                             "movl $1, %%eax\n\t"
                             "divl %%ecx"
                             ::: "eax", "ecx", "edx");
    } else {
        err("Bad command: %s  (type HELP)\n", cmd);
    }
}

/* ============================================================ 入口 */
static void print_banner(void)
{
    gt_color(C_LCYAN);
    console_puts(
        "\n  ____ _____     ____   ___  ____\n"
        " / ___|_   _|   |  _ \\ / _ \\/ ___|\n"
        "| |  _  | |     | | | | | | \\___ \\\n"
        "| |_| | | |     | |_| | |_| |___) |\n"
        " \\____| |_|     |____/ \\___/|____/\n"
        "\n");
    gt_color_reset();
    gt_color(C_LMAGENTA);
    console_puts(L("shell.banner"));
    gt_color_reset();
    kprintf("Console: VGA + COM1. Wheel/Ctrl+Up/Down = scroll, Up/Down = history.\n");
    kprintf(L("shell.hint"), user_name(), gcfg.hostname);
}

void shell_run(void)
{
    static char line[LINE_MAX];

    ui_set_scroll_cb(scroll_console);
    print_banner();

    for (;;) {
        console_puts("\n");
        char prompt[LINE_MAX];
        int n = 0;
        const char *u = user_name();
        while (*u && n < LINE_MAX - 40) prompt[n++] = *u++;
        prompt[n++] = '@';
        const char *h = gcfg.hostname;
        while (*h && n < LINE_MAX - 40) prompt[n++] = *h++;
        prompt[n++] = ' ';
        prompt[n++] = fat_drive();
        prompt[n++] = ':';
        if (cwd[0]) {
            const char *c = cwd;
            while (*c && n < LINE_MAX - 4) prompt[n++] = *c++;
        } else {
            prompt[n++] = '\\';
        }
        prompt[n++] = '>';
        prompt[n] = '\0';

        int r = ui_read_line(prompt, line, LINE_MAX, 0, true);
        if (r == -1)
            power_reboot();
        if (r == 1)
            continue;                 /* Ctrl-C */
        if (line[0]) {
            ui_hist_add(line);
            execute(line);
            tui_status_refresh();     /* cd/D:/登录 后刷新状态栏 */
        }
    }
}