/* ==========================================================================
 *  GT-DOS 系统配置持久化 (GTOS.CFG)
 *  ---------------------------------------------------------------------------
 *  以 "key=value" 文本行存放在 FAT 根目录, 便于 TYPE 命令直接查看.
 *  磁盘不可用时退化为内存默认值 (仍可正常开机).
 * ========================================================================== */
#include "gt.h"

#define CFG_FILE "\\GT-DOS\\GTOS.CFG"

/* 系统文件固定放在系统盘的 \GT-DOS\ 目录 (如 "C:\GT-DOS\GTOS.CFG") */
static void sys_path(char *out)
{
    out[0] = gcfg.sys_drive ? gcfg.sys_drive : 'C';
    out[1] = ':';
    strcpy(out + 2, CFG_FILE);
}

struct gt_config gcfg = {
    false,          /* oobe_done */
    C_LGRAY,        /* theme_fg */
    C_BLACK,        /* theme_bg */
    0,              /* lang */
    "admin",        /* last_user */
    "GT-DOS",       /* hostname */
    'C',            /* sys_drive */
};

/* ------------------------------------------------------------ 行解析 */
static void set_key(const char *key, const char *val)
{
    if (!strcmp(key, "oobe_done"))
        gcfg.oobe_done = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
    else if (!strcmp(key, "theme_fg"))
        gcfg.theme_fg = (u8)atoi_dec(val);
    else if (!strcmp(key, "theme_bg"))
        gcfg.theme_bg = (u8)atoi_dec(val);
    else if (!strcmp(key, "last_user")) {
        strncpy(gcfg.last_user, val, sizeof(gcfg.last_user) - 1);
        gcfg.last_user[sizeof(gcfg.last_user) - 1] = '\0';
    } else if (!strcmp(key, "hostname")) {
        strncpy(gcfg.hostname, val, sizeof(gcfg.hostname) - 1);
        gcfg.hostname[sizeof(gcfg.hostname) - 1] = '\0';
    } else if (!strcmp(key, "lang")) {
        gcfg.lang = (char)atoi_dec(val);
    } else if (!strcmp(key, "sys_drive")) {
        gcfg.sys_drive = (char)toupper_(val[0]);
    }
}

static void parse_text(char *s)
{
    char *line = s;
    while (*line) {
        char *nl = line;
        while (*nl && *nl != '\n')
            nl++;
        if (*nl) {
            *nl = '\0';
            nl++;
        }
        /* 去掉行尾 \r */
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' '))
            line[--l] = '\0';

        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (*eq == '=' && eq != line) {
            *eq = '\0';
            set_key(line, eq + 1);
        }
        line = nl;
    }
}

bool cfg_load(void)
{
    if (!fat_ready_drive(gcfg.sys_drive ? gcfg.sys_drive : 'C'))
        return false;

    char path[32];
    sys_path(path);
    static char buf[512];
    u32 got = 0;
    if (!fat_read_file(path, buf, sizeof(buf) - 1, &got))
        return false;
    buf[got] = '\0';
    parse_text(buf);
    return true;
}

bool cfg_save(void)
{
    if (!fat_ready_drive(gcfg.sys_drive ? gcfg.sys_drive : 'C'))
        return false;

    char path[32];
    sys_path(path);

    char buf[256];
    int n = 0;
    const char *s;

    s = "oobe_done=";
    while (*s) buf[n++] = *s++;
    buf[n++] = gcfg.oobe_done ? '1' : '0';
    buf[n++] = '\n';

    s = "theme_fg=";
    while (*s) buf[n++] = *s++;
    n += fmt_uint(gcfg.theme_fg, buf + n);
    buf[n++] = '\n';

    s = "theme_bg=";
    while (*s) buf[n++] = *s++;
    n += fmt_uint(gcfg.theme_bg, buf + n);
    buf[n++] = '\n';

    s = "lang=";
    while (*s) buf[n++] = *s++;
    buf[n++] = (char)('0' + (gcfg.lang ? 1 : 0));
    buf[n++] = '\n';

    s = "last_user=";
    while (*s) buf[n++] = *s++;
    for (int i = 0; gcfg.last_user[i] && n < 240; i++)
        buf[n++] = gcfg.last_user[i];
    buf[n++] = '\n';

    s = "hostname=";
    while (*s) buf[n++] = *s++;
    for (int i = 0; gcfg.hostname[i] && n < 240; i++)
        buf[n++] = gcfg.hostname[i];
    buf[n++] = '\n';

    s = "sys_drive=";
    while (*s) buf[n++] = *s++;
    buf[n++] = gcfg.sys_drive;
    buf[n++] = '\n';

    fat_set_allow_sys(true);
    bool ok = fat_create(path, buf, (u32)n);
    fat_set_allow_sys(false);
    return ok;
}

void cfg_apply_theme(void)
{
    gt_color2(gcfg.theme_fg, gcfg.theme_bg);
}
