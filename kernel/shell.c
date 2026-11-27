/* 命令行解释器 (类 DOS Shell). 输入层在 ui.c, 提供行编辑/命令历史/彩色输出/
   滚轮回看. 命令涵盖 exFAT 磁盘操作, 用户系统, 配置持久化, 电源管理
   (Ctrl+Alt+Del 热重启). */
#include "gt.h"

#define LINE_MAX   160
#define ARG_MAX    8
#define FILE_BUF_MAX 65536

/* 文件命令共用缓冲 (TYPE/CP/MV/HEAD/WC), 避免 4KB 截断 */
static u8 file_buf[FILE_BUF_MAX] __attribute__((section(".highbss")));

/* PIT 通道2 分频, 约 896 Hz 的提示音 */
#define PIT_FREQ_DIV 1331

static const char *version = GT_VERSION;

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
    while (*pre && k < 43) msg[k++] = *pre++;
    char num[8];
    itoa_base((u32)off, num, 10, false);
    for (char *q = num; *q && k < 43; q++) msg[k++] = *q;
    const char *mid = "/";
    while (*mid && k < 43) msg[k++] = *mid++;
    itoa_base((u32)max_off, num, 10, false);
    for (char *q = num; *q && k < 43; q++) msg[k++] = *q;
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

const char *shell_cwd(void) { return cwd; }

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
        if (k >= PATHMAX - 1) return false;
        out[k++] = '\\';
        for (const char *s = stack[i]; *s; s++) {
            if (k >= PATHMAX - 1) return false;
            out[k++] = *s;
        }
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

/* 系统文件 (\GT-DOS\) 拒绝读取: 加密内容不对外暴露, 管理员亦然 */
static bool read_denied(const char *abs)
{
    if (fat_path_protected(abs)) {
        err("System file is protected (read denied).\n");
        return true;
    }
    return false;
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

/* KERNEL: 显示内核容器 (GTK1) 自检结果, 并核对磁盘上的受保护 .gtp 视图.
   KERNEL VERIFY: 只做运行时只读段 CRC32 复核 (防篡改). */
static void cmd_kernel(int argc, char **argv)
{
    if (argc > 1 && !strcasecmp(argv[1], "verify")) {
        int rc = kernel_reverify();
        if (rc == 0)
            ok("Kernel runtime check: verified (code+rodata CRC32)\n");
        else
            err("Kernel runtime check FAILED (%d)\n", rc);
        return;
    }

    u32 ilen = 0, crc = 0;
    u8 digest[32];
    if (!kernel_protected_info(&ilen, &crc, digest)) {
        err("Kernel container: NOT VERIFIED\n");
        return;
    }

    static const char hx[] = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = hx[digest[i] >> 4];
        hex[i * 2 + 1] = hx[digest[i] & 15];
    }
    hex[64] = 0;

    gt_color(C_LCYAN);
    kprintf("Kernel container : GTK1 v1 (verified at boot)\n");
    gt_color_reset();
    kprintf("Image size       : %u bytes (%u sectors)\n",
            ilen, (ilen + 511) / 512);
    kprintf("CRC32            : 0x%08x\n", crc);
    kprintf("SHA-256          : %s\n", hex);
    kprintf("Runtime check    : %s\n",
            kernel_reverify() == 0 ? "verified (code+rodata RO, 4KB pages)"
                                   : "FAILED");
    kprintf("Runtime task     : kernel.core / kernel.image (SYS, protected)\n");

    if (!fat_ready()) {
        ui_info("Disk image: volume not mounted (KERNEL.GTP not checked).\n");
        return;
    }

    char abs[PATHMAX];
    resolve_path("\\GT-DOS\\KERNEL.GTP", abs);
    struct gt_statx sx;
    if (fat_statx(abs, &sx) != 0) {
        ui_info("Disk image: \\GT-DOS\\KERNEL.GTP not found.\n");
        return;
    }

    u32 got = 0;
    if (!fat_read_file(abs, (void *)PROG_BASE, (u32)PROG_MAX, &got)) {
        err("Disk image: read failed.\n");
        return;
    }
    u8 ddisk[32] = { 0 };
    if (got >= ilen)
        gt_sha256((const u8 *)PROG_BASE, ilen, ddisk);
    /* 磁盘视图 = 内核镜像 + 48 字节容器尾部 */
    if (got == ilen + 48 && memcmp(ddisk, digest, 32) == 0)
        ok("Disk image       : \\GT-DOS\\KERNEL.GTP matches (protected .gtp)\n");
    else
        err("Disk image       : mismatch (size %u, expected %u)\n",
            got, ilen + 48);
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
        if (fat_ready()) {
            fat_mkdir("\\USERS");                 /* 可能已存在, 忽略返回 */
            char home[PATHMAX];
            int n = 0;
            const char *pre = "\\USERS\\";
            while (*pre && n < PATHMAX - 1) home[n++] = *pre++;
            for (int i = 0; argv[1][i] && n < PATHMAX - 1; i++)
                home[n++] = argv[1][i];
            home[n] = '\0';
            fat_mkdir(home);                      /* 该用户家目录 */
        }
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
        kprintf("gui_default= %s\n", gcfg.gui_default ? "yes" : "no");
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
    } else if (!strncasecmp(sub, "gui", 3)) {
        if (argc < 3) {
            kprintf("gui_default = %s\n", gcfg.gui_default ? "yes" : "no");
            return;
        }
        if (!need_admin()) return;
        bool on = !strcasecmp(argv[2], "on") || !strcasecmp(argv[2], "yes") ||
                  !strcmp(argv[2], "1");
        bool off = !strcasecmp(argv[2], "off") || !strcasecmp(argv[2], "no") ||
                   !strcmp(argv[2], "0");
        if (!on && !off) {
            err("Usage: CONFIG GUI ON|OFF\n");
            return;
        }
        gcfg.gui_default = on;
        if (cfg_save())
            ok("Default GUI at startup: %s\n", on ? "on" : "off");
        else
            ui_info("Default GUI set (not saved: no disk).\n");
    } else {
        err("Usage: CONFIG SHOW|RESET|GUI ON|OFF\n");
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
    if (!need_admin()) return;
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
        err("Mount failed: no exFAT volume.\n");
    }
}

static void cmd_run(int argc, char **argv)
{
    if (!need_admin()) return;
    if (argc < 2) { err("Usage: RUN <file.gtp>\n"); return; }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    char abs[PATHMAX];
    resolve_path(argv[1], abs);
    int code = exec_run(abs);
    if (code < 0) {
        const char *why;
        switch (code) {
        case -1: why = "file not found"; break;
        case -2: why = "is a directory"; break;
        case -3: why = "empty or too large"; break;
        case -4: why = "a program is already running"; break;
        case -5: why = "read failed"; break;
        case -6: why = "short read"; break;
        case -8: why = "integrity check failed (CRC mismatch)"; break;
        case -9: why = "kernel image is protected"; break;
        default: why = "bad load layout"; break;
        }
        err("RUN failed: %s\n", why);
        return;
    }
    ok("Program exited with code %d.\n", code);
}

/* TC: 编译 C 子集 -> .gtp (默认输出到当前目录, 可用 -o 指定) */
static void tc_usage(void)
{
    err("Usage: TC <src.c> [more.c ...] [-o out] [-O0..-O4] [-Os|-Ofast] [-g] [-v]\n");
    kprintf("  -fconst -fstrength -fdce -floop   (-fno-X disables; linked)\n");
    kprintf("  Multiple sources are flattened into one program\n");
}

static bool tc_flag(struct tc_opts *o, const char *name, bool on)
{
    if (!strcmp(name, "const")) {
        o->fold = on;
        if (!on) {
            o->strength = false;
            o->dce = false;
            o->loops = false;
        }
    } else if (!strcmp(name, "strength")) {
        o->strength = on;
        if (on) o->fold = true;
    } else if (!strcmp(name, "dce")) {
        o->dce = on;
        if (on) o->fold = true;
        else o->loops = false;
    } else if (!strcmp(name, "loop")) {
        o->loops = on;
        if (on) {
            o->fold = true;
            o->dce = true;
        }
    } else {
        return false;
    }
    return true;
}

static void cmd_tc(int argc, char **argv)
{
    struct tc_opts opt;
    tc_opts_default(&opt);

    const char *srcargs[TC_MOD_MAX];
    int nsrc = 0;
    const char *oarg = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) {
            oarg = argv[++i];
            continue;
        }
        if (a[0] != '-' || !a[1]) {
            if (nsrc >= TC_MOD_MAX) {
                err("TC: too many source files (max %d)\n", TC_MOD_MAX);
                return;
            }
            srcargs[nsrc++] = a;
            continue;
        }
        if (!strcmp(a, "-h")) {
            tc_usage();
            return;
        }
        if (!strcmp(a, "-v")) {
            opt.verbose = true;
            continue;
        }
        if (!strcmp(a, "-g")) {
            tc_opts_level(&opt, 0);       /* 调试: 关闭全部优化 */
            continue;
        }
        if (!strcmp(a, "-Os")) {
            tc_opts_level(&opt, 2);       /* 体积优先: 折叠+削弱+分支消除 */
            opt.dce = true;
            opt.loops = false;
            continue;
        }
        if (!strcmp(a, "-Ofast")) {
            tc_opts_level(&opt, 4);
            continue;
        }
        if (a[1] == 'O' && a[2] >= '0' && a[2] <= '4' && !a[3]) {
            tc_opts_level(&opt, a[2] - '0');
            continue;
        }
        if (!strncmp(a, "-fno-", 5)) {
            if (!tc_flag(&opt, a + 5, false)) {
                err("TC: unknown option %s\n", a);
                return;
            }
            continue;
        }
        if (a[1] == 'f') {
            if (!tc_flag(&opt, a + 2, true)) {
                err("TC: unknown option %s\n", a);
                return;
            }
            continue;
        }
        err("TC: unknown option %s\n", a);
        return;
    }
    if (nsrc == 0) {
        tc_usage();
        return;
    }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }

    /* 多模块: 所有源文件读入同一缓冲区, 编译器按顺序自动扁平化 */
    static u8 srcbuf[TC_SRC_MAX] __attribute__((section(".highbss")));
    static char spath[TC_MOD_MAX][PATHMAX] __attribute__((section(".highbss")));
    struct tc_src mods[TC_MOD_MAX];
    u32 used = 0;
    for (int i = 0; i < nsrc; i++) {
        resolve_path(srcargs[i], spath[i]);
        if (read_denied(spath[i]))
            return;
        if (used + 2 >= sizeof(srcbuf)) {
            err("TC: sources too large (max %u bytes)\n",
                (unsigned)sizeof(srcbuf) - 1);
            return;
        }
        u32 got = 0;
        if (!fat_read_file(spath[i], srcbuf + used,
                           sizeof(srcbuf) - used - 1, &got) || got == 0) {
            err("Cannot read %s\n", spath[i]);
            return;
        }
        srcbuf[used + got] = 0;
        mods[i].src = (const char *)srcbuf + used;
        mods[i].path = spath[i];
        used += got + 1;
    }

    u32 plen = 0;
    const u8 *prog = tc_compile_files(mods, nsrc, &plen, &opt);
    if (!prog) {
        err("TC: %s\n", tc_error());
        return;
    }

    char out[PATHMAX];
    if (oarg) {
        resolve_path(oarg, out);
    } else {
        const char *base = spath[0];
        for (const char *p = spath[0]; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        int k = 0;
        out[k++] = fat_drive();
        out[k++] = ':';
        out[k++] = '\\';
        const char *q = base;
        while (*q && *q != '.' && k < PATHMAX - 8)
            out[k++] = *q++;
        const char *ext = ".gtp";
        while (*ext && k < PATHMAX - 1)
            out[k++] = *ext++;
        out[k] = 0;
    }

    if (fat_create(out, prog, plen))
        ok("Compiled %u bytes -> %s\n", plen, out);
    else
        err("Cannot write %s (use -o in your home directory)\n", out);

    if (opt.verbose) {
        u32 cb = 0, db = 0;
        tc_stats(&cb, &db);
        kprintf("TC:-O%d %c%c%c%c code=%u data=%u mods=%d\n", opt.level,
                opt.fold ? 'F' : '-', opt.strength ? 'S' : '-',
                opt.dce ? 'D' : '-', opt.loops ? 'L' : '-', cb, db, nsrc);
    }
}

/* ASM: 汇编源码 -> .gtp (默认输出到当前目录, 可用 -o 指定) */
static void asm_usage(void)
{
    err("Usage: ASM <src.s|src.asm> [-o out.gtp]\n");
    kprintf("  NASM-style: .text/.data, labels, equ, db/dw/dd/dq, times\n");
    kprintf("  64/32-bit registers; mov/lea/alu/jcc/call/push/pop/int...\n");
}

static void cmd_asm(int argc, char **argv)
{
    const char *sarg = 0, *oarg = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) {
            oarg = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "-h")) { asm_usage(); return; }
            err("ASM: unknown option %s\n", a);
            return;
        }
        if (!sarg)
            sarg = a;
    }
    if (!sarg) {
        asm_usage();
        return;
    }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }

    char sa[PATHMAX];
    resolve_path(sarg, sa);
    if (read_denied(sa))
        return;
    static u8 asrc[TC_SRC_MAX] __attribute__((section(".highbss")));
    u32 got = 0;
    if (!fat_read_file(sa, asrc, sizeof(asrc) - 1, &got) || got == 0) {
        err("Cannot read %s\n", sa);
        return;
    }
    asrc[got] = 0;

    u32 plen = 0;
    const u8 *prog = asm_assemble((const char *)asrc, got, &plen);
    if (!prog) {
        err("ASM: %s\n", asm_error());
        return;
    }

    char out[PATHMAX];
    if (oarg) {
        resolve_path(oarg, out);
    } else {
        const char *base = sa;
        for (const char *p = sa; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        int k = 0;
        out[k++] = fat_drive();
        out[k++] = ':';
        out[k++] = '\\';
        const char *q = base;
        while (*q && *q != '.' && k < PATHMAX - 8)
            out[k++] = *q++;
        const char *ext = ".gtp";
        while (*ext && k < PATHMAX - 1)
            out[k++] = *ext++;
        out[k] = 0;
    }

    if (fat_create(out, prog, plen))
        ok("Assembled %u bytes -> %s\n", plen, out);
    else
        err("Cannot write %s (use -o in your home directory)\n", out);
}

/* ------------------------------------------------------------ 网络命令 */
static void mac_str(const u8 *mac, char *out)
{
    static const char *hx = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < 6; i++) {
        out[k++] = hx[mac[i] >> 4];
        out[k++] = hx[mac[i] & 15];
        if (i != 5)
            out[k++] = ':';
    }
    out[k] = 0;
}

static void cmd_net(void)
{
    if (!net_init()) {
        err("No supported NIC (RTL8139) found.\n");
        return;
    }
    struct net_info ni;
    net_get_info(&ni);
    char m[20], a[20], b[20], c[20], d[20];
    mac_str(ni.mac, m);
    net_ip_str(ni.ip, a);
    net_ip_str(ni.mask, b);
    net_ip_str(ni.gw, c);
    net_ip_str(ni.dns, d);
    kprintf("Link      : %s (RTL8139)\n", ni.link ? "up" : "down");
    kprintf("MAC       : %s\n", m);
    kprintf("IPv4      : %s\n", a);
    kprintf("Mask      : %s\n", b);
    kprintf("Gateway   : %s\n", c);
    kprintf("DNS       : %s\n", d);
    kprintf("MTU       : %u\n", ni.mtu);
    kprintf("TCP socks : %d\n", tcp_socket_count());
    kprintf("UDP socks : %d\n", udp_socket_count());
}

static void cmd_dhcp(void)
{
    if (!net_init()) {
        err("No supported NIC (RTL8139) found.\n");
        return;
    }
    if (net_dhcp() == 0) {
        char a[20];
        net_ip_str(net_ip(), a);
        ok("DHCP: %s\n", a);
    } else {
        err("DHCP failed (no server reply).\n");
    }
}

static void cmd_ping(int argc, char **argv)
{
    if (argc < 2) { err("Usage: PING <a.b.c.d> [count]\n"); return; }
    u32 ip;
    if (!net_parse_ip(argv[1], &ip)) { err("Bad IPv4 address.\n"); return; }
    if (!net_init()) { err("No supported NIC (RTL8139) found.\n"); return; }
    int count = argc > 2 ? atoi_dec(argv[2]) : 4;
    if (count < 1) count = 1;
    if (count > 10) count = 10;

    char s[20];
    net_ip_str(ip, s);
    kprintf("PING %s:\n", s);
    int ok_n = 0;
    for (int i = 0; i < count; i++) {
        u32 rtt = 0;
        if (net_ping_one(ip, (u16)i, &rtt)) {
            ok_n++;
            kprintf("  reply from %s seq=%d time=%u ms\n", s, i, rtt);
        } else {
            kprintf("  request timed out (seq=%d)\n", i);
        }
    }
    kprintf("  %d/%d replies\n", ok_n, count);
}

static void cmd_tcp(int argc, char **argv)
{
    if (argc < 3) {
        err("Usage: TCP <a.b.c.d> <port> [text]\n");
        return;
    }
    u32 ip;
    if (!net_parse_ip(argv[1], &ip)) { err("Bad IPv4 address.\n"); return; }
    int port = atoi_dec(argv[2]);
    if (port < 1 || port > 65535) { err("Bad port.\n"); return; }
    if (!net_init()) { err("No supported NIC (RTL8139) found.\n"); return; }

    char text[512];
    size_t used = 0;
    text[0] = 0;
    for (int i = 3; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (used + l + 2 >= sizeof(text))
            break;
        if (used)
            text[used++] = ' ';
        memcpy(text + used, argv[i], l);
        used += l;
        text[used] = 0;
    }

    int fd = tcp_connect(ip, (u16)port);
    if (fd < 0) {
        err("TCP connect failed.\n");
        return;
    }
    ok("Connected, sending %u byte(s).\n", (u32)used);
    if (used)
        tcp_send(fd, text, (u32)used);
    int n = tcp_recv(fd, file_buf, 4096);
    if (n > 0) {
        kprintf("Received %d bytes:\n", n);
        console_write((const char *)file_buf, (u32)n);
        if (file_buf[n - 1] != '\n')
            console_putc('\n');
    } else {
        kprintf("No data received.\n");
    }
    tcp_close(fd);
}

/* UDP 用户 API 测试: 发送 size 字节模式串并等待回显 */
static u8 udp_buf[4096] __attribute__((section(".highbss")));

static void cmd_udp(int argc, char **argv)
{
    if (argc < 3) { err("Usage: UDP <a.b.c.d> <port> [size]\n"); return; }
    u32 ip;
    if (!net_parse_ip(argv[1], &ip)) { err("Bad IPv4 address.\n"); return; }
    int port = atoi_dec(argv[2]);
    if (port < 1 || port > 65535) { err("Bad port.\n"); return; }
    int size = argc > 3 ? atoi_dec(argv[3]) : 24;
    if (size < 1) size = 1;
    if (size > (int)sizeof(udp_buf)) size = (int)sizeof(udp_buf);
    if (!net_init()) { err("No supported NIC (RTL8139) found.\n"); return; }

    int fd = udp_open(0);
    if (fd < 0) { err("UDP: no free socket.\n"); return; }

    static const char pat[] = "UDP-TEST:0123456789abcdef";
    int plen = (int)sizeof(pat) - 1;
    for (int i = 0; i < size; i++)
        udp_buf[i] = (u8)pat[i % plen];

    if (udp_sendto(fd, ip, (u16)port, udp_buf, (u32)size) < 0) {
        err("UDP: send failed.\n");
        udp_close(fd);
        return;
    }
    u32 src = 0;
    u16 sport = 0;
    int n = udp_recvfrom(fd, file_buf, 4096, 2000, &src, &sport);
    if (n == -2) {
        err("UDP: ICMP unreachable (type 3).\n");
    } else if (n < 0) {
        err("UDP: no reply.\n");
    } else {
        bool same = (n == size) && memcmp(file_buf, udp_buf, (u32)n) == 0;
        char s[20];
        net_ip_str(src, s);
        ok("UDP: sent %d, received %d from %s:%u%s\n",
           size, n, s, (u32)sport, same ? " (match)" : " (diff)");
    }
    udp_close(fd);
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    return b;
}

static void cmd_dns(int argc, char **argv)
{
    if (argc < 2) { err("Usage: DNS <name>\n"); return; }
    u32 ip = 0;
    if (net_dns_resolve(argv[1], &ip) == 0) {
        char s[20];
        net_ip_str(ip, s);
        ok("%s = %s\n", argv[1], s);
    } else {
        err("DNS lookup failed: %s\n", argv[1]);
    }
}

static void cmd_tftp(int argc, char **argv)
{
    if (argc < 4 || strcasecmp(argv[1], "get")) {
        err("Usage: TFTP GET <ip[:port]> <remote> [local]\n");
        return;
    }
    char host[64];
    u16 port = 69;
    int k = 0;
    const char *p = argv[2];
    while (*p && *p != ':' && k < 63)
        host[k++] = *p++;
    host[k] = 0;
    if (*p == ':')
        port = (u16)atoi_dec(p + 1);
    u32 ip;
    if (!net_parse_ip(host, &ip) || port == 0) {
        err("Bad server address.\n");
        return;
    }
    const char *remote = argv[3];
    const char *lname = argc > 4 ? argv[4] : base_name(remote);
    if (!lname[0])
        lname = "download.bin";
    char loc[PATHMAX];
    resolve_path(lname, loc);

    int n = net_tftp_fetch(ip, port, remote, loc);
    if (n < 0)
        err("TFTP download failed.\n");
    else
        ok("TFTP: %d bytes -> %s\n", n, loc);
}

static void cmd_http(int argc, char **argv)
{
    if (argc < 4 || strcasecmp(argv[1], "get")) {
        err("Usage: HTTP GET <host[:port]> <path> [local]\n");
        return;
    }
    const char *path = argv[3];
    const char *lname = argc > 4 ? argv[4] : base_name(path);
    if (!lname[0])
        lname = "index.html";
    char loc[PATHMAX];
    resolve_path(lname, loc);

    int n = net_http_fetch(argv[2], path, loc);
    if (n < 0)
        err("HTTP download failed.\n");
    else
        ok("HTTP: %d bytes -> %s\n", n, loc);
}

/* ------------------------------------------------------------ 任务/服务 */
static void cmd_tasks(void)
{
    task_dump();
}

/* THREADS: 当前线程表 (主线程 + 程序创建的线程) */
static void cmd_threads(void)
{
    sched_dump();
}

/* DESKTOP/GUI: 进入图形桌面; TEXTMODE: 返回纯文本 */
static void cmd_gui(void)
{
    if (gui_active()) {
        kprintf("desktop already active (TEXTMODE to leave)\n");
        return;
    }
    if (!gui_enter())
        ui_err("GUI unavailable: no graphics mode\n");
}

static void cmd_textmode(void)
{
    if (!gui_active()) {
        kprintf("desktop not active\n");
        return;
    }
    gui_leave();
}

/* SHOW <file.png>: 图形模式查看 PNG (完整解码: 所有颜色类型/位深/过滤/隔行).
   桌面激活时借用画布, 否则暂停控制台绘制; 任意键返回. */
static void cmd_show(int argc, char **argv)
{
    if (argc < 2) { err("Usage: SHOW <file.png>\n"); return; }
    if (!gfx_active()) { err("No graphics mode (SHOW needs VBE).\n"); return; }
    if (!fat_ready()) { err("No volume mounted. Type MOUNT first.\n"); return; }

    char abs[PATHMAX];
    resolve_path(argv[1], abs);

    bool canvas = false;
    if (gui_active()) {
        canvas = gui_canvas_begin();
        if (!canvas)
            vga_hold_display(true);
    } else {
        vga_hold_display(true);
    }

    gfx_clear(0x101820);
    bool drawn = png_draw_file_fit(abs);
    gfx_flip();
    gfx_poked = false;

    kprintf("%s: %s\n", abs, drawn ? "displayed" : "decode failed");
    if (!drawn)
        kprintf("  (no graphics, missing file, or unsupported PNG)\n");
    kprintf("Press any key to continue...\n");
    ui_read_key();

    if (canvas) {
        gui_canvas_end();
    } else {
        vga_hold_display(false);
        vga_refresh();
    }
}

/* PNGINFO <file.png>: 打印 PNG 头信息 (宽高/位深/颜色类型/隔行) */
static void cmd_pnginfo(int argc, char **argv)
{
    if (argc < 2) { err("Usage: PNGINFO <file.png>\n"); return; }
    if (!fat_ready()) { err("No volume mounted. Type MOUNT first.\n"); return; }

    static u8 hdr[2048] __attribute__((section(".highbss")));
    char abs[PATHMAX];
    resolve_path(argv[1], abs);

    struct fat_dirent de;
    if (!fat_find(abs, &de) || de.is_dir) {
        err("No such file: %s\n", abs);
        return;
    }
    u32 got = 0;
    if (!fat_read_file(abs, hdr, sizeof(hdr), &got) || got < 33) {
        err("Not a PNG (too small or read failed)\n");
        return;
    }
    struct png_info pi;
    if (png_info(hdr, got, &pi) != 0) {
        err("Not a valid PNG: %s\n", abs);
        return;
    }
    static const char *cname[7] = {
        0, "gray", "rgb", "palette", "gray+alpha", 0, "rgba"
    };
    const char *cn = (pi.ctype < 7 && cname[pi.ctype]) ? cname[pi.ctype] : "?";
    ok("PNG %s: %d x %d, depth=%d, color=%s, interlace=%d\n",
       abs, pi.w, pi.h, pi.depth, cn, pi.interlace);
}


/* 系统服务是 \GT-DOS\SERVICES 下的 .gtp 程序, 以 SYS 任务身份运行且受保护 */
static void cmd_service(int argc, char **argv)
{
    char abs[PATHMAX];
    if (argc < 2 || !strcasecmp(argv[1], "list")) {
        if (!fat_ready()) { err("No volume mounted. Type MOUNT first.\n"); return; }
        resolve_path("\\GT-DOS\\SERVICES", abs);
        struct fat_dirent ents[32];
        fat_set_show_all(true);
        int n = fat_list_dir(abs, ents, 32);
        fat_set_show_all(false);
        if (n < 0) { err("No system services installed.\n"); return; }
        kprintf("System services (\\GT-DOS\\SERVICES, SYS/protected):\n");
        for (int i = 0; i < n; i++) {
            if (ents[i].is_dir)
                continue;
            kprintf("  [SYS] %-14s %6u bytes\n", ents[i].name, ents[i].size);
        }
        return;
    }
    if (!strcasecmp(argv[1], "run")) {
        if (argc < 3) { err("Usage: SERVICE RUN <name>\n"); return; }
        if (!need_admin()) return;
        char name[40];
        int k = 0;
        for (const char *p = argv[2]; *p && k < 31; p++)
            name[k++] = *p;
        name[k] = 0;
        bool dot = false;
        for (int i = 0; name[i]; i++)
            if (name[i] == '.') dot = true;
        if (!dot) {
            if (k > 34) k = 34;
            name[k++] = '.'; name[k++] = 'g'; name[k++] = 't'; name[k++] = 'p';
            name[k] = 0;
        }
        char rel[96];
        int m = 0;
        const char *pre = "\\GT-DOS\\SERVICES\\";
        while (*pre && m < 80) rel[m++] = *pre++;
        for (int i = 0; name[i] && m < 90; i++) rel[m++] = name[i];
        rel[m] = 0;
        resolve_path(rel, abs);
        int rc = exec_run(abs);
        if (rc < 0)
            err("Service failed to start (%d).\n", rc);
        else
            ok("Service %s exited with code %d.\n", name, rc);
        return;
    }
    if (!strcasecmp(argv[1], "stop")) {
        if (argc < 3) { err("Usage: SERVICE STOP <name>\n"); return; }
        int rc = task_stop(argv[2]);
        if (rc == -2)
            err("Permission denied: system service is protected (SYS).\n");
        else if (rc == -3)
            err("Service is running (cooperative); cannot stop.\n");
        else if (rc < 0)
            err("No such service task: %s\n", argv[2]);
        else
            ok("Service stopped.\n");
        return;
    }
    err("Usage: SERVICE LIST | RUN <name> | STOP <name>\n");
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
    if (read_denied(sa)) return;

    struct fat_dirent d;
    if (!fat_find(sa, &d) || d.is_dir) { err("Cannot read %s\n", sa); return; }
    if (d.size >= FILE_BUF_MAX) { err("File too large (max 64 KB): %s\n", sa); return; }
    u32 got = 0;
    if (!fat_read_file(sa, file_buf, FILE_BUF_MAX - 1, &got)) {
        err("Cannot read %s\n", sa);
        return;
    }
    if (!fat_create(da, file_buf, got)) {
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
    if (read_denied(sa)) return;

    struct fat_dirent d;
    if (!fat_find(sa, &d) || d.is_dir) { err("Cannot read %s\n", sa); return; }
    if (d.size >= FILE_BUF_MAX) { err("File too large (max 64 KB): %s\n", sa); return; }
    u32 got = 0;
    if (!fat_read_file(sa, file_buf, FILE_BUF_MAX - 1, &got)) {
        err("Cannot read %s\n", sa);
        return;
    }
    if (!fat_create(da, file_buf, got)) {
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
        if (lines < 1) lines = 1;
        if (lines > 1000) lines = 1000;
        fi = 2;
        if (argc < 3) { err("Usage: HEAD -n <file>\n"); return; }
    }
    char abs[PATHMAX];
    resolve_path(argv[fi], abs);
    if (read_denied(abs)) return;

    u32 got = 0;
    if (!fat_read_file(abs, file_buf, FILE_BUF_MAX - 1, &got)) {
        err("File not found: %s\n", abs);
        return;
    }
    file_buf[got] = '\0';
    int shown = 0;
    char *p = (char *)file_buf;
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
    if (read_denied(abs)) return;

    struct fat_dirent d;
    if (!fat_find(abs, &d) || d.is_dir) { err("File not found: %s\n", abs); return; }
    if (d.size >= FILE_BUF_MAX) { err("File too large (max 64 KB): %s\n", abs); return; }
    u32 got = 0;
    if (!fat_read_file(abs, file_buf, FILE_BUF_MAX - 1, &got)) {
        err("File not found: %s\n", abs);
        return;
    }
    file_buf[got] = '\0';
    u32 lines = 0, words = 0, bytes = got;
    bool in_w = false;
    for (u32 i = 0; i < got; i++) {
        char c = (char)file_buf[i];
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
    if (read_denied(abs)) return;

    struct fat_dirent d;
    if (!fat_find(abs, &d) || d.is_dir) { err("File not found: %s\n", abs); return; }
    if (d.size >= FILE_BUF_MAX) { err("File too large (max 64 KB): %s\n", abs); return; }
    u32 got = 0;
    if (!fat_read_file(abs, file_buf, FILE_BUF_MAX - 1, &got)) {
        err("File not found: %s\n", abs);
        return;
    }
    file_buf[got] = '\0';
    console_write((const char *)file_buf, got);
    if (got && file_buf[got - 1] != '\n')
        console_putc('\n');
}

/* CAT: 一次显示多个文件, -n 显示行号 */
static void cmd_cat(int argc, char **argv)
{
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    bool nums = false;
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        for (const char *o = argv[first] + 1; *o; o++)
            if (*o == 'n')
                nums = true;
        first++;
    }
    if (first >= argc) { err("Usage: CAT [-n] <file...>\n"); return; }

    int line = 1;
    for (int fi = first; fi < argc; fi++) {
        char abs[PATHMAX];
        resolve_path(argv[fi], abs);
        if (read_denied(abs))
            continue;
        struct fat_dirent d;
        if (!fat_find(abs, &d) || d.is_dir) {
            err("CAT: %s: not a file\n", argv[fi]);
            continue;
        }
        if (d.size >= FILE_BUF_MAX) {
            err("CAT: %s: too large (max 64 KB)\n", argv[fi]);
            continue;
        }
        u32 got = 0;
        if (!fat_read_file(abs, file_buf, FILE_BUF_MAX - 1, &got)) {
            err("CAT: cannot read %s\n", abs);
            continue;
        }
        if (!nums) {
            console_write((const char *)file_buf, got);
            if (got && file_buf[got - 1] != '\n')
                console_putc('\n');
            continue;
        }
        u32 p = 0;
        while (p < got) {
            u32 q = p;
            while (q < got && file_buf[q] != '\n')
                q++;
            kprintf("%6d  ", line++);
            console_write((const char *)file_buf + p, q - p);
            console_putc('\n');
            p = q + 1;
        }
    }
}

/* RICH: Markdown 子集渲染 (标题/粗体/斜体/代码/列表/引用/表格) */
static void cmd_rich(int argc, char **argv)
{
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    if (argc < 2) { err("Usage: RICH <file.md>\n"); return; }
    for (int fi = 1; fi < argc; fi++) {
        char abs[PATHMAX];
        resolve_path(argv[fi], abs);
        if (read_denied(abs))
            continue;
        struct fat_dirent d;
        if (!fat_find(abs, &d) || d.is_dir) {
            err("RICH: %s: not a file\n", argv[fi]);
            continue;
        }
        if (d.size >= FILE_BUF_MAX) {
            err("RICH: %s: too large (max 64 KB)\n", argv[fi]);
            continue;
        }
        u32 got = 0;
        if (!fat_read_file(abs, file_buf, FILE_BUF_MAX - 1, &got)) {
            err("RICH: cannot read %s\n", abs);
            continue;
        }
        rich_render((const char *)file_buf, got, base_name(abs));
    }
}

/* VIM: 全屏模态编辑器 (normal / insert / command / search) */
static void cmd_vim(int argc, char **argv)
{
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    if (argc < 2) { err("Usage: VIM <file>\n"); return; }
    char abs[PATHMAX];
    resolve_path(argv[1], abs);
    if (read_denied(abs))
        return;

    int rc = vim_run(abs);
    vga_refresh();
    tui_status_refresh();
    if (rc == -1)
        err("VIM: graphics mode is required\n");
    else if (rc == -2)
        err("VIM: file too large (max 256 KB)\n");
    else if (rc == -3)
        err("VIM: read failed\n");
    else
        ok("VIM: %s\n", abs);
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

/* APPEND: 追加一行文本 (便于逐行写入 C 源文件) */
static void cmd_append(const char *name, const char *text)
{
    if (!name) { err("Usage: APPEND <file> <text>\n"); return; }
    if (!fat_ready()) { err("No volume mounted.\n"); return; }
    if (!text) text = "";
    char abs[PATHMAX];
    resolve_path(name, abs);
    if (read_denied(abs)) return;

    u32 got = 0;
    struct fat_dirent fd;
    if (fat_find(abs, &fd) && !fd.is_dir)
        fat_read_file(abs, file_buf, FILE_BUF_MAX - 1, &got);
    size_t tl = strlen(text);
    if (got + tl + 2 >= FILE_BUF_MAX) { err("File too large (max 64 KB)\n"); return; }
    if (got && file_buf[got - 1] != '\n')
        file_buf[got++] = '\n';
    memcpy(file_buf + got, text, tl);
    got += (u32)tl;
    file_buf[got++] = '\n';
    if (fat_create(abs, file_buf, got))
        ok("Appended %u bytes to %s\n", (u32)tl + 1, abs);
    else
        err("Failed to write %s\n", abs);
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
    } else if (!strncasecmp(cmd, "kernel", 8)) {
        cmd_kernel(argc, argv);
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
    } else if (!strcasecmp(cmd, "net")) {
        cmd_net();
    } else if (!strcasecmp(cmd, "dhcp")) {
        cmd_dhcp();
    } else if (!strcasecmp(cmd, "ping")) {
        cmd_ping(argc, argv);
    } else if (!strcasecmp(cmd, "tcp")) {
        cmd_tcp(argc, argv);
    } else if (!strcasecmp(cmd, "udp")) {
        cmd_udp(argc, argv);
    } else if (!strcasecmp(cmd, "dns")) {
        cmd_dns(argc, argv);
    } else if (!strcasecmp(cmd, "tftp")) {
        cmd_tftp(argc, argv);
    } else if (!strcasecmp(cmd, "http")) {
        cmd_http(argc, argv);
    } else if (!strcasecmp(cmd, "tasks") || !strcasecmp(cmd, "ps")) {
        cmd_tasks();
    } else if (!strcasecmp(cmd, "threads")) {
        cmd_threads();
    } else if (!strcasecmp(cmd, "desktop") || !strcasecmp(cmd, "gui")) {
        cmd_gui();
    } else if (!strcasecmp(cmd, "textmode")) {
        cmd_textmode();
    } else if (!strcasecmp(cmd, "show") || !strcasecmp(cmd, "image") ||
               !strcasecmp(cmd, "view")) {
        cmd_show(argc, argv);
    } else if (!strcasecmp(cmd, "pnginfo")) {
        cmd_pnginfo(argc, argv);
    } else if (!strcasecmp(cmd, "service") || !strcasecmp(cmd, "services")) {
        cmd_service(argc, argv);
    } else if (!strcasecmp(cmd, "tc")) {
        cmd_tc(argc, argv);
    } else if (!strcasecmp(cmd, "asm")) {
        cmd_asm(argc, argv);
    } else if (!strncasecmp(cmd, "run", 8)) {
        cmd_run(argc, argv);
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
    } else if (!strncasecmp(cmd, "type", 8)) {
        cmd_type(argc > 1 ? argv[1] : 0);
    } else if (!strncasecmp(cmd, "cat", 8)) {
        cmd_cat(argc, argv);
    } else if (!strncasecmp(cmd, "rich", 8)) {
        cmd_rich(argc, argv);
    } else if (!strncasecmp(cmd, "vim", 8) || !strncasecmp(cmd, "vi", 8)) {
        cmd_vim(argc, argv);
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
    } else if (!strncasecmp(cmd, "append", 8)) {
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
        cmd_append(argc > 1 ? argv[1] : 0, text);
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
        if (!need_admin()) return;
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