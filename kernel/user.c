/* ==========================================================================
 *  GT-DOS 用户管理系统 (USERS.SYS)
 *  ---------------------------------------------------------------------------
 *  - 用户库以 "name:hash:flags" 文本行持久化到 FAT
 *  - 口令用 FNV-1a 混合盐 (用户名+固定域盐) 哈希, 不存明文
 *  - 提供增删改查 / 登录校验 / 当前会话用户
 * ========================================================================== */
#include "gt.h"

#define USER_FILE "\\GT-DOS\\USERS.SYS"
#define PASS_SALT "gt-dos-1f3a"

/* 用户库固定放在系统盘的 \GT-DOS\ 目录 */
static void user_path(char *out)
{
    out[0] = gcfg.sys_drive ? gcfg.sys_drive : 'C';
    out[1] = ':';
    strcpy(out + 2, USER_FILE);
}

struct gt_user users[USER_MAX];
static int user_n;
static int cur_user = -1;             /* 当前登录用户索引 */

/* 默认用户 (磁盘不可用 / 无用户库时的兜底, 拥有管理员权限) */
static const struct gt_user default_user = { "admin", 0, UF_ADMIN };

/* --------------------------------------------------------------- 哈希 */
u32 str_hash(const char *salt, const char *s)
{
    u32 h = 2166136261u;
    for (const char *p = salt; p && *p; p++) {
        h ^= (u8)*p;
        h *= 16777619u;
    }
    for (const char *p = s; *p; p++) {
        h ^= (u8)*p;
        h *= 16777619u;
    }
    /* 再打散几轮, 让短口令不那么容易被字典秒破 (玩具级) */
    for (int i = 0; i < 1024; i++) {
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
    }
    if (h == 0)
        h = 1;                        /* 0 表示"无口令", 避免碰撞 */
    return h;
}

static u32 hash_pass(const char *name, const char *pass)
{
    if (!pass || !pass[0])
        return 0;                     /* 空口令 = 0 */
    char salt[64];
    int n = 0;
    const char *p;
    for (p = PASS_SALT; *p; p++) salt[n++] = *p;
    for (p = name; *p && n < 60; p++) salt[n++] = *p;
    salt[n] = '\0';
    return str_hash(salt, pass);
}

/* ------------------------------------------------------------- 持久化 */
static int find(const char *name)
{
    for (int i = 0; i < user_n; i++)
        if (!strncasecmp(users[i].name, name, UNAME_MAX))
            return i;
    return -1;
}

bool user_save(void)
{
    if (!fat_ready_drive(gcfg.sys_drive ? gcfg.sys_drive : 'C'))
        return false;

    char path[32];
    user_path(path);
    char buf[1024];
    int n = 0;
    for (int i = 0; i < user_n; i++) {
        const char *nm = users[i].name;
        while (*nm && n < 900) buf[n++] = *nm++;
        buf[n++] = ':';
        n += fmt_uint(users[i].hash, buf + n);
        buf[n++] = ':';
        buf[n++] = (char)('0' + users[i].flags);
        buf[n++] = '\n';
    }
    fat_set_allow_sys(true);
    bool ok = fat_create(path, buf, (u32)n);
    fat_set_allow_sys(false);
    return ok;
}

static void parse_line(char *s)
{
    char *name = s;
    char *c1 = s;
    while (*c1 && *c1 != ':')
        c1++;
    if (*c1 != ':')
        return;
    *c1++ = '\0';
    char *c2 = c1;
    while (*c2 && *c2 != ':')
        c2++;
    if (*c2 != ':')
        return;
    *c2++ = '\0';

    if (name[0] == '\0' || user_n >= USER_MAX)
        return;

    struct gt_user *u = &users[user_n];
    strncpy(u->name, name, UNAME_MAX - 1);
    u->name[UNAME_MAX - 1] = '\0';
    u->hash = (u32)atoi_dec(c1);
    u->flags = (u8)atoi_dec(c2);
    user_n++;
}

void user_init(void)
{
    user_n = 0;
    cur_user = -1;

    if (!fat_ready_drive(gcfg.sys_drive ? gcfg.sys_drive : 'C'))
        goto fallback;

    {
        char path[32];
        user_path(path);
        static char buf[1024];
        u32 got = 0;
        if (!fat_read_file(path, buf, sizeof(buf) - 1, &got))
            goto fallback;
        buf[got] = '\0';

        char *line = buf;
        while (*line) {
            char *nl = line;
            while (*nl && *nl != '\n')
                nl++;
            if (*nl) {
                *nl = '\0';
                nl++;
            }
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\r'))
                line[--l] = '\0';
            if (line[0])
                parse_line(line);
            line = nl;
        }
    }

fallback:
    /* 用户库为空时兜底: 保证始终存在一个 admin 管理员,
     * 否则 OOBE 跳过建号后无人可登录 */
    if (user_n == 0) {
        user_add("admin", "", UF_ADMIN);
        user_save();
    }
}

int  user_count(void) { return user_n; }

const struct gt_user *user_get(int i)
{
    if (i < 0 || i >= user_n)
        return 0;
    return &users[i];
}

bool user_exists(const char *name) { return find(name) >= 0; }

/* ------------------------------------------------------------- 增删改 */
bool user_add(const char *name, const char *pass, u8 flags)
{
    if (user_n >= USER_MAX)
        return false;
    if (!name || !name[0])
        return false;
    /* 用户名规则: 1-15 字符, 字母/数字/下划线/连字符, 不以数字开头 */
    size_t l = strlen(name);
    if (l >= UNAME_MAX)
        return false;
    if (!(isalpha_(name[0]) || name[0] == '_'))
        return false;
    for (size_t i = 0; i < l; i++) {
        char c = name[i];
        if (!(isalpha_(c) || isdigit_(c) || c == '_' || c == '-'))
            return false;
    }
    if (find(name) >= 0)
        return false;

    struct gt_user *u = &users[user_n];
    strncpy(u->name, name, UNAME_MAX - 1);
    u->name[UNAME_MAX - 1] = '\0';
    u->hash = hash_pass(name, pass);
    u->flags = flags;
    user_n++;
    return true;
}

bool user_del(const char *name)
{
    int i = find(name);
    if (i < 0)
        return false;
    if (!strncasecmp(users[i].name, "admin", 5) && (users[i].flags & UF_ADMIN))
        return false;                 /* 内置 admin 不可删 (类 Windows) */
    if (i == cur_user)
        return false;                 /* 不能删当前登录用户 */
    for (int j = i; j < user_n - 1; j++)
        users[j] = users[j + 1];
    user_n--;
    if (cur_user > i)
        cur_user--;
    return true;
}

bool user_setpass(const char *name, const char *pass)
{
    int i = find(name);
    if (i < 0)
        return false;
    users[i].hash = hash_pass(name, pass);
    return true;
}

bool user_check(const char *name, const char *pass)
{
    int i = find(name);
    if (i < 0)
        return false;
    if (users[i].hash == 0)
        return true;                  /* 无口令用户 */
    return users[i].hash == hash_pass(name, pass);
}

/* ------------------------------------------------------------- 会话 */
const struct gt_user *user_current(void)
{
    if (cur_user >= 0 && cur_user < user_n)
        return &users[cur_user];
    return &default_user;
}

bool user_is_admin(void)
{
    return (user_current()->flags & UF_ADMIN) != 0;
}

const char *user_name(void) { return user_current()->name; }

void user_login(const struct gt_user *u)
{
    if (!u) {
        cur_user = -1;
        return;
    }
    cur_user = find(u->name);
    strncpy(gcfg.last_user, u->name, sizeof(gcfg.last_user) - 1);
    gcfg.last_user[sizeof(gcfg.last_user) - 1] = '\0';
    cfg_save();
}

void user_logout(void)
{
    cur_user = -1;
}

/* --------------------------------------------------------- 登录界面 */
const struct gt_user *ui_login_screen(void)
{
    char name[UNAME_MAX];
    char pass[64];
    const struct gt_user *result = &default_user;

    console_puts("\n");
    tui_frame_open(L("login.title"));
    ui_info("%s", L("login.hint"));

    for (int tries = 0; tries < 5; tries++) {
        int r = ui_read_line(L("login.user"), name, sizeof(name), 0, false);
        if (r == -1) { power_reboot(); }
        if (r == 1)
            break;                    /* Ctrl-C: 跳过登录 */
        if (name[0] == '\0') {
            ui_err("%s", L("login.empty"));
            continue;                 /* 用户名不能为空 */
        }

        if (!user_exists(name)) {
            ui_err("%s", L("login.nouser"));
            continue;
        }

        r = ui_read_line(L("login.pass"), pass, sizeof(pass), '*', false);
        if (r == -1) { power_reboot(); }
        if (r == 1)
            break;

        if (user_check(name, pass)) {
            int i = find(name);
            ui_ok(L("login.welcome"), name,
                  (users[i].flags & UF_ADMIN) ? " (admin)" : "");
            result = &users[i];
            break;
        }
        ui_err("%s", L("login.wrong"));
        timer_sleep_ms(600);          /* 拖慢爆破 */
    }

    tui_frame_close();
    return result;                    /* 兜底: admin */
}
